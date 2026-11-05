/*
 * HP-UX 10.20 and 9.05 on PA-RISC 1.1: halt the vCPU in the idle loop
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * An idle HP-UX spins in the scheduler's idle loop with interrupts enabled,
 * so every idle vCPU keeps a host core busy. This halts the vCPU instead,
 * until an interrupt comes or, with 10.20 on several CPUs, another vCPU
 * gives it work, which HP-UX does without an interrupt. The kernel of 9.05
 * has one CPU and its idle loop is simple, see helper_hpux9_idle() at the
 * end; the rest of this comment is about 10.20.
 *
 * The translator finds the loop by its instructions (gen_hpux_idle() in
 * translate.c; the B2352B kernel of 9 June 1996 is the only one known) and
 * calls helper_hpux_idle() in every round, after the tests of the stop and
 * interrupt-disable state. With r3 the per-processor structure (%cr24), r4
 * the processor number and dp the kernel's data pointer (%r27), a round
 * looks at:
 *
 *   dp[0x3070]            threads on the global queue
 *   r3[0x520], r3[0x524]  number of threads on the run queues of this
 *                         processor: the scheduler adds one with a plain
 *                         store when it queues a thread, with no interrupt.
 *                         The loop takes a thread when these counts allow
 *                         it; otherwise, every 20 us while they are not 0,
 *                         it locks the queues and looks. In a loop of fork
 *                         and exec most threads were found the second way.
 *   r3[0x9c]              a request for this processor
 *   dp[-0x1800 + 0x68]    the stop protocol
 *   dp[-0x1800 + 0x594]   starts the round again
 *   dp[-0x1800 + 0x598]   the owner: the number of the one idle processor
 *                         that scans the others (-1: nobody; the first one
 *                         to finish a round takes it, and any processor that
 *                         takes a thread frees it). It takes a thread from
 *                         the queue of a processor that runs another one:
 *                         r[j][0x520] > 0 and r[j][0x10c] != -1.
 *
 * The helper halts the vCPU only when none of these has work for it, and
 * only the owner looks at the other queues. A halted vCPU does not poll, so
 * it has to be woken when that changes, or it sees the work at its next
 * clock tick, 10 ms later (a loop of 800 runs of /usr/bin/true took 8 s
 * instead of 1.4 s): the translator finds the two stores of the scheduler
 * that queue a thread (gen_hpux_idle() again) and calls helper_hpux_bell()
 * after them, which wakes the vCPU that waits for that queue and, if there
 * is none, the owner. The wake-up is a flag that hppa_cpu_has_work() reads.
 *
 * Waking a host thread takes longer than a gap in the work often lasts, so
 * the vCPU first spins on for some rounds, as KVM's halt polling does. With
 * none, that loop of 800 runs took 1.6 s. Between 16 and 64 rounds made no
 * difference to it. A spin that adapts to the wake-ups was no faster and kept
 * the idle guest busy. With 32 rounds an idle guest with four CPUs uses about
 * 3% of one host core, where it used 400% before.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/core/cpu.h"
#include "cpu.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "accel/tcg/probe.h"
#include "exec/helper-proto.h"
#include "exec/icount.h"
#include "exec/tlb-flags.h"
#include "trace.h"

/*
 * Rounds of the idle loop in which the vCPU only looks, before it halts. An
 * interrupt or any other exception starts the count again. It counts rounds,
 * not time, so that a host that deschedules the vCPU thread for a while does
 * not keep it spinning.
 */
#define HPUX_IDLE_SPIN_ROUNDS   32

/* The per-processor structure (r3, %cr24) and what is in it */
#define PCPU_WORK_FLAG      0x09c
#define PCPU_THREAD         0x10c   /* -1 when no thread runs on it */
#define PCPU_RUNQ           0x518   /* r11 in the loop */
#define RUNQ_LEN_1          0x008   /* number of threads on the queues */
#define RUNQ_LEN_2          0x00c
#define PCPU_STRIDE         0x2ad8

/* The kernel's globals, relative to its data pointer (dp, r27) */
#define DP_GLOBAL_QUEUE     0x3070
#define DP_NCPU             (-0x2000 + 0x6e0)
#define DP_STOP_FLAG        (-0x1800 + 0x068)
#define DP_AGAIN_FLAG       (-0x1800 + 0x594)
#define DP_OWNER            (-0x1800 + 0x598)
#define DP_STEAL_FLAG       (-0x1800 + 0x5a0)
#define DP_PCPU_ARRAY       (-0x1800 + 0x6d4)

/*
 * Read a word of the kernel's data as the idle loop would, without raising
 * an exception: all these words are in RAM that the loop has just read.
 */
static bool hpux_idle_ld(CPUHPPAState *env, uint32_t off, uint32_t *val)
{
    vaddr gva = hppa_form_gva(env, env->sr[4 + (off >> 30)], off);
    void *host = NULL;
    int flags;

    if (off & 3) {
        return false;
    }
    flags = probe_access_flags(env, gva, 4, MMU_DATA_LOAD,
                               cpu_mmu_index(env_cpu(env), false), true,
                               &host, 0);
    if (!host || (flags & (TLB_INVALID_MASK | TLB_MMIO))) {
        return false;
    }
    *val = ldl_be_p(host);
    return true;
}

/*
 * Is the loop idle: nothing it would take within a few rounds, no interrupt
 * due, and every word readable? Anything unexpected counts as not idle.
 */
static bool hpux_idle_nothing_to_do(CPUHPPAState *env)
{
    CPUState *cs = env_cpu(env);
    uint32_t pcpu = env->gr[3];
    uint32_t id = env->gr[4];
    uint32_t dp = env->gr[27];
    uint32_t runq = pcpu + PCPU_RUNQ;
    uint32_t len1, len2, flag, owner;

    /*
     * HP-UX leaves a bit pending in EIRR for as long as a second, masked in
     * EIEM, and eval_interrupt() keeps CPU_INTERRUPT_HARD set meanwhile. Such
     * a bit cannot wake a halted vCPU: only the guest can unmask it. So look
     * at the bits that are pending and enabled, not at that flag.
     */
    if (icount_enabled() || !(env->psw & PSW_I) ||
        (qatomic_read(&env->cr[CR_EIRR]) & env->cr[CR_EIEM]) ||
        cpu_test_interrupt(cs, CPU_INTERRUPT_NMI) ||
        env->gr[11] != runq) {
        return false;
    }

    /*
     * Threads on the queues of this processor. One that the counts in the
     * loop do not give away is found by its look at the queues every 20 us,
     * while they are not empty.
     */
    if (!hpux_idle_ld(env, runq + RUNQ_LEN_1, &len1) ||
        !hpux_idle_ld(env, runq + RUNQ_LEN_2, &len2) || len1 || len2) {
        return false;
    }

    /* The flags, and the global queue */
    if (!hpux_idle_ld(env, pcpu + PCPU_WORK_FLAG, &flag) || flag ||
        !hpux_idle_ld(env, dp + DP_STOP_FLAG, &flag) || flag ||
        !hpux_idle_ld(env, dp + DP_AGAIN_FLAG, &flag) || flag ||
        !hpux_idle_ld(env, dp + DP_GLOBAL_QUEUE, &flag) || flag) {
        return false;
    }

    /*
     * The owner of the scan of the others (or the one that is about to be)
     * takes a thread from the queue of a processor that runs another one.
     */
    if (!hpux_idle_ld(env, dp + DP_OWNER, &owner)) {
        return false;
    }
    if (owner == id || owner == -1) {
        uint32_t n, base, steal;

        if (!hpux_idle_ld(env, dp + DP_NCPU, &n) ||
            !hpux_idle_ld(env, dp + DP_PCPU_ARRAY, &base) ||
            !hpux_idle_ld(env, dp + DP_STEAL_FLAG, &steal) || n > 64) {
            return false;
        }
        for (uint32_t j = 0; j < n; j++) {
            uint32_t other = base + j * PCPU_STRIDE;
            uint32_t len, thread;

            if (j == id) {
                continue;
            }
            if (!hpux_idle_ld(env, other + PCPU_RUNQ + RUNQ_LEN_1, &len) ||
                !hpux_idle_ld(env, other + PCPU_THREAD, &thread) ||
                ((int32_t)len > 0 && (thread != -1 || steal))) {
                return false;
            }
        }
    }
    return true;
}

/*
 * The idle loop has come to the check of its run queue. Halt the vCPU until
 * the next interrupt or bell, if there is nothing to do.
 */
void HELPER(hpux_idle)(CPUHPPAState *env)
{
    HPPACPU *cpu = env_archcpu(env);
    CPUState *cs = env_cpu(env);

    if (!cpu->hpux_idle) {
        return;
    }

    /* The vCPU runs, so there is nobody to wake */
    if (qatomic_read(&cpu->hpux_idle_pcpu)) {
        qatomic_set(&cpu->hpux_idle_pcpu, 0);
        qatomic_set(&cpu->hpux_idle_wake, false);
    }

    if (cpu->hpux_idle_ints != cpu->hpux_idle_seen) {
        cpu->hpux_idle_seen = cpu->hpux_idle_ints;
        cpu->hpux_idle_rounds = 0;
    }
    if (!hpux_idle_nothing_to_do(env)) {
        cpu->hpux_idle_rounds = 0;
        return;
    }
    if (++cpu->hpux_idle_rounds <= HPUX_IDLE_SPIN_ROUNDS) {
        return;
    }

    /*
     * Tell the vCPUs that queue threads which processor this one waits for,
     * then look once more. A thread queued from now on is either seen by the
     * look or wakes the vCPU: the other side stores the queue length first
     * and looks here second, this side the other way round, with a barrier
     * in between (qatomic_xchg() is one).
     */
    qatomic_set(&cpu->hpux_idle_id, (uint32_t)env->gr[4]);
    qatomic_set(&cpu->hpux_idle_pcpu, (uint32_t)env->gr[3]);
    if (qatomic_xchg(&cpu->hpux_idle_wake, false) ||
        !hpux_idle_nothing_to_do(env)) {
        qatomic_set(&cpu->hpux_idle_pcpu, 0);
        return;
    }

    trace_hppa_hpux_idle(cs->cpu_index, env->iaoq_f);
    cs->halted = 1;
    HELPER(excp)(env, EXCP_HALTED);
}

/*
 * The idle loop of the HP-UX 9.05 kernel (loop_till_process_found_to_run()):
 * with interrupts on, it adds up the four run queue counts of mpproc_info
 * (r4 to r7 point at it, the counts are at 0x10c, 0x110, 0x114 and 0x118),
 * calls check_watchdog() and starts again until the sum is not 0. The one
 * thing check_watchdog() does is to look at EIRR bit 28, one of the bits that
 * the idle loop keeps masked in EIEM (it is 0x8 in the register): it
 * runs a soft interrupt that is posted there. Nothing else gives the loop work
 * but an interrupt, as there is one CPU: a handler queues the process.
 */
#define HPUX9_RUNQ_COUNTS   0x10c
#define HPUX9_EIRR_POLLED   0x00000008

static bool hpux9_idle_nothing_to_do(CPUHPPAState *env)
{
    CPUState *cs = env_cpu(env);
    uint32_t count;

    /* An interrupt it could take, or the soft interrupt that it polls for */
    if (icount_enabled() || !(env->psw & PSW_I) ||
        (qatomic_read(&env->cr[CR_EIRR]) &
         (env->cr[CR_EIEM] | HPUX9_EIRR_POLLED)) ||
        cpu_test_interrupt(cs, CPU_INTERRUPT_NMI)) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        if (!hpux_idle_ld(env, (uint32_t)env->gr[4 + i] + HPUX9_RUNQ_COUNTS +
                          4 * i, &count) || count) {
            return false;
        }
    }
    return true;
}

/*
 * The idle loop has come to the sum of the run queue counts. Halt the vCPU
 * until the next interrupt, if there is nothing to do.
 */
void HELPER(hpux9_idle)(CPUHPPAState *env)
{
    HPPACPU *cpu = env_archcpu(env);
    CPUState *cs = env_cpu(env);

    if (!cpu->hpux_idle) {
        return;
    }

    /* The vCPU runs, it does not wait: see hppa_cpu_has_work() */
    qatomic_set(&cpu->hpux_idle_pcpu, 0);

    if (cpu->hpux_idle_ints != cpu->hpux_idle_seen) {
        cpu->hpux_idle_seen = cpu->hpux_idle_ints;
        cpu->hpux_idle_rounds = 0;
    }
    if (!hpux9_idle_nothing_to_do(env)) {
        cpu->hpux_idle_rounds = 0;
        return;
    }
    if (++cpu->hpux_idle_rounds <= HPUX_IDLE_SPIN_ROUNDS) {
        return;
    }

    /*
     * Say that the vCPU waits (any value but 0, and no processor structure of
     * 10.20 has the address 1), so that it wakes only for an interrupt it can
     * take, and look once more: an interrupt that came in between has set
     * CPU_INTERRUPT_HARD, which counts from now on.
     */
    qatomic_set(&cpu->hpux_idle_pcpu, 1);
    smp_mb();
    if (!hpux9_idle_nothing_to_do(env)) {
        qatomic_set(&cpu->hpux_idle_pcpu, 0);
        return;
    }

    trace_hppa_hpux_idle(cs->cpu_index, env->iaoq_f);
    cs->halted = 1;
    HELPER(excp)(env, EXCP_HALTED);
}

/* Wake the vCPU @cs, if it waits for work and nobody has woken it yet */
static bool hpux_idle_kick(CPUState *me, CPUState *cs)
{
    HPPACPU *cpu = HPPA_CPU(cs);
    uint32_t waits = qatomic_read(&cpu->hpux_idle_pcpu);

    if (!waits || qatomic_cmpxchg(&cpu->hpux_idle_pcpu, waits, 0) != waits) {
        return false;
    }

    /* The flag and the signal go together, as for any wake-up of a vCPU */
    BQL_LOCK_GUARD();

    trace_hppa_hpux_bell(me->cpu_index, cs->cpu_index, waits);
    qatomic_set(&cpu->hpux_idle_wake, true);
    qemu_cpu_kick(cs);
    return true;
}

/*
 * The scheduler has just queued a thread on a run queue of the processor
 * structure r24 - 0x518, which is how it gives work to an idle processor:
 * there is no interrupt. Wake the vCPU that waits for that processor. If
 * there is none, the processor runs another thread, and the idle processor
 * that owns the scan of the others' queues has to take the new thread: wake
 * that one.
 */
void HELPER(hpux_bell)(CPUHPPAState *env, target_ulong r24)
{
    uint32_t pcpu = (uint32_t)r24 - PCPU_RUNQ;
    CPUState *me = env_cpu(env);
    CPUState *cs;
    uint32_t owner;
    bool woke = false;

    /*
     * The store of the queue length comes before this, the loads of the
     * waiting vCPUs' state after it. This pairs with the store of
     * hpux_idle_pcpu followed by qatomic_xchg() in helper_hpux_idle(), which
     * comes before the idle loop's load of the queue length.
     */
    smp_mb();

    CPU_FOREACH(cs) {
        if (cs != me && qatomic_read(&HPPA_CPU(cs)->hpux_idle_pcpu) == pcpu) {
            woke |= hpux_idle_kick(me, cs);
        }
    }
    if (woke || !hpux_idle_ld(env, (uint32_t)env->gr[27] + DP_OWNER, &owner)) {
        return;
    }
    CPU_FOREACH(cs) {
        if (cs != me &&
            (owner == -1 ||
             qatomic_read(&HPPA_CPU(cs)->hpux_idle_id) == owner) &&
            hpux_idle_kick(me, cs)) {
            return;
        }
    }
}

/*
 * Called at the start of each round of the counting loop in calibrate_cr16()
 * of the X server, see hpux_x11_calibrate[] in translate.c. The server wants
 * to see at least a millisecond go by in the round; a real CPU of that time
 * needs 6, ours less than 1. Wait a while: the timer of the guest keeps going
 * and, being whole milliseconds, the round then measures about the time it
 * took, so the server learns the frequency of the timer to within a few
 * percent. The 6 rounds add up to 0.12 seconds of waiting, once per start of
 * the server.
 */
#define HPUX_X11_PACE_US 20000

void HELPER(hpux_x11_pace)(CPUHPPAState *env)
{
    CPUState *cs = env_cpu(env);

    trace_hppa_hpux_pace(cs->cpu_index);
    g_usleep(HPUX_X11_PACE_US);
}
