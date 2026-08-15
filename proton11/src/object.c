#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "object.h"
#include "table.h"

static Obj* objects = NULL;
static Table strings; // intern pool
static bool internedInit = false;

static Obj* allocateObject(size_t size, ObjType type) {
    Obj* object = malloc(size);
    object->type = type;
    object->next = objects;
    objects = object;
    return object;
}

static uint32_t hashString(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

static ObjString* allocateString(char* chars, int length, uint32_t hash) {
    ObjString* string = (ObjString*)allocateObject(sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->chars = chars;
    string->hash = hash;
    string->isRegional = false;
    string->region = NULL; // permanently allocated, not region-owned
    tableSet(&strings, string, NIL_VAL);
    return string;
}

ObjString* takeString(char* chars, int length) {
    if (!internedInit) { initTable(&strings); internedInit = true; }
    uint32_t hash = hashString(chars, length);
    ObjString* interned = tableFindString(&strings, chars, length, hash);
    if (interned != NULL) {
        free(chars);
        return interned;
    }
    return allocateString(chars, length, hash);
}

ObjString* copyString(const char* chars, int length) {
    if (!internedInit) { initTable(&strings); internedInit = true; }
    uint32_t hash = hashString(chars, length);
    ObjString* interned = tableFindString(&strings, chars, length, hash);
    if (interned != NULL) return interned;

    char* heapChars = malloc(length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    return allocateString(heapChars, length, hash);
}

// Region-scoped counterpart of takeString: never interned, never linked
// into the `objects` list, and both the ObjString struct and its char
// buffer are carved out of `region` rather than malloc'd individually.
// `chars` is expected to be a malloc'd, NUL-terminated buffer the caller
// no longer needs after this call (mirrors takeString's "takes ownership"
// convention) -- it gets copied into the region and then freed, since the
// region needs its own contiguous copy to reclaim in one shot at
// regionDestroy time.
ObjString* regionTakeString(Region* region, char* chars, int length) {
    ObjString* string = (ObjString*)regionAlloc(region, sizeof(ObjString));
    char* regionChars = (char*)regionAlloc(region, (size_t)length + 1);
    memcpy(regionChars, chars, (size_t)length);
    regionChars[length] = '\0';
    free(chars);

    string->obj.type = OBJ_STRING;
    string->obj.next = NULL; // not linked into the permanent `objects` list
    string->length = length;
    string->chars = regionChars;
    string->hash = hashString(regionChars, length);
    string->isRegional = true;
    string->region = region;
    return string;
}

// See object.h for the rationale. Unlike regionTakeString, `chars` is a
// borrowed, non-owning pointer (const, not freed here) -- the source
// string (still backed by its own, soon-to-be-destroyed region) is left
// untouched; we just carve out fresh space in `dest` and memcpy into it.
ObjString* regionCopyString(Region* dest, const char* chars, int length) {
    ObjString* string = (ObjString*)regionAlloc(dest, sizeof(ObjString));
    char* regionChars = (char*)regionAlloc(dest, (size_t)length + 1);
    memcpy(regionChars, chars, (size_t)length);
    regionChars[length] = '\0';

    string->obj.type = OBJ_STRING;
    string->obj.next = NULL;
    string->length = length;
    string->chars = regionChars;
    string->hash = hashString(regionChars, length);
    string->isRegional = true; // still regional -- just in a different (enclosing) region now
    string->region = dest;
    return string;
}

// Region-scoped list: the ObjList header itself is carved out of
// `region` (mirrors regionTakeString's ObjString header), never linked
// into the permanent `objects` list, and never freed individually --
// its whole arena goes away in one shot at regionDestroy time.
ObjList* newList(Region* region, ProtonType elemType) {
    ObjList* list = (ObjList*)regionAlloc(region, sizeof(ObjList));
    list->obj.type = OBJ_LIST;
    list->obj.next = NULL; // not linked into the permanent `objects` list
    list->region = region;
    list->count = 0;
    list->capacity = 0;
    list->items = NULL;
    list->elemType = elemType;
    return list;
}

// Amortized O(1) append: when the current backing array is full, a new
// array of double the capacity is carved out of the region and the
// existing elements are copied over (direct pointer-arithmetic memcpy,
// not a per-element loop). The old backing array is simply abandoned in
// the region's bump arena -- there is no realloc()/free() anywhere in
// this path, by design (regions don't support freeing individual
// allocations, only growing and bulk-destroying).
void appendList(ObjList* list, Value value) {
    if (list->count + 1 > list->capacity) {
        int oldCapacity = list->capacity;
        int newCapacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        Value* newItems = (Value*)regionAlloc(list->region, sizeof(Value) * (size_t)newCapacity);
        if (list->count > 0) {
            memcpy(newItems, list->items, sizeof(Value) * (size_t)list->count);
        }
        list->items = newItems;
        list->capacity = newCapacity;
    }
    list->items[list->count++] = value;
}

// Public: see object.h for the full rationale. Originally a static
// helper private to regionCopyList/permanentCopyList (list elements);
// promoted to a public entry point so any other call site -- e.g. a
// callee writing a regional value into a list/map that belongs to an
// *outer* frame -- can reuse the exact same promotion logic instead of
// duplicating it. Only elements still tied to the region being torn down
// (or otherwise not already where they need to be) need copying;
// everything else is returned unchanged.
Value promoteRegionalValue(Region* dest, Value v) {
    if (IS_STRING(v) && AS_STRING(v)->isRegional) {
        ObjString* s = AS_STRING(v);
        if (s->region == dest) return v; // already in the right region
        if (dest != NULL) return OBJ_VAL(regionCopyString(dest, s->chars, s->length));
        return OBJ_VAL(copyString(s->chars, s->length));
    }
    if (IS_LIST(v)) {
        ObjList* nested = AS_LIST(v);
        if (nested->region != NULL && nested->region != dest) {
            // Still region-owned by a region other than dest (as opposed
            // to an already-permanent nested list, or one already living
            // in dest) -- recurse.
            return dest != NULL ? OBJ_VAL(regionCopyList(dest, nested))
                                 : OBJ_VAL(permanentCopyList(nested));
        }
    }
    return v;
}

ObjList* regionCopyList(Region* dest, ObjList* src) {
    ObjList* list = (ObjList*)regionAlloc(dest, sizeof(ObjList));
    list->obj.type = OBJ_LIST;
    list->obj.next = NULL; // not linked into the permanent `objects` list
    list->region = dest;
    list->count = src->count;
    list->capacity = src->count; // tight copy; further appends would grow normally
    list->elemType = src->elemType;
    list->items = NULL;
    if (src->count > 0) {
        list->items = (Value*)regionAlloc(dest, sizeof(Value) * (size_t)src->count);
        for (int i = 0; i < src->count; i++) {
            list->items[i] = promoteRegionalValue(dest, src->items[i]);
        }
    }
    return list;
}

ObjList* permanentCopyList(ObjList* src) {
    ObjList* list = (ObjList*)allocateObject(sizeof(ObjList), OBJ_LIST);
    list->region = NULL; // sentinel: permanently allocated, see freeObjects()
    list->count = src->count;
    list->capacity = src->count;
    list->elemType = src->elemType;
    list->items = NULL;
    if (src->count > 0) {
        list->items = (Value*)malloc(sizeof(Value) * (size_t)src->count);
        for (int i = 0; i < src->count; i++) {
            list->items[i] = promoteRegionalValue(NULL, src->items[i]);
        }
    }
    return list;
}

ObjFunction* newFunction(void) {
    ObjFunction* function = (ObjFunction*)allocateObject(sizeof(ObjFunction), OBJ_FUNCTION);
    function->arity = 0;
    function->name = NULL;
    initChunk(&function->chunk);
    return function;
}

// Permanently-allocated map (see object.h ObjMap comment for why maps
// don't follow ObjList's region-scoped lifetime). Backed directly by the
// existing Table implementation (table.c), so tableSet/tableGet give
// O(1) average-case indexed access with no new hashing logic.
ObjMap* newMap(void) {
    ObjMap* map = (ObjMap*)allocateObject(sizeof(ObjMap), OBJ_MAP);
    initTable(&map->table);
    return map;
}

// ObjError values are never routed through IS_OBJ/AS_OBJ (a VAL_ERROR
// Value stores its ObjError* in a dedicated union member, Value.as.error
// -- see value.h), but the ObjError struct itself still gets a normal Obj
// header and goes through allocateObject() so it's linked into the same
// `objects` list as every other permanent allocation (ObjString,
// ObjFunction, ObjMap) and freed the same way by freeObjects() below. It
// owns a permanently-interned ObjString message (via copyString).
Value protonMakeError(const char* message) {
    ObjError* err = (ObjError*)allocateObject(sizeof(ObjError), OBJ_ERROR);
    err->message = copyString(message, (int)strlen(message));
    Value v;
    v.type = VAL_ERROR;
    v.as.error = err;
    return v;
}

// Frees every permanently-allocated object (interned strings, functions).
// Regional (Faz 1 LAM) strings are never linked into this list -- they're
// reclaimed in bulk by regionDestroy() when their owning call frame
// returns, so there is nothing for this function to do for them.
void freeObjects(void) {
    Obj* object = objects;
    while (object != NULL) {
        Obj* next = object->next;
        switch (object->type) {
            case OBJ_STRING: {
                ObjString* s = (ObjString*)object;
                free(s->chars);
                free(s);
                break;
            }
            case OBJ_FUNCTION: {
                ObjFunction* f = (ObjFunction*)object;
                freeChunk(&f->chunk);
                free(f);
                break;
            }
            case OBJ_LIST: {
                // Freshly-built lists (see newList) are always
                // region-allocated and never linked into this permanent
                // `objects` list -- unreachable here. The only ObjList
                // nodes that DO show up in this list are ones that
                // escaped all the way out of the outermost frame and were
                // promoted by permanentCopyList (object.c), which sets
                // `region = NULL` as the marker for that case and mallocs
                // `items` directly instead of carving it out of an arena.
                ObjList* l = (ObjList*)object;
                free(l->items);
                free(l);
                break;
            }
            case OBJ_MAP: {
                ObjMap* m = (ObjMap*)object;
                freeTable(&m->table);
                free(m);
                break;
            }
            case OBJ_ERROR: {
                // message is an interned ObjString, freed separately when
                // freeObjects() reaches its own OBJ_STRING node in the
                // `objects` list (interning means it's linked there too,
                // independent of this ObjError) -- don't free it here.
                free(object);
                break;
            }
        }
        object = next;
    }
    objects = NULL;
    if (internedInit) { freeTable(&strings); internedInit = false; }
}
