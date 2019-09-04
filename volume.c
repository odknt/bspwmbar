/* See LICENSE file for copyright and license details. */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/audioio.h>
#include <X11/Xlib.h>

#include "bspwmbar.h"
#include "util.h"

#define check_device(dinfo) \
(((dinfo).type == AUDIO_MIXER_VALUE && \
  !strncmp((dinfo).label.name, AudioNmaster, MAX_AUDIO_DEV_LEN)) || \
 ((dinfo).type == AUDIO_MIXER_ENUM && \
  !strncmp((dinfo).label.name, AudioNmute, MAX_AUDIO_DEV_LEN)))

static char *file = NULL;
static mixer_ctrl_t vctrl = { 0 };
static mixer_ctrl_t mctrl = { 0 };
static int initialized = 0;

int
get_volume(int fd)
{
    if (ioctl(fd, AUDIO_MIXER_READ, &vctrl) < 0) {
        return 0;
    }
    return vctrl.un.value.num_channels == 1 ?
           vctrl.un.value.level[AUDIO_MIXER_LEVEL_MONO] :
           (vctrl.un.value.level[AUDIO_MIXER_LEVEL_LEFT] >
            vctrl.un.value.level[AUDIO_MIXER_LEVEL_RIGHT] ?
            vctrl.un.value.level[AUDIO_MIXER_LEVEL_LEFT] :
            vctrl.un.value.level[AUDIO_MIXER_LEVEL_RIGHT]);
}

int
is_muted(int fd)
{
    if (ioctl(fd, AUDIO_MIXER_READ, &mctrl) < 0) {
        return 0;
    }
    return mctrl.un.ord;
}

void
init_devinfo(int fd)
{
    mixer_devinfo_t dinfo;
    mixer_ctrl_t ctrl;

    int cls = -1;
    for (dinfo.index = 0; cls == -1; dinfo.index++) {
        if (ioctl(fd, AUDIO_MIXER_DEVINFO, &dinfo)) {
            close(fd);
            die("volume: failed to get device info\n");
        }
        if (dinfo.type == AUDIO_MIXER_CLASS &&
            !strncmp(dinfo.label.name, AudioCoutputs, MAX_AUDIO_DEV_LEN))
            cls = dinfo.index;
    }

    int vol = 1, mute = 1;
    for (dinfo.index = 0; vol || mute; dinfo.index++) {
        if (ioctl(fd, AUDIO_MIXER_DEVINFO, &dinfo) < 0) {
            close(fd);
            die("volume: failed to get device info\n");
        }
        if (dinfo.mixer_class == cls && check_device(dinfo)) {
            ctrl.dev = dinfo.index;
            ctrl.type = dinfo.type;
            ctrl.un.value.num_channels = 2;
            if (ioctl(fd, AUDIO_MIXER_READ, &ctrl) < 0) {
                ctrl.un.value.num_channels = 1;
                if (ioctl(fd, AUDIO_MIXER_READ, &ctrl) < 0) {
                    close(fd);
                    die("volume: failed to get mixer info\n");
                }
            }
            if (ctrl.type == AUDIO_MIXER_VALUE) {
                vctrl = ctrl;
                vol = 0;
            } else if (ctrl.type == AUDIO_MIXER_ENUM) {
                mctrl = ctrl;
                mute = 0;
            }
        }
    }

    initialized = 1;
}

void
volume(DC dc, const char *args)
{
    (void)args;

    if (!file) {
        if ((file = getenv("MIXERDEVICE")) == 0 || *file == '\0')
            file = "/dev/mixer";
    }

    int fd;
    if ((fd = open(file, O_RDONLY)) < 0)
        die("volume: failed to open %s\n", file);

    if (!initialized)
        init_devinfo(fd);

    const char *mark = is_muted(fd) ? "婢" : "墳";
    sprintf(buf, "%s %d％", mark, get_volume(fd) * 100 / 255);
    draw_text(dc, buf);

    close(fd);
}

void
toggle_mute(int fd)
{
    if (is_muted(fd))
        mctrl.un.ord = 0;
    else
        mctrl.un.ord = 1;

    ioctl(fd, AUDIO_MIXER_WRITE, &mctrl);
}

void
set_volume(int fd, int vol)
{
    if (vctrl.un.value.num_channels == 1) {
        vctrl.un.value.level[AUDIO_MIXER_LEVEL_MONO] = vol;
    } else {
        vctrl.un.value.level[AUDIO_MIXER_LEVEL_LEFT] = vol;
        vctrl.un.value.level[AUDIO_MIXER_LEVEL_RIGHT] = vol;
    }

    ioctl(fd, AUDIO_MIXER_WRITE, &vctrl);
}

void
volume_ev(XEvent ev)
{
    int fd;
    if ((fd = open(file, O_RDWR)) < 0)
        die("volume: failed to open %s\n", file);

    int vol = get_volume(fd);

    switch (ev.type) {
    case ButtonPress:
        switch (ev.xbutton.button) {
        case Button1:
            toggle_mute(fd);
            break;
        case Button4:
            set_volume(fd, SMALLER(vol + 12, 255));
            break;
        case Button5:
            set_volume(fd, BIGGER(vol - 12, 0));
            break;
        }
        break;
    }
    close(fd);
}
