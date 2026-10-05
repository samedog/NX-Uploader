/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// storage.c
#include <switch.h>
#include "storage.h"

// sdmc:/ is already mounted by the time this runs (the app reads and
// writes it directly), so reuse that device's FsFileSystem instead of
// opening a second handle.
static FsFileSystem *sdmc_fs(void){
    return fsdevGetDeviceFileSystem("sdmc");
}

long long storage_free_bytes(void){
    FsFileSystem *fs = sdmc_fs();
    if (!fs) return -1;
    s64 out = 0;
    if (R_FAILED(fsFsGetFreeSpace(fs, "/", &out))) return -1;
    return (long long)out;
}

long long storage_total_bytes(void){
    FsFileSystem *fs = sdmc_fs();
    if (!fs) return -1;
    s64 out = 0;
    if (R_FAILED(fsFsGetTotalSpace(fs, "/", &out))) return -1;
    return (long long)out;
}
