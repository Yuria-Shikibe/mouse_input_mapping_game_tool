#pragma once
#include <windows.h>
#include "config.hpp"
#include <functional>

namespace mouse_mapping {
// Local preview reads relative mouse input but never sends system keys.
// The provider validates and snapshots the current draft on each start.
HWND create_input_monitor_view(HWND parent, HFONT font, std::function<configuration()> config_provider);
}
