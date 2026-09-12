/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
// server.c
#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>
#include "app_state.h"
#include "html.h"
#include "multipart.h"
#include "server.h"

#define PORT        8080
#define MAX_HEADERS 8192
#define MAX_UPLOAD_BYTES (32LL * 1024 * 1024 * 1024)


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

// value here may or may not be quoted
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

    if (strcmp(method, "GET") == 0){
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            strlen(HTML_FORM));
        send_all(fd, hdr, (size_t)n);
        send_str(fd, HTML_FORM);
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

        if (clen > MAX_UPLOAD_BYTES){
            if (wants_100){
                send_str(fd,
                    "HTTP/1.1 413 Payload Too Large\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n\r\n"
                    "File exceeds maximum size (32 GiB).\n");
                return;
            } else {
                char sink[8192];
                long long remaining = clen;
                while (remaining > 0){
                    size_t want = remaining < (long long)sizeof(sink) ? (size_t)remaining : sizeof(sink);
                    ssize_t n = recv(fd, sink, want, 0);
                    if (n <= 0) break;
                    remaining -= n;
                }
                send_str(fd,
                    "HTTP/1.1 413 Payload Too Large\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n\r\n"
                    "File exceeds maximum size.\n");
                return;
            }
        }

        if (wants_100) send_str(fd, "HTTP/1.1 100 Continue\r\n\r\n");

        char ctype[512];
        if (copy_header_value(headers, "Content-Type", ctype, sizeof(ctype)) != 0 ||
            !strstr(ctype, "multipart/form-data")){
            send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nMissing or invalid Content-Type.\n");
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
        MultipartParser mp;
        mp_init(&mp, boundary);
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

        long long processed = 0;

        if (already > 0){
            if (mp_feed(&mp, body_start, (int)already) != 0){
                mp_abort(&mp);
                free(buf);
                g.uploading = 0;
                send_str(fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n"
                             "Connection: close\r\n\r\nParser error.\n");
                return;
            }
            processed += already;
            g.bytes_received = (long)processed;
        }


        int aborted = 0;
        while (processed < clen){
            long long remaining = clen - processed;
            size_t want = remaining < READ_CHUNK ? (size_t)remaining : READ_CHUNK;

            ssize_t n = recv(fd, buf, want, 0);
            if (n <= 0){
                aborted = 1;
                break;
            }
            if (mp_feed(&mp, buf, (int)n) != 0){
                aborted = 1;
                break;
            }
            processed += n;
            g.bytes_received = (long)processed;
        }

        int success = 0;
        if (!aborted && mp.state == PS_DONE){
            success = 1;
        }

        if (!success){
            mp_abort(&mp);
        } else {
            mp_close_file(&mp);
            if (mp.files_saved > 0){
                g.uploads++;
                strncpy(g.last_name, g.current_name, sizeof(g.last_name) - 1);
                g.last_name[sizeof(g.last_name) - 1] = 0;
            }
        }

        free(buf);
        g.uploading = 0;

        if (success && mp.files_saved > 0){
            char resp[256];
            int rl = snprintf(resp, sizeof(resp),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                "Connection: close\r\n\r\nUploaded %d file(s).\n", mp.files_saved);
            send_all(fd, resp, (size_t)rl);
        } else if (aborted){
            // client disconnected
        } else {
            send_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                         "Connection: close\r\n\r\nUpload failed.\n");
        }
        return;
    }

    send_str(fd, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
}


static int open_listener(void){
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0){ close(s); return -1; }
    if (listen(s, 4) < 0){ close(s); return -1; }
    return s;
}

void server_thread(void *arg){
    (void)arg;

    int ls = open_listener();
    if (ls < 0){
        snprintf(g.status, sizeof(g.status), "bind() failed: %s", strerror(errno));
        g.running = 0;
        return;
    }

    snprintf(g.status, sizeof(g.status), "Listening on :%d", PORT);

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
        struct timeval rcvto = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

        handle_client(c);
        close(c);
    }

    close(ls);
    g.running = 0;
}
