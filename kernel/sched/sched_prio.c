// kernel/sched/sched_prio.c -- the nice -> weight tables.
//
// These are ABI-fixed constants, identical to Linux's
// sched_prio_to_weight[] / sched_prio_to_wmult[]. Weight climbs ~1.25x
// per nice level so that a one-level difference is about a 10% CPU
// share shift. inv_weight is 2^32 / weight, precomputed so the hot
// path scales virtual time with a multiply-and-shift and never
// divides.
//
// Index = nice + 20  (nice -20 => 0, nice 0 => 20, nice 19 => 39).

#include "sched/sched_entity.h"

const int sched_prio_to_weight[40] = {
 /* -20 */  88761,  71755,  56483,  46273,  36291,
 /* -15 */  29154,  23254,  18705,  14949,  11916,
 /* -10 */   9548,   7620,   6100,   4904,   3906,
 /*  -5 */   3121,   2501,   1991,   1586,   1277,
 /*   0 */   1024,    820,    655,    526,    423,
 /*   5 */    335,    272,    215,    172,    137,
 /*  10 */    110,     87,     70,     56,     45,
 /*  15 */     36,     29,     23,     18,     15,
};

const uint32_t sched_prio_to_wmult[40] = {
 /* -20 */     48388,     59856,     76040,     92818,    118348,
 /* -15 */    147320,    184698,    229616,    287308,    360437,
 /* -10 */    449829,    563644,    704093,    875809,   1099582,
 /*  -5 */   1376151,   1717300,   2157191,   2708050,   3363326,
 /*   0 */   4194304,   5237765,   6557202,   8165337,  10153587,
 /*   5 */  12820798,  15790321,  19976592,  24970740,  31350126,
 /*  10 */  39045157,  49367440,  61356676,  76695844,  95443717,
 /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};
