/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
#pragma once

#define MAX_FILENAME 256

typedef struct {
    volatile int  running;
    volatile int  stop_requested;
    volatile int  uploads;
    char          last_name[MAX_FILENAME];
    char          status[128];

    volatile int  uploading;
    volatile long bytes_received;
    volatile long bytes_expected;
    volatile long speed_bps;

    char          current_name[MAX_FILENAME];
} AppState;

extern AppState g;