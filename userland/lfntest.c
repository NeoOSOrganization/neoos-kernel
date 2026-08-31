#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

static int failures;

static void fail(const char *what, int rc) {
    printf("[lfntest] FAILED: %s rc=%d\n", what, rc);
    failures++;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Placed on the image by the Makefile with mcopy, so this is a long
// name written by a REAL VFAT implementation -- the read path is being
// checked against something other than its own writer.
#define EXTERNAL_LONG "/A Long File Name.txt"

static void check_read_external(void) {
    struct stat s;
    int rc = stat(EXTERNAL_LONG, &s);
    if (rc != 0) { fail("stat on an mcopy-written long name", rc); return; }
    if (!S_ISREG(s.st_mode)) { fail("long-named entry is not a file", (int)s.st_mode); return; }

    int fd = open(EXTERNAL_LONG, O_RDONLY);
    if (fd < 0) { fail("open on an mcopy-written long name", fd); return; }
    char buf[64];
    for (int i = 0; i < 64; i++) { buf[i] = 0; }
    // Read exactly what stat said the file holds, rather than a
    // hardcoded length -- the content is set by the Makefile.
    int64_t n = read(fd, buf, (uint64_t)s.st_size);
    close(fd);
    if (n != s.st_size) { fail("read from the long-named file", (int)n); return; }
    if (buf[0] != 'a') { fail("contents wrong", buf[0]); return; }
    printf("[lfntest] read an externally-written long name passed\n");
}

// It must also LIST under its full name, not its ~1 alias.
static void check_listed(void) {
    DIR *d = opendir("/");
    if (!d) { fail("opendir(/)", -1); return; }
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (str_eq(e->d_name, "A Long File Name.txt")) { found = 1; break; }
    }
    closedir(d);
    if (!found) { fail("long name did not appear in the listing", 0); return; }
    printf("[lfntest] long name appears in readdir passed\n");
}

// The round trip that matters: NeoOS writes the name, NeoOS reads it.
static void check_create_roundtrip(void) {
    const char *name = "/a rather long file name with spaces.text";
    int fd = open(name, O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail("create with a long name", fd); return; }
    const char *msg = "roundtrip";
    if (write(fd, msg, 9) != 9) { fail("write to long-named file", -1); close(fd); return; }
    close(fd);

    // Reopen by the SAME long name.
    fd = open(name, O_RDONLY);
    if (fd < 0) { fail("reopen by long name", fd); return; }
    char buf[16];
    for (int i = 0; i < 16; i++) { buf[i] = 0; }
    int64_t n = read(fd, buf, 9);
    close(fd);
    if (n != 9 || !str_eq(buf, "roundtrip")) { fail("contents did not survive", (int)n); return; }

    // And it must list under the full name.
    DIR *d = opendir("/");
    int found = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != 0) {
        if (str_eq(e->d_name, "a rather long file name with spaces.text")) { found = 1; break; }
    }
    if (d) { closedir(d); }
    if (!found) { fail("created long name missing from the listing", 0); return; }
    printf("[lfntest] create/reopen/list round trip passed\n");
}

// Deleting must erase the LFN slots too. If it does not, the orphans
// get folded onto a later entry and the listing goes wrong.
static void check_unlink_erases_run(void) {
    const char *name = "/temporary long name to delete.dat";
    int fd = open(name, O_CREAT | O_RDWR);
    if (fd < 0) { fail("create for the unlink case", fd); return; }
    close(fd);

    if (unlink(name) != 0) { fail("unlink a long-named file", -1); return; }

    struct stat s;
    if (stat(name, &s) != -ENOENT) { fail("deleted long name still resolves", 0); return; }

    // No listing entry may carry the dead name, and none may be empty
    // (an orphaned LFN run folded onto a later entry shows up as one or
    // the other).
    DIR *d = opendir("/");
    if (!d) { fail("opendir after unlink", -1); return; }
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (str_eq(e->d_name, "temporary long name to delete.dat")) {
            fail("deleted name still listed", 0); closedir(d); return;
        }
        if (e->d_name[0] == '\0') { fail("empty name in listing after unlink", 0); closedir(d); return; }
    }
    closedir(d);
    printf("[lfntest] unlink erases the whole LFN run passed\n");
}

// A short name must still be stored as a plain 8.3 entry -- no LFN
// slots -- and must still work.
static void check_short_names_unaffected(void) {
    int fd = open("/SHORT.TXT", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail("create a short name", fd); return; }
    close(fd);
    struct stat s;
    if (stat("/SHORT.TXT", &s) != 0) { fail("stat a short name", -1); return; }
    // Case-insensitive lookup, as every FAT driver does.
    if (stat("/short.txt", &s) != 0) { fail("case-insensitive lookup of a short name", -1); return; }
    printf("[lfntest] short names and case-insensitive lookup passed\n");
}

static void check_long_dir(void) {
    const char *dir = "/a long directory name";
    if (mkdir(dir) != 0) { fail("mkdir with a long name", -1); return; }
    struct stat s;
    if (stat(dir, &s) != 0) { fail("stat the long-named directory", -1); return; }
    if (!S_ISDIR(s.st_mode)) { fail("long-named directory is not a directory", (int)s.st_mode); return; }

    // And a file inside it, reached by a long path.
    int fd = open("/a long directory name/inner file.txt", O_CREAT | O_RDWR);
    if (fd < 0) { fail("create inside a long-named directory", fd); return; }
    close(fd);
    if (stat("/a long directory name/inner file.txt", &s) != 0) {
        fail("stat a file inside a long-named directory", -1);
        return;
    }
    printf("[lfntest] long directory names passed\n");
}

int main(void) {
    check_read_external();
    check_listed();
    check_create_roundtrip();
    check_unlink_erases_run();
    check_short_names_unaffected();
    check_long_dir();

    if (failures) {
        printf("[lfntest] SOME CHECKS FAILED\n");
        return 1;
    }
    printf("[lfntest] ALL PASSED\n");
    return 0;
}
