/* See LICENSE file for copyright and license details. */

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <X11/Xlib.h>

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
} AlsaInfo;

static snd_ctl_t *ctl;
static int initialized = 0;
static AlsaInfo info = { 0 };
static PollFD pfd = { 0 };

static void
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

static void
toggle_mute(snd_mixer_elem_t *elem)
{
    if (!info.has_switch)
        return;
    info.unmuted = info.unmuted ? 0 : 1;
    snd_mixer_selem_set_playback_switch_all(elem, info.unmuted);
}

static void
set_volume(snd_mixer_elem_t *elem, long volume)
{
    if (!BETWEEN(volume, info.min, info.max))
        volume = SMALLER(BIGGER(volume, info.min), info.max);
    snd_mixer_selem_set_playback_volume_all(elem, volume);
}

static void
alsa_control(uint8_t ctlno)
{
    snd_mixer_t *h;
    snd_mixer_selem_id_t *sid;

    snd_mixer_open(&h, 0);
    snd_mixer_attach(h, "default");
    snd_mixer_selem_register(h, NULL, NULL);
    snd_mixer_load(h);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    snd_mixer_elem_t *elem = snd_mixer_find_selem(h, sid);

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

    snd_mixer_close(h);
}

int
alsa_connect()
{
    if (snd_ctl_open(&ctl, "default", SND_CTL_READONLY) < 0)
        return -1;
    if (snd_ctl_subscribe_events(ctl, 1))
        return -1;

    struct pollfd *pfds = alloca(sizeof(struct pollfd));
    if (!snd_ctl_poll_descriptors(ctl, pfds, 1))
        return -1;
    return pfds[0].fd;
}

PollResult
alsa_update(int fd)
{
    (void)fd;
    snd_ctl_event_t *event;

    snd_ctl_event_alloca(&event);
    if (snd_ctl_read(ctl, event) < 0)
        return PR_REINIT;
    if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM)
        return PR_NOOP;

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
volume(DC dc, const char *arg)
{
    (void)arg;
    if (!pfd.fd)
        alsa_init();

    if (!info.volume)
        alsa_control(ALSACTL_GETINFO);

    const char *mark = (info.unmuted) ? "墳" : "婢";
    sprintf(buf, "%s %.0lf%%", mark, (double)info.volume / info.max * 100);
    draw_text(dc, buf);
}

void
volume_ev(XEvent ev)
{
    switch (ev.type) {
    case ButtonPress:
        switch (ev.xbutton.button) {
        case Button1:
            alsa_control(ALSACTL_TOGGLE_MUTE);
            break;
        case Button4:
            alsa_control(ALSACTL_VOLUME_UP);
            break;
        case Button5:
            alsa_control(ALSACTL_VOLUME_DOWN);
            break;
        }
        break;
    }
}
