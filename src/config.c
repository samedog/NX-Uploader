/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// config.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

int  cfg_port = NXU_PORT;
int  cfg_read_only = 0;
int  cfg_allow_delete = 1;
int  cfg_log = 0;
char cfg_root[128] = "sdmc:/";
char cfg_upload_dir[256] = NXU_UPLOAD_DIR;
char cfg_auth_user[64] = "";
char cfg_auth_pass[64] = "";
char cfg_auth_header[320] = "";

static char *trim(char *s){
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = 0;
    return s;
}

// Stores the root, guaranteeing it ends with a single '/'.
static void set_root(const char *v){
    if (!v || !v[0]) return;
    size_t n = strlen(v);
    if (n > sizeof(cfg_root) - 2) n = sizeof(cfg_root) - 2;
    memcpy(cfg_root, v, n);
    cfg_root[n] = 0;
    if (n == 0 || cfg_root[n-1] != '/'){
        cfg_root[n] = '/';
        cfg_root[n+1] = 0;
    }
}

// Standard base64, used only to compare the Authorization header, so no
// decoder is needed.
static void base64_encode(const char *in, char *out, size_t out_sz){
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    size_t len = strlen(in);
    for (size_t i = 0; i < len && o + 4 < out_sz; i += 3){
        unsigned n = (unsigned char)in[i] << 16;
        if (i + 1 < len) n |= (unsigned char)in[i+1] << 8;
        if (i + 2 < len) n |= (unsigned char)in[i+2];
        out[o++] = tbl[(n >> 18) & 63];
        out[o++] = tbl[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[n & 63] : '=';
    }
    out[o] = 0;
}

void config_load(void){
    FILE *f = fopen(NXU_CONFIG_PATH, "r");
    if (!f) return;   // no config file: keep the defaults

    char line[256];
    while (fgets(line, sizeof(line), f)){
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;              // strip comments
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line);
        char *v = trim(eq + 1);
        if (k[0] == 0) continue;

        if      (!strcmp(k, "port"))        { int p = atoi(v); if (p > 0 && p < 65536) cfg_port = p; }
        else if (!strcmp(k, "read_only"))   cfg_read_only = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "allow_delete"))cfg_allow_delete = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "log"))         cfg_log = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "root"))        set_root(v);
        else if (!strcmp(k, "upload_dir"))  snprintf(cfg_upload_dir, sizeof(cfg_upload_dir), "%s", v);
        else if (!strcmp(k, "auth_user"))   snprintf(cfg_auth_user, sizeof(cfg_auth_user), "%s", v);
        else if (!strcmp(k, "auth_pass"))   snprintf(cfg_auth_pass, sizeof(cfg_auth_pass), "%s", v);
    }
    fclose(f);

    if (cfg_auth_user[0] && cfg_auth_pass[0]){
        char creds[128];
        snprintf(creds, sizeof(creds), "%s:%s", cfg_auth_user, cfg_auth_pass);
        char b64[256];
        base64_encode(creds, b64, sizeof(b64));
        snprintf(cfg_auth_header, sizeof(cfg_auth_header), "Basic %s", b64);
    }
}
