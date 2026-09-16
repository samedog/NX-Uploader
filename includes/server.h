/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// server.h
#pragma once

// Entry point for the HTTP server thread. Spawns and returns
// immediately; the caller should pass this to threadCreate().
// Runs until g.stop_requested is set (user pressed "+") or bind()
// fails. Sets g.running = 0 on exit.
void server_thread(void *arg);