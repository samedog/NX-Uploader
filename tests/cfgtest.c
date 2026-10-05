/* Host test for config.c parsing. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

int main(void){
    system("rm -rf sdmc: && mkdir -p 'sdmc:/switch'");
    FILE *f = fopen("sdmc:/switch/nxuploader.cfg", "w");
    fputs("# test config\nport=9000\nroot=sdmc:/switch\nupload_dir=sdmc:/incoming\nread_only=1\nallow_delete=0\nauth_user=me\nauth_pass=secret\n", f);
    fclose(f);

    int failures = 0;
    #define CHECK(c, m) do { int _c = (c); printf("%s: %s\n", _c ? "PASS" : "FAIL", m); if (!_c) failures++; } while (0)
    config_load();
    CHECK(cfg_port == 9000, "port parsed");
    CHECK(strcmp(cfg_root, "sdmc:/switch/") == 0, "root normalized with trailing slash");
    CHECK(strcmp(cfg_upload_dir, "sdmc:/incoming") == 0, "upload_dir parsed");
    CHECK(cfg_read_only == 1, "read_only parsed");
    CHECK(cfg_allow_delete == 0, "allow_delete parsed");
    CHECK(strcmp(cfg_auth_header, "Basic bWU6c2VjcmV0") == 0, "auth header base64 correct");

    system("rm -rf sdmc:");
    printf("\n%s (%d failures)\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", failures);
    return failures ? 1 : 0;
}
