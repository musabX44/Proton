#ifndef PROTON_VALUE_H
#define PROTON_VALUE_H

#include "common.h"

typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjFunction ObjFunction;
typedef struct ObjList ObjList;
typedef struct ObjMap ObjMap;
typedef struct ObjError ObjError;

typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_NUMBER,
    VAL_OBJ,
    VAL_ERROR
} ValueType;

// Primitive type tags used for compile-time / runtime type enforcement.
// PTYPE_NONE means "no annotation known / not checked" (e.g. struct types,
// pointer types, array types -- none of those are implemented yet, so they
// fall back to unchecked, matching prior behavior).
typedef enum {
    PTYPE_NONE = 0,
    PTYPE_BOOL,
    PTYPE_CHAR,
    PTYPE_STRING,
    PTYPE_BYTE,
    PTYPE_INT8,
    PTYPE_INT16,
    PTYPE_INT32,
    PTYPE_INT64,
    PTYPE_UINT,
    PTYPE_UINT8,
    PTYPE_UINT16,
    PTYPE_UINT32,
    PTYPE_UINT64,
    PTYPE_FLOAT32,
    PTYPE_FLOAT64,
    PTYPE_DECIMAL,
    PTYPE_INT,     // alias -> int32 range
    PTYPE_SHORT,   // alias -> int16 range
    PTYPE_LONG,    // alias -> int64 range
    PTYPE_FLOAT,   // alias -> float32
    PTYPE_DOUBLE,  // alias -> float64
    PTYPE_VOID
} ProtonType;

const char* protonTypeName(ProtonType type);

// Tag for which member of Number is authoritative. NUM_F64 is the default
// for untyped/float-typed numbers (matches historical behavior). NUM_I64
// and NUM_U64 are used once a value has been checked against a signed or
// unsigned 64-bit-range integer type (int8..int64, uint8..uint64, and the
// int/short/long aliases), so it carries an exact 64-bit representation
// instead of going through double (which can only represent integers
// exactly up to +-2^53-1).
typedef enum {
    NUM_F64,
    NUM_I64,
    NUM_U64
} NumKind;

typedef struct {
    NumKind kind;
    union {
        double f64;
        int64_t i64;
        uint64_t u64;
    } as;
} Number;

// ObjError is a tiny heap object carrying a human-readable message (full
// struct definition lives in object.h, alongside ObjString/ObjList/ObjMap,
// since it needs the complete `Obj` struct which isn't visible yet at
// this point in value.h -- only an opaque forward-declare, same as
// ObjString/ObjFunction/ObjList/ObjMap above). VAL_ERROR values wrap a
// pointer to one of these (see ERROR_VAL below). ObjError is deliberately
// NOT region-scoped -- error values are the exceptional path, not
// performance-critical the way lists/strings are, and keeping them
// permanently allocated means OP_TRY's stack-unwinding logic never has to
// worry about an error's backing memory being torn down by the very
// regionDestroy() call that unwinding triggers.
typedef struct {
    ValueType type;
    union {
        bool boolean;
        Number number;
        Obj* obj;
        ObjError* error;
    } as;
} Value;

#define BOOL_VAL(value)   ((Value){VAL_BOOL, {.boolean = value}})
#define NIL_VAL           ((Value){VAL_NIL, {.number = {NUM_F64, {.f64 = 0}}}})
#define NUMBER_VAL(value) ((Value){VAL_NUMBER, {.number = {NUM_F64, {.f64 = (value)}}}})
#define OBJ_VAL(object)   ((Value){VAL_OBJ, {.obj = (Obj*)object}})

// Construct a Value carrying an exact 64-bit integer representation
// (used once a value is known to be int64/uint64-typed, e.g. after an
// OP_CHECK_TYPE pass, or for literals large enough that double would
// lose precision).
#define INT64_VAL(value)  ((Value){VAL_NUMBER, {.number = {NUM_I64, {.i64 = (int64_t)(value)}}}})
#define UINT64_VAL(value) ((Value){VAL_NUMBER, {.number = {NUM_U64, {.u64 = (uint64_t)(value)}}}})
// int32 still fits exactly in a double, so this stays a plain f64 Value;
// kept as a named macro for call-site clarity/documentation purposes.
#define INT32_VAL(value)  NUMBER_VAL((double)(int32_t)(value))

// ERROR_VAL(msg) builds a VAL_ERROR value wrapping a permanent (interned)
// ObjString message. Implemented as a function (protonMakeError) rather
// than a pure macro because it needs to allocate the ObjError wrapper via
// object.c's allocateObject-style bookkeeping; declared here, defined in
// object.c, and macro'd below for call-site convenience matching the
// other *_VAL macros' calling convention.
Value protonMakeError(const char* message);
#define ERROR_VAL(msg) protonMakeError(msg)

#define AS_BOOL(value)    ((value).as.boolean)
#define AS_OBJ(value)     ((value).as.obj)
#define AS_ERROR(value)   ((value).as.error)
#define AS_ERROR_CSTRING(value) (((ObjError*)(value).as.error)->message->chars)

// AS_NUMBER returns a double view of any numeric Value regardless of its
// NumKind, for call sites (arithmetic, comparisons, printing fallback)
// that don't need exact 64-bit precision. NOTE: for NUM_I64/NUM_U64
// values outside +-2^53-1, this loses precision -- use AS_I64/AS_U64 with
// NUM_KIND() when exactness matters (e.g. int64/uint64 arithmetic).
static inline double protonAsNumberDouble(Value value) {
    switch (value.as.number.kind) {
        case NUM_I64: return (double)value.as.number.as.i64;
        case NUM_U64: return (double)value.as.number.as.u64;
        default:      return value.as.number.as.f64;
    }
}
#define AS_NUMBER(value)  (protonAsNumberDouble(value))
#define NUM_KIND(value)   ((value).as.number.kind)
#define AS_I64(value)     ((value).as.number.as.i64)
#define AS_U64(value)     ((value).as.number.as.u64)

#define IS_BOOL(value)    ((value).type == VAL_BOOL)
#define IS_NIL(value)     ((value).type == VAL_NIL)
#define IS_NUMBER(value)  ((value).type == VAL_NUMBER)
#define IS_OBJ(value)     ((value).type == VAL_OBJ)
// VAL_ERROR is intentionally its own ValueType tag (see enum above) and
// carries a distinct union member (as.error), NOT as.obj -- so IS_OBJ
// stays false for error values. This keeps every existing IS_OBJ-gated
// code path (AS_OBJ, OBJ_TYPE, isObjType, freeObjects's object-list walk,
// etc.) exactly as it was: none of them need to learn about VAL_ERROR,
// because an error value never flows through AS_OBJ. IS_ERROR is the
// dedicated predicate for the new type instead.
#define IS_ERROR(value)   ((value).type == VAL_ERROR)

typedef struct {
    int capacity;
    int count;
    Value* values;
} ValueArray;

void initValueArray(ValueArray* array);
void writeValueArray(ValueArray* array, Value value);
void freeValueArray(ValueArray* array);
void printValue(Value value);
bool valuesEqual(Value a, Value b);

#endif
