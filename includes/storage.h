/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// storage.h
//
// Free and total space of the SD card, for the on-screen readout and the
// browser UI. Values come from the already-mounted sdmc: device.
#pragma once

// Bytes available on sdmc:/, or -1 if it could not be read.
long long storage_free_bytes(void);

// Total bytes on sdmc:/, or -1 if it could not be read.
long long storage_total_bytes(void);
