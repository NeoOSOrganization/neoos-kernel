// The M1b-3 test child TERM forks onto its pty slave. Writes a
// deterministic pattern for TERM's pixel self-check, then exits.

#include <unistd.h>
#include <stdio.h>

int main(void) {
    printf("\x1b[2J\x1b[H");                 // clear, home
    printf("\x1b[31mR\x1b[0mENDER-OK\r\n");  // red 'R' at cell (0,0)
    printf("line two\r\n");

    // Give TERM time to drain the master and render before the slave
    // closes (there is no fsync/flush on a pty).
    for (volatile long i = 0; i < 30000000L; i++) { }
    return 0;
}
