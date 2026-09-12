/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
// multipart.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_state.h"
#include "multipart.h"

#define UPLOAD_DIR "sdmc:/switch/uploads"
#define MAX_FILENAME 256

static int extract_quoted(const char *hay, const char *key, char *out, size_t outsz){
    const char *p = strstr(hay, key);
    if (!p) return -1;
    p += strlen(key);
    if (*p != '"') return -1;
    p++;
    const char *q = strchr(p, '"');
    if (!q) return -1;
    size_t n = (size_t)(q - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

static void sanitize_filename(char *name){
    char *slash = strrchr(name, '/');
    char *bslash = strrchr(name, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);

    if (name[0] == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0){
        strcpy(name, "upload.bin");
    }
    for (char *p = name; *p; p++){
        if (*p == ':' || *p == '*' || *p == '?' || *p == '"' ||
            *p == '<' || *p == '>' || *p == '|' || (unsigned char)*p < 32){
            *p = '_';
        }
    }
}

static int memfind(const char *hay, int haylen, const char *needle, int nlen){
    if (nlen <= 0 || haylen < nlen) return -1;
    for (int i = 0; i + nlen <= haylen; i++){
        if (memcmp(hay + i, needle, nlen) == 0) return i;
    }
    return -1;
}

static int mp_write_data(MultipartParser *mp, const char *buf, int len){
    if (len <= 0) return 0;
    if (mp->out){
        if (fwrite(buf, 1, (size_t)len, mp->out) != (size_t)len){
             //error
            return -1;
        }
    }
    return 0;
}

void mp_close_file(MultipartParser *mp){
    if (mp->out){
        fclose(mp->out);
        mp->out = NULL;
        mp->files_saved++;
    }
}

void mp_abort(MultipartParser *mp){
    if (mp->out){
        fclose(mp->out);
        mp->out = NULL;
    }
    if (mp->opened_a_file && mp->out_path[0]){
        remove(mp->out_path);
    }
}


static int mp_process_headers(MultipartParser *mp){
    char fname[MAX_FILENAME];
    if (extract_quoted(mp->hdr, "filename=", fname, sizeof(fname)) == 0 && fname[0]){
        sanitize_filename(fname);
        char dir[520];
        if (mp->dest_dir[0]){
            snprintf(dir, sizeof(dir), "sdmc:/%s", mp->dest_dir);
        } else {
            snprintf(dir, sizeof(dir), "%s", UPLOAD_DIR);
        }
        char path[800];
        int pn = snprintf(path, sizeof(path), "%s/%s", dir, fname);
        if (pn < 0 || (size_t)pn >= sizeof(path)){
            mp->out = NULL;
            return 0;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, fname);
        FILE *f = fopen(path, "wb");
        if (!f){
            mp->out = NULL;
            return 0;
        }
        mp->out = f;
        strncpy(mp->out_path, path, sizeof(mp->out_path) - 1);
        mp->out_path[sizeof(mp->out_path) - 1] = 0;
        mp->opened_a_file = 1;
        strncpy(g.current_name, fname, sizeof(g.current_name) - 1);
        g.current_name[sizeof(g.current_name) - 1] = 0;
    } else {
        mp->out = NULL;
    }
    return 0;
}


int mp_feed(MultipartParser *mp, const char *chunk, int chunk_len){
    static char window[LOOKBACK_MAX + READ_CHUNK + 8];
    int wlen = 0;
    if (mp->lookback_len > 0){
        memcpy(window, mp->lookback, (size_t)mp->lookback_len);
        wlen = mp->lookback_len;
    }
    if (chunk_len > 0){
        memcpy(window + wlen, chunk, (size_t)chunk_len);
        wlen += chunk_len;
    }

    int pos = 0;

    while (pos < wlen){
        if (mp->state == PS_PREAMBLE){
            int idx = memfind(window + pos, wlen - pos, mp->first, mp->firstlen);
            if (idx < 0){
                // incomplete "--BOUNDARY" in the rest, keep last firstlen-1 bytes as lookback.
                int keep = mp->firstlen - 1;
                if (keep > wlen - pos) keep = wlen - pos;
                memmove(mp->lookback, window + wlen - keep, (size_t)keep);
                mp->lookback_len = keep;
                return 0;
            }
            pos += idx + mp->firstlen;
            if (pos + 1 < wlen && window[pos] == '\r' && window[pos + 1] == '\n'){
                pos += 2;
                mp->state = PS_HEADERS;
                mp->hdr_len = 0;
            } else if (pos + 1 < wlen && window[pos] == '-' && window[pos + 1] == '-'){
                mp->state = PS_DONE;
                return 0;
            } else {

                pos -= 0;
                int keep = wlen - pos;
                if (keep > LOOKBACK_MAX){
                    return -1;
                }
                memmove(mp->lookback, window + pos, (size_t)keep);
                mp->lookback_len = keep;
                return 0;
            }
        }
        else if (mp->state == PS_HEADERS){
            int idx = memfind(window + pos, wlen - pos, "\r\n\r\n", 4);
            if (idx < 0){
                int avail = wlen - pos;
                int space = HEADER_ACCUM_MAX - 1 - mp->hdr_len;
                int copy = avail < space ? avail : space;
                if (copy > 0){
                    memcpy(mp->hdr + mp->hdr_len, window + pos, (size_t)copy);
                    mp->hdr_len += copy;
                }
                mp->hdr[mp->hdr_len] = 0;

                int keep = 3;
                if (keep > wlen) keep = wlen;
                memmove(mp->lookback, window + wlen - keep, (size_t)keep);
                mp->lookback_len = keep;
                return 0;
            }
            int copy = idx;
            if (mp->hdr_len + copy < HEADER_ACCUM_MAX){
                memcpy(mp->hdr + mp->hdr_len, window + pos, (size_t)copy);
                mp->hdr_len += copy;
            }
            mp->hdr[mp->hdr_len < HEADER_ACCUM_MAX ? mp->hdr_len : HEADER_ACCUM_MAX - 1] = 0;

            if (mp_process_headers(mp) != 0){
                mp->state = PS_ERROR;
                return -1;
            }
            pos += idx + 4;
            mp->state = mp->out ? PS_DATA : PS_SKIP;
        }
        else if (mp->state == PS_DATA || mp->state == PS_SKIP){
            int idx = memfind(window + pos, wlen - pos, mp->mid, mp->midlen);
            if (idx >= 0){
                if (mp_write_data(mp, window + pos, idx) != 0){
                    mp->state = PS_ERROR;
                    return -1;
                }
                mp_close_file(mp);
                pos += idx + mp->midlen;

                if (pos + 1 < wlen && window[pos] == '-' && window[pos + 1] == '-'){
                    mp->state = PS_DONE;
                    return 0;
                } else if (pos + 1 < wlen && window[pos] == '\r' && window[pos + 1] == '\n'){
                    pos += 2;
                    mp->state = PS_PREAMBLE;
                    mp->hdr_len = 0;
                } else if (pos == wlen || pos + 1 == wlen){
                    int keep = wlen - pos;
                    memmove(mp->lookback, window + pos, (size_t)keep);
                    mp->lookback_len = keep;
                    return 0;
                } else {
                    // malformed
                    mp->state = PS_ERROR;
                    return -1;
                }
            } else {
                int safe = wlen - pos - (mp->midlen - 1);
                if (safe > 0){
                    if (mp_write_data(mp, window + pos, safe) != 0){
                        mp->state = PS_ERROR;
                        return -1;
                    }
                    pos += safe;
                }
                int keep = wlen - pos;
                if (keep > LOOKBACK_MAX) keep = LOOKBACK_MAX;
                memmove(mp->lookback, window + wlen - keep, (size_t)keep);
                mp->lookback_len = keep;
                return 0;
            }
        }
        else {
            // DONE or ERROR
            return mp->state == PS_ERROR ? -1 : 0;
        }
    }
    mp->lookback_len = 0;
    return 0;
}

void mp_init(MultipartParser *mp, const char *boundary){
    memset(mp, 0, sizeof(*mp));
    mp->state = PS_PREAMBLE;
    
    mp->firstlen = snprintf(mp->first, sizeof(mp->first), "--%s", boundary);
    mp->midlen   = snprintf(mp->mid,   sizeof(mp->mid),   "\r\n--%s", boundary);
    mp->closinglen = snprintf(mp->closing, sizeof(mp->closing),
                              "\r\n--%s--", boundary);
}
