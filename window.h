#ifndef BSPWMBAR_WINDOW_H_
#define BSPWMBAR_WINDOW_H_

#include <xcb/xcb.h>

struct bb_window {
	xcb_window_t xw;

	int x, y, width, height;
};

#endif /* BSPWMBAR_WINDOW_H_ */
