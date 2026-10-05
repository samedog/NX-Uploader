/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
// multipart.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "app_state.h"
#include "config.h"
#include "multipart.h"

// Monotonic suffix so a burst of same-named parts never collide on their
// temp file names. Only the single server thread touches this.
static unsigned mp_tmp_seq = 0;


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

// Turns a browser-supplied filename into a path relative to the upload
// directory. A plain name stays as is; a relative path (from a folder
// upload) keeps its subdirectories. "." and ".." segments are dropped, and
// characters exFAT rejects are replaced. Falls back to "upload.bin" when
// nothing usable is left.
static void sanitize_relpath(char *name){
    char in[MAX_FILENAME];
    snprintf(in, sizeof(in), "%s", name);

    for (char *p = in; *p; p++){          // normalize separators
        if (*p == '\\') *p = '/';
    }

    char out[MAX_FILENAME];
    size_t o = 0;
    const char *seg = in;
    while (*seg){
        while (*seg == '/') seg++;
        if (!*seg) break;
        const char *end = strchr(seg, '/');
        size_t len = end ? (size_t)(end - seg) : strlen(seg);

        int drop = (len == 0) ||
                   (len == 1 && seg[0] == '.') ||
                   (len == 2 && seg[0] == '.' && seg[1] == '.');
        if (!drop){
            char segbuf[MAX_FILENAME];
            size_t l = len < sizeof(segbuf) - 1 ? len : sizeof(segbuf) - 1;
            memcpy(segbuf, seg, l);
            segbuf[l] = 0;
            for (char *q = segbuf; *q; q++){
                if (*q == ':' || *q == '*' || *q == '?' || *q == '"' ||
                    *q == '<' || *q == '>' || *q == '|' || (unsigned char)*q < 32){
                    *q = '_';
                }
            }
            if (o + l + 1 >= sizeof(out)){ o = 0; break; }   // too long, bail
            if (o) out[o++] = '/';
            memcpy(out + o, segbuf, l);
            o += l;
        }
        seg += len;
    }
    out[o] = 0;

    if (o == 0) snprintf(name, MAX_FILENAME, "upload.bin");
    else        memcpy(name, out, o + 1);
}

// Best-effort "mkdir -p" for an absolute path's directory part. Used to
// create the subdirectories a folder upload asks for.
static void make_parents(const char *full){
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", full);
    char *start = strchr(tmp, '/');       // skip the "sdmc:" device part
    if (!start) return;
    for (char *p = start + 1; *p; p++){
        if (*p == '/'){ *p = 0; mkdir(tmp, 0777); *p = '/'; }
    }
    mkdir(tmp, 0777);                     // and the directory itself
}

// Finds `needle` inside `hay`. Anchors on the first byte with memchr, which
// is much faster than a memcmp at every position when the needle is rare,
// as a multipart boundary is inside ordinary file data.
static int memfind(const char *hay, int haylen, const char *needle, int nlen){
    if (nlen <= 0 || haylen < nlen) return -1;
    const char *p = hay;
    int remaining = haylen;
    while (remaining >= nlen){
        const char *hit = memchr(p, (unsigned char)needle[0],
                                 (size_t)(remaining - nlen + 1));
        if (!hit) return -1;
        if (memcmp(hit, needle, (size_t)nlen) == 0) return (int)(hit - hay);
        int adv = (int)(hit - p) + 1;
        p += adv;
        remaining -= adv;
    }
    return -1;
}

static int mp_write_data(MultipartParser *mp, const char *buf, int len){
    if (len <= 0) return 0;
    if (mp->out){
        if (fwrite(buf, 1, (size_t)len, mp->out) != (size_t)len){
            if (mp->cur_slot >= 0 && mp->parts[mp->cur_slot].err == 0)
                mp->parts[mp->cur_slot].err = errno;
            return -1;
        }
    }
    return 0;
}

// Flushes, closes, and commits the current file. Returns 0 on success, or
// -1 if any step failed, in which case the partial temp file is removed
// and the part stays marked as failed. Safe to call with no file open, in
// which case it is a no-op returning 0.
int mp_close_file(MultipartParser *mp){
    if (!mp->out) return 0;

    FILE *f = mp->out;
    mp->out = NULL;

    int ok = 1;
    int keep_partial = 0;
    if (fflush(f) != 0) ok = 0;
    fsync(fileno(f));   // best effort: push bytes to the card, ignore if unsupported
    if (fclose(f) != 0) ok = 0;

    if (ok && mp->final_path[0]){
        if (rename(mp->out_path, mp->final_path) != 0){
            // libnx's exFAT device refuses to rename onto an existing name
            // (it maps the FS error to EEXIST), so an overwrite fails here.
            // Clear the old file and try once more.
            remove(mp->final_path);
            if (rename(mp->out_path, mp->final_path) != 0){
                // The retry failed after the old file was already gone. Keep
                // the temp so the uploaded data is not lost outright.
                keep_partial = 1;
                ok = 0;
            }
        }
    }

    if (!ok){
        if (!keep_partial && mp->out_path[0]) remove(mp->out_path);
        if (mp->cur_slot >= 0 && mp->parts[mp->cur_slot].err == 0)
            mp->parts[mp->cur_slot].err = errno;
        return -1;
    }

    if (mp->cur_slot >= 0) mp->parts[mp->cur_slot].ok = 1;
    mp->files_saved++;
    return 0;
}

// Closes the current file and deletes it from disk when the upload fails
// or the connection drops: the parser has already written the partial
// bytes to the temp path, and they should not be left behind. final_path
// is never touched here, so a cancelled overwrite leaves any existing file
// intact.
void mp_abort(MultipartParser *mp){
    if (mp->out){
        fclose(mp->out);
        mp->out = NULL;
    }
    if (mp->opened_a_file && mp->out_path[0]){
        remove(mp->out_path);
    }
    mp->opened_a_file = 0;
}


static int mp_process_headers(MultipartParser *mp){
    char fname[MAX_FILENAME];
    if (extract_quoted(mp->hdr, "filename=", fname, sizeof(fname)) == 0 && fname[0]){
        sanitize_relpath(fname);

        int slot = -1;
        if (mp->files_attempted < MAX_PART_LOG){
            slot = mp->files_attempted;
            snprintf(mp->parts[slot].name, MAX_PART_NAME, "%s", fname);
            mp->parts[slot].ok = 0;
            mp->parts[slot].err = 0;
        }
        mp->files_attempted++;

        char dir[520];
        snprintf(dir, sizeof(dir), "%s", mp->dest_dir);

        // A folder upload sends a relative path like "roms/gba/game.gba";
        // a plain upload sends just a name. Split off the directory part so
        // nested folders land under the destination, and so the temp file
        // sits beside the target instead of in a ".roms" lookalike.
        char sub[MAX_FILENAME] = "";
        char base[MAX_FILENAME];
        const char *slash = strrchr(fname, '/');
        if (slash){
            size_t n = (size_t)(slash - fname);
            if (n >= sizeof(sub)) n = sizeof(sub) - 1;
            memcpy(sub, fname, n);
            sub[n] = 0;
            snprintf(base, sizeof(base), "%s", slash + 1);
        } else {
            snprintf(base, sizeof(base), "%s", fname);
        }
        if (base[0] == 0) snprintf(base, sizeof(base), "upload.bin");

        char dirfull[1024];
        int dn = sub[0]
               ? snprintf(dirfull, sizeof(dirfull), "%s/%s", dir, sub)
               : snprintf(dirfull, sizeof(dirfull), "%s", dir);
        if (dn < 0 || (size_t)dn >= sizeof(dirfull)){
            mp->out = NULL;
            mp->cur_slot = -1;
            if (slot >= 0) mp->parts[slot].err = ENAMETOOLONG;
            return 0;
        }

        // Create any subdirectories the folder upload asked for.
        make_parents(dirfull);

        char path[1024];
        int pn = snprintf(path, sizeof(path), "%s/%s", dirfull, base);
        if (pn < 0 || (size_t)pn >= sizeof(path)){
            mp->out = NULL;
            mp->cur_slot = -1;
            if (slot >= 0) mp->parts[slot].err = ENAMETOOLONG;
            return 0;
        }

        // Stream into a temp file beside the destination. The temp is
        // renamed onto "path" only when the part completes, so a partial
        // or cancelled upload never shows up under the real name and never
        // clobbers an existing file.
        char tmp[1024];
        int tn = snprintf(tmp, sizeof(tmp), "%s/.%s.%u.part",
                          dirfull, base, ++mp_tmp_seq);
        if (tn < 0 || (size_t)tn >= sizeof(tmp)){
            mp->out = NULL;
            mp->cur_slot = -1;
            if (slot >= 0) mp->parts[slot].err = ENAMETOOLONG;
            return 0;
        }

        FILE *f = fopen(tmp, "wb");
        if (!f){
            mp->out = NULL;
            mp->cur_slot = -1;
            if (slot >= 0) mp->parts[slot].err = errno;
            return 0;
        }

        mp->out = f;
        mp->cur_slot = slot;
        snprintf(mp->out_path, sizeof(mp->out_path), "%s", tmp);
        snprintf(mp->final_path, sizeof(mp->final_path), "%s", path);
        mp->opened_a_file = 1;
        strncpy(g.current_name, fname, sizeof(g.current_name) - 1);
        g.current_name[sizeof(g.current_name) - 1] = 0;
    } else {
        mp->out = NULL;
        mp->cur_slot = -1;
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
                // The boundary is present but there are not yet enough bytes
                // after it to tell whether the body is empty ("--") or the
                // first part's CRLF. Hold the boundary itself (discarding any
                // preamble junk before it) so the next feed can decide.
                int start = pos - mp->firstlen;
                int keep = wlen - start;
                if (keep > LOOKBACK_MAX) return -1;
                memmove(mp->lookback, window + start, (size_t)keep);
                mp->lookback_len = keep;
                return 0;
            }
        }
        else if (mp->state == PS_HEADERS){
            int idx = memfind(window + pos, wlen - pos, "\r\n\r\n", 4);
            if (idx < 0){
                int avail = wlen - pos;

                // Hold the last few bytes in case "\r\n\r\n" straddles the
                // next chunk. Those bytes are NOT copied into hdr now; they
                // are re-examined (and copied once) on the next feed, so the
                // header block is never accumulated twice.
                int keep = 3;
                if (keep > avail) keep = avail;
                int copy = avail - keep;

                int space = HEADER_ACCUM_MAX - 1 - mp->hdr_len;
                if (copy > space) copy = space;
                if (copy > 0){
                    memcpy(mp->hdr + mp->hdr_len, window + pos, (size_t)copy);
                    mp->hdr_len += copy;
                }
                mp->hdr[mp->hdr_len] = 0;

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
                int after = pos + idx + mp->midlen;

                // Not enough bytes past the boundary to tell whether it is
                // the closing boundary or the start of the next part. Write
                // the file data up to the boundary, then hold the boundary
                // and any partial trailer for the next feed.
                if (after + 1 >= wlen){
                    if (mp_write_data(mp, window + pos, idx) != 0){
                        mp->state = PS_ERROR;
                        return -1;
                    }
                    pos += idx;
                    int keep = wlen - pos;
                    if (keep > LOOKBACK_MAX) keep = LOOKBACK_MAX;
                    memmove(mp->lookback, window + wlen - keep, (size_t)keep);
                    mp->lookback_len = keep;
                    return 0;
                }

                // Decide on the trailer before committing anything, so a
                // part that turns out malformed is never written out.
                if (window[after] == '-' && window[after + 1] == '-'){
                    // Closing boundary: this was the last part.
                    if (mp_write_data(mp, window + pos, idx) != 0 ||
                        mp_close_file(mp) != 0){
                        mp->state = PS_ERROR;
                        return -1;
                    }
                    mp->state = PS_DONE;
                    return 0;
                } else if (window[after] == '\r' && window[after + 1] == '\n'){
                    // Another part follows.
                    if (mp_write_data(mp, window + pos, idx) != 0 ||
                        mp_close_file(mp) != 0){
                        mp->state = PS_ERROR;
                        return -1;
                    }
                    pos = after + 2;
                    mp->state = PS_HEADERS;
                    mp->hdr_len = 0;
                } else {
                    // The byte after the boundary is neither CRLF nor "--",
                    // so this was not a real boundary. Fail without
                    // committing; mp_abort() removes the temp file.
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
    mp->cur_slot = -1;
    
    mp->firstlen = snprintf(mp->first, sizeof(mp->first), "--%s", boundary);
    mp->midlen   = snprintf(mp->mid,   sizeof(mp->mid),   "\r\n--%s", boundary);
    mp->closinglen = snprintf(mp->closing, sizeof(mp->closing),
                              "\r\n--%s--", boundary);
}
