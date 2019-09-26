/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_CONFIG_H_
#define BSPWMBAR_CONFIG_H_

#include "bspwmbar.h"

/* intel */
#define THERMAL_PATH "/sys/class/thermal/thermal_zone0/temp"
/* k10temp */
/* #define THERMAL_PATH "/sys/class/hwmon/hwmon1/temp1_input" */

/* max length of monitor output name and bspwm desktop name */
#define NAME_MAXSZ  32
/* max length of active window title */
#define TITLE_MAXSZ 50
/* set window height */
#define BAR_HEIGHT  24
/* set tray icon size */
#define TRAY_ICONSZ 16

/* set font pattern for find fonts, see fonts-conf(5) */
const char *fontname = "sans-serif:size=10";

/*
 * color map for bspwmbar
 */
const char *colors[] = {
	"#222222", /* black */
	"#7f7f7f", /* gray */
	"#e5e5e5", /* white */
	"#1793d1", /* logo color */

	"#449f3d", /* success color */
	"#2f8419", /* normal color */
	"#f5a70a", /* warning color */
	"#ed5456", /* critical color */

	"#555555", /* dark gray */
};

/*
 * color settings by index of color map
 */
/* bspwmbar bg color */
#define BGCOLOR    0
/* inactive fg color */
#define ALTFGCOLOR 1
/* graph bg color */
#define ALTBGCOLOR 8
/* general fg color */
#define FGCOLOR    2
/* logo color */
#define LOGOCOLOR  3

/*
 * Module definition
 *
 * function:
 *   text           render the given string
 *   desktops       bspwm desktop states
 *   windowtitle    active window title
 *   datetime       the current time in the given format
 *   thermal        temperature of given sensor file
 *   volume         playback volume
 *   memgraph       memory usage
 *   cpugraph       cpu usage per core
 *   systray        systray icons
 *
 * option:
 *   prefix         prefix string for module
 *   suffix         suffix string for module
 *   arg            string argument for general modules
 *   vol            argument for volume module
 *                      muted:   string for muted state
 *                      unmuted: string for unmuted state
 *   desk           argument for desktops module
 *                      active:   string for active state
 *                      inactive: string for inactive state
 *   text           argument for text module
 *                      label: render string
 *                      color: render color
 * handler:
 *    volume_ev     handle click ButtonPress for voluem control
 *                      button1: toggle mute/unmute
 *                      button4: volume up (scroll up)
 *                      button5: volume down (scroll down)
 */

/* modules on the left */
const Module left_modules[] = {
	{ /* Arch logo */
		.func = text,
		.opts = { .text = { .label = "", .color = LOGOCOLOR } },
	},
	{ /* bspwm desktop state */
		.func = desktops,
		.opts = { .desk = { .active = "", .inactive = "" } }
	},
	{ /* active window title */
		.func = windowtitle,
		.opts = { .arg = "…" }
	},
};

/* modules on the right */
const Module right_modules[] = {
	{ /* system tray */
		.func = systray
	},
	{ /* cpu usage */
		.func = cpugraph,
		.opts = { .prefix = "cpu: " },
	},
	{ /* memory usage */
		.func = memgraph, .opts = { .prefix = "mem: ", },
	},
	{ /* master playback volume */
		.func = volume,
		.opts = { .suffix = "％", .vol = { .muted = "婢", .unmuted = "墳" } },
		.handler = volume_ev,
	},
	{ /* used space of root file system */
		.func = filesystem,
		.opts = { .arg = "/", .prefix = " ", .suffix = "％" },
	},
	{ /* cpu temperature */
		.func = thermal,
		.opts = { .arg = THERMAL_PATH, .prefix = " ", .suffix = "℃" },
	},
	{ /* clock */
		.func = datetime,
		.opts = { .prefix = " ", .arg = "%H:%M" },
	},
};

#endif
