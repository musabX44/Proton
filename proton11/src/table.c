#include <stdlib.h>
#include <string.h>
#include "table.h"
#include "object.h" // full ObjString definition (table.h only forward-declares it)

#define TABLE_MAX_LOAD 0.75

void initTable(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void freeTable(Table* table) {
    free(table->entries);
    initTable(table);
}

static Entry* findEntry(Entry* entries, int capacity, ObjString* key) {
    uint32_t index = key->hash % capacity;
    Entry* tombstone = NULL;
    for (;;) {
        Entry* entry = &entries[index];
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) {
                return tombstone != NULL ? tombstone : entry;
            } else {
                if (tombstone == NULL) tombstone = entry;
            }
        } else if (entry->key == key) {
            return entry;
        }
        index = (index + 1) % capacity;
    }
}

static void adjustCapacity(Table* table, int capacity) {
    Entry* entries = malloc(sizeof(Entry) * capacity);
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }

    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key == NULL) continue;
        Entry* dest = findEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }

    free(table->entries);
    table->entries = entries;
    table->capacity = capacity;
}

bool tableGet(Table* table, ObjString* key, Value* value) {
    if (table->count == 0) return false;
    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;
    *value = entry->value;
    return true;
}

bool tableSet(Table* table, ObjString* key, Value value) {
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
        adjustCapacity(table, capacity);
    }
    Entry* entry = findEntry(table->entries, table->capacity, key);
    bool isNewKey = entry->key == NULL;
    if (isNewKey && IS_NIL(entry->value)) table->count++;

    entry->key = key;
    entry->value = value;
    return isNewKey;
}

bool tableDelete(Table* table, ObjString* key) {
    if (table->count == 0) return false;
    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == NULL) return false;
    entry->key = NULL;
    entry->value = BOOL_VAL(true); // tombstone marker
    return true;
}

ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;
    uint32_t index = hash % table->capacity;
    for (;;) {
        Entry* entry = &table->entries[index];
        if (entry->key == NULL) {
            if (IS_NIL(entry->value)) return NULL;
        } else if (entry->key->length == length &&
                   entry->key->hash == hash &&
                   memcmp(entry->key->chars, chars, length) == 0) {
            return entry->key;
        }
        index = (index + 1) % table->capacity;
    }
}
