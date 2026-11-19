/*
 * Solaris 2.x on sun4m: halt the CPU in the idle loop
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * With one CPU, the idle thread of Solaris 2.5.1 to 9 runs this loop in
 * idle() at interrupt level 0:
 *
 *   while (ncpus == 1) {
 *       if (CPU->cpu_disp.disp_nrunnable != 0)
 *           swtch();
 *       else
 *           (*idle_cpu)();                   <- the translator's hook
 *   }
 *
 * idle_cpu is the CPU module's way to stop the processor. Only the
 * microSPARC-II module sets it, to a function that asks the power
 * management driver (pmc) to write the APC register, which QEMU turns
 * into a halt. On every other CPU, the SuperSPARC of the SPARCstation 10
 * and 20 among them, it is a function that just returns, so the loop
 * spins and keeps a host core busy.
 *
 * The translator finds the call through idle_cpu by the instructions
 * around it (gen_solaris_idle() in translate.c), and takes from them the
 * register and offset by which the loop loads disp_nrunnable. At the call
 * it calls helper_solaris_idle(), which halts the CPU when disp_nrunnable
 * is 0, interrupts are enabled at level 0 and none is pending, which is
 * where idle_cpu exists to stop the processor. On a machine with one CPU
 * only an interrupt handler can then make a thread runnable, and the board
 * turns this on only for one CPU (with more, Solaris runs another loop and
 * a CPU queues work for an idle one without an interrupt). The helper
 * reads disp_nrunnable again, as an interrupt may have come since the loop
 * loaded it.
 *
 * The CPU goes on at the call after the next interrupt (the clock ticks
 * at 100 Hz), so idle_cpu still runs when the loop finds no work, and the
 * guest counts its idle time as before.
 *
 * As in sunos_idle.c, the loop spins on for 100 us before the first halt,
 * as a disk request often completes that soon.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "cpu.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cpu-common.h"
#include "exec/helper-proto.h"
#include "qemu/rcu.h"
#include "system/memory.h"
#include "trace.h"

/*
 * Spin on for this long after the loop first found no work. A round that
 * comes more than SOLARIS_IDLE_GAP_NS after the one before means that the
 * guest left the loop between them.
 */
#define SOLARIS_IDLE_POLL_NS  (100 * SCALE_US)
#define SOLARIS_IDLE_GAP_NS   (20 * SCALE_US)

static bool solaris_idle_polled(CPUSPARCState *env)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (now - env->solaris_idle_last > SOLARIS_IDLE_GAP_NS) {
        env->solaris_idle_since = now;
    }
    env->solaris_idle_last = now;
    return now - env->solaris_idle_since >= SOLARIS_IDLE_POLL_NS;
}

/*
 * The host address of disp_nrunnable at addr in the kernel, or NULL if it
 * is not in RAM. The kernel maps its CPU structures for good, so this is
 * looked up once (by a page table walk: another CPU may have flushed the
 * TLB entry since the loop's load).
 */
static uint32_t *solaris_idle_host(CPUSPARCState *env, target_ulong addr)
{
    CPUState *cs = env_cpu(env);
    MemoryRegion *mr;
    hwaddr pa, xlat, len = 4;

    if (env->solaris_idle_nr && env->solaris_idle_va == addr) {
        return env->solaris_idle_nr;
    }
    if (addr & 3) {
        return NULL;
    }
    if (env->mmuregs[0] & MMU_E) {
        pa = sparc_cpu_get_phys_addr_debug(cs, addr);
        if (pa == -1) {
            return NULL;
        }
    } else {
        /*
         * MMU off: data addresses are physical. (The debug lookup tries an
         * instruction fetch first, which boot mode takes from the PROM.)
         */
        pa = addr;
    }
    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(cs->as, pa, &xlat, &len, false,
                                 MEMTXATTRS_UNSPECIFIED);
    if (!memory_region_is_ram(mr) || memory_region_is_ram_device(mr) ||
        len < 4) {
        return NULL;
    }
    env->solaris_idle_va = addr;
    env->solaris_idle_nr = qemu_map_ram_ptr(mr->ram_block, xlat);
    return env->solaris_idle_nr;
}

/* Read a 4-byte kernel variable; false if it is not in RAM */
static bool solaris_idle_read(CPUSPARCState *env, target_ulong addr,
                              uint32_t *val)
{
    uint32_t *host = solaris_idle_host(env, addr);

    if (!host) {
        return false;
    }
    *val = be32_to_cpu(qatomic_read(host));
    return true;
}

/*
 * Solaris is at the call through idle_cpu in its idle loop. Halt until the
 * next interrupt when no thread is runnable, and go on at the call then.
 */
void HELPER(solaris_idle)(CPUSPARCState *env, target_ulong pc,
                          target_ulong nrunnable)
{
    CPUState *cs = env_cpu(env);
    uint32_t val;

    /* Interrupts are enabled and none is pending */
    if (!env->psret || env->psrpil ||
        cpu_test_interrupt(cs, CPU_INTERRUPT_HARD)) {
        return;
    }
    if (!solaris_idle_read(env, nrunnable, &val) || val) {
        return;
    }
    if (!solaris_idle_polled(env)) {
        return;
    }

    trace_sparc_solaris_idle(pc);
    env->pc = pc;
    env->npc = pc + 4;
    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}

/*
 * With more than one CPU, the idle thread runs this loop instead:
 *
 *   for (;;) {
 *       while (CPU->cpu_flags & CPU_QUIESCED)
 *           ;
 *       if (CPU->cpu_disp.disp_nrunnable != 0) {
 *           swtch();
 *       } else {
 *           if (CPU->cpu_flags & CPU_OFFLINE)
 *               continue;
 *           if ((t = disp_getwork(CPU)) == NULL)   <- the translator's hook
 *               continue;
 *           swtch_to(t);
 *       }
 *   }
 *
 * It never calls idle_cpu. disp_getwork() takes a thread from the
 * partition's kernel preemption queue or from another CPU that has more
 * than one queued. No interrupt tells an idle CPU that it has work:
 * setbackdq() and setfrontdq() put a thread on its queue and count it in
 * disp_nrunnable, but cpu_resched() pokes only a CPU that runs a thread,
 * and only one CPU takes the clock.
 *
 * So the halt at the call of disp_getwork is bounded and comes with two
 * wake-ups of its own:
 *
 * - The CPU that queues the thread wakes the halted one. Solaris queues
 *   threads at a raised interrupt level and lowers it with a write to
 *   %psr afterwards; helper_wrpsr() calls solaris_idle_mp_poke(), which
 *   wakes every CPU halted here whose disp_nrunnable is no longer 0, and
 *   one of them when the writing CPU has more than one thread queued (the
 *   work that disp_getwork() would take).
 * - A timer wakes the CPU SOLARIS_IDLE_MP_NS after it halted, so work that
 *   reaches it in any other way (a thread on the kernel preemption queue,
 *   a race) waits at most that long. The CPU polls once then and halts
 *   again.
 *
 * Interrupts (cross-calls, pokes, the clock) wake it as with one CPU.
 * Either way the CPU goes on at the call of disp_getwork.
 */
#define SOLARIS_IDLE_MP_NS    (10 * SCALE_MS)

/* env->solaris_idle_mp_timeout */
enum {
    SOLARIS_IDLE_MP_NONE,
    SOLARIS_IDLE_MP_WOKEN,      /* the timer ended the halt */
    SOLARIS_IDLE_MP_LOOKED,     /* and disp_getwork has run once since */
};

/* The CPUs halted in the multiprocessor idle loop, by cpu_index */
static uint32_t solaris_idle_mp_halted;

static bool solaris_idle_mp_polled(CPUSPARCState *env)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (env->solaris_idle_mp_timeout) {
    case SOLARIS_IDLE_MP_WOKEN:
        /* The timer ended the halt: let disp_getwork look once */
        env->solaris_idle_mp_timeout = SOLARIS_IDLE_MP_LOOKED;
        env->solaris_idle_last = now;
        return false;
    case SOLARIS_IDLE_MP_LOOKED:
        /* It found nothing: halt again without polling */
        env->solaris_idle_mp_timeout = SOLARIS_IDLE_MP_NONE;
        if (now - env->solaris_idle_last <= SOLARIS_IDLE_GAP_NS) {
            env->solaris_idle_last = now;
            return true;
        }
        break;
    }
    if (now - env->solaris_idle_last > SOLARIS_IDLE_GAP_NS) {
        env->solaris_idle_since = now;
    }
    env->solaris_idle_last = now;
    return now - env->solaris_idle_since >= SOLARIS_IDLE_POLL_NS;
}

/* Called with the BQL held, so that the vCPU can't miss the kick */
static void solaris_idle_mp_kick(CPUState *cs, const char *why)
{
    CPUSPARCState *env = cpu_env(cs);

    if (!qatomic_read(&env->solaris_idle_kick)) {
        trace_sparc_solaris_idle_mp_wake(cs->cpu_index, why);
        qatomic_set(&env->solaris_idle_kick, true);
        qemu_cpu_kick(cs);
    }
}

static void solaris_idle_mp_timer(void *opaque)
{
    CPUState *cs = opaque;
    CPUSPARCState *env = cpu_env(cs);

    if (qatomic_read(&env->solaris_idle_mp_halted)) {
        env->solaris_idle_mp_timeout = SOLARIS_IDLE_MP_WOKEN;
        solaris_idle_mp_kick(cs,
            be32_to_cpu(qatomic_read(env->solaris_idle_nr)) ?
            "timer, work queued" : "timer");
    }
}

/* The CPU runs again: it is no longer halted in the idle loop */
void sparc_solaris_idle_mp_leave(CPUSPARCState *env)
{
    CPUState *cs = env_cpu(env);

    if (!qatomic_read(&env->solaris_idle_mp_halted)) {
        return;
    }
    qatomic_set(&env->solaris_idle_mp_halted, false);
    qatomic_and(&solaris_idle_mp_halted, ~(1u << cs->cpu_index));
    if (env->solaris_idle_timer) {
        timer_del(env->solaris_idle_timer);
    }
    qatomic_set(&env->solaris_idle_kick, false);
}

/*
 * The CPU of env writes %psr, with the BQL held. Wake the CPUs halted in
 * the idle loop that have work.
 */
void sparc_solaris_idle_mp_poke(CPUSPARCState *env)
{
    CPUState *self = env_cpu(env);
    uint32_t halted;
    bool steal;
    CPUState *cs;

    sparc_solaris_idle_mp_leave(env);
    /* Pairs with the barrier in helper_solaris_idle_mp() */
    smp_mb();
    halted = qatomic_read(&solaris_idle_mp_halted);
    if (!halted) {
        return;
    }
    /* More than one thread queued here: one for an idle CPU to take */
    steal = env->solaris_idle_nr &&
            be32_to_cpu(qatomic_read(env->solaris_idle_nr)) > 1;
    CPU_FOREACH(cs) {
        CPUSPARCState *e = cpu_env(cs);

        if (cs == self || !(halted & (1u << cs->cpu_index))) {
            continue;
        }
        if (be32_to_cpu(qatomic_read(e->solaris_idle_nr))) {
            solaris_idle_mp_kick(cs, "queued");
        } else if (steal) {
            solaris_idle_mp_kick(cs, "steal");
            steal = false;
        }
    }
}

void sparc_solaris_idle_mp_reset(CPUSPARCState *env)
{
    sparc_solaris_idle_mp_leave(env);
    env->solaris_idle_mp_timeout = SOLARIS_IDLE_MP_NONE;
    env->solaris_idle_nr = NULL;
    env->solaris_idle_va = 0;
}

/*
 * Solaris is at the call of disp_getwork in the multiprocessor idle loop.
 * Halt until there is work, the next interrupt or the timer, and go on at
 * the call then.
 */
void HELPER(solaris_idle_mp)(CPUSPARCState *env, target_ulong pc,
                             target_ulong nrunnable)
{
    CPUState *cs = env_cpu(env);
    uint32_t *nr;

    sparc_solaris_idle_mp_leave(env);
    if (cs->cpu_index >= 32) {
        return;
    }
    /* Interrupts are enabled and none is pending */
    if (!env->psret || env->psrpil ||
        cpu_test_interrupt(cs, CPU_INTERRUPT_HARD)) {
        return;
    }
    nr = solaris_idle_host(env, nrunnable);
    if (!nr || be32_to_cpu(qatomic_read(nr))) {
        return;
    }
    if (!solaris_idle_mp_polled(env)) {
        return;
    }

    if (!env->solaris_idle_timer) {
        env->solaris_idle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                               solaris_idle_mp_timer, cs);
    }
    qatomic_set(&env->solaris_idle_kick, false);
    qatomic_set(&env->solaris_idle_mp_halted, true);
    qatomic_or(&solaris_idle_mp_halted, 1u << cs->cpu_index);
    /*
     * Full barrier, pairs with the one in sparc_solaris_idle_mp_poke(): a
     * CPU that queues work from now on sees this one in the mask, or this
     * one sees its disp_nrunnable below.
     */
    smp_mb();
    if (be32_to_cpu(qatomic_read(nr))) {
        sparc_solaris_idle_mp_leave(env);
        return;
    }
    timer_mod(env->solaris_idle_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SOLARIS_IDLE_MP_NS);

    trace_sparc_solaris_idle_mp(cs->cpu_index, pc);
    env->pc = pc;
    env->npc = pc + 4;
    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}
