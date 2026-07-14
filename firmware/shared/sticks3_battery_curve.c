#include "sticks3_battery_curve.h"

#include <stddef.h>

typedef struct {
    int voltage_mv;
    int percent;
} battery_curve_point_t;

/*
 * Calibrated from the 2026-07-14 always-on-screen full discharge:
 * 4138 mV to 3012 mV over 7698 seconds. Percent represents remaining
 * runtime under that measured load profile.
 */
static const battery_curve_point_t DISCHARGE_CURVE[] = {
    {4133, 100},
    {4058, 95},
    {3995, 90},
    {3939, 85},
    {3882, 80},
    {3834, 75},
    {3781, 70},
    {3732, 65},
    {3692, 60},
    {3654, 55},
    {3628, 50},
    {3602, 45},
    {3578, 40},
    {3560, 35},
    {3537, 30},
    {3518, 25},
    {3484, 20},
    {3446, 15},
    {3401, 10},
    {3352, 5},
    {3002, 0},
};

int vibe_sticks3_battery_percent(int voltage_mv)
{
    const size_t point_count = sizeof(DISCHARGE_CURVE) / sizeof(DISCHARGE_CURVE[0]);
    if (voltage_mv >= DISCHARGE_CURVE[0].voltage_mv) {
        return DISCHARGE_CURVE[0].percent;
    }
    if (voltage_mv <= DISCHARGE_CURVE[point_count - 1].voltage_mv) {
        return DISCHARGE_CURVE[point_count - 1].percent;
    }

    for (size_t index = 1; index < point_count; ++index) {
        const battery_curve_point_t high = DISCHARGE_CURVE[index - 1];
        const battery_curve_point_t low = DISCHARGE_CURVE[index];
        if (voltage_mv < low.voltage_mv) {
            continue;
        }

        const int voltage_span = high.voltage_mv - low.voltage_mv;
        const int percent_span = high.percent - low.percent;
        const int voltage_offset = voltage_mv - low.voltage_mv;
        return low.percent
               + (voltage_offset * percent_span + voltage_span / 2) / voltage_span;
    }
    return 0;
}
