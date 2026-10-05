/* Host-side test for the streaming multipart parser. Feeds a body one byte
 * at a time so boundaries straddle chunk edges, then checks the temp+rename
 * commit, abort cleanup, overwrite protection, and malformed handling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include "app_state.h"
#include "multipart.h"

AppState g;   /* multipart.c reads g.current_name */

/* Rename shim, used only when multipart.c is built with
 * -Drename=test_rename_multipart, to mimic libnx's exFAT device refusing to
 * rename onto an existing name. Exercises the remove-and-retry path. */
int test_rename_multipart(const char *from, const char *to){
    struct stat st;
    if (stat(to, &st) == 0){ errno = EEXIST; return -1; }
    return rename(from, to);
}

static int failures = 0;
static void check(int cond, const char *msg){
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) failures++;
}
static void read_file(const char *p, char *out, size_t n){
    FILE *f = fopen(p, "rb");
    out[0] = 0;
    if (!f) return;
    size_t r = fread(out, 1, n - 1, f);
    out[r] = 0;
    fclose(f);
}
static int file_exists(const char *p){
    struct stat st; return stat(p, &st) == 0;
}
static int count_part_files(void){
    DIR *d = opendir("sdmc:/switch/uploads");
    if (!d) return -1;
    int c = 0; struct dirent *e;
    while ((e = readdir(d))){
        size_t l = strlen(e->d_name);
        if (l > 5 && strcmp(e->d_name + l - 5, ".part") == 0) c++;
    }
    closedir(d);
    return c;
}

int main(void){
    char buf[128];

    /* ---- Test 1: two files, fed one byte at a time ---- */
    system("rm -rf sdmc: && mkdir -p 'sdmc:/switch/uploads'");
    const char *body =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"hello.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "HELLO WORLD\r\n"
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"sub\\b.txt\"\r\n"
        "\r\n"
        "SECOND PART\r\n"
        "--BOUND--\r\n";

    MultipartParser mp;
    mp_init(&mp, "BOUND");
    snprintf(mp.dest_dir, sizeof(mp.dest_dir), "%s", "sdmc:/switch/uploads");
    int rc = 0;
    for (size_t i = 0; body[i]; i++){
        rc = mp_feed(&mp, body + i, 1);
        if (rc != 0) break;
    }
    check(rc == 0, "byte-by-byte feed returns 0");
    check(mp.state == PS_DONE, "state is PS_DONE after complete body");
    if (mp.state == PS_DONE) mp_close_file(&mp);
    check(mp.files_saved == 2, "two files saved");
    check(mp.files_attempted == 2, "two files attempted");
    check(mp.parts[0].ok && mp.parts[1].ok, "both parts marked ok");
    read_file("sdmc:/switch/uploads/hello.txt", buf, sizeof(buf));
    check(strcmp(buf, "HELLO WORLD") == 0, "hello.txt content correct");
    read_file("sdmc:/switch/uploads/sub/b.txt", buf, sizeof(buf));
    check(strcmp(buf, "SECOND PART") == 0, "nested part kept its subdirectory");
    check(count_part_files() == 0, "no .part files left after success");

    /* ---- Test 2: aborted overwrite must not destroy the original ---- */
    system("rm -rf sdmc: && mkdir -p 'sdmc:/switch/uploads'");
    { FILE *f = fopen("sdmc:/switch/uploads/keep.txt", "wb"); fwrite("ORIGINAL", 1, 8, f); fclose(f); }

    MultipartParser mp2;
    mp_init(&mp2, "BOUND");
    snprintf(mp2.dest_dir, sizeof(mp2.dest_dir), "%s", "sdmc:/switch/uploads");
    const char *partial =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"keep.txt\"\r\n"
        "\r\n"
        "NEW DATA THAT NEVER FINISHES";
    rc = mp_feed(&mp2, partial, (int)strlen(partial));
    check(rc == 0, "partial feed returns 0");
    check(mp2.state != PS_DONE, "state not DONE on truncated upload");
    mp_abort(&mp2);
    read_file("sdmc:/switch/uploads/keep.txt", buf, sizeof(buf));
    check(strcmp(buf, "ORIGINAL") == 0, "existing file untouched after aborted overwrite");
    check(count_part_files() == 0, "no temp file left after abort");

    /* ---- Test 3: malformed boundary trailer must not commit ---- */
    system("rm -rf sdmc: && mkdir -p 'sdmc:/switch/uploads'");
    MultipartParser mp3;
    mp_init(&mp3, "BOUND");
    snprintf(mp3.dest_dir, sizeof(mp3.dest_dir), "%s", "sdmc:/switch/uploads");
    const char *malformed =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"bad.txt\"\r\n"
        "\r\n"
        "DATA\r\n--BOUNDX nope";
    rc = mp_feed(&mp3, malformed, (int)strlen(malformed));
    check(rc != 0, "malformed trailer returns -1");
    check(mp3.state == PS_ERROR, "state is PS_ERROR for malformed trailer");
    mp_abort(&mp3);
    check(!file_exists("sdmc:/switch/uploads/bad.txt"), "malformed part not committed");
    check(count_part_files() == 0, "no temp left after malformed part");

    /* ---- Test 4: overwrite an existing file (exercises remove-and-retry) ---- */
    system("rm -rf sdmc: && mkdir -p 'sdmc:/switch/uploads'");
    { FILE *f = fopen("sdmc:/switch/uploads/dup.txt", "wb"); fwrite("OLD CONTENT", 1, 11, f); fclose(f); }
    const char *dupbody =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"dup.txt\"\r\n"
        "\r\n"
        "BRAND NEW\r\n"
        "--BOUND--\r\n";
    MultipartParser mp4;
    mp_init(&mp4, "BOUND");
    snprintf(mp4.dest_dir, sizeof(mp4.dest_dir), "%s", "sdmc:/switch/uploads");
    rc = mp_feed(&mp4, dupbody, (int)strlen(dupbody));
    check(rc == 0, "overwrite feed returns 0");
    check(mp4.state == PS_DONE, "overwrite state is PS_DONE");
    if (mp4.state == PS_DONE) mp_close_file(&mp4);
    check(mp4.files_saved == 1, "overwrite counted one file saved");
    check(mp4.parts[0].ok, "overwritten part marked ok");
    read_file("sdmc:/switch/uploads/dup.txt", buf, sizeof(buf));
    check(strcmp(buf, "BRAND NEW") == 0, "existing file replaced with new content");
    check(count_part_files() == 0, "no temp left after overwrite");

    /* ---- Test 5: nested relative path (folder upload) ---- */
    system("rm -rf sdmc: && mkdir -p 'sdmc:/switch/uploads'");
    const char *foldbody =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"roms/gba/game.gba\"\r\n"
        "\r\n"
        "ROMDATA\r\n"
        "--BOUND--\r\n";
    MultipartParser mp5;
    mp_init(&mp5, "BOUND");
    snprintf(mp5.dest_dir, sizeof(mp5.dest_dir), "%s", "sdmc:/switch/uploads");
    rc = mp_feed(&mp5, foldbody, (int)strlen(foldbody));
    check(rc == 0, "folder upload feed returns 0");
    check(mp5.state == PS_DONE, "folder upload state is PS_DONE");
    if (mp5.state == PS_DONE) mp_close_file(&mp5);
    check(mp5.files_saved == 1, "folder upload saved one file");
    read_file("sdmc:/switch/uploads/roms/gba/game.gba", buf, sizeof(buf));
    check(strcmp(buf, "ROMDATA") == 0, "nested file landed in the right subdirectory");

    /* ---- Test 6: traversal in the filename is neutralized ---- */
    system("rm -rf sdmc: && mkdir -p 'sdmc:/switch/uploads'");
    const char *evilbody =
        "--BOUND\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"../../escaped.txt\"\r\n"
        "\r\n"
        "NOPE\r\n"
        "--BOUND--\r\n";
    MultipartParser mp6;
    mp_init(&mp6, "BOUND");
    snprintf(mp6.dest_dir, sizeof(mp6.dest_dir), "%s", "sdmc:/switch/uploads");
    rc = mp_feed(&mp6, evilbody, (int)strlen(evilbody));
    if (mp6.state == PS_DONE) mp_close_file(&mp6);
    check(!file_exists("sdmc:/escaped.txt"), "path traversal did not escape the destination");
    check(file_exists("sdmc:/switch/uploads/escaped.txt"), "traversal file stayed under the destination");

    system("rm -rf sdmc:");
    printf("\n%s (%d failures)\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", failures);
    return failures ? 1 : 0;
}
