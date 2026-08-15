#ifndef PROTON_OBJECT_H
#define PROTON_OBJECT_H

#include "common.h"
#include "value.h"
#include "chunk.h"
#include "region.h"
#include "table.h"

typedef enum {
    OBJ_STRING,
    OBJ_FUNCTION,
    OBJ_LIST,
    OBJ_MAP,
    OBJ_ERROR
} ObjType;

struct Obj {
    ObjType type;
    struct Obj* next; // for GC / cleanup list (permanent/interned objects only -- see below)
};

// ObjError: see value.h for the rationale (permanently allocated, never
// region-scoped, carries a message string). It DOES get an Obj header and
// IS linked into the normal `objects` list / freed by freeObjects() below
// -- unlike a Value's `obj` union member, which is only ever set for
// VAL_OBJ values, a VAL_ERROR Value stores its ObjError* directly in a
// separate union member (Value.as.error, see value.h), so IS_OBJ/AS_OBJ
// never see it. Having a normal Obj header here still lets freeObjects()
// walk and free it exactly like every other permanent allocation, via the
// same allocateObject() path as ObjString/ObjFunction/ObjMap.
struct ObjError {
    Obj obj;
    ObjString* message;
};

struct ObjString {
    Obj obj;
    int length;
    char* chars;
    uint32_t hash;
    // Faz 1 LAM: true if this ObjString's memory (both this struct and
    // ->chars) was bump-allocated out of a Region rather than malloc'd.
    // Regional strings are NEVER interned and NEVER linked into the
    // `objects` list that freeObjects() walks -- their backing memory is
    // reclaimed all at once when their owning Region is destroyed
    // (see vm.c: OP_RETURN), not by freeObjects() at program exit.
    bool isRegional;
    // Owning region, mirroring ObjList::region: NULL for permanent
    // (interned/malloc'd) strings, non-NULL for regional strings --
    // set alongside isRegional in regionTakeString/regionCopyString.
    // Lets a callee (e.g. one writing this string into a list/map that
    // lives in an *outer* frame) tell exactly which region a regional
    // string belongs to, rather than just "regional vs not" -- needed to
    // promote it into the right destination when it's about to be
    // written somewhere outside its own frame (see promoteRegionalValue).
    Region* region;
};

struct ObjFunction {
    Obj obj;
    int arity;
    Chunk chunk;
    ObjString* name;
};

// Faz 1 LAM: region-scoped, typed dynamic array (Proton's `T x[] = [...]`).
// Backing storage lives in `region` (the owning call frame's arena), grown
// by regionAlloc()-and-copy doubling -- there is no realloc()/free() call
// anywhere in this path, matching the "no realloc/free" constraint the
// whole LAM design follows. `count` is O(1) element-count metadata (no
// walk needed for len()), and index reads/writes are direct pointer
// arithmetic into `items` -- O(1), no hashing, no traversal.
//
// Lifetime mirrors ObjString: a freshly-built list is never interned and
// never linked into the permanent `objects` list, reclaimed in bulk when
// `region` is destroyed (vm.c: OP_RETURN). A list returned out of its
// owning frame is promoted the same way regional strings are --
// `promoteEscapingValue` (vm.c) calls regionCopyList to relocate it into
// the caller's (enclosing) region, riding the region chain call after
// call, exactly like string escape (see README's LAM section). Only if
// it escapes all the way out of the outermost frame does it fall back to
// `permanentCopyList`, a malloc'd, `objects`-list-linked list -- signaled
// by `region == NULL` (see freeObjects()'s OBJ_LIST case in object.c).
// Everywhere else in this codebase `region` is non-NULL and items are
// carved out of that arena.
struct ObjList {
    Obj obj;
    Region* region;   // owning region; NULL means permanently (malloc) allocated
    int count;
    int capacity;
    Value* items;
    ProtonType elemType; // PTYPE_NONE if untyped/unchecked element type
};

// Map (dictionary) object: string-keyed only, backed directly by Proton's
// existing Table (see table.h) -- the same hash table implementation used
// for globals and string interning, so tableGet/tableSet already give
// O(1) average-case get/set with zero new hashing code. ObjMap is always
// permanently (malloc) allocated and linked into the normal `objects`
// list (see newMap/freeObjects below); it is NOT region-scoped, unlike
// ObjList, because map literals are commonly assigned to globals (e.g.
// `var ayarlar = { ... };` at top level) and returned from functions --
// giving it the same escape hazard ObjList has without a promotion path.
// A permanently-allocated Table sidesteps that entirely; its Entry array
// is a normal malloc'd buffer freed by freeObjects(), same lifetime class
// as ObjFunction's Chunk.
struct ObjMap {
    Obj obj;
    Table table;
};

// Interned, permanent strings: literals in source code. Live until
// program exit; safe to share across scopes since they're never
// invalidated by a region tear-down.
ObjString* takeString(char* chars, int length);
ObjString* copyString(const char* chars, int length);

// Region-scoped, non-interned string: used for values produced at
// runtime (string concatenation, number/bool-to-string coercion) that
// are local to the currently executing call frame. `region` must be the
// call frame's own Region (see vm.c). The returned ObjString and its
// backing chars are both carved out of `region` -- do not free() them
// directly and do not let a reference to one outlive its Region.
ObjString* regionTakeString(struct Region* region, char* chars, int length);

// Escape-copy: used when a regional ObjString outlives its own Region
// (e.g. it's the return value of OP_RETURN) but there IS an enclosing
// region to promote it into (i.e. this isn't the outermost frame). Copies
// `chars` into `dest` -- a *different*, still-live region (typically the
// caller's) -- without touching the permanent/interned heap. This is what
// keeps "return a fresh unique string from a function called in a loop"
// from growing permanent memory: the value rides the region chain
// (region->enclosing) instead of being promoted straight to permanent
// storage. Does not take ownership of `chars` (unlike regionTakeString);
// safe to call with a still-owned buffer such as another ObjString's
// ->chars.
ObjString* regionCopyString(struct Region* dest, const char* chars, int length);

ObjFunction* newFunction(void);

// Creates a new, empty region-scoped list carved out of `region` (the
// owning call frame's arena). `elemType` is PTYPE_NONE for an untyped
// list, or a primitive tag to enforce per-element type checking at
// OP_BUILD_LIST / OP_SET_INDEX time (see vm.c).
ObjList* newList(Region* region, ProtonType elemType);

// Appends `value` to `list`, growing its backing store if needed.
// Amortized O(1): growth is doubling, so total cost across N appends is
// O(N), same complexity class as a realloc-based dynamic array, but
// implemented purely with regionAlloc()-and-copy (no realloc/free calls
// -- old backing storage is simply abandoned in the region's arena,
// reclaimed in bulk at regionDestroy time along with everything else).
void appendList(ObjList* list, Value value);

// Escape-copy for lists, mirroring regionCopyString: used when a
// region-scoped ObjList outlives its own Region (it's the return value
// of OP_RETURN) but there IS an enclosing region to promote it into.
// Copies the ObjList header and its items array into `dest`. Any element
// that is itself a regional string or a still region-owned nested list
// (i.e. one that lives in the *same* dying region, not one already
// promoted elsewhere) is recursively promoted into `dest` too -- a
// shallow copy of the items array alone would leave those elements
// pointing into the region that's about to be destroyed.
ObjList* regionCopyList(Region* dest, ObjList* src);

// Terminal fallback for regionCopyList: used when there is no enclosing
// region left to ride into (the returning frame is the outermost one).
// Produces a permanently (malloc-)allocated ObjList, linked into the
// normal `objects` list so freeObjects() can reclaim it, with
// `list->region` set to NULL as the "no longer region-owned" sentinel
// (see freeObjects() in object.c and the OBJ_LIST case there). Elements
// are promoted the same way as regionCopyList, just with a NULL
// destination region (regional strings go to copyString/permanent
// storage, regional nested lists recurse into permanentCopyList).
ObjList* permanentCopyList(ObjList* src);

// Promotes a single regional value (string or list) so it's safe to
// store somewhere outside the region it currently lives in -- shared by
// regionCopyList/permanentCopyList internally (list elements) and by any
// other call site that writes a possibly-regional value into a
// longer-lived container (e.g. a list/map living in an outer frame).
// `dest == NULL` means "no enclosing region left, go straight to
// permanent storage" (mirrors promoteEscapingValue's dest==NULL branch in
// vm.c). Non-regional values (numbers, bools, nil, already-permanent
// maps, already-promoted strings/lists) are returned unchanged.
Value promoteRegionalValue(Region* dest, Value v);

// Creates a new, empty, permanently-allocated map. See ObjMap comment
// above for why maps are malloc'd/objects-list-linked rather than
// region-scoped like ObjList.
ObjMap* newMap(void);

#define OBJ_TYPE(value)    (AS_OBJ(value)->type)
#define IS_STRING(value)   isObjType(value, OBJ_STRING)
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define IS_LIST(value)     isObjType(value, OBJ_LIST)
#define IS_MAP(value)      isObjType(value, OBJ_MAP)

#define AS_STRING(value)   ((ObjString*)AS_OBJ(value))
#define AS_CSTRING(value)  (((ObjString*)AS_OBJ(value))->chars)
#define AS_FUNCTION(value) ((ObjFunction*)AS_OBJ(value))
#define AS_LIST(value)     ((ObjList*)AS_OBJ(value))
#define AS_MAP(value)      ((ObjMap*)AS_OBJ(value))

static inline bool isObjType(Value value, ObjType type) {
    return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

void freeObjects(void);

#endif
