/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
// fileman.c
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "config.h"

// Returns 0 on success, -1 if the joined path escapes root.
// this normalizes and pass multiple checks
static int join_and_check(const char *rel, char *out, size_t out_sz)
{
    size_t root_len = strlen(cfg_root);
    if (snprintf(out, out_sz, "%s%s", cfg_root, rel) >= (int)out_sz)
        return -1;

    char *segments[64];
    int nseg = 0;
    char *p = out + root_len;   // skip the root prefix
    char *save = NULL;
    for (char *tok = strtok_r(p, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (nseg > 0) nseg--; continue; }
        if (nseg >= 64) return -1;
        segments[nseg++] = tok;
    }

    char tmp[1024];
    size_t off = 0;
    memcpy(tmp, cfg_root, root_len);
    off = root_len;
    for (int i = 0; i < nseg; i++) {
        size_t l = strlen(segments[i]);
        if (off + l + 2 >= sizeof(tmp)) return -1;
        memcpy(tmp + off, segments[i], l);
        off += l;
        if (i != nseg - 1) tmp[off++] = '/';
    }
    tmp[off] = 0;
    memcpy(out, tmp, off + 1);
    return 0;
}

int list_dir(const char *rel, char *buf, size_t buf_sz)
{
    char full[768];
    if (join_and_check(rel, full, sizeof(full)) < 0)
        return -1;

    DIR *d = opendir(full);
    if (!d) return -1;

    size_t off = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        char child[1200];
        snprintf(child, sizeof(child), "%s/%s", full, ent->d_name);

        // stat() can fail (broken symlink, permission, race). Only trust
        // st when it succeeded, otherwise report a zero-size file rather
        // than reading uninitialized memory.
        struct stat st;
        int is_dir = 0;
        long long size = 0;
        if (stat(child, &st) == 0){
            if (S_ISDIR(st.st_mode)){
                is_dir = 1;
            } else {
                size = (long long)st.st_size;
            }
        }

        int n = snprintf(buf + off, buf_sz - off,
                         "%c\t%s\t%lld\n",
                         is_dir ? 'D' : 'F', ent->d_name, size);
        if (n < 0 || (size_t)n >= buf_sz - off) break;   // buffer full
        off += n;
    }
    closedir(d);
    return (int)off;
}

// Depth cap for recursive removal. SD layouts are shallow; the cap keeps a
// pathological tree from overflowing the small server-thread stack.
#define RM_MAX_DEPTH 24

// Removes a file, or a directory and everything under it.
static int rm_tree(const char *full, int depth){
    if (depth > RM_MAX_DEPTH) return -1;

    struct stat st;
    if (stat(full, &st) != 0) return -1;

    if (!S_ISDIR(st.st_mode))
        return remove(full) == 0 ? 0 : -1;

    DIR *d = opendir(full);
    if (!d) return -1;

    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d))){
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[768];
        if (snprintf(child, sizeof(child), "%s/%s", full, e->d_name) >= (int)sizeof(child)){
            rc = -1;
            continue;
        }
        if (rm_tree(child, depth + 1) != 0) rc = -1;
    }
    closedir(d);

    // remove() drops an empty directory.
    if (remove(full) != 0) rc = -1;
    return rc;
}

int delete_path(const char *rel){
    if (!rel || !rel[0]) return -1;

    char full[768];
    if (join_and_check(rel, full, sizeof(full)) < 0)
        return -1;

    if (strcmp(full, cfg_root) == 0)
        return -1;

    return rm_tree(full, 0);
}


int make_dir(const char *rel){
    if (!rel || !rel[0]) return -1;   // refuse to create root

    char full[768];
    if (join_and_check(rel, full, sizeof(full)) < 0)
        return -1;

    // Don't try to mkdir the root itself.
    if (strcmp(full, cfg_root) == 0)
        return -1;

    // mkdir with mode 0777 — exFAT ignores the mode, but newlib wants an arg.
    return mkdir(full, 0777) == 0 ? 0 : -1;
}

// Creates every missing component of rel under sdmc:/. Existing pieces are
// left alone, so calling this on an already-present path is a no-op. Used
// to create an upload destination that the user typed/navigated to but
// which does not exist yet.
int make_dirs(const char *rel){
    if (!rel || !rel[0]) return 0;   // nothing to create

    char full[768];
    if (join_and_check(rel, full, sizeof(full)) < 0)
        return -1;

    size_t len = strlen(full);
    for (size_t i = strlen(cfg_root); i < len; i++){
        if (full[i] == '/'){
            full[i] = 0;
            mkdir(full, 0777);       // ignore EEXIST and friends here
            full[i] = '/';
        }
    }

    if (mkdir(full, 0777) != 0 && errno != EEXIST)
        return -1;

    // EEXIST is only fine when the name really is a directory.
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;

    return 0;
}

// Renames or moves from_rel to to_rel, both relative to the root. An
// existing destination file is replaced (exFAT refuses a plain rename onto
// an existing name, so the target is removed and the rename retried).
int rename_path(const char *from_rel, const char *to_rel){
    if (!from_rel || !from_rel[0] || !to_rel || !to_rel[0]) return -1;

    char from[768];
    char to[768];
    if (join_and_check(from_rel, from, sizeof(from)) < 0) return -1;
    if (join_and_check(to_rel, to, sizeof(to)) < 0) return -1;

    // Never move or replace the root itself.
    if (strcmp(from, cfg_root) == 0 || strcmp(to, cfg_root) == 0) return -1;
    if (strcmp(from, to) == 0) return 0;   // nothing to do

    if (rename(from, to) == 0) return 0;

    // If the destination exists (and is a file), clear it and retry.
    struct stat st;
    if (stat(to, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) return -1;     // refuse to clobber a directory
    if (remove(to) != 0) return -1;
    return rename(from, to) == 0 ? 0 : -1;
}

int stat_file(const char *rel, char *full_out, size_t full_sz, long long *out_size){
    if (!rel || !rel[0]) return -1;

    if (join_and_check(rel, full_out, full_sz) < 0)
        return -1;

    struct stat st;
    if (stat(full_out, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) return -1;

    *out_size = (long long)st.st_size;
    return 0;
}