#pragma once

#include <stddef.h>

namespace path_util {

constexpr size_t kMaxPathLength = 128;

// Builds an absolute, canonical path by combining an existing absolute base
// path with an input path that may be absolute or relative. Returns false if
// the resolved path exceeds kMaxPathLength or the inputs are invalid.
bool build_absolute_path(const char* base,
                         const char* input,
                         char (&out)[kMaxPathLength]);

}  // namespace path_util
