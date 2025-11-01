#include "path_util.hpp"

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

bool parse_into_segments(const char* path,
                         bool path_is_absolute,
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
            if (count > 0) {
                pop_segment(count);
            } else if (!path_is_absolute) {
                // For relative paths we do not allow traversing above the root
                // of the combined base path, so ignore extra ".." segments.
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

}  // namespace

bool build_absolute_path(const char* base,
                         const char* input,
                         char (&out)[kMaxPathLength]) {
    Segment segments[kMaxSegments];
    size_t segment_count = 0;

    const char* effective_base = (base != nullptr && base[0] != '\0')
                                     ? base
                                     : "/";
    if (!parse_into_segments(effective_base, true, segments, segment_count)) {
        return false;
    }

    if (input == nullptr || input[0] == '\0') {
        return write_segments(segments, segment_count, out);
    }

    if (input[0] == '/') {
        segment_count = 0;
        if (!parse_into_segments(input, true, segments, segment_count)) {
            return false;
        }
        return write_segments(segments, segment_count, out);
    }

    if (!parse_into_segments(input, false, segments, segment_count)) {
        return false;
    }
    return write_segments(segments, segment_count, out);
}

}  // namespace path_util
