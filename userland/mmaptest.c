#include <unistd.h>
#include <stdio.h>
#include <signal.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

// Must match STALE_PROBE_ADDR in userland/exec_target.c.
#define STALE_PROBE_ADDR 0x500000000000UL

extern long mmap_raw(unsigned long addr, unsigned long len, int prot, int flags);
extern int  munmap_raw(unsigned long addr, unsigned long len);
extern int  mprotect(void *addr, unsigned long length, int prot);

static volatile int segv_seen;
static void segv_handler(int sig) { (void)sig; segv_seen = 1; exit(88); }

// fork must hand the child the address-space BOOKKEEPING, not just the
// page tables. Duplicating the PTEs alone gives the child every page the
// parent had already touched and nothing else, so:
//   - a fault on an mmap'd page the parent never touched finds no vma
//     and becomes SIGSEGV;
//   - the child's mmap cursor restarts at the bottom of the mmap range
//     and can hand back an address the child already holds;
//   - munmap and mprotect on an inherited region silently do nothing.
// A shell hits all three immediately: it forks per command, and the
// allocator gets its heap from mmap.
static int check_fork_inherits_mappings(void) {
    unsigned long len = 4096 * 4;
    long a = mmap_raw(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (a < 0) { printf("[mmaptest] FAILED: mmap for the fork test\n"); return 0; }
    volatile unsigned char *m = (volatile unsigned char *)(unsigned long)a;
    m[0] = 0x11;                       // ONLY the first page is touched here

    int child = fork();
    if (child < 0) { printf("[mmaptest] FAILED: fork\n"); return 0; }
    if (child == 0) {
        if (m[0] != 0x11) { exit(60); }             // inherited page kept its bytes
        m[4096] = 0x22;                             // untouched page: needs the vma
        if (m[4096] != 0x22) { exit(61); }
        m[len - 1] = 0x33;                          // and the last one
        if (m[len - 1] != 0x33) { exit(62); }

        // The mmap cursor came along too, so a fresh mapping lands past
        // everything inherited rather than on top of it.
        long n = mmap_raw(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        if (n < 0) { exit(63); }
        if (n < a + (long)len) { exit(64); }
        *(volatile unsigned char *)(unsigned long)n = 0x44;
        if (m[0] != 0x11) { exit(65); }             // ...and did not alias us

        // mprotect and munmap of an inherited region are real operations.
        if (mprotect((void *)(unsigned long)a, len, PROT_READ) != 0) { exit(66); }
        if (m[0] != 0x11) { exit(67); }
        if (munmap_raw((unsigned long)a, len) != 0) { exit(68); }
        exit(0);
    }

    int code = wait(child);
    if (code != 0) {
        printf("[mmaptest] FAILED: fork mapping inheritance, child exited %d\n", code);
        return 0;
    }
    // The parent's own copy is untouched by anything the child did.
    if (m[0] != 0x11) {
        printf("[mmaptest] FAILED: child's writes reached the parent\n");
        return 0;
    }
    munmap_raw((unsigned long)a, len);
    printf("[mmaptest] fork inherits the vma list passed\n");
    return 1;
}

// The other half of the same defect: exec frees the old address space,
// so it must forget the mappings that described it. A stale vma would
// have the fault handler answer a dead address with a fresh zero page
// instead of the SIGSEGV the new image expects. EXECTARG does the
// probing -- it is the one running in the new address space.
static int check_exec_forgets_mappings(void) {
    int child = fork();
    if (child < 0) { printf("[mmaptest] FAILED: fork for the exec test\n"); return 0; }
    if (child == 0) {
        long a = mmap_raw(STALE_PROBE_ADDR, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
        if (a < 0 || (unsigned long)a != STALE_PROBE_ADDR) { exit(50); }
        *(volatile unsigned char *)STALE_PROBE_ADDR = 0x77;   // populate it
        exec("/BIN/EXECTARG.ELF");                            // must not return
        exit(51);
    }
    int code = wait(child);
    if (code != 0) {
        printf("[mmaptest] FAILED: exec left a stale mapping, child exited %d\n", code);
        return 0;
    }
    printf("[mmaptest] exec forgets the old mappings passed\n");
    return 1;
}

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

    if (!check_fork_inherits_mappings()) { return 1; }
    if (!check_exec_forgets_mappings()) { return 1; }

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

    // W^X: the kernel refuses a mapping that is writable AND executable.
    long wx = mmap_raw(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS);
    if (wx >= 0) {
        printf("[mmaptest] FAILED: W+X mmap was allowed (%ld)\n", wx);
        return 1;
    }
    long rw = mmap_raw(0, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS);
    if (rw < 0) { printf("[mmaptest] FAILED: plain RW mmap\n"); return 1; }
    if (mprotect((void *)(unsigned long)rw, 4096,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != -1) {
        printf("[mmaptest] FAILED: mprotect to W+X was allowed\n");
        return 1;
    }
    munmap_raw((unsigned long)rw, 4096);
    printf("[mmaptest] W^X mmap/mprotect rejection passed\n");

    // mprotect must not DESTROY the range it reprotects. Making a
    // written page read-only used to unmap it and free the frame, so
    // the data came back as zeroes -- the one thing mprotect may never
    // do. Both directions are checked: RW -> RO keeps the bytes, and
    // RO -> RW keeps them and makes the page writable again.
    unsigned long plen = 4096 * 3;
    long pa = mmap_raw(0, plen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (pa < 0) { printf("[mmaptest] FAILED: mmap for mprotect test\n"); return 1; }
    volatile unsigned char *pm = (volatile unsigned char *)(unsigned long)pa;
    for (unsigned long i = 0; i < plen; i++) { pm[i] = (unsigned char)(i * 7 + 3); }

    if (mprotect((void *)(unsigned long)pa, plen, PROT_READ) != 0) {
        printf("[mmaptest] FAILED: mprotect to PROT_READ\n"); return 1;
    }
    for (unsigned long i = 0; i < plen; i++) {
        if (pm[i] != (unsigned char)(i * 7 + 3)) {
            printf("[mmaptest] FAILED: PROT_READ lost byte %d\n", (int)i);
            return 1;
        }
    }
    printf("[mmaptest] mprotect PROT_READ preserves contents passed\n");

    // ...and it really is read-only now, not merely still readable.
    int rochild = fork();
    if (rochild == 0) {
        struct sigaction sa;
        sa.sa_handler = segv_handler; sa.sa_flags = 0; sa.sa_mask = 0;
        sigaction(SIGSEGV, &sa, 0);
        pm[0] = 0;          // must fault
        exit(2);
    }
    if (wait(rochild) != 88) {
        printf("[mmaptest] FAILED: write after mprotect PROT_READ did not fault\n");
        return 1;
    }

    if (mprotect((void *)(unsigned long)pa, plen, PROT_READ | PROT_WRITE) != 0) {
        printf("[mmaptest] FAILED: mprotect back to PROT_WRITE\n"); return 1;
    }
    for (unsigned long i = 0; i < plen; i++) {
        if (pm[i] != (unsigned char)(i * 7 + 3)) {
            printf("[mmaptest] FAILED: PROT_WRITE restore lost byte %d\n", (int)i);
            return 1;
        }
    }
    pm[0] = 0x5a;
    if (pm[0] != 0x5a) {
        printf("[mmaptest] FAILED: page not writable again after mprotect\n");
        return 1;
    }
    pm[0] = (unsigned char)3;
    printf("[mmaptest] mprotect RO->RW restores write access passed\n");

    // PROT_NONE parks the frame rather than freeing it: reads fault
    // while it is in force, and the bytes are still there afterwards.
    if (mprotect((void *)(unsigned long)pa, plen, PROT_NONE) != 0) {
        printf("[mmaptest] FAILED: mprotect to PROT_NONE\n"); return 1;
    }
    int nchild = fork();
    if (nchild == 0) {
        struct sigaction sa;
        sa.sa_handler = segv_handler; sa.sa_flags = 0; sa.sa_mask = 0;
        sigaction(SIGSEGV, &sa, 0);
        volatile unsigned char sink = pm[0];   // must fault: even reading
        (void)sink;
        exit(2);
    }
    if (wait(nchild) != 88) {
        printf("[mmaptest] FAILED: read of a PROT_NONE page did not fault\n");
        return 1;
    }
    if (mprotect((void *)(unsigned long)pa, plen, PROT_READ) != 0) {
        printf("[mmaptest] FAILED: mprotect PROT_NONE -> PROT_READ\n"); return 1;
    }
    for (unsigned long i = 0; i < plen; i++) {
        if (pm[i] != (unsigned char)(i * 7 + 3)) {
            printf("[mmaptest] FAILED: PROT_NONE lost byte %d\n", (int)i);
            return 1;
        }
    }
    printf("[mmaptest] mprotect PROT_NONE round trip passed\n");
    munmap_raw((unsigned long)pa, plen);

    // Frame reclamation. munmap does not hand its frames straight back
    // to the allocator -- it defers them until a TLB shootdown says
    // every CPU has dropped the translation -- and for a long time
    // nothing ever performed that shootdown, so every page ever
    // unmapped leaked. This maps and touches far more memory in total
    // than the machine has; without reclamation it runs out.
    const int rounds = 900;
    unsigned long rlen = 4096 * 64;      // 900 * 256KiB = 225MiB total
    for (int r = 0; r < rounds; r++) {
        long ra = mmap_raw(0, rlen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        if (ra < 0) {
            printf("[mmaptest] FAILED: mmap round %d exhausted memory (%d)\n", r, (int)ra);
            return 1;
        }
        volatile unsigned char *rm = (volatile unsigned char *)(unsigned long)ra;
        for (unsigned long i = 0; i < rlen; i += 4096) { rm[i] = (unsigned char)r; }
        if (munmap_raw((unsigned long)ra, rlen) != 0) {
            printf("[mmaptest] FAILED: munmap round %d\n", r);
            return 1;
        }
    }
    printf("[mmaptest] unmapped frames are reclaimed passed\n");

    printf("[mmaptest] ALL PASSED\n");
    return 0;
}
