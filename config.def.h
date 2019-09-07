/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_CONFIG_H_
#define BSPWMBAR_CONFIG_H_

#include "bspwmbar.h"

/* intel */
#define THERMAL_PATH "/sys/class/thermal/thermal_zone0/temp"
/* k10temp */
/* #define THERMAL_PATH "/sys/class/hwmon/hwmon1/temp1_input" */

#define BACKLIGHT "intel_backlight"
/* for laptops define the battery (/sys/class/power_supply/<battery>) */
#define BATTERY "BAT1"

/* max length of monitor output name and bspwm desktop name */
#define NAME_MAXSZ  32
/* max length of active window title */
#define TITLE_MAXSZ 80
/* set window height */
#define BAR_HEIGHT  24
/* set tray icon size */
#define TRAY_ICONSZ 16

/* set font pattern for find fonts, see fonts-conf(5) */
const char *fontname = "MonoSpace:size=10";

/* set strings for uses on render bspwm desktop */
#define WS_ACTIVE   ""
#define WS_INACTIVE ""

// Available colors: WHITE, BLACK, GRAY, DARK_GRAY, GREEN, BLUE, ORANGE, RED
typedef int Color;
const Color BGCOLOR = BLACK;  // Bar background color
const Color FGCOLOR = WHITE;  // Bar foreground color
const Color ALTBGCOLOR = DARK_GRAY;  // BarGraph (cpu & mem) background color
const Color ALTFGCOLOR = GRAY;  // BarGraph (cpu & mem) foreground color
const Color LOGOCOLOR = BLUE;  // Color of the logo

/*
 * function       description
 *
 * logo           render the given string
 * desktops       bspwm desktop states
 * windowtitle    active window title
 * datetime       the current time in the given format
 * thermal        temperature of given sensor file
 * volume         playback volume
 * memgraph       memory usage
 * cpugraph       cpu usage per core
 * systray        systray icons
 */
/* for modules on the left (float: left;) */
const Module left_modules[] = {
    /* function, argument, event handler */
    /* float: left; */
    //{ logo, "", NULL }, 
    { desktops, NULL, NULL }, 
    { windowtitle, "…", NULL }, 
};

/* for modules on the right (float: right;) */
const Module right_modules[] = {
    /* float: right; */
    { datetime, "%d-%m-%Y  %H:%M:%S", NULL }, 
    { battery, BATTERY, NULL }, 
    { volume, NULL, volume_ev },
    { brightness, BACKLIGHT, NULL },
    { wireless_network, "wlp1s0", NULL }, 
    { thermal, THERMAL_PATH, NULL }, 
    { memgraph, NULL, NULL }, 
    { cpugraph, NULL, NULL }, 
    //{ filesystem, "/", NULL }, 
    //{ systray, NULL, NULL }, 
};

#endif
