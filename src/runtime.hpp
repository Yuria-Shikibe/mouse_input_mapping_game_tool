#pragma once
#include "config.hpp"

namespace mouse_mapping {
void check_driver();
bool driver_available();
void run(const configuration& config);
}
