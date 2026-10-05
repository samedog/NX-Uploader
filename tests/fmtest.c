/* Host test for fileman.c: sandboxing, listing, make_dirs, recursive delete,
 * and rename. Links src/config.c for the root default. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "fileman.h"
#include "config.h"

static int isdir(const char *p){ struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int isfile(const char *p){ struct stat st; return stat(p, &st) == 0 && !S_ISDIR(st.st_mode); }

int main(void){
    system("rm -rf sdmc: && mkdir -p sdmc:");
    int failures = 0;
    #define CHECK(c, m) do { int _c = (c); printf("%s: %s\n", _c ? "PASS" : "FAIL", m); if (!_c) failures++; } while (0)

    CHECK(strcmp(cfg_root, "sdmc:/") == 0, "default root is sdmc:/");

    CHECK(make_dirs("a/b/c") == 0, "make_dirs creates nested paths");
    CHECK(isdir("sdmc:/a/b/c"), "nested dir created");
    CHECK(make_dirs("a/b/c") == 0, "make_dirs is idempotent");
    CHECK(make_dirs("") == 0, "make_dirs on empty string is a no-op");
    { FILE *f = fopen("sdmc:/blocker", "wb"); if (f){ fputc('x', f); fclose(f); } }
    CHECK(make_dirs("blocker/sub") != 0, "make_dirs fails when a component is a file");

    { FILE *f = fopen("sdmc:/a/hello.txt", "wb"); if (f){ fwrite("hi", 1, 2, f); fclose(f); } }
    char buf[4096];
    int n = list_dir("a", buf, sizeof(buf));
    CHECK(n > 0, "list_dir returns data");
    CHECK(strstr(buf, "F\thello.txt\t2\n") != NULL, "list_dir shows the file with its size");
    CHECK(strstr(buf, "D\tb\t0\n") != NULL, "list_dir shows the subdirectory");

    CHECK(rename_path("a/hello.txt", "a/renamed.txt") == 0, "rename_path renames a file");
    CHECK(isfile("sdmc:/a/renamed.txt") && !isfile("sdmc:/a/hello.txt"), "rename moved the file");
    CHECK(rename_path("a/renamed.txt", "a/b/moved.txt") == 0, "rename_path moves into a subdir");
    CHECK(isfile("sdmc:/a/b/moved.txt"), "moved file is at the new path");
    CHECK(rename_path("../escape", "x") != 0, "rename fails when the source is missing");
    { FILE *f = fopen("sdmc:/a/target.txt", "wb"); if (f){ fwrite("OLD", 1, 3, f); fclose(f); } }
    CHECK(rename_path("a/b/moved.txt", "a/target.txt") == 0, "rename replaces an existing file");
    { char c[16] = {0}; FILE *f = fopen("sdmc:/a/target.txt", "rb"); if (f){ fread(c, 1, 15, f); fclose(f); } CHECK(strcmp(c, "hi") == 0, "replaced file holds the source content"); }

    make_dirs("tree/sub/deep");
    { FILE *f = fopen("sdmc:/tree/f1", "wb"); if (f){ fwrite("1", 1, 1, f); fclose(f); } }
    { FILE *f = fopen("sdmc:/tree/sub/f2", "wb"); if (f){ fwrite("2", 1, 1, f); fclose(f); } }
    { FILE *f = fopen("sdmc:/tree/sub/deep/f3", "wb"); if (f){ fwrite("3", 1, 1, f); fclose(f); } }
    CHECK(delete_path("tree") == 0, "delete_path removes a non-empty tree");
    CHECK(!isdir("sdmc:/tree"), "tree is gone");
    CHECK(delete_path("") != 0, "delete_path refuses empty path / root");
    CHECK(delete_path("nope") != 0, "delete_path fails on a missing path");

    system("rm -rf sdmc:");
    printf("\n%s (%d failures)\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", failures);
    return failures ? 1 : 0;
}
