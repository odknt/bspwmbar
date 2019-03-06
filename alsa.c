#include <alloca.h>
#include <alsa/asoundlib.h>

#include "bspwmbar.h"

snd_ctl_t *ctl;
AlsaInfo info = { 0 };

void
alsa_update()
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

	long min, max;
	if (!snd_mixer_selem_get_playback_volume_range(elem, &min, &max)) {
		if (!snd_mixer_selem_get_playback_volume(elem, 0, &info.volume))
			info.volume = (double)info.volume / max * 100 + 0.5;
	}
	snd_mixer_selem_get_playback_switch(elem, 0, &info.unmuted);
	snd_mixer_close(h);
}

AlsaInfo
alsa_info()
{
	if (!info.volume)
		alsa_update();

	return info;
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

int
alsa_need_update()
{
	snd_ctl_event_t *event;

	snd_ctl_event_alloca(&event);
	if (snd_ctl_read(ctl, event) < 0)
		return 0;
	if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM)
		return 0;

	unsigned int mask = snd_ctl_event_elem_get_mask(event);

	if (!(mask & SND_CTL_EVENT_MASK_VALUE))
		return 0;

	alsa_update();
	return 1;
}

void
alsa_disconnect()
{
	snd_ctl_close(ctl);
}
