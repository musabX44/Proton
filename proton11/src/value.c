#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "value.h"
#include "object.h"

void initValueArray(ValueArray* array) {
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void writeValueArray(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        array->capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        array->values = realloc(array->values, sizeof(Value) * array->capacity);
    }
    array->values[array->count] = value;
    array->count++;
}

void freeValueArray(ValueArray* array) {
    free(array->values);
    initValueArray(array);
}

void printValue(Value value) {
    switch (value.type) {
        case VAL_NIL: printf("nil"); break;
        case VAL_BOOL: printf(AS_BOOL(value) ? "true" : "false"); break;
        case VAL_NUMBER: {
            switch (NUM_KIND(value)) {
                case NUM_I64:
                    printf("%lld", (long long)AS_I64(value));
                    break;
                case NUM_U64:
                    printf("%llu", (unsigned long long)AS_U64(value));
                    break;
                default: {
                    double n = AS_NUMBER(value);
                    if (n == (long long)n) {
                        printf("%lld", (long long)n);
                    } else {
                        printf("%g", n);
                    }
                    break;
                }
            }
            break;
        }
        case VAL_OBJ: {
            if (IS_STRING(value)) {
                printf("%s", AS_CSTRING(value));
            } else if (IS_FUNCTION(value)) {
                ObjFunction* fn = AS_FUNCTION(value);
                printf("<fn %s>", fn->name ? fn->name->chars : "?");
            } else if (IS_LIST(value)) {
                ObjList* list = AS_LIST(value);
                printf("[");
                for (int i = 0; i < list->count; i++) {
                    if (i > 0) printf(", ");
                    printValue(list->items[i]);
                }
                printf("]");
            } else if (IS_MAP(value)) {
                ObjMap* map = AS_MAP(value);
                printf("{");
                bool first = true;
                for (int i = 0; i < map->table.capacity; i++) {
                    Entry* entry = &map->table.entries[i];
                    if (entry->key == NULL) continue;
                    if (!first) printf(", ");
                    first = false;
                    printf("\"%s\": ", entry->key->chars);
                    printValue(entry->value);
                }
                printf("}");
            }
            break;
        }
        case VAL_ERROR: {
            printf("error: %s", AS_ERROR_CSTRING(value));
            break;
        }
    }
}

const char* protonTypeName(ProtonType type) {
    switch (type) {
        case PTYPE_BOOL: return "bool";
        case PTYPE_CHAR: return "char";
        case PTYPE_STRING: return "string";
        case PTYPE_BYTE: return "byte";
        case PTYPE_INT8: return "int8";
        case PTYPE_INT16: return "int16";
        case PTYPE_INT32: return "int32";
        case PTYPE_INT64: return "int64";
        case PTYPE_UINT: return "uint";
        case PTYPE_UINT8: return "uint8";
        case PTYPE_UINT16: return "uint16";
        case PTYPE_UINT32: return "uint32";
        case PTYPE_UINT64: return "uint64";
        case PTYPE_FLOAT32: return "float32";
        case PTYPE_FLOAT64: return "float64";
        case PTYPE_DECIMAL: return "decimal";
        case PTYPE_INT: return "int";
        case PTYPE_SHORT: return "short";
        case PTYPE_LONG: return "long";
        case PTYPE_FLOAT: return "float";
        case PTYPE_DOUBLE: return "double";
        case PTYPE_VOID: return "void";
        default: return "?";
    }
}

bool valuesEqual(Value a, Value b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NIL: return true;
        case VAL_ERROR: return AS_ERROR(a) == AS_ERROR(b);
        case VAL_BOOL: return AS_BOOL(a) == AS_BOOL(b);
        case VAL_NUMBER: {
            // Exact comparison when both sides carry the same integer kind,
            // to avoid precision loss for values beyond +-2^53-1. Mixed
            // kinds (or either side being NUM_F64) fall back to a double
            // comparison, matching prior behavior.
            if (NUM_KIND(a) == NUM_I64 && NUM_KIND(b) == NUM_I64) {
                return AS_I64(a) == AS_I64(b);
            }
            if (NUM_KIND(a) == NUM_U64 && NUM_KIND(b) == NUM_U64) {
                return AS_U64(a) == AS_U64(b);
            }
            return AS_NUMBER(a) == AS_NUMBER(b);
        }
        case VAL_OBJ: {
            if (IS_STRING(a) && IS_STRING(b)) {
                ObjString* sa = AS_STRING(a);
                ObjString* sb = AS_STRING(b);
                return sa->length == sb->length &&
                       memcmp(sa->chars, sb->chars, sa->length) == 0;
            }
            return AS_OBJ(a) == AS_OBJ(b);
        }
    }
    return false;
}
