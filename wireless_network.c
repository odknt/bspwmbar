/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

#include <netinet/in.h>
#include <net/if.h>
#include <linux/wireless.h>
#include <iwlib.h>

struct wireless_stats {
    struct sockaddr addr;
    char name[IFNAMSIZ + 1];
    char essid[IW_ESSID_MAX_SIZE + 2];
    char mode[16];
    char freq[16];
    int channel;
    char bitrate[16];
    int link_qual;  /* calculate percentage from link_qual and link_qual_max */
    int link_qual_max;
    int link_level; /* dBm */
    int link_noise; /* dBm */
};

static int ya_int_get_wireless_info(struct wireless_stats* ws, const char *dev_name) {
    int skfd;
    struct wireless_info winfo;

    /* not all fields may be initialized, so set them to zero */
    memset(ws, 0, sizeof(struct wireless_stats));
    memset(&winfo, 0, sizeof(struct wireless_info));

    skfd = iw_sockets_open();

    snprintf(ws->name, IFNAMSIZ+1, "%s", dev_name);

    if (iw_get_basic_config(skfd, dev_name, &(winfo.b)) > -1) {

        /* Check availability of variables */
        if (iw_get_range_info(skfd, dev_name, &(winfo.range)) >= 0) {
            winfo.has_range = 1;
        }

        if (iw_get_stats(skfd, dev_name, &(winfo.stats), &winfo.range, winfo.has_range) >= 0) {
            winfo.has_stats = 1;
        }

        /* Link Quality */
        if (winfo.has_range && winfo.has_stats && ((winfo.stats.qual.level != 0)
                || (winfo.stats.qual.updated & IW_QUAL_DBM))) {
            if (!(winfo.stats.qual.updated & IW_QUAL_QUAL_INVALID)) {
                ws->link_qual = winfo.stats.qual.qual;
                ws->link_qual_max = winfo.range.max_qual.qual;
                ws->link_noise = winfo.stats.qual.noise;
                ws->link_level = winfo.stats.qual.level;
            }
        }

        /* ESSID */
        if (winfo.b.has_essid && winfo.b.essid_on) {
            snprintf(ws->essid, IW_ESSID_MAX_SIZE+2, "%s", winfo.b.essid);
        }

        /* Channel and Frequency */
        if (winfo.b.has_freq && winfo.has_range == 1) {
            ws->channel = iw_freq_to_channel(winfo.b.freq, &(winfo.range));
            iw_print_freq_value(ws->freq, 16, winfo.b.freq);
        }

        snprintf(ws->mode, 16, "%s", iw_operation_mode[winfo.b.mode]);

    }
    iw_sockets_close(skfd);

    return 0;
}

//static char *airplaine_icon = "";
// Todo: Check if rfkill is on and if so show airplaine icon

static char *format = " %s (%d%%)";

void
wireless_network(DC dc, const char *interface)
{
    static char *essid;
    static int link_qual;
    static time_t prevtime;
    static struct wireless_stats ws;

    time_t curtime = time(NULL);
    if (curtime - prevtime < 1)
        goto DRAW_WIFI;
    prevtime = curtime;

    if (ya_int_get_wireless_info(&ws, interface) || ws.link_qual_max == 0) {
        essid = "-";
        link_qual = 0;
    } else {
        essid = ws.essid;
        link_qual = ws.link_qual*100 / ws.link_qual_max;
    }

DRAW_WIFI:
    sprintf(buf, format, essid, link_qual);
    draw_text(dc, buf);
}
