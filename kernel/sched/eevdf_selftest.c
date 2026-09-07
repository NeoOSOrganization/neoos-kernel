// kernel/sched/eevdf_selftest.c -- boot-time proof of the EEVDF
// virtual-time arithmetic (kernel/sched/fair.c). Pure computation on a
// stack rq: no threads, no scheduling. Threaded weighted-share
// behaviour is exercised by the gauntlet's [smp] parallel/steal
// selftests, the musltest gauntlet and BusyBox.
//
// Prints one of:
//   [sched] eevdf selftest passed
//   [sched] eevdf selftest FAILED: <why>

#include "sched/rq.h"
#include "sched/sched_entity.h"
#include "drivers/char/serial.h"
#include <stdint.h>

// fair.c test hooks.
uint64_t eevdf_test_V(struct cfs_rq *cfs);
int64_t  eevdf_test_lag(struct cfs_rq *cfs, struct sched_entity *se);
void     eevdf_test_place(struct cfs_rq *cfs, struct sched_entity *se, int initial);
void     eevdf_test_enqueue(struct cfs_rq *cfs, struct sched_entity *se);
void     eevdf_test_dequeue(struct cfs_rq *cfs, struct sched_entity *se);
void     eevdf_test_set_curr(struct cfs_rq *cfs, struct sched_entity *se);
void     eevdf_test_advance(struct cfs_rq *cfs, uint64_t now_ns);
struct sched_entity *eevdf_test_pick(struct cfs_rq *cfs);
uint64_t eevdf_test_min_vruntime(struct cfs_rq *cfs);

static void fail(const char *why) {
    serial_write_string("[sched] eevdf selftest FAILED: ");
    serial_write_string(why);
    serial_write_string("\n");
}

static void se_init(struct sched_entity *se, int nice) {
    for (unsigned i = 0; i < sizeof(*se); i++) { ((uint8_t *)se)[i] = 0; }
    se->policy = SCHED_NORMAL;
    se->nice   = nice;   // enqueue_entity re-applies weight from se->nice
    set_load_weight(se, nice);
    se->slice = 0;   // -> base
}

// abs for i64
static int64_t i64abs(int64_t x) { return x < 0 ? -x : x; }

void eevdf_selftest(void) {
    struct rq rq;
    for (unsigned i = 0; i < sizeof(rq); i++) { ((uint8_t *)&rq)[i] = 0; }
    cfs_rq_init(&rq.cfs);
    struct cfs_rq *cfs = &rq.cfs;

    struct sched_entity a, b, cc, d;
    se_init(&a, 0);
    se_init(&b, 0);
    se_init(&cc, 5);    // lighter
    se_init(&d, -5);    // heavier

    // 1. Empty rq: V == min_vruntime (0).
    if (eevdf_test_V(cfs) != 0) { fail("V of empty rq != 0"); return; }

    // 2. Enqueue two equal-weight entities; V sits between their
    //    vruntimes (they were placed at V, so both ~equal).
    eevdf_test_advance(cfs, 0);
    eevdf_test_enqueue(cfs, &a);
    eevdf_test_enqueue(cfs, &b);
    if (cfs->nr_running != 2) { fail("nr_running != 2 after 2 enqueues"); return; }
    {
        int64_t la = eevdf_test_lag(cfs, &a);
        int64_t lb = eevdf_test_lag(cfs, &b);
        // lag conservation: weighted sum of lags == 0 (both weight 1024)
        if (i64abs(la + lb) > 2) { fail("lag not conserved (a,b)"); return; }
    }

    // 3. Run `a` as curr for 4 ms of real time; its vruntime advances
    //    ~1:1 (weight 1024). It should end with negative lag (ran more
    //    than its share), `b` with positive lag, summing to ~0.
    eevdf_test_set_curr(cfs, &a);
    eevdf_test_advance(cfs, 4 * 1000000ULL);
    {
        int64_t la = eevdf_test_lag(cfs, &a);
        int64_t lb = eevdf_test_lag(cfs, &b);
        if (la >= 0) { fail("curr that ran did not go negative in lag"); return; }
        if (lb <= 0) { fail("waiting entity did not gain positive lag"); return; }
        // weighted: la*w_a + lb*w_b ~ 0
        int64_t wsum = la * (int64_t)a.load.weight + lb * (int64_t)b.load.weight;
        // tolerance: a few weight-units of integer rounding
        if (i64abs(wsum) > 8 * (int64_t)a.load.weight) {
            fail("weighted lag sum drifted"); return;
        }
    }

    // 4. Heavier vs lighter: charge each the same real time; the
    //    lighter task's vruntime advances FASTER (weight in the
    //    denominator: vdelta = real * NICE_0 / weight).
    {
        struct rq r2; for (unsigned i=0;i<sizeof(r2);i++){((uint8_t*)&r2)[i]=0;}
        cfs_rq_init(&r2.cfs);
        struct cfs_rq *c2 = &r2.cfs;
        eevdf_test_advance(c2, 0);
        eevdf_test_enqueue(c2, &cc);   // nice +5  (weight 335)
        eevdf_test_enqueue(c2, &d);    // nice -5  (weight 3121)

        // cc runs from t=0 to t=10ms.
        eevdf_test_set_curr(c2, &cc);
        uint64_t cc_v0 = cc.vruntime;
        eevdf_test_advance(c2, 10 * 1000000ULL);
        uint64_t cc_adv = cc.vruntime - cc_v0;

        // d runs from t=10ms to t=20ms.
        eevdf_test_set_curr(c2, &d);
        uint64_t d_v0 = d.vruntime;
        eevdf_test_advance(c2, 20 * 1000000ULL);
        uint64_t d_adv = d.vruntime - d_v0;

        if (d_adv == 0 || cc_adv <= d_adv) {
            fail("lighter task's vruntime did not advance faster"); return;
        }
        // ratio ~ weight_d / weight_cc = 3121 / 335 ~ 9.3. Wide band.
        uint64_t ratio = cc_adv / d_adv;
        if (ratio < 4 || ratio > 20) {
            fail("weight ratio out of band"); return;
        }
    }

    // 5. min_vruntime is monotonic across a place/advance/dequeue run.
    {
        uint64_t prev = eevdf_test_min_vruntime(cfs);
        for (int i = 0; i < 50; i++) {
            eevdf_test_advance(cfs, (uint64_t)(i + 2) * 3 * 1000000ULL);
            uint64_t m = eevdf_test_min_vruntime(cfs);
            if ((int64_t)(m - prev) < 0) { fail("min_vruntime went backwards"); return; }
            prev = m;
        }
    }

    // 6. Wrap safety: seed everything near UINT64_MAX and rerun a short
    //    sequence -- the wrap-safe comparisons must still hold.
    {
        struct rq r3; for (unsigned i=0;i<sizeof(r3);i++){((uint8_t*)&r3)[i]=0;}
        cfs_rq_init(&r3.cfs);
        struct cfs_rq *c3 = &r3.cfs;
        c3->min_vruntime = 0xFFFFFFFFFFFFFF00ULL;

        struct sched_entity x, y;
        se_init(&x, 0);
        se_init(&y, 0);
        eevdf_test_advance(c3, 0);
        eevdf_test_enqueue(c3, &x);
        eevdf_test_enqueue(c3, &y);
        eevdf_test_set_curr(c3, &x);
        eevdf_test_advance(c3, 8 * 1000000ULL);   // x->vruntime wraps past 0
        int64_t lx = eevdf_test_lag(c3, &x);
        int64_t ly = eevdf_test_lag(c3, &y);
        if (lx >= 0 || ly <= 0) { fail("wrap: lag signs wrong"); return; }
        if (i64abs(lx + ly) > 2) { fail("wrap: lag not conserved"); return; }
        struct sched_entity *picked = eevdf_test_pick(c3);
        if (picked != &x && picked != &y) { fail("wrap: pick returned garbage"); return; }
    }

    serial_write_string("[sched] eevdf selftest passed\n");
}
