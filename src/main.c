/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
 //main.c
#include <switch.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include "app_state.h"
#include "net_info.h"
#include "server.h"

#define UPLOAD_DIR "sdmc:/switch/uploads"
#define PORT 8080

AppState g;

// Formats a bytes/sec rate as a human-readable string. Picks the unit
// based on magnitude (B/s through GB/s).
static void format_speed(long bps, char *out, size_t outsz){
    if (bps < 0) bps = 0;
    const char *unit;
    double value;

    if (bps < 1024){
        unit = "B/s"; value = (double)bps;
    } else if (bps < 1024L * 1024L){
        unit = "KB/s"; value = (double)bps / 1024.0;
    } else if (bps < 1024L * 1024L * 1024L){
        unit = "MB/s"; value = (double)bps / (1024.0 * 1024.0);
    } else {
        unit = "GB/s"; value = (double)bps / (1024.0 * 1024.0 * 1024.0);
    }
    if (value < 10.0) snprintf(out, outsz, "%.2f %s", value, unit);
    else              snprintf(out, outsz, "%.1f %s", value, unit);
}


// Renders the console UI. Called once per main-loop iteration.
//
// Also updates g.speed_bps as a side effect: it samples bytes_received
// on a fixed cadence (every ~250ms) and applies an exponential moving
// average so the displayed rate doesn't jitter. This is the only place
// speed_bps is updated.
static void draw_ui(void){
    static u64 last_sample_tick = 0;
    static long last_sample_bytes = 0;

    if (g.uploading){
        u64 now = armGetSystemTick();
        if (last_sample_tick == 0){
            last_sample_tick = now;
            last_sample_bytes = g.bytes_received;
            g.speed_bps = 0;
        } else {
            u64 elapsed_ticks = now - last_sample_tick;
            u64 freq = armGetSystemTickFreq();
            if (elapsed_ticks > freq / 4){
                long delta_bytes = g.bytes_received - last_sample_bytes;
                double elapsed_sec = (double)elapsed_ticks / (double)freq;
                long instant_bps = (long)(delta_bytes / elapsed_sec);
                if (g.speed_bps == 0) g.speed_bps = instant_bps;
                else g.speed_bps = (g.speed_bps * 7 + instant_bps * 3) / 10;
                last_sample_tick = now;
                last_sample_bytes = g.bytes_received;
            }
        }
    } else {
        last_sample_tick = 0;
        last_sample_bytes = 0;
        g.speed_bps = 0;
    }

    consoleClear();
    printf("\x1b[2;2HNX Uploader");

    if (strcmp(g_ip, "0.0.0.0") == 0){
        printf("\x1b[4;2H\x1b[31mOffline \x1b[0m- connect to Wi-Fi or Ethernet");
        printf("\x1b[5;2HServer is listening on :%d anyway", PORT);
    } else {
        printf("\x1b[4;2H\x1b[32mhttp://%s:%d\x1b[0m", g_ip, PORT);
        printf("\x1b[5;2HOpen that URL from a browser on your LAN");
    }

    printf("\x1b[7;2HStatus:   %s", g.status);
    printf("\x1b[8;2HUploads:  %d", g.uploads);

    if (g.uploading && g.bytes_expected > 0){
        printf("\x1b[9;2HReceiving: %.60s",
               g.current_name[0] ? g.current_name : "...");

        long pct = (g.bytes_received * 100) / g.bytes_expected;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        const int width = 40;
        int filled = (int)((pct * width) / 100);

        char bar[64];
        int j = 0;
        bar[j++] = '[';
        for (int i = 0; i < width; i++) bar[j++] = (i < filled) ? '#' : '.';
        bar[j++] = ']';
        bar[j] = 0;

        char speed[32];
        format_speed(g.speed_bps, speed, sizeof(speed));

        printf("\x1b[10;2H%s %3ld%%", bar, pct);
        printf("\x1b[11;2H%ld / %ld bytes  @ %s",
               g.bytes_received, g.bytes_expected, speed);
    } else {
        printf("\x1b[9;2HLast:     %s", g.last_name[0] ? g.last_name : "(none)");
        printf("\x1b[10;2H");
        printf("\x1b[11;2H");
    }
    printf("\x1b[13;2HFiles land in %s", UPLOAD_DIR);
    printf("\x1b[15;2HPress + to stop and exit");
    consoleUpdate(NULL);
}

int main(int argc, char **argv){
    (void)argc; (void)argv;

    consoleInit(NULL);
    socketInitializeDefault();
    appletSetAutoSleepDisabled(true);
    detect_local_ip();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    if (mkdir(UPLOAD_DIR, 0777) != 0) {
        strcpy(g.status, "Failed to create uploads dir");
    }

    memset(&g, 0, sizeof(g));
    g.running = 1;
    strcpy(g.status, "Starting...");
    strcpy(g.last_name, "(none)");

    draw_ui();

    Thread t;
    threadCreate(&t, server_thread, NULL, NULL, 0x20000, 0x2C, -2);
    threadStart(&t);

    while (appletMainLoop() && !g.stop_requested){
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_Plus){
            g.stop_requested = 1;
        }

        static u64 last_ip_check = 0;
        u64 now = armGetSystemTick();
        if (now - last_ip_check > armGetSystemTickFreq()){
            detect_local_ip();
            last_ip_check = now;
        }

        draw_ui();
        svcSleepThread(100'000'000ull);
    }

    g.stop_requested = 1;
    threadWaitForExit(&t);
    threadClose(&t);

    consoleExit(NULL);
    socketExit();
    return 0;
}