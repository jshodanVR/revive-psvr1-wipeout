#pragma once

#include <functional>

#include "./filename_t.h"

namespace spdlog {
struct file_event_handlers {
    file_event_handlers()
        : before_open(nullptr),
          after_open(nullptr),
          before_close(nullptr),
          after_close(nullptr) {}

    std::function<void(const filename_t &filename)> before_open;
    std::function<void(const filename_t &filename, std::FILE *file_stream)> after_open;
    std::function<void(const filename_t &filename, std::FILE *file_stream)> before_close;
    std::function<void(const filename_t &filename)> after_close;
};
}  // namespace spdlog
