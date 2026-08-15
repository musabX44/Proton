#ifndef PROTON_TABLE_H
#define PROTON_TABLE_H

#include "common.h"
#include "value.h"
// Note: ObjString is only used here by pointer/opaque reference, and is
// already forward-declared as an incomplete type in value.h (typedef
// struct ObjString ObjString;) -- no need to pull in the full object.h,
// which would create a circular include now that object.h itself
// includes table.h (for ObjMap's embedded Table, see object.h).

typedef struct {
    ObjString* key;
    Value value;
} Entry;

typedef struct {
    int count;
    int capacity;
    Entry* entries;
} Table;

void initTable(Table* table);
void freeTable(Table* table);
bool tableGet(Table* table, ObjString* key, Value* value);
bool tableSet(Table* table, ObjString* key, Value value);
bool tableDelete(Table* table, ObjString* key);
ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash);

#endif
