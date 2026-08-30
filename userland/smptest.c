#include <unistd.h>
#include <stdio.h>

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
    printf("[smptest] ALL PASSED\n");
    return 0;
}
