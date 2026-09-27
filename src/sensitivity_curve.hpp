#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <locale>
#include <vector>

namespace mouse_mapping {
struct curve_point { double x, y; };
using curve_points = std::vector<curve_point>;
inline constexpr std::size_t minimum_curve_points = 2;
inline constexpr std::size_t maximum_curve_points = 16;
inline constexpr double minimum_curve_spacing = 0.001;

inline void validate_curve(const curve_points& points) {
    if (points.size() < minimum_curve_points || points.size() > maximum_curve_points)
        throw std::runtime_error("Curve requires 2..16 points");
    if (points.front().x != 0 || points.back().x != 1)
        throw std::runtime_error("Curve endpoints must have input 0 and 1");
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto [x, y] = points[i];
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1)
            throw std::runtime_error("Curve coordinates must be finite and 0..1");
        if (i && x - points[i - 1].x < minimum_curve_spacing - 1e-12)
            throw std::runtime_error("Curve inputs must increase with spacing >= 0.001");
    }
}

inline curve_points parse_curve(std::string_view text) {
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    curve_points points;
    points.reserve(maximum_curve_points);
    for (;;) {
        double x, y; char colon;
        if (!(input >> x >> colon >> y) || colon != ':')
            throw std::runtime_error("Expected curve points: 0:0,0.5:0.25,1:1");
        points.push_back({x, y});
        if (points.size() > maximum_curve_points) throw std::runtime_error("Curve requires 2..16 points");
        input >> std::ws;
        if (input.eof()) break;
        char comma;
        if (!(input >> comma) || comma != ',') throw std::runtime_error("Expected comma between curve points");
    }
    validate_curve(points);
    return points;
}
inline std::string format_curve(const curve_points& points) {
    std::string output;
    output.reserve(points.size() * 48);
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i) output.push_back(',');
        std::format_to(std::back_inserter(output), "{:.17g}:{:.17g}", points[i].x, points[i].y);
    }
    return output;
}

// PCHIP: weighted harmonic interior slopes and limited one-sided end slopes.
// https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html
class sensitivity_curve {
public:
    explicit sensitivity_curve(curve_points points = {{0, 0}, {1, 1}}) : points_(std::move(points)) {
        validate_curve(points_);
        const auto n = points_.size();
        std::array<double, maximum_curve_points - 1> widths{};
        std::array<double, maximum_curve_points - 1> deltas{};
        for (std::size_t i = 0; i + 1 < n; ++i) {
            widths[i] = points_[i + 1].x - points_[i].x;
            deltas[i] = (points_[i + 1].y - points_[i].y) / widths[i];
        }
        if (n == minimum_curve_points) { slopes_[0] = slopes_[1] = deltas[0]; return; }
        for (std::size_t i = 1; i + 1 < n; ++i) {
            if (deltas[i - 1] * deltas[i] <= 0) slopes_[i] = 0;
            else {
                const auto first_weight = 2 * widths[i] + widths[i - 1];
                const auto second_weight = widths[i] + 2 * widths[i - 1];
                slopes_[i] = (first_weight + second_weight)
                    / (first_weight / deltas[i - 1] + second_weight / deltas[i]);
            }
        }
        slopes_[0] = endpoint(widths[0], widths[1], deltas[0], deltas[1]);
        slopes_[n - 1] = endpoint(widths[n - 2], widths[n - 3], deltas[n - 2], deltas[n - 3]);
    }
    double evaluate(double x) const {
        if (!std::isfinite(x)) return points_.front().y;
        x = std::clamp(x, 0.0, 1.0);
        std::size_t i = 0;
        while (i + 2 < points_.size() && x > points_[i + 1].x) ++i;
        const auto a = points_[i], b = points_[i + 1];
        const double h = b.x - a.x, t = (x - a.x) / h, t2 = t * t, t3 = t2 * t;
        return std::clamp((2*t3 - 3*t2 + 1)*a.y + (t3 - 2*t2 + t)*h*slopes_[i]
            + (-2*t3 + 3*t2)*b.y + (t3 - t2)*h*slopes_[i + 1], 0.0, 1.0);
    }
private:
    static double endpoint(double h0, double h1, double d0, double d1) {
        const double m = ((2*h0 + h1)*d0 - h0*d1) / (h0 + h1);
        if (m*d0 <= 0) return 0;
        if (d0*d1 <= 0 && std::abs(m) > 3*std::abs(d0)) return 3*d0;
        return m;
    }
    curve_points points_;
    std::array<double, maximum_curve_points> slopes_{};
};

struct sensitivity_settings {
    bool enabled = false;
    double full_speed = 1000;
    curve_points points{{0, 0}, {1, 1}};
};
}
