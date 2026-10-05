/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// server.c
//
// Minimal HTTP/1.1 server. Two request types are handled:
//
//   GET  /            -> serves the embedded web UI (HTML_FORM)
//   POST /            -> dispatches on Content-Type:
//       text/plain        -> one-line command, e.g. "LIST switch" or
//                            "DEL switch/foo.nro". See the LIST branch
//                            below for the full verb list.
//       multipart/form-data -> file upload. Destination directory comes
//                            from the X-Upload-Dir header (relative to
//                            sdmc:/); falls back to sdmc:/switch/uploads.
//
// One connection at a time, no keep-alive, no pipelining. Requests are
// handled synchronously and the socket is closed after the response.
// This is deliberate: the only client is a browser on the LAN, and
// concurrent uploads aren't a use case worth the complexity (yet).

#include <switch.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>
#include "app_state.h"
#include "config.h"
#include "html.h"
#include "multipart.h"
#include "fileman.h"
#include "storage.h"
#include "server.h"

#define MAX_HEADERS 8192
#define MAX_UPLOAD_BYTES (32LL * 1024 * 1024 * 1024)

// send() on a TCP socket may write fewer bytes than requested. Loop
// until everything is out, or return -1 on error.
static int send_all(int fd, const void *buf, size_t len){
    size_t sent = 0;
    while (sent < len){
        ssize_t n = send(fd, (const char*)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void send_str(int fd, const char *s){
    send_all(fd, s, strlen(s));
}

// Appends one line per request when logging is turned on in the config file.
// Best effort: a failed open or write is ignored.
static void log_request(const char *method, const char *path){
    if (!cfg_log) return;
    FILE *f = fopen(NXU_LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s %s\n", method, path);
    fclose(f);
}

// Reads and discards up to `remaining` bytes from the socket. Used to
// swallow a body we are about to reject, so the client does not see the
// connection drop mid-write.
static void drain_body(int fd, long long remaining){
    char sink[8192];
    while (remaining > 0){
        size_t want = remaining < (long long)sizeof(sink) ? (size_t)remaining : sizeof(sink);
        ssize_t n = recv(fd, sink, want, 0);
        if (n <= 0) break;
        remaining -= n;
    }
}

// Locates the value of an HTTP header. Header names are matched
// case-insensitively. Returns a pointer into `headers` (not
// NUL-terminated at the value boundary, use copy_header_value for
// a clean string), or NULL if the header isn't present.
static const char *find_header(const char *headers, const char *name){
    size_t nlen = strlen(name);
    const char *line = headers;
    while (*line){
        const char *eol = strstr(line, "\r\n");
        if (!eol) eol = line + strlen(line);
        const char *colon = memchr(line, ':', (size_t)(eol - line));
        if (colon && (size_t)(colon - line) == nlen &&
            strncasecmp(line, name, nlen) == 0){
            const char *v = colon + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            return v;
        }
        if (!*eol) break;
        line = eol + 2;
    }
    return NULL;
}

// Same as find_header(), but copies the value into "out" and
// NUL-terminates it. Returns 0 on success, -1 if the header is missing.
static int copy_header_value(const char *headers, const char *name, char *out, size_t outsz){
    const char *v = find_header(headers, name);
    if (!v) return -1;
    const char *eol = strstr(v, "\r\n");
    if (!eol) eol = v + strlen(v);
    size_t n = (size_t)(eol - v);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, v, n);
    out[n] = 0;
    return 0;
}

// Extracts a parameter value from a header liek:
//   Content-Type: multipart/form-data; boundary=----WebKitFormBoundary...
//   Content-Type: multipart/form-data; boundary="----WebKitFormBoundary..."
// handles both  quoted and unquoted forms. Returns 0 on success,
// -1 if the key isn't found or the value is empty.
static int extract_token(const char *hay, const char *key, char *out, size_t outsz){
    const char *p = strstr(hay, key);
    if (!p) return -1;
    p += strlen(key);

    if (*p == '"'){
        p++;
        const char *q = strchr(p, '"');
        if (!q) return -1;
        size_t n = (size_t)(q - p);
        if (n >= outsz) n = outsz - 1;
        memcpy(out, p, n);
        out[n] = 0;
        return 0;
    }

    size_t i = 0;
    while (p[i] && p[i] != ';' && p[i] != ' ' && p[i] != '\t' &&
           p[i] != '\r' && p[i] != '\n'){
        if (i + 1 >= outsz) break;
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
    return (i > 0) ? 0 : -1;
}

// URL-decodes %XX sequences in place. "+" is NOT converted to space because of 
// encodeURIComponent.
static void url_decode(char *s){
    char *r = s, *w = s;
    while (*r){
        if (*r == '%' && r[1] && r[2]){
            int hi = r[1] >= '0' && r[1] <= '9' ? r[1] - '0'
                   : r[1] >= 'a' && r[1] <= 'f' ? r[1] - 'a' + 10
                   : r[1] >= 'A' && r[1] <= 'F' ? r[1] - 'A' + 10 : -1;
            int lo = r[2] >= '0' && r[2] <= '9' ? r[2] - '0'
                   : r[2] >= 'a' && r[2] <= 'f' ? r[2] - 'a' + 10
                   : r[2] >= 'A' && r[2] <= 'F' ? r[2] - 'A' + 10 : -1;
            if (hi < 0 || lo < 0){
                *w++ = *r++;
                continue;
            }
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}


// Handles GET /dl?path=<url-encoded-rel> (and HEAD). Streams the file back
// to the client, or just the headers when head_only is set.
static void handle_download(int fd, const char *query, int head_only){
    if (strncmp(query, "path=", 5) != 0){
        send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                     "Connection: close\r\n\r\nmissing path\n");
        return;
    }

    char rel[512];
    snprintf(rel, sizeof(rel), "%s", query + 5);
    url_decode(rel);

    char full[768];
    long long size = 0;
    if (stat_file(rel, full, sizeof(full), &size) < 0){
        send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                     "Connection: close\r\n\r\nfile not found\n");
        return;
    }

    FILE *f = fopen(full, "rb");
    if (!f){
        send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                     "Connection: close\r\n\r\ncannot open file\n");
        return;
    }

    const char *base = strrchr(full, '/');
    base = base ? base + 1 : full;

    // Filename escaping: strip anything that would break header syntax.
    // exFAT forbids these in real filenames but just in case.
    char safe_name[256];
    size_t si = 0;
    for (const char *p = base; *p && si < sizeof(safe_name) - 1; p++){
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 32) safe_name[si++] = '_';
        else safe_name[si++] = *p;
    }
    safe_name[si] = 0;

    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n\r\n",
        safe_name, size);
    send_all(fd, hdr, (size_t)hl);

    if (head_only){
        fclose(f);
        return;
    }

    char *buf = malloc(READ_CHUNK);
    if (!buf){
        fclose(f);
        return;
    }

    size_t n;
    while ((n = fread(buf, 1, READ_CHUNK, f)) > 0){
        if (send_all(fd, buf, n) != 0) break;
    }

    free(buf);
    fclose(f);
}

// Handles a single HTTP request end-to-end.
//
// Order matters:
//   1. Read headers into a fixed buffer. Bail if the request never
//      sends a blank line (no complete request).
//   2. Dispatch on method: GET serves the UI, POST does everything else.
//   3. For POST, the Content-Length check happens *before* the
//      Content-Type check, so an oversized body can be refused without
//      parsing headers we don't care about.
//   4. The Expect: 100-continue handshake must happen before any body
//      read. If we don't send "100 Continue" clients wait forever and time out.
//   5. Only then do we look at Content-Type to decide
//      text/plain (command) vs multipart/form-data (upload).
static void handle_client(int fd){
    char req[MAX_HEADERS];
    size_t got = 0;

    while (got < sizeof(req) - 1){
        ssize_t n = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0) return;
        got += (size_t)n;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }

    char *hdr_end = strstr(req, "\r\n\r\n");
    if (!hdr_end) return;
    *hdr_end = 0;
    const char *headers = req;

    char method[16] = {0}, path[256] = {0};
    sscanf(headers, "%15s %255s", method, path);

    log_request(method, path);

    // Optional HTTP Basic auth. config_load() precomputes the exact
    // "Basic <base64>" string, so this is a straight compare. The browser
    // prompts natively on the 401 for the page load and then reuses the
    // credentials on its later same-origin requests.
    if (cfg_auth_header[0]){
        char auth[320] = {0};
        int ok = (copy_header_value(headers, "Authorization", auth, sizeof(auth)) == 0)
                 && strcmp(auth, cfg_auth_header) == 0;
        if (!ok){
            if (strcmp(method, "POST") == 0){
                char cl[32];
                if (copy_header_value(headers, "Content-Length", cl, sizeof(cl)) == 0){
                    long long clen = atoll(cl);
                    size_t already = (size_t)(got - (hdr_end + 4 - req));
                    if (already > (size_t)clen) already = (size_t)clen;
                    drain_body(fd, clen - (long long)already);
                }
            }
            send_str(fd, "HTTP/1.1 401 Unauthorized\r\n"
                         "WWW-Authenticate: Basic realm=\"NX Uploader\"\r\n"
                         "Content-Length: 0\r\n"
                         "Connection: close\r\n\r\n");
            return;
        }
    }

    int is_head = (strcmp(method, "HEAD") == 0);
    if (strcmp(method, "GET") == 0 || is_head){
        // /dl?path=<url-encoded relative path> streams a file. For HEAD we
        // send the same headers without the body.
        if (strncmp(path, "/dl?", 4) == 0){
            handle_download(fd, path + 4, is_head);
            return;
        }

        // Browsers probe for a favicon on every load; answer 404 rather
        // than handing back the whole page.
        if (strcmp(path, "/favicon.ico") == 0){
            send_str(fd, "HTTP/1.1 404 Not Found\r\n"
                         "Content-Length: 0\r\n"
                         "Connection: close\r\n\r\n");
            return;
        }

        // Everything else: serve the UI.
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            strlen(HTML_FORM));
        send_all(fd, hdr, (size_t)n);
        if (!is_head) send_str(fd, HTML_FORM);
        return;
    }

    if (strcmp(method, "POST") == 0){
        char clen_s[32];
        if (copy_header_value(headers, "Content-Length", clen_s, sizeof(clen_s)) != 0){
            send_str(fd, "HTTP/1.1 411 Length Required\r\nConnection: close\r\n\r\n");
            return;
        }
        long long clen = atoll(clen_s);

        if (clen <= 0){
            send_str(fd, "HTTP/1.1 411 Length Required\r\nConnection: close\r\n\r\n");
            return;
        }

        int wants_100 = 0;
        {
            const char *e = find_header(headers, "Expect");
            if (e && strncasecmp(e, "100-continue", 12) == 0) wants_100 = 1;
        }

        // Bytes of the body that already arrived with the headers. The
        // drain paths below must not wait for these again.
        size_t body_already = (size_t)(got - ((hdr_end + 4) - req));
        if (body_already > (size_t)clen) body_already = (size_t)clen;

        if (clen > MAX_UPLOAD_BYTES){
            if (!wants_100) drain_body(fd, clen - (long long)body_already);
            send_str(fd,
                "HTTP/1.1 413 Payload Too Large\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n\r\n"
                "File exceeds maximum size (32 GiB).\n");
            return;
        }

        if (wants_100) send_str(fd, "HTTP/1.1 100 Continue\r\n\r\n");

        char ctype[512];
        if (copy_header_value(headers, "Content-Type", ctype, sizeof(ctype)) != 0){
            send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nMissing Content-Type.\n");
            return;
        }

        if (strncmp(ctype, "text/plain", 10) == 0){
            // Drain the (tiny) body into a local buffer.
            char cmd[600];
            size_t have = 0;

            char *body_start = hdr_end + 4;
            size_t already = (size_t)(got - (body_start - req));
            if (already > (size_t)clen) already = (size_t)clen;
            if (already > 0){
                size_t copy = already < sizeof(cmd) - 1 ? already : sizeof(cmd) - 1;
                memcpy(cmd, body_start, copy);
                have = copy;
            }

            long long got_body = (long long)already;
            while (got_body < clen && have < sizeof(cmd) - 1){
                ssize_t n = recv(fd, cmd + have, sizeof(cmd) - 1 - have, 0);
                if (n <= 0) break;
                have += (size_t)n;
                got_body += n;
            }
            cmd[have] = 0;

            // Parse: "<VERB> <arg>" where arg may be empty.
            char verb[16] = {0};
            char arg[512] = {0};
            sscanf(cmd, "%15s %511[^\r\n]", verb, arg);

            // Trim trailing whitespace from arg.
            size_t al = strlen(arg);
            while (al > 0 && (arg[al-1] == ' ' || arg[al-1] == '\t' ||
                              arg[al-1] == '\r' || arg[al-1] == '\n')){
                arg[--al] = 0;
            }

            if (strcmp(verb, "LIST") == 0){
                char listing[LIST_BUF_SIZE];
                int n = list_dir(arg, listing, sizeof(listing));
                if (n < 0){
                    send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\ncannot list directory\n");
                    return;
                }
                char hdr[256];
                int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n", n);
                send_all(fd, hdr, (size_t)hl);
                send_all(fd, listing, (size_t)n);
                return;
            }

            if (strcmp(verb, "DEL") == 0){
                if (cfg_read_only || !cfg_allow_delete){
                    send_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nDelete is disabled.\n");
                    return;
                }
                if (delete_path(arg) == 0){
                    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nok\n");
                } else {
                    send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\ncannot delete\n");
                }
                return;
            }


            if (strcmp(verb, "MKDIR") == 0){
                if (cfg_read_only){
                    send_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nRead-only mode.\n");
                    return;
                }
                if (arg[0] == 0){
                    send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nmissing folder name\n");
                    return;
                }
                if (make_dir(arg) == 0){
                    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nok\n");
                } else {
                    send_str(fd, "HTTP/1.1 409 Conflict\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\ncannot create folder\n");
                }
                return;
            }

            if (strcmp(verb, "REN") == 0){
                if (cfg_read_only){
                    send_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nRead-only mode.\n");
                    return;
                }
                char *tab = strchr(arg, '\t');
                if (!tab || tab == arg || tab[1] == 0){
                    send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nmissing rename target\n");
                    return;
                }
                *tab = 0;
                if (rename_path(arg, tab + 1) == 0){
                    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nok\n");
                } else {
                    send_str(fd, "HTTP/1.1 409 Conflict\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\ncannot rename\n");
                }
                return;
            }

            if (strcmp(verb, "FREE") == 0){
                long long fb = storage_free_bytes();
                long long tb = storage_total_bytes();
                char body[96];
                int bl = snprintf(body, sizeof(body), "%lld\t%lld\n", fb, tb);
                char hdr[256];
                int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n", bl);
                send_all(fd, hdr, (size_t)hl);
                send_all(fd, body, (size_t)bl);
                return;
            }

            send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nUnknown command.\n");
            return;
        }


        if (!strstr(ctype, "multipart/form-data")){
            send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nInvalid Content-Type.\n");
            return;
        }

        char boundary[MAX_BOUNDARY];
        if (extract_token(ctype, "boundary=", boundary, sizeof(boundary)) != 0){
            send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nMissing multipart boundary.\n");
            return;
        }

        char *body_start = hdr_end + 4;
        size_t already = (size_t)(got - (body_start - req));
        if (already > (size_t)clen) already = (size_t)clen;

        if (cfg_read_only){
            drain_body(fd, clen - (long long)body_already);
            send_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nRead-only mode: uploads are disabled.\n");
            return;
        }

        // Work out the absolute destination. The browser always sends
        // X-Upload-Dir (the folder it is showing, relative to the root);
        // scripts may omit it and get cfg_upload_dir instead.
        char dest_base[512];
        {
            char upload_dir[256] = {0};
            int has_dir = (copy_header_value(headers, "X-Upload-Dir", upload_dir,
                                             sizeof(upload_dir)) == 0);
            if (has_dir){
                // Reject anything that could escape the sandbox.
                if (upload_dir[0] == '/' || strstr(upload_dir, "..") ||
                    strchr(upload_dir, ':') || strchr(upload_dir, '\\')){
                    upload_dir[0] = 0;   // fall back to the root
                }
                snprintf(dest_base, sizeof(dest_base), "%s%s", cfg_root, upload_dir);
                if (make_dirs(upload_dir) != 0){
                    drain_body(fd, clen - (long long)body_already);
                    send_str(fd, "HTTP/1.1 409 Conflict\r\nContent-Type: text/plain\r\n"
                                 "Connection: close\r\n\r\nCannot create destination folder.\n");
                    return;
                }
            } else {
                snprintf(dest_base, sizeof(dest_base), "%s", cfg_upload_dir);
            }
        }

        // Refuse early when the card cannot possibly hold this body, so we
        // don't spend minutes pushing bytes straight into a full disk. Only
        // uploads land here, so a full card never blocks LIST or DEL.
        {
            long long freeb = storage_free_bytes();
            if (freeb >= 0 && clen > freeb){
                drain_body(fd, clen - (long long)body_already);
                send_str(fd, "HTTP/1.1 413 Payload Too Large\r\n"
                             "Content-Type: text/plain\r\n"
                             "Connection: close\r\n\r\n"
                             "Not enough space on the SD card.\n");
                return;
            }
        }

        MultipartParser mp;
        mp_init(&mp, boundary);
        snprintf(mp.dest_dir, sizeof(mp.dest_dir), "%s", dest_base);
        char *buf = malloc(READ_CHUNK);
        if (!buf){
            send_str(fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nOut of memory.\n");
            return;
        }

        g.bytes_expected = (long)clen;
        g.bytes_received = (long)already;
        g.uploading = 1;
        g.current_name[0] = 0;
        g.speed_bps = 0;

        long long processed = (long long)already;   // body bytes read off the socket
        int parse_error = 0;
        int aborted = 0;

        if (already > 0){
            if (mp_feed(&mp, body_start, (int)already) != 0)
                parse_error = 1;
            g.bytes_received = (long)processed;
        }

        while (!parse_error && processed < clen){
            long long remaining = clen - processed;
            size_t want = remaining < READ_CHUNK ? (size_t)remaining : READ_CHUNK;

            ssize_t n = recv(fd, buf, want, 0);
            if (n <= 0){
                aborted = 1;
                break;
            }
            processed += n;
            g.bytes_received = (long)processed;

            if (mp_feed(&mp, buf, (int)n) != 0){
                parse_error = 1;
                break;
            }
        }

        int success = (!parse_error && !aborted && mp.state == PS_DONE);

        if (!success){
            mp_abort(&mp);
        } else {
            // Commits the final part. A failure here is already recorded in
            // mp.parts[].ok, so the response body reports that file as failed.
            mp_close_file(&mp);
            if (mp.files_saved > 0){
                g.uploads += mp.files_saved;
                strncpy(g.last_name, g.current_name, sizeof(g.last_name) - 1);
                g.last_name[sizeof(g.last_name) - 1] = 0;
            }
        }

        free(buf);
        g.uploading = 0;

        if (parse_error){
            // The parser rejected the body: a bad boundary, or a part that
            // could not be committed. Drain what is left and answer, so the
            // browser shows a real message instead of a connection error.
            drain_body(fd, clen - processed);
            send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nUpload failed: the file could not be saved.\n");
            return;
        }
        if (aborted){
            // The client disconnected mid-body. There is no one to answer.
            return;
        }
        if (!success){
            send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nUpload failed: incomplete request.\n");
            return;
        }

        // Parsed to completion. Report the tally, listing any files that did
        // not land, and pick the status from how many actually saved.
        {
            char body[4096];
            int bl = 0;
            #define APPEND(...) do { \
                if (bl >= 0 && (size_t)bl < sizeof(body)){ \
                    int _n = snprintf(body + bl, sizeof(body) - bl, __VA_ARGS__); \
                    if (_n > 0) bl += _n; \
                } \
            } while (0)

            APPEND("Uploaded %d of %d file(s).\n",
                   mp.files_saved, mp.files_attempted);

            int logged = mp.files_attempted < MAX_PART_LOG
                       ? mp.files_attempted : MAX_PART_LOG;
            int failed = 0;
            for (int i = 0; i < logged; i++){
                if (!mp.parts[i].ok){
                    if (failed == 0) APPEND("Failed:\n");
                    if (mp.parts[i].err)
                        APPEND("  %s: %s\n", mp.parts[i].name,
                               strerror(mp.parts[i].err));
                    else
                        APPEND("  %s: not saved\n", mp.parts[i].name);
                    failed++;
                }
            }
            if (mp.files_attempted > MAX_PART_LOG){
                APPEND("(and %d more, log full)\n",
                       mp.files_attempted - MAX_PART_LOG);
            }

            #undef APPEND
            if (bl < 0) bl = 0;
            if ((size_t)bl >= sizeof(body)) bl = (int)sizeof(body) - 1;
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 %s\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                mp.files_saved > 0 ? "200 OK" : "400 Bad Request", bl);
            send_all(fd, hdr, (size_t)hl);
            send_all(fd, body, (size_t)bl);
        }
        return;
    }

    send_str(fd, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
}


// Binds the first free port in [NXU_PORT, NXU_PORT + 3] and reports which
// one it got via *out_port. Returns the listening socket, or -1 if every
// candidate was taken.
static int open_listener(int *out_port){
    for (int i = 0; i < 4; i++){
        int port = cfg_port + i;
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) continue;

        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = INADDR_ANY;

        if (bind(s, (struct sockaddr*)&a, sizeof(a)) == 0 && listen(s, 4) == 0){
            if (out_port) *out_port = port;
            return s;
        }
        close(s);
    }
    return -1;
}



void server_thread(void *arg){
    (void)arg;

    int port = cfg_port;
    int ls = open_listener(&port);
    if (ls < 0){
        snprintf(g.status, sizeof(g.status),
                 "bind() failed on ports %d..%d", cfg_port, cfg_port + 3);
        g.running = 0;
        return;
    }

    g.port = port;
    snprintf(g.status, sizeof(g.status), "Listening on :%d", port);

    while (!g.stop_requested){
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };

        int r = select(ls + 1, &rf, NULL, NULL, &tv);
        if (r <= 0) continue;

        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = accept(ls, (struct sockaddr*)&peer, &plen);
        if (c < 0) continue;

        // Bound both directions so a stalled peer can never pin the only
        // server thread forever. Without SO_SNDTIMEO a client that stops
        // reading a download blocks send() indefinitely.
        struct timeval rcvto = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));
        struct timeval sndto = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

        handle_client(c);
        close(c);
    }

    close(ls);
    g.running = 0;
}
