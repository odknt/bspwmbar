/* See LICENSE file for copyright and license details. */

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <xcb/xcb.h>

#include "bspwmbar.h"

enum {
	ALSACTL_GETINFO = 1,
	ALSACTL_TOGGLE_MUTE,
	ALSACTL_VOLUME_UP,
	ALSACTL_VOLUME_DOWN,
};

typedef struct {
	long volume;
	int  unmuted;
	long max, min;
	long oneper;
	int  has_switch;
} alsa_info_t;

static snd_ctl_t *ctl;
static snd_mixer_t *mixer;
static int initialized = 0;
static alsa_info_t info = { 0 };
static poll_fd_t pfd = { 0 };

/* functions */
static void get_info(snd_mixer_elem_t *);
static void toggle_mute(snd_mixer_elem_t *);
static void set_volume(snd_mixer_elem_t *, long);
static void alsa_control(uint8_t);
static int alsa_connect();
static int alsa_disconnect();
static poll_result_t alsa_update(int);

void
get_info(snd_mixer_elem_t *elem)
{
	if (!initialized) {
		snd_mixer_selem_get_playback_volume_range(elem, &info.min, &info.max);
		info.has_switch = snd_mixer_selem_has_playback_switch(elem);
		info.oneper = BIGGER((info.max - info.min) / 100, 1);
		initialized = 1;
	}

	snd_mixer_selem_get_playback_volume(elem, 0, &info.volume);
	if (info.has_switch)
		snd_mixer_selem_get_playback_switch(elem, 0, &info.unmuted);
	else
		info.unmuted = 1;
}

void
toggle_mute(snd_mixer_elem_t *elem)
{
	if (!info.has_switch)
		return;
	info.unmuted = info.unmuted ? 0 : 1;
	snd_mixer_selem_set_playback_switch_all(elem, info.unmuted);
}

void
set_volume(snd_mixer_elem_t *elem, long volume)
{
	if (!BETWEEN(volume, info.min, info.max))
		volume = SMALLER(BIGGER(volume, info.min), info.max);
	snd_mixer_selem_set_playback_volume_all(elem, volume);
}

void
alsa_control(uint8_t ctlno)
{
	snd_mixer_elem_t *elem = NULL;
	snd_mixer_selem_id_t *sid = NULL;

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "Master");
	if (!(elem = snd_mixer_find_selem(mixer, sid))) {
		info.max = 1;
		return;
	}

	switch (ctlno) {
	case ALSACTL_GETINFO:
		get_info(elem);
		break;
	case ALSACTL_TOGGLE_MUTE:
		toggle_mute(elem);
		break;
	case ALSACTL_VOLUME_UP:
		set_volume(elem, info.volume + info.oneper * 5);
		break;
	case ALSACTL_VOLUME_DOWN:
		set_volume(elem, info.volume - info.oneper * 5);
		break;
	}
}

int
alsa_connect()
{
	if (snd_ctl_open(&ctl, "default", SND_CTL_READONLY | SND_CTL_NONBLOCK))
		return -1;

	if (snd_ctl_subscribe_events(ctl, 1)) {
		snd_ctl_close(ctl);
		return -1;
	}

	/* mixer initialization */
	if (snd_mixer_open(&mixer, 0)) {
		snd_ctl_close(ctl);
		return -1;
	}
	snd_mixer_attach(mixer, "default");
	snd_mixer_selem_register(mixer, NULL, NULL);
	snd_mixer_load(mixer);

	/* get poll fd */
	struct pollfd pfds;
	if (!snd_ctl_poll_descriptors(ctl, &pfds, 1)) {
		snd_ctl_close(ctl);
		return -1;
	}
	return pfds.fd;
}

poll_result_t
alsa_update(int fd)
{
	(void)fd;
	snd_ctl_event_t *event;

	snd_ctl_event_alloca(&event);
	if (snd_ctl_read(ctl, event) < 0)
		return PR_REINIT;
	if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM)
		return PR_NOOP;

	snd_mixer_handle_events(mixer);

	unsigned int mask = snd_ctl_event_elem_get_mask(event);
	if (!(mask & SND_CTL_EVENT_MASK_VALUE))
		return PR_NOOP;

	alsa_control(ALSACTL_GETINFO);

	return PR_UPDATE;
}

int
alsa_disconnect()
{
	return snd_ctl_close(ctl);
}

void
alsa_init()
{
	pfd.fd = alsa_connect();
	pfd.init = alsa_connect;
	pfd.deinit = alsa_disconnect;
	pfd.handler = alsa_update;
	poll_add(&pfd);
}

void
volume(draw_context_t *dc, module_option_t *opts)
{
	if (!pfd.fd)
		alsa_init();

	if (!opts->vol.prefix)
		opts->vol.prefix = "";
	if (!opts->vol.suffix)
		opts->vol.suffix = "";

	if (!info.volume)
		alsa_control(ALSACTL_GETINFO);

	const char *mark = (info.unmuted) ? opts->vol.unmuted : opts->vol.muted;
	sprintf(buf, "%s%s %.0lf%s", opts->vol.prefix,  mark,
	                             (double)info.volume / info.max * 100,
	                             opts->vol.suffix);
	draw_text(dc, buf);
}

void
volume_ev(xcb_generic_event_t *ev)
{
	xcb_button_press_event_t *button;
	switch (ev->response_type & ~0x80) {
	case XCB_BUTTON_PRESS:
		button = (xcb_button_press_event_t *)ev;
		switch (button->detail) {
		case XCB_BUTTON_INDEX_1:
			alsa_control(ALSACTL_TOGGLE_MUTE);
			break;
		case XCB_BUTTON_INDEX_4:
			alsa_control(ALSACTL_VOLUME_UP);
			break;
		case XCB_BUTTON_INDEX_5:
			alsa_control(ALSACTL_VOLUME_DOWN);
			break;
		}
		break;
	}
}
