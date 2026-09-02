// Exercises thread-local storage and the process entry contract it
// rests on: argc/argv, the auxiliary vector, and __thread variables
// that must be genuinely per-thread.
//
// The interesting assertion is not that __thread compiles. It is that
// four threads writing the same named variable do not see each other's
// values -- and, because NeoOS migrates threads between CPUs, that the
// thread pointer survives being moved. A per-CPU FS base rather than a
// per-thread one would pass a single-threaded test and fail this one.

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <auxv.h>
#include <tls.h>
#include <pthread.h>

// .tdata: a non-zero initialiser, so the template really is copied
// rather than merely zeroed.
static __thread int tls_counter = 0x5A5A;
// .tbss: no initialiser, so it must come out zero in every thread.
static __thread int tls_zero;
// Large enough to span more than one machine word, catching a template
// copy that only moves the first few bytes.
static __thread char tls_name[32];

#define WORKERS 4

static volatile int worker_ok[WORKERS];
static volatile unsigned long worker_tp[WORKERS];

static unsigned long read_tp(void) {
    unsigned long tp = 0;
    arch_prctl(ARCH_GET_FS, (unsigned long)(void *)&tp);
    return tp;
}

static void *tls_worker(void *arg) {
    int idx = (int)(long)arg;

    // Every thread starts from the SAME template, so both of these are
    // assertions about the copy, not about this thread.
    if (tls_counter != 0x5A5A) {
        printf("[tlstest] FAILED: thread %d saw tdata %x, want 5a5a\n",
               idx, tls_counter);
        return 0;
    }
    if (tls_zero != 0) {
        printf("[tlstest] FAILED: thread %d saw tbss %d, want 0\n", idx, tls_zero);
        return 0;
    }

    worker_tp[idx] = read_tp();

    // Now diverge, and keep diverging across many scheduling points so
    // that migration has every chance to move this thread to another
    // CPU mid-way. If the thread pointer were per-CPU, the value would
    // change under us and this would fail.
    tls_counter = idx;
    tls_zero    = idx * 7;
    for (int i = 0; i < 32; i++) { tls_name[i] = (char)('a' + idx); }

    for (int round = 0; round < 200; round++) {
        for (volatile int k = 0; k < 2000; k++) { }
        if (tls_counter != idx || tls_zero != idx * 7) {
            printf("[tlstest] FAILED: thread %d lost its TLS at round %d "
                   "(counter=%d zero=%d)\n", idx, round, tls_counter, tls_zero);
            return 0;
        }
        for (int i = 0; i < 32; i++) {
            if (tls_name[i] != (char)('a' + idx)) {
                printf("[tlstest] FAILED: thread %d TLS array corrupted\n", idx);
                return 0;
            }
        }
    }

    worker_ok[idx] = 1;
    return 0;
}

static int check_entry_contract(int argc, char **argv) {
    if (argc < 1 || !argv || !argv[0]) {
        printf("[tlstest] FAILED: argc=%d argv=%p\n", argc, (void *)argv);
        return 0;
    }
    if (argv[argc] != 0) {
        printf("[tlstest] FAILED: argv is not NULL-terminated\n");
        return 0;
    }
    // environ used to be asserted EMPTY, which was right when the
    // kernel pushed a bare NULL for it. BB3 gave every process a real
    // environment from init, so the check is now that the vector is
    // well-formed: present, NUL-terminated strings of the form NAME=...,
    // and terminated by a NULL pointer. That is the part the entry
    // contract actually guarantees -- WHICH variables are init's
    // business, not this test's.
    if (!environ) {
        printf("[tlstest] FAILED: environ is NULL\n");
        return 0;
    }
    int nenv = 0;
    for (char **e = environ; *e; e++) {
        int has_eq = 0;
        for (const char *c = *e; *c; c++) { if (*c == '=') { has_eq = 1; break; } }
        if (!has_eq) {
            printf("[tlstest] FAILED: environ[%d] has no '=': %s\n", nenv, *e);
            return 0;
        }
        if (++nenv > 256) {
            printf("[tlstest] FAILED: environ is not NULL-terminated\n");
            return 0;
        }
    }
    printf("[tlstest] entry contract: argc=%d, %d environment variables\n",
           argc, nenv);
    // The auxv entries TLS actually depends on.
    if (getauxval(AT_PHDR) == 0 || getauxval(AT_PHNUM) == 0 ||
        getauxval(AT_PHENT) == 0) {
        printf("[tlstest] FAILED: auxv lacks AT_PHDR/PHNUM/PHENT\n");
        return 0;
    }
    if (getauxval(AT_PAGESZ) != 4096) {
        printf("[tlstest] FAILED: AT_PAGESZ=%d, want 4096\n",
               (int)getauxval(AT_PAGESZ));
        return 0;
    }
    if (getauxval(AT_ENTRY) == 0) {
        printf("[tlstest] FAILED: auxv lacks AT_ENTRY\n");
        return 0;
    }
    if (getauxval(AT_RANDOM) == 0) {
        printf("[tlstest] FAILED: auxv lacks AT_RANDOM\n");
        return 0;
    }
    // An unknown type must read as 0 rather than walking off the end.
    if (getauxval(0xDEAD) != 0) {
        printf("[tlstest] FAILED: unknown auxv type did not return 0\n");
        return 0;
    }
    printf("[tlstest] entry contract passed, argv[0]=%s\n", argv[0]);
    return 1;
}

static int check_main_tls(void) {
    unsigned long tp = read_tp();
    if (tp == 0) {
        printf("[tlstest] FAILED: main thread has no thread pointer\n");
        return 0;
    }
    // %fs:0 is a self-pointer; that is the ABI, and glibc- or
    // musl-compiled code relies on it.
    unsigned long self = 0;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(self));
    if (self != tp) {
        printf("[tlstest] FAILED: fs:0 is %lx, want %lx\n", self, tp);
        return 0;
    }
    if (tls_counter != 0x5A5A || tls_zero != 0) {
        printf("[tlstest] FAILED: main thread template wrong\n");
        return 0;
    }
    tls_counter = 0x1234;
    printf("[tlstest] main thread TLS passed\n");
    return 1;
}

static int check_per_thread(void) {
    for (int i = 0; i < WORKERS; i++) { worker_ok[i] = 0; worker_tp[i] = 0; }

    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], 0, tls_worker, (void *)(long)i) != 0) {
            printf("[tlstest] FAILED: pthread_create %d\n", i);
            return 0;
        }
    }
    for (int i = 0; i < WORKERS; i++) { pthread_join(t[i], 0); }

    for (int i = 0; i < WORKERS; i++) {
        if (!worker_ok[i]) {
            printf("[tlstest] FAILED: thread %d did not finish cleanly\n", i);
            return 0;
        }
    }
    // Distinct thread pointers are the whole point: a shared one would
    // mean the threads were sharing storage and merely not noticing.
    for (int i = 0; i < WORKERS; i++) {
        if (worker_tp[i] == 0) {
            printf("[tlstest] FAILED: thread %d had no thread pointer\n", i);
            return 0;
        }
        for (int j = i + 1; j < WORKERS; j++) {
            if (worker_tp[i] == worker_tp[j]) {
                printf("[tlstest] FAILED: threads %d and %d share a thread pointer\n",
                       i, j);
                return 0;
            }
        }
    }
    // And the main thread's own value must have survived all of that.
    if (tls_counter != 0x1234) {
        printf("[tlstest] FAILED: main thread TLS clobbered by workers (%x)\n",
               tls_counter);
        return 0;
    }
    printf("[tlstest] per-thread storage passed across %d threads\n", WORKERS);
    return 1;
}

static int check_arch_prctl_errors(void) {
    // A non-canonical or kernel address must be refused: the kernel
    // would #GP on the WRMSR otherwise, turning a bad argument into a
    // kernel fault.
    if (arch_prctl(ARCH_SET_FS, 0xFFFF800000000000UL) == 0) {
        printf("[tlstest] FAILED: a kernel address was accepted as a thread pointer\n");
        return 0;
    }
    // ARCH_SET_GS is deliberately unsupported.
    if (arch_prctl(0x1001, 0) == 0) {
        printf("[tlstest] FAILED: ARCH_SET_GS was accepted\n");
        return 0;
    }
    printf("[tlstest] arch_prctl rejects bad arguments\n");
    return 1;
}

int main(int argc, char **argv) {
    int ok = 1;
    ok &= check_entry_contract(argc, argv);
    ok &= check_main_tls();
    ok &= check_per_thread();
    ok &= check_arch_prctl_errors();

    printf("[tlstest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
