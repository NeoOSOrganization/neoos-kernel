#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <thread.h>

static volatile int shared_counter;
static volatile unsigned long worker_local_addr[4];
static volatile int worker_tid[4];

static void worker(void *arg) {
    int idx = (int)(long)arg;
    volatile int local = 0;          // lives on THIS thread's stack
    worker_local_addr[idx] = (unsigned long)(void *)&local;
    worker_tid[idx] = thread_self();

    for (int i = 0; i < 1000; i++) {
        __atomic_fetch_add(&shared_counter, 1, __ATOMIC_SEQ_CST);
    }
    local = idx;
    thread_exit(100 + idx);
}

static int check_shared_address_space(void) {
    shared_counter = 0;
    thread_t t[4];
    for (int i = 0; i < 4; i++) {
        if (thread_create(&t[i], worker, (void *)(long)i) != 0) {
            printf("[threadtest] FAILED: thread_create %d\n", i);
            return 0;
        }
    }
    for (int i = 0; i < 4; i++) {
        int code = -1;
        if (thread_join(t[i], &code) != 0) {
            printf("[threadtest] FAILED: thread_join %d\n", i);
            return 0;
        }
        if (code != 100 + i) {
            printf("[threadtest] FAILED: join %d got code %d\n", i, code);
            return 0;
        }
    }
    if (shared_counter != 4000) {
        printf("[threadtest] FAILED: counter=%d want 4000\n", shared_counter);
        return 0;
    }
    printf("[threadtest] shared address space passed (counter=%d)\n", shared_counter);
    return 1;
}

static int check_separate_stacks(void) {
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (worker_local_addr[i] == worker_local_addr[j]) {
                printf("[threadtest] FAILED: threads %d and %d share a stack\n", i, j);
                return 0;
            }
        }
    }
    printf("[threadtest] separate stacks passed\n");
    return 1;
}

static int check_distinct_tids(void) {
    for (int i = 0; i < 4; i++) {
        if (worker_tid[i] == thread_self() || worker_tid[i] == 0) {
            printf("[threadtest] FAILED: bad tid %d\n", worker_tid[i]);
            return 0;
        }
    }
    printf("[threadtest] distinct tids passed\n");
    return 1;
}

static volatile int fd_from_worker = -1;

static void fd_worker(void *arg) {
    (void)arg;
    fd_from_worker = open("/tmp/THREADFD.TXT", O_CREAT | O_RDWR | O_TRUNC);
    thread_exit(0);
}

static int check_shared_fd_table(void) {
    thread_t t;
    if (thread_create(&t, fd_worker, 0) != 0) {
        printf("[threadtest] FAILED: thread_create for fd test\n");
        return 0;
    }
    thread_join(t, 0);
    if (fd_from_worker < 0) {
        printf("[threadtest] FAILED: worker could not open file (%d)\n", fd_from_worker);
        return 0;
    }
    // The fd another thread opened must be usable from this one.
    if (write(fd_from_worker, "shared-fd", 9) != 9) {
        printf("[threadtest] FAILED: inherited fd not writable\n");
        return 0;
    }
    close(fd_from_worker);
    printf("[threadtest] shared fd table passed\n");
    return 1;
}

// Spins forever, writing its own stack the whole time. If exec() frees
// the address space with this thread still running, this is the loop
// that scribbles over whatever pmm handed the frames to next.
static void exec_sibling(void *arg) {
    (void)arg;
    unsigned sum = 0;
    for (;;) {
        volatile unsigned char scratch[512];
        for (int i = 0; i < 512; i++) { scratch[i] = (unsigned char)i; }
        for (int i = 0; i < 512; i++) { sum += scratch[i]; }
        yield();
    }
}

// exec() must reduce the process to a single thread BEFORE it frees the
// old address space -- Linux execve() does, and the siblings are
// executing in exactly the address space being freed. Run in a forked
// child, since a successful exec never returns.
static int check_exec_kills_threads(void) {
    int child = fork();
    if (child < 0) {
        printf("[threadtest] FAILED: fork for exec test\n");
        return 0;
    }
    if (child == 0) {
        thread_t t[3];
        for (int i = 0; i < 3; i++) {
            if (thread_create(&t[i], exec_sibling, 0) != 0) { exit(1); }
        }
        for (volatile int i = 0; i < 500000; i++) { }   // let them get going
        exec("/usr/tests/exectarg.nex");                      // must not return
        exit(2);
    }
    int code = wait(child);
    if (code != 0) {
        printf("[threadtest] FAILED: exec with live siblings exited %d\n", code);
        return 0;
    }
    printf("[threadtest] exec reduces the process to one thread passed\n");
    return 1;
}

// Never exits, so anything joining it blocks forever.
static void spinner(void *arg) {
    (void)arg;
    for (;;) { yield(); }
}

static void blocker(void *arg) {
    thread_t target = (thread_t)(long)arg;
    // Genuinely blocks: `target` never exits. NOTE: an earlier version
    // waited on a nonexistent pid, which does NOT block -- wait_for_pid
    // returns -1 immediately for an unknown pid -- so the check passed
    // only by losing a race.
    thread_join(target, 0);
    // Unreachable if exit() works: the kill is noticed on the syscall
    // return path, so a killed thread never gets back to user mode.
    printf("[threadtest] FAILED: blocked thread resumed\n");
    thread_exit(1);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int ok = 1;
    ok &= check_shared_address_space();
    ok &= check_separate_stacks();
    ok &= check_distinct_tids();
    ok &= check_shared_fd_table();
    ok &= check_exec_kills_threads();

    printf("[threadtest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");

    // Last: prove exit() kills a thread blocked in a syscall. If this
    // is broken the process never exits and the boot hangs, which the
    // timeout catches.
    thread_t spin, b;
    if (thread_create(&spin, spinner, 0) != 0) {
        printf("[threadtest] FAILED: thread_create for spinner\n");
        return 1;
    }
    if (thread_create(&b, blocker, (void *)(long)spin) != 0) {
        printf("[threadtest] FAILED: thread_create for blocker\n");
        return 1;
    }
    for (volatile int i = 0; i < 1000000; i++) { } // let the blocker reach its join
    printf("[threadtest] exiting with a blocked sibling\n");
    return ok ? 0 : 1;
}
