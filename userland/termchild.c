// The M1b-3 test child TERM forks onto its pty slave. Writes a
// deterministic pattern for TERM's pixel self-check, then exits.
//
// Uses write(1) directly, NOT printf: libneoos's printf goes to
// /dev/kmsg (serial), but this program's output is terminal content --
// it must reach the pty slave (fd 1) so TERM renders it.

#include <unistd.h>
#include <string.h>

static void out(const char *s) { write(1, s, strlen(s)); }

int main(void) {
    out("\x1b[2J\x1b[H");                 // clear, home
    out("\x1b[31mR\x1b[0mENDER-OK\r\n");  // red 'R' at cell (0,0)
    out("line two\r\n");

    // Give TERM time to drain the master and render before the slave
    // closes (there is no fsync/flush on a pty).
    for (volatile long i = 0; i < 30000000L; i++) { }
    return 0;
}
