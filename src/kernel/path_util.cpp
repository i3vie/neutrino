#include "path_util.hpp"

#include "fs/vfs.hpp"
#include "lib/mem.hpp"

namespace path_util {

namespace {

struct Segment {
    const char* data;
    size_t length;
};

constexpr size_t kMaxSegments = 64;

bool push_segment(Segment (&segments)[kMaxSegments],
                  size_t& count,
                  const char* start,
                  size_t length) {
    if (length == 0) {
        return true;
    }
    if (count >= kMaxSegments) {
        return false;
    }
    segments[count++] = Segment{start, length};
    return true;
}

void pop_segment(size_t& count) {
    if (count > 0) {
        --count;
    }
}

void reset_to_mount_root(size_t floor_count, size_t& count) {
    if (count > floor_count) {
        count = floor_count;
    }
}

bool parse_into_segments(const char* path,
                         bool path_is_absolute,
                         size_t floor_count,
                         const Segment* mount_root_segment,
                         Segment (&segments)[kMaxSegments],
                         size_t& count) {
    if (path == nullptr) {
        return true;
    }

    const char* cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        const char* start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        size_t len = static_cast<size_t>(cursor - start);
        if (len == 0) {
            continue;
        }
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (count > floor_count) {
                pop_segment(count);
            } else if (!path_is_absolute) {
                // For relative paths we do not allow traversing above the root
                // of the combined base path, so ignore extra ".." segments.
            }
            continue;
        }
        if (!path_is_absolute &&
            len == 3 &&
            start[0] == '.' &&
            start[1] == '.' &&
            start[2] == '.') {
            if (mount_root_segment != nullptr &&
                mount_root_segment->data != nullptr &&
                mount_root_segment->length != 0) {
                segments[0] = *mount_root_segment;
                count = 1;
            } else {
                reset_to_mount_root(floor_count, count);
            }
            continue;
        }
        if (!push_segment(segments, count, start, len)) {
            return false;
        }
    }
    return true;
}

bool write_segments(const Segment (&segments)[kMaxSegments],
                    size_t count,
                    char (&out)[kMaxPathLength]) {
    size_t length = 0;
    out[length++] = '/';

    for (size_t i = 0; i < count; ++i) {
        if (length > 1) {
            if (length + 1 >= kMaxPathLength) {
                return false;
            }
            out[length++] = '/';
        }
        if (length + segments[i].length >= kMaxPathLength) {
            return false;
        }
        memcpy(out + length, segments[i].data, segments[i].length);
        length += segments[i].length;
    }

    if (length > 1 && out[length - 1] == '/') {
        --length;
    }
    out[length] = '\0';
    return true;
}

size_t string_length(const char* str) {
    size_t len = 0;
    if (str == nullptr) {
        return 0;
    }
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

}  // namespace

bool build_absolute_path(const char* base,
                         const char* input,
                         char (&out)[kMaxPathLength]) {
    Segment segments[kMaxSegments];
    size_t segment_count = 0;
    Segment mount_root_segment{nullptr, 0};

    const char* effective_base = (base != nullptr && base[0] != '\0')
                                     ? base
                                     : "/";
    size_t floor_count = vfs::has_explicit_mount_prefix(effective_base) ? 1 : 0;
    if (!parse_into_segments(effective_base,
                             true,
                             0,
                             nullptr,
                             segments,
                             segment_count)) {
        return false;
    }
    // "..." is the sysroot anchor, not the root of whichever mounted
    // filesystem contains the current working directory.  In particular,
    // commands must continue resolving through .../binary after cd'ing into
    // a removable filesystem such as /USBMS_0_0.
    const char* root_mount = vfs::root_mount_name();
    if (root_mount != nullptr && root_mount[0] != '\0') {
        mount_root_segment = Segment{root_mount, string_length(root_mount)};
    } else if (floor_count == 1 && segment_count > 0) {
        // Preserve the old mount-root behavior only during early boot before
        // a system root has been selected.
        mount_root_segment = segments[0];
    }

    if (input == nullptr || input[0] == '\0') {
        return write_segments(segments, segment_count, out);
    }

    if (input[0] == '/') {
        segment_count = 0;
        if (!parse_into_segments(input, true, 0, nullptr, segments, segment_count)) {
            return false;
        }
        return write_segments(segments, segment_count, out);
    }

    if (!parse_into_segments(input,
                             false,
                             floor_count,
                             &mount_root_segment,
                             segments,
                             segment_count)) {
        return false;
    }
    return write_segments(segments, segment_count, out);
}

}  // namespace path_util
