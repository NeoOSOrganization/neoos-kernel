// userland/dyntest.c -- built WITHOUT -static.
//
// That it runs at all is the milestone. Reaching main means the kernel
// found PT_INTERP, loaded ld-musl at a base of its choosing, described
// the executable with AT_PHDR/AT_ENTRY and the interpreter with
// AT_BASE, and entered through the interpreter -- which then relocated
// itself and everything else.

#include <stdio.h>
#include <dlfcn.h>

static int failures;

// dlopen is the same musl code path ld.so has already run to get this
// program started, so it should need nothing new from the kernel. That
// it is worth testing anyway is the point: "should" is not evidence.
static void test_dlopen(void) {
    void *h = dlopen("/lib/dynlib.so", RTLD_NOW);
    if (!h) {
        printf("[dyn] FAILED: dlopen: %s\n", dlerror());
        failures++;
        return;
    }

    int (*answer)(int) = (int (*)(int))dlsym(h, "neoos_dl_answer");
    if (!answer) {
        printf("[dyn] FAILED: dlsym: %s\n", dlerror());
        failures++;
        dlclose(h);
        return;
    }

    int got = answer(20);
    if (got != 41) {
        printf("[dyn] FAILED: dlsym returned %d, wanted 41\n", got);
        failures++;
    } else {
        printf("[dyn] dlopen ok\n");
    }

    if (dlclose(h) != 0) {
        printf("[dyn] FAILED: dlclose\n");
        failures++;
    }
}

int main(void) {
    printf("[dyn] dynamic executable running\n");
    test_dlopen();
    if (failures) {
        printf("[dyn] %d FAILURES\n", failures);
        fflush(stdout);
        return 1;
    }
    printf("[dyn] ALL PASSED\n");
    fflush(stdout);
    return 0;
}
