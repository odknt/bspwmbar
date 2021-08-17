/* See LICENSE file for copyright and license details. */

#if defined(__linux)

#include "volume_alsa.c"

#elif defined(__OpenBSD)

#include "volume_bsd.c"

#endif
