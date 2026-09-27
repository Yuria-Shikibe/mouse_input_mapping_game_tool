#pragma once
#include <windows.h>
#include "sensitivity_curve.hpp"
namespace mouse_mapping {
// Modal draft editor. Returns false on cancel without modifying points.
bool edit_curve(HWND owner, curve_points& points, const wchar_t* title);
}
