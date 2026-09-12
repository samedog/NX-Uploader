/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog" 
 */
// multipart.h
#pragma once
#include <stdio.h>

#define MAX_BOUNDARY   128
#define LOOKBACK_MAX   256
#define HEADER_ACCUM_MAX 2048
#define READ_CHUNK     (64 * 1024)

typedef enum {
    PS_PREAMBLE, PS_HEADERS, PS_DATA, PS_SKIP, PS_DONE, PS_ERROR
} ParseState;

typedef struct {
    ParseState state;
    char   mid[4 + MAX_BOUNDARY];
    int    midlen;
    char   first[3 + MAX_BOUNDARY];
    int    firstlen;
    char   closing[4 + MAX_BOUNDARY + 2];
    int    closinglen;
    char   lookback[LOOKBACK_MAX];
    int    lookback_len;
    char   hdr[HEADER_ACCUM_MAX];
    int    hdr_len;
    FILE  *out;
    char   out_path[512];
    int    files_saved;
    int    opened_a_file;
    char   dest_dir[512];
} MultipartParser;

void mp_init(MultipartParser *mp, const char *boundary);
int  mp_feed(MultipartParser *mp, const char *chunk, int chunk_len);
void mp_close_file(MultipartParser *mp);
void mp_abort(MultipartParser *mp);