#include <unistd.h>
#include <stdio.h>
#include <signal.h>

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

extern long mmap_raw(unsigned long addr, unsigned long len, int prot, int flags);
extern int  munmap_raw(unsigned long addr, unsigned long len);

static volatile int segv_seen;
static void segv_handler(int sig) { (void)sig; segv_seen = 1; exit(88); }

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    // Several pages, so demand paging is exercised more than once.
    unsigned long len = 4096 * 4;
    long a = mmap_raw(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (a < 0) { printf("[mmaptest] FAILED: mmap returned %d\n", (int)a); return 1; }

    volatile unsigned char *m = (volatile unsigned char *)(unsigned long)a;
    for (unsigned long i = 0; i < len; i += 4096) { m[i] = (unsigned char)(i / 4096 + 1); }
    for (unsigned long i = 0; i < len; i += 4096) {
        if (m[i] != (unsigned char)(i / 4096 + 1)) {
            printf("[mmaptest] FAILED: page %d readback\n", (int)(i / 4096));
            return 1;
        }
    }
    printf("[mmaptest] anonymous mmap + demand paging passed\n");

    if (munmap_raw((unsigned long)a, len) != 0) {
        printf("[mmaptest] FAILED: munmap\n"); return 1;
    }
    printf("[mmaptest] munmap passed\n");

    // A read-only mapping must fault on write -- proving the protection
    // check rather than just the happy path. Done in a child so the
    // parent can report.
    int child = fork();
    if (child == 0) {
        struct sigaction sa;
        sa.sa_handler = segv_handler; sa.sa_flags = 0; sa.sa_mask = 0;
        sigaction(SIGSEGV, &sa, 0);
        long r = mmap_raw(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS);
        if (r < 0) { exit(1); }
        *(volatile unsigned char *)(unsigned long)r = 1;   // must fault
        exit(2);
    }
    int code = wait(child);
    if (code != 88) {
        printf("[mmaptest] FAILED: write to PROT_READ gave %d, want 88\n", code);
        return 1;
    }
    printf("[mmaptest] PROT_READ write faults passed\n");

    // W^X: the program's own .text is mapped read-only + executable, so
    // writing through a code pointer must fault -- not silently succeed
    // as it did while elf_load mapped every segment writable. Done in a
    // child; the parent reports.
    int tchild = fork();
    if (tchild == 0) {
        struct sigaction sa;
        sa.sa_handler = segv_handler; sa.sa_flags = 0; sa.sa_mask = 0;
        sigaction(SIGSEGV, &sa, 0);
        *(volatile unsigned char *)(unsigned long)&main = 0x90;   // must fault
        exit(2);
    }
    int tcode = wait(tchild);
    if (tcode != 88) {
        printf("[mmaptest] FAILED: write to .text gave %d, want 88\n", tcode);
        return 1;
    }
    printf("[mmaptest] .text write faults passed\n");

    printf("[mmaptest] ALL PASSED\n");
    return 0;
}
