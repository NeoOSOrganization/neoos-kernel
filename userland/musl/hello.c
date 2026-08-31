// The first program on NeoOS built against a REAL C library.
//
// Nothing here is NeoOS-aware: these are ordinary musl calls, and every
// one reaches the kernel through the shim in third_party/shim, which
// translates Linux's syscall numbers into NeoOS's own and reshapes the
// arguments of the calls whose NeoOS form differs.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>

static int failures;

static void fail(const char *what) {
    printf("[musltest] FAILED: %s\n", what);
    failures++;
}

int main(int argc, char **argv)
{
    // printf goes through musl's stdio, which writes ONLY via writev.
    printf("[musl] hello from musl on NeoOS\n");
    printf("[musl] argc=%d argv0=%s\n", argc, argc > 0 ? argv[0] : "(none)");
    if (argc < 1) { fail("argc"); }

    // musl's own string code.
    char buf[64];
    strcpy(buf, "strcpy");
    strcat(buf, "+strcat");
    if (strlen(buf) != 13) { fail("strlen"); }
    printf("[musl] %s len=%d\n", buf, (int)strlen(buf));

    // mallocng, over NeoOS's anonymous mmap and a SPLITTING mprotect.
    char *heap = malloc(4096);
    if (!heap) { fail("malloc"); }
    else {
        memset(heap, 'x', 4095);
        heap[4095] = 0;
        if (strlen(heap) != 4095) { fail("malloc/memset"); }
        printf("[musl] malloc+memset ok (%d bytes)\n", (int)strlen(heap));
        free(heap);
    }

    // stat, through the shim's path reshaping.
    struct stat st;
    if (stat("/HELLO.TXT", &st) != 0) { fail("stat"); }
    else {
        printf("[musl] stat /HELLO.TXT size=%d isreg=%d\n",
               (int)st.st_size, S_ISREG(st.st_mode) ? 1 : 0);
        if (st.st_size != 24 || !S_ISREG(st.st_mode)) { fail("stat fields"); }
    }

    // open/read through musl's fd layer.
    int fd = open("/HELLO.TXT", O_RDONLY);
    if (fd < 0) { fail("open"); }
    else {
        char fbuf[64];
        ssize_t n = read(fd, fbuf, sizeof(fbuf) - 1);
        close(fd);
        if (n <= 0) { fail("read"); }
        else { fbuf[n] = 0; printf("[musl] read %d bytes: %s", (int)n, fbuf); }
    }

    // A long name, written by NeoOS's VFAT layer and opened by musl.
    fd = open("/A Long File Name.txt", O_RDONLY);
    if (fd < 0) { fail("open a long name"); }
    else { close(fd); printf("[musl] opened a VFAT long name\n"); }

    // opendir/readdir over the Linux getdents64 records.
    DIR *d = opendir("/");
    if (!d) { fail("opendir"); }
    else {
        int entries = 0, saw_dir = 0, saw_long = 0;
        struct dirent *e;
        while ((e = readdir(d)) != 0) {
            entries++;
            if (e->d_type == DT_DIR) { saw_dir = 1; }
            if (!strcmp(e->d_name, "A Long File Name.txt")) { saw_long = 1; }
        }
        closedir(d);
        if (!entries || !saw_dir) { fail("readdir"); }
        if (!saw_long) { fail("readdir did not report the long name"); }
        printf("[musl] readdir listed %d entries, long name seen=%d\n", entries, saw_long);
    }

    // fopen/fgets: stdio's read path, buffering and all.
    FILE *f = fopen("/HELLO.TXT", "r");
    if (!f) { fail("fopen"); }
    else {
        char line[64];
        if (!fgets(line, sizeof(line), f)) { fail("fgets"); }
        else { printf("[musl] fgets: %s", line); }
        fclose(f);
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { fail("clock_gettime"); }
    else {
        printf("[musl] clock_gettime %d.%03ds since boot\n",
               (int)ts.tv_sec, (int)(ts.tv_nsec / 1000000));
    }

    printf("[musl] isatty(1)=%d getpid=%d\n", isatty(1), (int)getpid());

    if (failures) { printf("[musltest] SOME CHECKS FAILED\n"); return 1; }
    printf("[musltest] ALL PASSED\n");
    return 0;
}
