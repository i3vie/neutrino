#include "loader.hpp"

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"
#include "vm.hpp"

namespace {

constexpr uint8_t kElfMagic[4] = {0x7F, 'E', 'L', 'F'};
constexpr uint64_t kPageSize = 0x1000;

enum class ElfIdent : size_t {
    Class = 4,
    Data = 5,
    Version = 6,
};

enum : uint8_t {
    ELFCLASS64 = 2,
    ELFDATA2LSB = 1,
};

enum : uint16_t {
    ET_EXEC = 2,
    ET_DYN = 3,
    EM_X86_64 = 62,
};

enum : uint32_t {
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
};

struct Elf64Ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct Elf64Phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct Elf64Dyn {
    int64_t tag;
    uint64_t val;
};

struct Elf64Rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};

enum : int64_t {
    DT_NULL = 0,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
};

enum : uint32_t {
    R_X86_64_RELATIVE = 8,
};

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

bool looks_like_elf(const uint8_t* data, size_t size) {
    if (data == nullptr || size < sizeof(Elf64Ehdr)) {
        return false;
    }
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(data);
    return header->ident[0] == kElfMagic[0] &&
           header->ident[1] == kElfMagic[1] &&
           header->ident[2] == kElfMagic[2] &&
           header->ident[3] == kElfMagic[3];
}

bool setup_user_stack(process::Process& proc) {
    proc.stack_region = vm::allocate_user_stack(16 * 1024);
    if (proc.stack_region.top == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to allocate stack for process %u",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }
    uint64_t aligned_top = (proc.stack_region.top - 16ull) & ~0xFull;
    proc.user_sp = aligned_top;
    return true;
}

bool load_flat_binary(const loader::ProgramImage& image,
                      process::Process& proc) {
    uint64_t entry_point = 0;
    proc.code_region = vm::map_user_code(image.data, image.size,
                                         image.entry_offset, entry_point);
    if (proc.code_region.base == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to map flat binary for process %u",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }

    proc.user_ip = entry_point;
    return true;
}

bool validate_elf_header(const Elf64Ehdr& header) {
    if (header.ident[static_cast<size_t>(ElfIdent::Class)] != ELFCLASS64 ||
        header.ident[static_cast<size_t>(ElfIdent::Data)] != ELFDATA2LSB ||
        header.ident[static_cast<size_t>(ElfIdent::Version)] != 1) {
        log_message(LogLevel::Error,
                    "Loader: unsupported ELF identification");
        return false;
    }

    if (header.type != ET_EXEC && header.type != ET_DYN) {
        log_message(LogLevel::Error,
                    "Loader: unsupported ELF type %u",
                    static_cast<unsigned int>(header.type));
        return false;
    }

    if (header.machine != EM_X86_64 || header.version != 1) {
        log_message(LogLevel::Error,
                    "Loader: unsupported ELF target");
        return false;
    }

    if (header.phoff == 0 || header.phnum == 0) {
        log_message(LogLevel::Error,
                    "Loader: ELF missing program headers");
        return false;
    }

    if (header.phentsize != sizeof(Elf64Phdr)) {
        log_message(LogLevel::Error,
                    "Loader: unexpected ELF program header size %u",
                    static_cast<unsigned int>(header.phentsize));
        return false;
    }

    return true;
}

bool load_elf_binary(const loader::ProgramImage& image,
                     process::Process& proc) {
    if (image.data == nullptr || image.size < sizeof(Elf64Ehdr)) {
        log_message(LogLevel::Error,
                    "Loader: ELF image too small for header");
        return false;
    }

    const auto* header =
        reinterpret_cast<const Elf64Ehdr*>(image.data);

    if (!validate_elf_header(*header)) {
        return false;
    }

    uint64_t ph_table_end =
        header->phoff + static_cast<uint64_t>(header->phnum) * header->phentsize;
    if (ph_table_end > image.size || ph_table_end < header->phoff) {
        log_message(LogLevel::Error,
                    "Loader: ELF program headers exceed image");
        return false;
    }

    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    size_t loadable_segments = 0;
    const Elf64Phdr* dynamic_phdr = nullptr;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        uint64_t ph_offset = header->phoff + static_cast<uint64_t>(i) * header->phentsize;
        const auto* ph = reinterpret_cast<const Elf64Phdr*>(image.data + ph_offset);
        if (ph->type == PT_DYNAMIC) {
            dynamic_phdr = ph;
        }
        if (ph->type != PT_LOAD) {
            continue;
        }

        ++loadable_segments;
        if (ph->memsz == 0) {
            continue;
        }

        if (ph->filesz > ph->memsz) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment filesz exceeds memsz");
            return false;
        }

        if (ph->offset + ph->filesz > image.size ||
            ph->offset + ph->filesz < ph->offset) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment exceeds image size");
            return false;
        }

        uint64_t seg_start = ph->vaddr;
        uint64_t seg_end = seg_start + ph->memsz;
        if (seg_end < seg_start) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment address overflow");
            return false;
        }

        if (seg_start < min_vaddr) {
            min_vaddr = seg_start;
        }
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    if (loadable_segments == 0 || min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) {
        log_message(LogLevel::Error,
                    "Loader: ELF has no loadable segments");
        return false;
    }

    uint64_t entry = header->entry;
    if (entry < min_vaddr || entry >= max_vaddr) {
        log_message(LogLevel::Error,
                    "Loader: ELF entry point 0x%llx outside load range",
                    static_cast<unsigned long long>(entry));
        return false;
    }

    uint64_t aligned_min = align_down(min_vaddr, kPageSize);
    uint64_t aligned_max = align_up(max_vaddr, kPageSize);
    uint64_t aligned_span = aligned_max - aligned_min;

    vm::Region region =
        vm::allocate_user_region(static_cast<size_t>(aligned_span));
    if (region.base == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to allocate region for ELF process %u",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }

    uint64_t load_bias = region.base - aligned_min;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        uint64_t ph_offset = header->phoff + static_cast<uint64_t>(i) * header->phentsize;
        const auto* ph = reinterpret_cast<const Elf64Phdr*>(image.data + ph_offset);
        if (ph->type != PT_LOAD || ph->memsz == 0) {
            continue;
        }

        uint64_t dest = load_bias + ph->vaddr;
        void* dest_ptr = reinterpret_cast<void*>(dest);
        if (ph->filesz != 0) {
            const uint8_t* src = image.data + ph->offset;
            memcpy(dest_ptr, src, static_cast<size_t>(ph->filesz));
        }
        if (ph->memsz > ph->filesz) {
            void* bss_ptr = reinterpret_cast<void*>(dest + ph->filesz);
            memset(bss_ptr, 0, static_cast<size_t>(ph->memsz - ph->filesz));
        }
    }

    if (dynamic_phdr != nullptr) {
        auto* dyn_table =
            reinterpret_cast<const Elf64Dyn*>(load_bias + dynamic_phdr->vaddr);
        size_t dyn_count =
            static_cast<size_t>(dynamic_phdr->memsz / sizeof(Elf64Dyn));

        uint64_t rela_addr = 0;
        uint64_t rela_size = 0;
        uint64_t rela_ent = 0;

        for (size_t i = 0; i < dyn_count; ++i) {
            int64_t tag = dyn_table[i].tag;
            if (tag == DT_NULL) {
                break;
            }
            switch (tag) {
                case DT_RELA:
                    rela_addr = dyn_table[i].val;
                    break;
                case DT_RELASZ:
                    rela_size = dyn_table[i].val;
                    break;
                case DT_RELAENT:
                    rela_ent = dyn_table[i].val;
                    break;
                default:
                    break;
            }
        }

        if (rela_addr != 0 && rela_size != 0) {
            if (rela_ent == 0) {
                rela_ent = sizeof(Elf64Rela);
            }
            size_t rela_count = static_cast<size_t>(rela_size / rela_ent);
            auto* rela_table = reinterpret_cast<const Elf64Rela*>(load_bias + rela_addr);
            for (size_t i = 0; i < rela_count; ++i) {
                const Elf64Rela& rela = rela_table[i];
                uint32_t type = static_cast<uint32_t>(rela.info & 0xFFFFFFFFu);
                switch (type) {
                    case R_X86_64_RELATIVE: {
                        uint64_t* target =
                            reinterpret_cast<uint64_t*>(load_bias + rela.offset);
                        *target = load_bias + static_cast<uint64_t>(rela.addend);
                        break;
                    }
                    default:
                        log_message(LogLevel::Error,
                                    "Loader: unsupported relocation type %u",
                                    type);
                        return false;
                }
            }
        }
    }

    proc.code_region = region;
    proc.user_ip = load_bias + entry;
    return true;
}

}  // namespace

namespace loader {

bool load_into_process(const ProgramImage& image, process::Process& proc) {
    bool loaded = false;
    if (looks_like_elf(image.data, image.size)) {
        loaded = load_elf_binary(image, proc);
    } else {
        loaded = load_flat_binary(image, proc);
    }

    if (!loaded) {
        return false;
    }

    if (!setup_user_stack(proc)) {
        return false;
    }

    proc.has_context = false;
    proc.state = process::State::Ready;
    return true;
}

}  // namespace loader
