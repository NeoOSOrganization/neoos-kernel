#include <unistd.h>
#include <stdio.h>
#include <thread.h>

// Proves USER threads really run on more than one CPU, not just kernel
// threads. Every thread is created on this process's current CPU and
// nothing places them anywhere else, so a thread that reports a
// different CPU can only have been migrated there by the scheduler.
//
// This is the userland half of the kernel's "[smp] steal selftest": the
// kernel one shows the mechanism works, this one shows it is safe to
// point at a thread carrying an address space, an fd table and signal
// state.
// The threads keep looking until the spread is seen, rather than
// sampling a fixed window and asserting on whatever it caught. A CPU
// steals only when its own run queue is empty, and with every other
// suite running there is often no idle CPU for a while -- a fixed
// window turned that into a one-in-four spurious failure. Stopping as
// soon as two CPUs have been observed keeps the common case fast.
#define MIG_THREADS 8
#define MIG_ROUNDS  1500

static volatile unsigned char mig_cpu_seen[64];
static volatile int mig_spread;      // set once >= 2 CPUs have been seen

static void mig_note_cpu(long online) {
    int c = sched_getcpu();
    if (c < 0 || c >= 64) { return; }
    mig_cpu_seen[c] = 1;

    int seen = 0;
    for (int i = 0; i < online && i < 64; i++) { if (mig_cpu_seen[i]) { seen++; } }
    if (seen >= 2) { __atomic_store_n((int *)&mig_spread, 1, __ATOMIC_RELEASE); }
}

static void mig_worker(void *arg) {
    long online = (long)arg;
    for (int round = 0; round < MIG_ROUNDS; round++) {
        mig_note_cpu(online);
        if (__atomic_load_n((int *)&mig_spread, __ATOMIC_ACQUIRE)) { break; }
        for (volatile int i = 0; i < 4000; i++) { }
    }
    thread_exit(0);
}

static int check_thread_migration(long online) {
    for (int i = 0; i < 64; i++) { mig_cpu_seen[i] = 0; }
    mig_spread = 0;

    thread_t t[MIG_THREADS];
    for (int i = 0; i < MIG_THREADS; i++) {
        if (thread_create(&t[i], mig_worker, (void *)online) != 0) {
            printf("[smptest] FAILED: thread_create %d\n", i);
            return 0;
        }
    }
    for (int i = 0; i < MIG_THREADS; i++) {
        if (thread_join(t[i], 0) != 0) {
            printf("[smptest] FAILED: thread_join %d\n", i);
            return 0;
        }
    }

    int seen = 0;
    for (int i = 0; i < online && i < 64; i++) { if (mig_cpu_seen[i]) { seen++; } }
    if (seen < 2) {
        printf("[smptest] FAILED: user threads never left one cpu (seen=%d)\n", seen);
        return 0;
    }
    printf("[smptest] user threads ran on %d cpus\n", seen);
    return 1;
}

// Exercises the SMP visibility calls from ring 3. The point is that a
// user program can see the machine's CPU count and which CPU it is on
// through POSIX-shaped calls, not a raw syscall number.
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    long online = sysconf(_SC_NPROCESSORS_ONLN);
    long conf   = sysconf(_SC_NPROCESSORS_CONF);
    if (online < 1) {
        printf("[smptest] FAILED: sysconf(_SC_NPROCESSORS_ONLN)=%d\n", (int)online);
        return 1;
    }
    if (conf != online) {
        printf("[smptest] FAILED: CONF=%d disagrees with ONLN=%d\n",
               (int)conf, (int)online);
        return 1;
    }
    // NeoOS has no CPU hotplug, so CONF == ONLN by construction.
    if (online < 2) {
        printf("[smptest] FAILED: expected a multiprocessor, got %d cpu\n",
               (int)online);
        return 1;
    }

    int cpu = sched_getcpu();
    if (cpu < 0 || cpu >= online) {
        printf("[smptest] FAILED: sched_getcpu()=%d out of range 0..%d\n",
               cpu, (int)online - 1);
        return 1;
    }

    printf("[smptest] cpus=%d, running on cpu %d\n", (int)online, cpu);

    if (!check_thread_migration(online)) { return 1; }

    printf("[smptest] ALL PASSED\n");
    return 0;
}
