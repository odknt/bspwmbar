/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

static char *format = "%s%s %d%%";


int get_state(const char *battery, char **state) {
    // Check if charging or not!
    char spath[128];
    snprintf(spath, 128, "/sys/class/power_supply/%s/status", battery);

    FILE* sfile = fopen(spath, "r");
    int c = fgetc(sfile);

    *state = (c == 67) ? " " : "";

    return 0;
}


void
battery(DC dc, const char *battery)
{
    static char *symbol, *state;
    static time_t prevtime;
    static uintmax_t capacity;
    static int battery_found = -1;

    char cpath[128];
    snprintf(cpath, 128, "/sys/class/power_supply/%s/capacity", battery);

    if (battery_found == -1) {
        if (access(cpath, F_OK) != -1)
            battery_found = 1;
        else
            battery_found = 0;
    }
    if (!battery_found)
        return;

    time_t curtime = time(NULL);
    if (curtime - prevtime < 1)
        goto DRAW_BATTERY;
    prevtime = curtime;

    if (pscanf(cpath, "%ju", &capacity) == -1)
        return;

    if (get_state(battery, &state) != 0)
        return;

    switch (capacity) {
        case 0 ... 10:
            // empty
            symbol = "!";
            break;
        case 11 ... 19:
            // almost empty
            symbol = "";
            break;
        case 20 ... 49:
            // 1/3 full
            symbol = "";
            break;
        case 50 ... 85:
            // 3/4 full
            symbol = "";
            break;
        case 86 ... 100:
            // full
            symbol = "";
            break;
    }

DRAW_BATTERY:
    sprintf(buf, format, state, symbol, capacity);
    draw_text(dc, buf);
}