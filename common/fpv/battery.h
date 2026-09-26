// Battery state-of-charge estimate from voltage alone. Rough by nature: voltage sags under
// throttle, so callers should filter and prefer readings taken while coasting.
#pragma once
#include <cstdint>

namespace fpv::battery {

enum class Chemistry : uint8_t { LiPo = 0, NiMH = 1 };

struct Point {
    uint16_t mv;  // per-cell millivolts
    uint8_t pct;
};

// Resting-voltage curves, descending.
constexpr Point kLipo[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
    {3910, 65},  {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
    {3770, 30},  {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},  {3270, 0},
};
constexpr Point kNimh[] = {
    {1420, 100}, {1350, 90}, {1300, 80}, {1270, 70}, {1250, 60}, {1230, 50},
    {1210, 40},  {1190, 30}, {1160, 20}, {1120, 10}, {1000, 0},
};

inline uint16_t fullCellMv(Chemistry c) { return c == Chemistry::LiPo ? 4200 : 1420; }
inline uint16_t lowCellMv(Chemistry c) { return c == Chemistry::LiPo ? 3500 : 1100; }

// Guesses the series cell count from a pack voltage measured at power-up. For LiPo this is
// ceil(v / 4.3V), which is right for any pack between 3.0 V/cell and 4.3 V/cell up to 6S.
inline uint8_t detectCells(uint32_t packMv, Chemistry c) {
    uint32_t perCellMax = c == Chemistry::LiPo ? 4300 : 1500;
    if (packMv < 1000) return 0;
    uint32_t cells = (packMv + perCellMax - 1) / perCellMax;
    return static_cast<uint8_t>(cells < 1 ? 1 : cells);
}

inline uint8_t percent(uint32_t packMv, uint8_t cells, Chemistry c) {
    if (cells == 0) return 0;
    const Point* t = c == Chemistry::LiPo ? kLipo : kNimh;
    const int n = c == Chemistry::LiPo ? int(sizeof kLipo / sizeof *kLipo) : int(sizeof kNimh / sizeof *kNimh);
    uint32_t mv = packMv / cells;
    if (mv >= t[0].mv) return 100;
    for (int i = 1; i < n; ++i) {
        if (mv >= t[i].mv) {
            uint32_t span = t[i - 1].mv - t[i].mv;
            return static_cast<uint8_t>(t[i].pct + (t[i - 1].pct - t[i].pct) * (mv - t[i].mv) / span);
        }
    }
    return 0;
}

}  // namespace fpv::battery
