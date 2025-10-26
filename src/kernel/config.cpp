#include "config.hpp"

#include "lib/mem.hpp"
#include "string_util.hpp"

namespace config {
namespace {

bool is_space(char ch) {
    return ch == ' ' || ch == '\t';
}

bool strings_equal_n(const char* a, const char* b, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

Entry* find_entry(Table& table, const char* key, size_t key_len) {
    for (size_t i = 0; i < table.count; ++i) {
        size_t existing_len = string_util::length(table.entries[i].key);
        if (existing_len == key_len &&
            strings_equal_n(table.entries[i].key, key, key_len)) {
            return &table.entries[i];
        }
    }
    return nullptr;
}

const Entry* find_entry(const Table& table, const char* key) {
    size_t key_len = string_util::length(key);
    for (size_t i = 0; i < table.count; ++i) {
        size_t existing_len = string_util::length(table.entries[i].key);
        if (existing_len == key_len &&
            strings_equal_n(table.entries[i].key, key, key_len)) {
            return &table.entries[i];
        }
    }
    return nullptr;
}

bool copy_token(const char* source,
                size_t length,
                char* dest,
                size_t dest_size) {
    if (dest == nullptr || dest_size == 0) {
        return false;
    }
    if (length >= dest_size) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        dest[i] = source[i];
    }
    dest[length] = '\0';
    return true;
}

}  // namespace

void init(Table& table) {
    memset(&table, 0, sizeof(Table));
}

bool parse(const char* data, size_t length, Table& table) {
    if (data == nullptr) {
        return false;
    }

    init(table);
    bool success = true;
    size_t cursor = 0;

    while (cursor < length) {
        size_t line_start = cursor;
        size_t line_end = cursor;
        while (line_end < length && data[line_end] != '\n' &&
               data[line_end] != '\r') {
            ++line_end;
        }

        cursor = line_end;
        while (cursor < length &&
               (data[cursor] == '\n' || data[cursor] == '\r')) {
            ++cursor;
        }

        size_t line_length = line_end - line_start;
        const char* line = data + line_start;

        size_t leading = 0;
        while (leading < line_length && is_space(line[leading])) {
            ++leading;
        }
        if (leading >= line_length) {
            continue;
        }

        if (line[leading] == '#' || line[leading] == ';') {
            continue;
        }

        size_t trailing = line_length;
        while (trailing > leading && is_space(line[trailing - 1])) {
            --trailing;
        }
        if (trailing <= leading) {
            continue;
        }

        const char* trimmed = line + leading;
        size_t trimmed_len = trailing - leading;

        size_t colon = 0;
        while (colon < trimmed_len && trimmed[colon] != ':') {
            ++colon;
        }
        if (colon >= trimmed_len) {
            success = false;
            continue;
        }

        size_t key_end = colon;
        while (key_end > 0 && is_space(trimmed[key_end - 1])) {
            --key_end;
        }
        if (key_end == 0) {
            success = false;
            continue;
        }

        size_t value_start = colon + 1;
        while (value_start < trimmed_len && is_space(trimmed[value_start])) {
            ++value_start;
        }

        size_t value_end = trimmed_len;
        while (value_end > value_start && is_space(trimmed[value_end - 1])) {
            --value_end;
        }

        size_t key_len = key_end;
        size_t value_len = (value_end > value_start)
                               ? (value_end - value_start)
                               : 0;

        Entry* entry = find_entry(table, trimmed, key_len);
        if (entry == nullptr) {
            if (table.count >= kMaxEntries) {
                success = false;
                continue;
            }
            entry = &table.entries[table.count++];
        }

        if (!copy_token(trimmed, key_len, entry->key, kMaxKeyLength)) {
            success = false;
            continue;
        }

        if (!copy_token(trimmed + value_start,
                        value_len,
                        entry->value,
                        kMaxValueLength)) {
            success = false;
            continue;
        }
    }

    return success;
}

const char* get(const Table& table, const char* key) {
    if (key == nullptr) {
        return nullptr;
    }
    const Entry* entry = find_entry(table, key);
    if (entry == nullptr) {
        return nullptr;
    }
    return entry->value;
}

bool get(const Table& table, const char* key, const char*& value_out) {
    const char* value = get(table, key);
    if (value == nullptr) {
        return false;
    }
    value_out = value;
    return true;
}

}  // namespace config
