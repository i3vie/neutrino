#pragma once

#include <stddef.h>
#include <stdint.h>

namespace config {

constexpr size_t kMaxEntries = 32;
constexpr size_t kMaxKeyLength = 64;
constexpr size_t kMaxValueLength = 128;

struct Entry {
    char key[kMaxKeyLength];
    char value[kMaxValueLength];
};

struct Table {
    Entry entries[kMaxEntries];
    size_t count;
};

void init(Table& table);
bool parse(const char* data, size_t length, Table& table);
const char* get(const Table& table, const char* key);
bool get(const Table& table, const char* key, const char*& value_out);

}  // namespace config

