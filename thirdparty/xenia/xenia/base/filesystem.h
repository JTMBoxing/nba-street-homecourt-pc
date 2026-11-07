#pragma once
#include <cstdio>
#include <filesystem>
#include "xenia/base/math.h"
namespace xe::filesystem {
inline FILE* OpenFile(const std::filesystem::path& p, const char* mode) { return fopen(p.string().c_str(), mode); }
}
