#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char msg[] = "spin test program running\n";
    printf("%s", msg);
    return 0;
}
