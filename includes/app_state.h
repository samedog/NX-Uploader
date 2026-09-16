/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// app_state.h
//
// Shared state between the server thread and the main applet loop.
//
// Two threads touch this struct:
//   - server_thread()  writes most fields as requests come in
//   - main loop        reads everything for the on-screen UI, and
//                      writes stop_requested when "+" is pressed
//
// Every mutable field is marked volatile so the compiler doesn't cache
// reads across the loop iteration. No locks: individual field accesses
// are atomic at this size on ARM64, and the UI is tolerant of reading a
// slightly stale value. Do not add multi-field "transactions" to this
// struct without also adding a lock.
#pragma once

#define MAX_FILENAME 256

typedef struct {
    // --- lifecycle ---

    // 1 while the server thread is running, 0 once it has exited
    // (on bind failure or after a clean shutdown). Read by the UI.
    volatile int  running;

    // Set to 1 by the main loop when the user presses "+". The server
    // thread polls this in its select() loop and exits when set.
    // Write-once; never cleared.
    volatile int  stop_requested;

    // --- counters ---

    // Total number of files successfully uploaded since launch.
    // Incremented by mp.files_saved after each successful request, so a
    // multi-file upload bumps this by the number of files that landed.
    volatile int  uploads;

    // Filename of the most recently completed upload, for the "Last:"
    // line. NUL-terminated. Written by the server thread after a
    // successful mp_close_file().
    char          last_name[MAX_FILENAME];

    // Free-form status string shown on the "Status:" line. Written by
    // the server thread (bind errors, "Listening on...") and by the
    // main loop (startup). Not synchronized; last writer wins.
    char          status[128];

    // --- in-flight upload ---
    // These are only meaningful while uploading == 1. The UI samples
    // them once per frame to draw the progress bar and speed estimate.

    // 1 while a multipart upload is in progress, 0 otherwise.
    volatile int  uploading;

    // Bytes received so far for the current upload.
    volatile long bytes_received;

    // Content-Length of the current upload, from the request header.
    // Used as the denominator for the progress bar.
    volatile long bytes_expected;

    // Smoothed transfer rate in bytes/sec, updated by draw_ui() as a
    // moving average. Zero when no upload is in progress.
    volatile long speed_bps;

    // Filename of the file part currently being written, for the
    // "Receiving:" line. Updated each time the parser opens a new part,
    // so during a multi-file upload this cycles through the filenames as
    // they're written. Empty during the multipart preamble before the
    // first file part's headers have been parsed.
    char          current_name[MAX_FILENAME];
    
} AppState;

extern AppState g;