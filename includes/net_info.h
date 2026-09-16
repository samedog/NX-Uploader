/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// net_info.h
//
// Best-effort local IP discovery for the applet's on-screen URL.
// No interface enumeration, no DNS, no fallbacks — just whatever
// gethostid() gives us, formatted for display.
#pragma once

// Populated by detect_local_ip(). "0.0.0.0" if detection failed.
// This is a display value only; the server binds to INADDR_ANY and
// does not use this.
extern char g_ip[32];

// Fills g_ip with the console's IPv4 address, or "0.0.0.0" on failure.
// Called once at startup, before the server thread is spawned.
void detect_local_ip(void);