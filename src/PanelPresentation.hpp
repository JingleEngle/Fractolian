#pragma once
// Host-independent geometry and formatting for the compact 28 HP native panel.
#include "FractalCore.hpp"
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace fractal { namespace panel {
constexpr double WIDTH_MM = 142.24;
constexpr double HEIGHT_MM = 128.5;
constexpr int CONTROL_COUNT = 12;
constexpr double CELL_W = (WIDTH_MM - 4) / 6;
constexpr double READOUT_W = 20.6;
constexpr double READOUT_H = 5.2;
inline double controlX(int index) { return 2 + CELL_W * ((index % 6) + .5); }
inline double titleY(int index) { return index < 6 ? 57.3 : 88.3; }
inline double mainY(int index) { return index < 6 ? 64 : 95; }
inline double readoutY(int index) { return index < 6 ? 71.8 : 102.8; }
inline double modulationY(int index) { return index < 6 ? 81.9 : 112.9; }
inline double legendY(int index) { return index < 6 ? 77.1 : 108.1; }
inline double seedLeft(int corner) { return 58 + corner * 20.56; }
inline double seedX(int corner) { return seedLeft(corner) + 10; }
inline int algorithmIndex(double value) {
    return static_cast<int>(std::llround(safe(value, 0, ALGORITHMS - 1)));
}
inline std::string fixed(double value, int places) {
    if (!std::isfinite(value)) value = 0;
    if (std::abs(value) < .5 * std::pow(10., -places)) value = 0;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(places) << value;
    return out.str();
}
// Panel order: X, Y, Zoom, Position, Roughness, Smoothing,
// Tune, Octave, Glide, Level, Position slew, Zoom slew.
// Zoom, frequency, time, and level are supplied in physical units.
inline std::string readout(int control, double value) {
    switch (control) {
        case 0: case 1: case 3: case 4:
            return fixed(safe(value, 0, 1, .5), 5);
        case 2: return fixed(safe(value, 1, 64, 1), 3) + "x";
        case 5: return fixed(safe(value, 0, 1, .25) * 100, 1) + "%";
        case 6: return fixed(safe(value, .05, 1000000, 220), 2) + " Hz";
        case 7: return fixed(std::round(safe(value, 0, 8, 3)), 0);
        case 8: return fixed(safe(value, 0, 5000, 20), 2) + " ms";
        case 9: return fixed(safe(value, -60, -3, -18), 1) + " dB";
        case 10: return fixed(safe(value,0,5000),2)+" ms";
        case 11: return fixed(safe(value, 0, 1000), 1) + " ms";
        default: return "";
    }
}
inline double defaultValue(int control) {
    const double values[] = {.5, .5, 1, .5, .68, .25, 220, 3, 20, -18, 80, 35};
    return control >= 0 && control < CONTROL_COUNT ? values[control] : 0;
}
}} // namespace fractal::panel
