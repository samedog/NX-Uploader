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

// Recursively deletes the file or directory tree at rel, relative to the
// configured root. Refuses the root itself. Returns 0 on success, -1 on error.
int delete_path(const char *rel);

// Creates a directory at rel, relative to sdmc:/.
// Returns 0 on success, -1 on error (parent missing, already exists, escapes root).
int make_dir(const char *rel);

// Resolves rel, stats it, and returns the file size in bytes via *out_size.
// full_out receives the resolved absolute path so  the caller can fopen() it.
// Returns 0 on success, -1 on error (It omits dirs).
int stat_file(const char *rel, char *full_out, size_t full_sz, long long *out_size);

// Creates rel and any missing parent directories, relative to the configured
// root. Existing components are left alone. Returns 0 on success, -1 if the
// path escapes root or a component could not be created.
int make_dirs(const char *rel);

// Renames or moves from_rel to to_rel, both relative to the configured root,
// replacing an existing destination file. Returns 0 on success, -1 on error.
int rename_path(const char *from_rel, const char *to_rel);

#endif /* FILEMAN_H */