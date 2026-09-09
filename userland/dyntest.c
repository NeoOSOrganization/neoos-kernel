// userland/dyntest.c -- built WITHOUT -static.
//
// That it runs at all is the milestone. Reaching main means the kernel
// found PT_INTERP, loaded ld-musl at a base of its choosing, described
// the executable with AT_PHDR/AT_ENTRY and the interpreter with
// AT_BASE, and entered through the interpreter -- which then relocated
// itself and everything else.

#include <stdio.h>

int main(void) {
    printf("[dyn] dynamic executable running\n");
    printf("[dyn] ALL PASSED\n");
    fflush(stdout);
    return 0;
}
