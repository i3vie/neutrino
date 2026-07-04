#include "./limine_requests.hpp"

namespace {

constexpr size_t kMaxPreservedModules = 16;
constexpr size_t kMaxPreservedString = 192;

PreservedLimineModule g_preserved_modules[kMaxPreservedModules];
char g_preserved_paths[kMaxPreservedModules][kMaxPreservedString];
char g_preserved_strings[kMaxPreservedModules][kMaxPreservedString];
size_t g_preserved_module_count = 0;
bool g_modules_preserved = false;

void copy_string(char* out, size_t out_size, const char* in) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    if (in == nullptr) {
        out[0] = '\0';
        return;
    }

    size_t i = 0;
    while (i + 1 < out_size && in[i] != '\0') {
        out[i] = in[i];
        ++i;
    }
    out[i] = '\0';
}

}  // namespace

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
#if LIMINE_API_REVISION >= 2
volatile struct limine_executable_file_request kernel_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST,
#else
volatile struct limine_kernel_file_request kernel_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
#endif
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
#if LIMINE_API_REVISION >= 2
volatile struct limine_executable_address_request kernel_addr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
#else
volatile struct limine_kernel_address_request kernel_addr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
#endif
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
#ifdef LIMINE_MP_REQUEST
volatile struct LIMINE_MP(request) smp_request = {
    .id = LIMINE_MP_REQUEST,
    .revision = 0,
    .flags = 0
};
#else
volatile struct LIMINE_MP(request) smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0
};
#endif

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

void preserve_limine_modules() {
    if (g_modules_preserved) {
        return;
    }
    g_modules_preserved = true;

    const volatile struct limine_module_response* response =
        module_request.response;
    if (response == nullptr || response->modules == nullptr) {
        return;
    }

    uint64_t module_count = response->module_count;
    if (module_count > kMaxPreservedModules) {
        module_count = kMaxPreservedModules;
    }

    for (uint64_t i = 0; i < module_count; ++i) {
        volatile struct limine_file* file = response->modules[i];
        if (file == nullptr) {
            continue;
        }

        PreservedLimineModule& module =
            g_preserved_modules[g_preserved_module_count];
        module.address = const_cast<const void*>(file->address);
        module.size = file->size;

        copy_string(g_preserved_paths[g_preserved_module_count],
                    kMaxPreservedString,
                    file->path);
#if LIMINE_API_REVISION >= 3
        copy_string(g_preserved_strings[g_preserved_module_count],
                    kMaxPreservedString,
                    file->string);
#else
        copy_string(g_preserved_strings[g_preserved_module_count],
                    kMaxPreservedString,
                    file->cmdline);
#endif
        module.path = g_preserved_paths[g_preserved_module_count];
        module.string = g_preserved_strings[g_preserved_module_count];
        ++g_preserved_module_count;
    }
}

size_t preserved_limine_module_count() {
    return g_preserved_module_count;
}

const PreservedLimineModule* preserved_limine_module(size_t index) {
    if (index >= g_preserved_module_count) {
        return nullptr;
    }
    return &g_preserved_modules[index];
}
