/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
// fileman.h
#ifndef FILEMAN_H
#define FILEMAN_H

#include <stddef.h>

#define LIST_BUF_SIZE (32 * 1024)

// Lists a directory relative to sdmc:/ into buf as tab-separated lines:
//   D\t<name>\t0\n        for directories
//   F\t<name>\t<size>\n   for files
// Returns bytes written, or -1 on error. Output is silently truncated if
// buf_sz is too small for all entries.
int list_dir(const char *rel, char *buf, size_t buf_sz);

// Deletes the file (or empty directory) at rel, relative to sdmc:/.
// Returns 0 on success, -1 on error (doesn't exist, escapes root, non-empty dir).
// This will always refuse to delete the root even if join_and_check allowed it.
int delete_path(const char *rel);

// Creates a directory at rel, relative to sdmc:/.
// Returns 0 on success, -1 on error (parent missing, already exists, escapes root).
int make_dir(const char *rel);

#endif /* FILEMAN_H */