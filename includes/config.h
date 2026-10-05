/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// config.h
//
// Single source of truth for values that used to be copied across main.c,
// server.c, and multipart.c. Keeping them here means the on-screen UI, the
// listener, and the uploader can never drift apart.
#pragma once

// TCP port the HTTP server listens on. Shown on screen and bound by the
// listener thread.
#define NXU_PORT 8080

// Default upload directory, relative to sdmc:/, used when a request does
// not carry an X-Upload-Dir header.
#define NXU_UPLOAD_DIR "sdmc:/switch/uploads"

// Where the optional config file is read from at startup.
#define NXU_CONFIG_PATH "sdmc:/switch/nxuploader.cfg"

// Runtime configuration, loaded from NXU_CONFIG_PATH by config_load().
// Anything not present in the file keeps the default below.
//
//   port=8080            base TCP port (fallbacks try +1 through +3)
//   root=sdmc:/          sandbox base for browse, delete, mkdir, rename,
//                        and the browser's upload destination
//   upload_dir=...       absolute upload dir used when a request omits the
//                        X-Upload-Dir header (for scripts, not the browser)
//   read_only=0          1 rejects uploads, delete, mkdir, and rename
//   allow_delete=1       0 rejects delete and rename
//   auth_user=           with auth_pass, turns on HTTP Basic auth
//   auth_pass=
//   log=0                1 appends each request line to the log file below

// Request log, written only when log=1.
#define NXU_LOG_PATH "sdmc:/switch/nxuploader.log"

extern int  cfg_port;             // base TCP port
extern int  cfg_read_only;        // 1 rejects any write operation
extern int  cfg_allow_delete;     // 1 allows delete and rename
extern int  cfg_log;              // 1 appends each request to NXU_LOG_PATH
extern char cfg_root[128];        // sandbox root, absolute, ends with "/"
extern char cfg_upload_dir[256];  // absolute default upload dir
extern char cfg_auth_user[64];
extern char cfg_auth_pass[64];
extern char cfg_auth_header[320]; // "Basic <base64>", empty when auth is off

// Reads NXU_CONFIG_PATH if it exists and applies it. A missing file leaves
// the defaults in place. Call once, before the server thread starts.
void config_load(void);
