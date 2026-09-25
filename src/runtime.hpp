#pragma once
#include "config.hpp"
#include "game_process.hpp"

namespace mouse_mapping {
void check_driver();
bool driver_available();
void run(const configuration& config, const game_command* game = nullptr);
}
