#pragma once
#include "config.hpp"
#include "game_process.hpp"

namespace mouse_mapping {
enum class input_priority { normal, above_normal };
inline input_priority parse_input_priority(std::wstring_view value) {
    if (value == L"normal") return input_priority::normal;
    if (value == L"above-normal") return input_priority::above_normal;
    throw std::runtime_error("--input-priority requires normal or above-normal");
}
void check_driver();
bool driver_available();
void run(const configuration& config, const game_command* game = nullptr,
    input_priority priority = input_priority::normal);
}
