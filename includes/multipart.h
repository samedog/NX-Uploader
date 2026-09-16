/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Diego Cardenas "The Samedog"
 */
// multipart.h
//
// Streaming multipart/form-data parser.
//
// Feed bytes with mp_feed() as they arrive off the socket. The parser
// writes the body of each file part straight to disk as it goes, so a
// single file larger than RAM works fine. A small lookback window keeps
// boundary sequences that straddle two mp_feed() calls from being missed.
//
// Typical use:
//     MultipartParser mp;
//     mp_init(&mp, boundary);
//     snprintf(mp.dest_dir, sizeof(mp.dest_dir), "%s", relative_dir);
//     while (bytes remain) mp_feed(&mp, chunk, chunk_len);
//     if (mp.state == PS_DONE) mp_close_file(&mp);
//     else                     mp_abort(&mp);
#pragma once
#include <stdio.h>

#define MAX_BOUNDARY   128
#define LOOKBACK_MAX   256
#define HEADER_ACCUM_MAX 2048
#define READ_CHUNK     (64 * 1024)
#define MAX_PART_LOG 64
#define MAX_PART_NAME 128

typedef enum {
    PS_PREAMBLE, PS_HEADERS, PS_DATA, PS_SKIP, PS_DONE, PS_ERROR
} ParseState;

typedef struct {
    // --- caller-settable ---

    // Destination directory for uploaded files, relative to sdmc:/.
    // E.g. "switch/roms/gba". Empty string means fall back to the
    // compiled-in default (UPLOAD_DIR). Set between mp_init() and the
    // first mp_feed() call. Not validated here; the caller is
    // responsible for rejecting paths containing "..", ":", "\\", or a
    // leading "/".
    char   dest_dir[512];

    // --- parser state (read-only to callers) ---

    ParseState state;

    char   mid[4 + MAX_BOUNDARY];
    int    midlen;
    char   first[3 + MAX_BOUNDARY];
    int    firstlen;
    char   closing[4 + MAX_BOUNDARY + 2];
    int    closinglen;

    // Bytes held back from the previous mp_feed() call so a boundary
    // split across two chunks isn't missed.
    char   lookback[LOOKBACK_MAX];
    int    lookback_len;

    // Accumulator for the current part's headers.
    char   hdr[HEADER_ACCUM_MAX];
    int    hdr_len;

    // --- parser state, also useful to read after the parse ---

    // Currently open output file, or NULL between parts.
    FILE  *out;

    // Path of the file currently being written. Used by mp_abort() to
    // delete a partial file when the connection drops mid-upload.
    char   out_path[512];

    // Number of file parts fully written. A successful upload has this
    // > 0 and state == PS_DONE.
    int    files_saved;

    // Set to 1 once a file has been opened for the current part, so
    // mp_abort() knows whether out_path points at a real file worth
    // removing.
    int    opened_a_file;
    struct {
        char name[MAX_PART_NAME];
        int  ok;
    } parts[MAX_PART_LOG];
    int files_attempted;
} MultipartParser;

// Initializes the parser with boundary extracted from the request
// Content-Type header. The boundary should not include the leading "--".
// Zeroes all state including dest_dir, so set dest_dir afterwards if
// uploads go somewhere other than the default.
void mp_init(MultipartParser *mp, const char *boundary);

// Feeds a chunk of raw request body into the parser. Call repeatedly
// until the body is exhausted. File data is written to disk as it's
// parsed; the caller never buffers the whole upload.
//
// Returns 0 on success (including "need more data"), -1 on parse error.
// Check mp->state after the final call: PS_DONE means the closing
// boundary was seen and all files were flushed; anything else means the
// upload is incomplete or malformed and should call mp_abort().
int  mp_feed(MultipartParser *mp, const char *chunk, int chunk_len);

// Flushes and closes the current file, incrementing files_saved.
// Safe to call when no file is open (no-op). Called automatically by
// mp_feed() at each part boundary; callers only need this to explicitly
// finalize after PS_DONE is reached.
void mp_close_file(MultipartParser *mp);

// Closes the current file and deletes it from disk when the upload fails 
// or the connection drops: the parser has already written the partial 
// bytes to disk, and they should not be left behind. 
// After this call, the parser is in an unusable state until re-initialized.
void mp_abort(MultipartParser *mp);