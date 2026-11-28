/*
 * DSP56001 core: what the code from Previous expects of its emulator
 *
 * Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
 *
 * The core in this folder is the ARAnyM DSP56001 emulation as the
 * Previous emulator has it. Its logging goes to qemu_log; its tracing and
 * its disassembler belong to Previous's debugger and are left out. The
 * hooks at the end are the NeXT's, in hw/audio/next-dsp.c.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef HW_AUDIO_DSP56K_GLUE_H
#define HW_AUDIO_DSP56K_GLUE_H

#include "qemu/log.h"

/* logging */
#define LOG_WARN    3
#define LOG_DEBUG   5
#define Log_Printf(level, ...) \
    do { \
        if ((level) <= LOG_WARN) { \
            qemu_log_mask(LOG_GUEST_ERROR, __VA_ARGS__); \
        } \
    } while (0)

/* Previous's tracing: QEMU's LOG_TRACE is a log mask, not used here */
#undef LOG_TRACE
#define LOG_TRACE(flags, ...)       do { } while (0)
#define LOG_TRACE_LEVEL(flags)      0
#define LOG_TRACE_DIRECT_INIT()     do { } while (0)
#define TraceFile                   stderr

/* Previous's debugger, which stops on DSP exceptions */
#define ExceptionDebugMask          0
#define EXCEPT_DSP                  1
#define REASON_DSP_EXCEPTION        0
#define DebugUI(reason)             do { } while (0)

/* Previous's status bar */
#define Statusbar_SetDspLed(on)     do { } while (0)

/* Previous's disassembler */
#define DSP_DISASM_MODE 0
#define DSP_TRACE_MODE  1

static inline void dsp56k_disasm_init(void)
{
}

static inline uint16_t dsp56k_disasm(int mode, FILE *out)
{
    return 0;
}

static inline const char *dsp56k_getInstructionText(void)
{
    return "";
}

static inline void dsp56k_disasm_reg_save(void)
{
}

static inline void dsp56k_disasm_reg_compare(FILE *out)
{
}

/* The NeXT's side, in next-dsp.c */
void DSP_HandleTXD(int set);
void DSP_InitSSI(uint32_t value);
void DSP_SsiTransmit_SC1(void);
void DSP_SsiTransmit_SC2(uint32_t frame);

#endif /* HW_AUDIO_DSP56K_GLUE_H */
