// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>

namespace Common::FS {
// Guest paths can be short while their installation prefix makes the host path
// exceed MAX_PATH. Extended paths do not depend on machine-wide registry policy.
inline std::filesystem::path NativePath(const std::filesystem::path& path) {
#ifdef _WIN32
    if (path.empty()) {
        return path;
    }
    if (path.native().starts_with(L"\\\\?\\") || path.native().starts_with(L"\\\\.\\")) {
        auto native = path;
        return native.make_preferred();
    }
    if (path.is_absolute() && path.native().size() < 248) {
        return path;
    }
    auto absolute = std::filesystem::absolute(path).lexically_normal().make_preferred();
    const auto& name = absolute.native();
    if (name.size() < 248) {
        return path;
    }
    if (name.starts_with(L"\\\\")) {
        return std::filesystem::path{L"\\\\?\\UNC\\" + name.substr(2)};
    }
    return std::filesystem::path{L"\\\\?\\" + name};
#else
    return path;
#endif
}
} // namespace Common::FS
