#ifndef NEOOS_MPI_H
#define NEOOS_MPI_H

#include <stdint.h>

// A SUBSET of MPI-1: enough to write a real parallel program, and
// deliberately not more.
//
// Ranks are separate PROCESSES, each listening on its own UDP port on
// 127.0.0.1, and a message is one datagram. That choice is worth
// stating: the alternative was a bespoke kernel "port" object, and
// building on sockets instead means the transport is one already
// tested, already visible to a debugger, and already the thing a real
// MPI uses for its TCP path. When NeoOS gets a NIC, this runs between
// machines with no change above the socket calls.
//
// Every function returns MPI_SUCCESS or an MPI_ERR_* code, as MPI
// requires -- not a negative errno like the rest of this library.

typedef int MPI_Comm;
typedef int MPI_Datatype;
typedef int MPI_Op;

#define MPI_COMM_WORLD 0
#define MPI_COMM_NULL  (-1)

#define MPI_SUCCESS     0
#define MPI_ERR_COMM    5
#define MPI_ERR_COUNT   2
#define MPI_ERR_TYPE    3
#define MPI_ERR_TAG     4
#define MPI_ERR_RANK    6
#define MPI_ERR_OP      9
#define MPI_ERR_TRUNCATE 14
#define MPI_ERR_OTHER   15
#define MPI_ERR_INTERN  16

#define MPI_ANY_SOURCE (-1)
#define MPI_ANY_TAG    (-1)

// The datatypes are tags, not descriptors: each is just a size plus a
// rule for MPI_Reduce. Derived types (MPI_Type_contiguous and friends)
// do not exist.
#define MPI_BYTE   0
#define MPI_CHAR   1
#define MPI_INT    2
#define MPI_LONG   3
#define MPI_DOUBLE 4

#define MPI_SUM  0
#define MPI_PROD 1
#define MPI_MAX  2
#define MPI_MIN  3

// Linux MPI implementations put MPI_SOURCE/MPI_TAG/MPI_ERROR first and
// keep the count private; this exposes it, since there is no
// MPI_Get_count-only reason to hide it and a caller usually wants it.
typedef struct {
    int MPI_SOURCE;
    int MPI_TAG;
    int MPI_ERROR;
    int count;          // bytes actually received
} MPI_Status;

// The largest message one MPI_Send can carry. A message is one UDP
// datagram, so this is a real limit rather than a buffer size -- there
// is no segmentation layer. Recorded in docs/stdlib.md.
#define MPI_MAX_MESSAGE 8192

int MPI_Init(int *argc, char ***argv);
int MPI_Finalize(void);
int MPI_Initialized(int *flag);

int MPI_Comm_size(MPI_Comm comm, int *size);
int MPI_Comm_rank(MPI_Comm comm, int *rank);

int MPI_Send(const void *buf, int count, MPI_Datatype type,
             int dest, int tag, MPI_Comm comm);
// `source` may be MPI_ANY_SOURCE and `tag` MPI_ANY_TAG. A message
// larger than the buffer is an error (MPI_ERR_TRUNCATE), not a silent
// truncation -- MPI's rule, and the opposite of recvfrom's.
int MPI_Recv(void *buf, int count, MPI_Datatype type,
             int source, int tag, MPI_Comm comm, MPI_Status *status);

int MPI_Barrier(MPI_Comm comm);
int MPI_Bcast(void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm);
int MPI_Reduce(const void *sendbuf, void *recvbuf, int count,
               MPI_Datatype type, MPI_Op op, int root, MPI_Comm comm);
int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                  MPI_Datatype type, MPI_Op op, MPI_Comm comm);

int MPI_Type_size(MPI_Datatype type, int *size);

// NeoOS-specific, and the piece MPI leaves to `mpirun`: starts `size`
// copies of `path`, each with its rank and the world size in its argv,
// and returns their pids in `out_pids`. A rank calls MPI_Init, which
// reads those arguments back.
int MPI_Launch(const char *path, int size, int *out_pids);

#endif
