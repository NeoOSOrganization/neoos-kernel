// One binary, two jobs. Run with no arguments it is the launcher: it
// starts RANKS copies of itself and waits for them. Run with a rank and
// a world size in its argv -- which is what MPI_Launch supplies -- it is
// a rank, and runs the actual parallel program.
//
// Doing it in one binary rather than two is not a trick to save a file:
// it means the launcher and the ranks cannot drift apart, and it is how
// a real MPI program looks from the outside, where `mpirun ./prog` runs
// the same executable everywhere.

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <mpi.h>

#define RANKS 4

// ------------------------------------------------------------- the ranks

static int rank_main(int argc, char **argv) {
    if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
        printf("[mpitest] FAILED: MPI_Init\n");
        return 1;
    }

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size != RANKS) {
        printf("[mpitest] FAILED: size=%d, want %d\n", size, RANKS);
        return 1;
    }
    // MPI_Init is required to strip its own arguments, so a program can
    // parse its options afterwards without knowing they were there.
    if (argc != 1) {
        printf("[mpitest] FAILED: rank %d has argc=%d after MPI_Init, want 1\n",
               rank, argc);
        return 1;
    }

    // 1. A ring. Each rank sends to the next and receives from the
    //    previous, which deadlocks immediately if sends are synchronous
    //    -- they are not here, since a datagram send does not wait for
    //    a receiver -- and which exercises tag and source matching.
    int token = rank * 100;
    int next = (rank + 1) % size;
    int prev = (rank + size - 1) % size;
    int got  = -1;
    if (MPI_Send(&token, 1, MPI_INT, next, 1, MPI_COMM_WORLD) != MPI_SUCCESS) {
        printf("[mpitest] FAILED: rank %d ring send\n", rank);
        return 1;
    }
    MPI_Status st;
    if (MPI_Recv(&got, 1, MPI_INT, prev, 1, MPI_COMM_WORLD, &st) != MPI_SUCCESS) {
        printf("[mpitest] FAILED: rank %d ring recv\n", rank);
        return 1;
    }
    if (got != prev * 100 || st.MPI_SOURCE != prev || st.MPI_TAG != 1) {
        printf("[mpitest] FAILED: rank %d got %d from %d tag %d, want %d from %d\n",
               rank, got, st.MPI_SOURCE, st.MPI_TAG, prev * 100, prev);
        return 1;
    }

    // 2. Out-of-order tags, which is what the unexpected-message queue
    //    exists for. Rank 0 sends tag 20 then tag 10; every other rank
    //    asks for tag 10 FIRST. Without the queue the tag-20 message
    //    would be consumed by the tag-10 receive or dropped, and this
    //    would either hang or return the wrong value.
    if (rank == 0) {
        for (int r = 1; r < size; r++) {
            int a = 20, b = 10;
            MPI_Send(&a, 1, MPI_INT, r, 20, MPI_COMM_WORLD);
            MPI_Send(&b, 1, MPI_INT, r, 10, MPI_COMM_WORLD);
        }
    } else {
        int ten = 0, twenty = 0;
        MPI_Recv(&ten, 1, MPI_INT, 0, 10, MPI_COMM_WORLD, 0);
        MPI_Recv(&twenty, 1, MPI_INT, 0, 20, MPI_COMM_WORLD, 0);
        if (ten != 10 || twenty != 20) {
            printf("[mpitest] FAILED: rank %d out-of-order tags gave %d,%d\n",
                   rank, ten, twenty);
            return 1;
        }
    }

    // 3. Barrier, then broadcast. The barrier must not let the
    //    broadcast's data overtake it -- distinct tags are what make
    //    that true.
    if (MPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
        printf("[mpitest] FAILED: rank %d barrier\n", rank);
        return 1;
    }

    int shared[4];
    if (rank == 0) {
        shared[0] = 11; shared[1] = 22; shared[2] = 33; shared[3] = 44;
    } else {
        memset(shared, 0, sizeof(shared));
    }
    if (MPI_Bcast(shared, 4, MPI_INT, 0, MPI_COMM_WORLD) != MPI_SUCCESS) {
        printf("[mpitest] FAILED: rank %d bcast\n", rank);
        return 1;
    }
    if (shared[0] != 11 || shared[3] != 44) {
        printf("[mpitest] FAILED: rank %d bcast gave %d..%d\n",
               rank, shared[0], shared[3]);
        return 1;
    }

    // 4. Reduce and Allreduce. The sum of 0..size-1 is a value every
    //    rank can check independently, so a reduction that lost or
    //    double-counted a contribution shows up as a wrong number
    //    rather than a hang.
    int contribution = rank + 1;
    int total = 0;
    if (MPI_Reduce(&contribution, &total, 1, MPI_INT, MPI_SUM, 0,
                   MPI_COMM_WORLD) != MPI_SUCCESS) {
        printf("[mpitest] FAILED: rank %d reduce\n", rank);
        return 1;
    }
    int want = size * (size + 1) / 2;
    if (rank == 0 && total != want) {
        printf("[mpitest] FAILED: reduce sum %d, want %d\n", total, want);
        return 1;
    }

    int all = 0;
    if (MPI_Allreduce(&contribution, &all, 1, MPI_INT, MPI_MAX,
                      MPI_COMM_WORLD) != MPI_SUCCESS) {
        printf("[mpitest] FAILED: rank %d allreduce\n", rank);
        return 1;
    }
    if (all != size) {
        printf("[mpitest] FAILED: rank %d allreduce max %d, want %d\n",
               rank, all, size);
        return 1;
    }

    // 5. A message big enough to be worth carrying, so the payload path
    //    is not only ever tested with four bytes.
    if (size > 1) {
        static int big[512];
        if (rank == 0) {
            for (int i = 0; i < 512; i++) { big[i] = i * 3; }
            MPI_Send(big, 512, MPI_INT, 1, 55, MPI_COMM_WORLD);
        } else if (rank == 1) {
            MPI_Recv(big, 512, MPI_INT, 0, 55, MPI_COMM_WORLD, &st);
            if (st.count != 512 * (int)sizeof(int)) {
                printf("[mpitest] FAILED: big message was %d bytes\n", st.count);
                return 1;
            }
            for (int i = 0; i < 512; i++) {
                if (big[i] != i * 3) {
                    printf("[mpitest] FAILED: big message corrupt at %d\n", i);
                    return 1;
                }
            }
        }
    }

    // 6. A message too large for the receiver's buffer is an ERROR in
    //    MPI, not a silent truncation -- the opposite of recvfrom.
    if (size > 1) {
        if (rank == 0) {
            int four[4] = { 1, 2, 3, 4 };
            MPI_Send(four, 4, MPI_INT, 1, 66, MPI_COMM_WORLD);
        } else if (rank == 1) {
            int one = 0;
            if (MPI_Recv(&one, 1, MPI_INT, 0, 66, MPI_COMM_WORLD, &st)
                != MPI_ERR_TRUNCATE) {
                printf("[mpitest] FAILED: an oversized message was not refused\n");
                return 1;
            }
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Finalize();

    if (rank == 0) { printf("[mpitest] all %d ranks completed\n", size); }
    return 0;
}

// ---------------------------------------------------------- the launcher

int main(int argc, char **argv) {
    if (argc >= 3) { return rank_main(argc, argv); }

    int pids[RANKS];
    if (MPI_Launch("/BIN/MPITEST.ELF", RANKS, pids) != MPI_SUCCESS) {
        printf("[mpitest] FAILED: MPI_Launch\n");
        printf("[mpitest] SOME CHECKS FAILED\n");
        return 1;
    }

    int ok = 1;
    for (int i = 0; i < RANKS; i++) {
        int code = wait(pids[i]);
        if (code != 0) {
            printf("[mpitest] FAILED: rank %d exited %d\n", i, code);
            ok = 0;
        }
    }

    printf("[mpitest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
