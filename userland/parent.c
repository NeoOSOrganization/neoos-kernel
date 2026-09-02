#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int child_pid = spawn("/usr/tests/child.nex");
    int exit_code = wait(child_pid);
    printf("[parent] child exit code=%d\n", exit_code);
    return 0;
}
