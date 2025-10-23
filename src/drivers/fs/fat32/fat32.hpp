#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../block_device.hpp"

struct Fat32DirEntry {
    char name[12];
    uint8_t attributes;
    uint32_t first_cluster;
    uint32_t size;
};

struct Fat32Volume {
    bool mounted;
    fs::BlockDevice device;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_size_sectors;
    uint32_t fat_begin_lba;
    uint32_t cluster_begin_lba;
    uint32_t root_dir_first_cluster;
};

bool fat32_mount(Fat32Volume& volume, const fs::BlockDevice& device);
size_t fat32_list_root(Fat32Volume& volume, Fat32DirEntry* out_entries,
                       size_t max_entries);
bool fat32_read_file(Fat32Volume& volume, const char* name, void* buffer,
                     size_t buffer_size, size_t& out_size);
