// wifi.c

#include <stdio.h>
#include <string.h>

#include "bspwmbar.h"

#define MAX_BUFFER_SIZE 256

void wifi(draw_context_t *dc, module_option_t *opts) {
    module_wifi_t *wifi_opts = (module_wifi_t *)opts;

    char status[MAX_BUFFER_SIZE] = {0};
    char ssid[MAX_BUFFER_SIZE] = {0};
    char ip[MAX_BUFFER_SIZE] = {0};
    bool wifiConnected = false;

    // Check Wi-Fi status
    FILE *wifi_status = popen("iwconfig wlan0 | grep ESSID", "r");
    if (wifi_status) {
        while (fgets(status, sizeof(status), wifi_status) != NULL) {
            char *essid = strstr(status, "ESSID:");
            if (essid) {
                // Extract the SSID
                if (strstr(essid, "off/any") != NULL) {
                    wifiConnected = false;
                } else {
                    sscanf(essid, "ESSID:\"%[^\"]\"", ssid);
                    wifiConnected = true;
                }
            }
        }
        pclose(wifi_status);
    }

    // Get local IP address
    FILE *local_ip = popen("ip addr show wlan0 | grep 'inet ' | awk '{print $2}' | cut -d'/' -f1 | tr -d '\n\r'", "r");
    if (local_ip) {
        fgets(ip, sizeof(ip), local_ip);
        pclose(local_ip);
    }

    // Define color
    color_t *greenColor = color_load("#449f3d");  // Green
    color_t *redColor = color_load("#ed5456");    // Red

    // Display Wi-Fi status and local IP address
    if (wifiConnected) {
        draw_text(dc, "Wi-Fi: ");
        draw_color_text(dc, greenColor, ssid);

        if (strcmp(wifi_opts->show_ip, "true") == 0) {
            draw_text(dc, " | IP: ");
            draw_color_text(dc, greenColor, ip);
        }
    } else {
        draw_text(dc, "Wi-Fi: ");
        draw_color_text(dc, redColor, "Disabled");
    }
}
