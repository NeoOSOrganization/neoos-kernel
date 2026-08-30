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
#define MIG_THREADS 8

static volatile unsigned char mig_cpu_seen[64];
static volatile int mig_started;

static void mig_worker(void *arg) {
    (void)arg;
    // Long enough to be preempted several times, so an idle CPU has
    // repeated chances to come and take this thread.
    for (int round = 0; round < 200; round++) {
        int c = sched_getcpu();
        if (c >= 0 && c < 64) { mig_cpu_seen[c] = 1; }
        for (volatile int i = 0; i < 20000; i++) { }
    }
    __atomic_fetch_add((int *)&mig_started, 1, __ATOMIC_ACQ_REL);
    thread_exit(0);
}

static int check_thread_migration(long online) {
    for (int i = 0; i < 64; i++) { mig_cpu_seen[i] = 0; }
    mig_started = 0;

    thread_t t[MIG_THREADS];
    for (int i = 0; i < MIG_THREADS; i++) {
        if (thread_create(&t[i], mig_worker, 0) != 0) {
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
