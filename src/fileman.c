/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
// fileman.c
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FM_ROOT "sdmc:/"
#define FM_ROOT_LEN (sizeof(FM_ROOT) - 1)

// Returns 0 on success, -1 if the joined path escapes root.
// this normalizes and pass multiple checks
static int join_and_check(const char *rel, char *out, size_t out_sz)
{
    if (snprintf(out, out_sz, "%s%s", FM_ROOT, rel) >= (int)out_sz)
        return -1;

    char *segments[64];
    int nseg = 0;
    char *p = out + FM_ROOT_LEN;   // skip "sdmc:/"
    char *save = NULL;
    for (char *tok = strtok_r(p, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (nseg > 0) nseg--; continue; }
        if (nseg >= 64) return -1;
        segments[nseg++] = tok;
    }

    char tmp[1024];
    size_t off = 0;
    memcpy(tmp, FM_ROOT, FM_ROOT_LEN);
    off = FM_ROOT_LEN;
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

        struct stat st;
        int is_dir = (stat(child, &st) == 0 && S_ISDIR(st.st_mode));
        long long size = is_dir ? 0 : (long long)st.st_size;

        int n = snprintf(buf + off, buf_sz - off,
                         "%c\t%s\t%lld\n",
                         is_dir ? 'D' : 'F', ent->d_name, size);
        if (n < 0 || (size_t)n >= buf_sz - off) break;   // buffer full
        off += n;
    }
    closedir(d);
    return (int)off;
}

int delete_path(const char *rel){
    if (!rel || !rel[0]) return -1;

    char full[768];
    if (join_and_check(rel, full, sizeof(full)) < 0)
        return -1;

    if (strcmp(full, FM_ROOT) == 0)
        return -1;

    return remove(full) == 0 ? 0 : -1;
}


int make_dir(const char *rel){
    if (!rel || !rel[0]) return -1;   // refuse to create root

    char full[768];
    if (join_and_check(rel, full, sizeof(full)) < 0)
        return -1;

    // Don't try to mkdir the root itself.
    if (strcmp(full, FM_ROOT) == 0)
        return -1;

    // mkdir with mode 0777 — exFAT ignores the mode, but newlib wants an arg.
    return mkdir(full, 0777) == 0 ? 0 : -1;
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