#include "sticks3_battery_curve.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(vibe_sticks3_battery_percent(4300) == 100);
    assert(vibe_sticks3_battery_percent(4133) == 100);
    assert(vibe_sticks3_battery_percent(4058) == 95);
    assert(vibe_sticks3_battery_percent(3628) == 50);
    assert(vibe_sticks3_battery_percent(3401) == 10);
    assert(vibe_sticks3_battery_percent(3352) == 5);
    assert(vibe_sticks3_battery_percent(3002) == 0);
    assert(vibe_sticks3_battery_percent(2800) == 0);

    int previous = 0;
    for (int voltage_mv = 2800; voltage_mv <= 4300; ++voltage_mv) {
        const int percent = vibe_sticks3_battery_percent(voltage_mv);
        assert(percent >= previous);
        assert(percent >= 0);
        assert(percent <= 100);
        previous = percent;
    }

    puts("battery curve tests passed");
    return 0;
}
