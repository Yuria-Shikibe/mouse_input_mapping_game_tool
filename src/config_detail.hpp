#pragma once
#include "config.hpp"

namespace mouse_mapping::config_detail {
std::string trim(std::string_view value);
double parse_number(std::string_view value);
double parse_ratio(std::string_view value);
bool parse_switch(std::string_view value);
bool supported_key(key_code code);
}
