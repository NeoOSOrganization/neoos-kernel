#include <unistd.h>
#include <stdio.h>
#include <signal.h>

// The address mmaptest's exec check maps MAP_FIXED before exec'ing this
// program. exec must leave nothing of the old address space behind --
// neither the pages nor the vma bookkeeping that says those addresses
// are legal -- so touching it here has to fault.
#define STALE_PROBE_ADDR 0x500000000000UL

static void segv_handler(int sig) {
    (void)sig;
    // Faulted, which is the correct outcome: whatever the previous image
    // had mapped here is gone. There is nowhere to return to from a
    // SIGSEGV handler, so the success path exits from inside it.
    printf("[exec_target] predecessor's mapping is gone\n");
    exit(0);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("[exec_target pid=%d] running, exec succeeded\n", getpid());

    struct sigaction sa;
    sa.sa_handler = segv_handler; sa.sa_flags = 0; sa.sa_mask = 0;
    sigaction(SIGSEGV, &sa, 0);

    volatile unsigned char sink = *(volatile unsigned char *)STALE_PROBE_ADDR;
    (void)sink;

    // Reached only if the address was still readable, which means exec
    // carried a stale mapping (or a stale vma that the fault handler
    // answered with a fresh zero page) into the new image.
    printf("[exec_target] FAILED: stale mapping survived exec\n");
    return 9;
}
