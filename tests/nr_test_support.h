#pragma once
#define NOMINMAX
#include <windows.h>
#include "../extern/XeSS/samples/basic_sample_frame_generation/d3dx12.h"
#include <cstdio>
#include <format>
#include <utility>
namespace logger {
template<class... T> void info(std::format_string<T...> format, T&&... args) {
    const auto text = std::format(format, std::forward<T>(args)...);
    std::puts(text.c_str());
}
template<class... T> void warn(std::format_string<T...> format, T&&... args) {
    info(format, std::forward<T>(args)...);
}
}
