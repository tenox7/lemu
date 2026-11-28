#ifndef HW_I8257_H
#define HW_I8257_H

#include "hw/isa/isa.h"
#include "system/ioport.h"
#include "qom/object.h"

#define TYPE_I8257 "i8257"
OBJECT_DECLARE_SIMPLE_TYPE(I8257State, I8257)

typedef struct I8257Regs {
    int now[2];
    uint16_t base[2];
    uint8_t mode;
    uint8_t page;
    uint8_t pageh;
    uint8_t dack;
    uint8_t eop;
    IsaDmaTransferHandler transfer_handler;
    void *opaque;
} I8257Regs;

struct I8257State {
    /* <private> */
    ISADevice parent_obj;

    /* <public> */
    int32_t base;
    int32_t page_base;
    int32_t pageh_base;
    int32_t dshift;

    uint8_t status;
    uint8_t command;
    uint8_t mask;
    uint8_t flip_flop;
    I8257Regs regs[4];
    uint8_t counth[4];
    uint8_t stop[4][3];
    uint8_t ext_mode[4];
    /* the EISA extended mode register sets each channel's width */
    bool ext_mode_on;
    /* 82378/82374 scatter-gather, per channel */
    uint32_t sg_ptr[4];         /* descriptor table pointer */
    uint32_t sg_addr[4];        /* current buffer: address */
    uint32_t sg_len[4];         /* current buffer: length in bytes */
    uint8_t sg_status[4];       /* S/G status register */
    uint8_t sg_irq;             /* S/G interrupt status, one bit a channel */
    qemu_irq sg_irq_out;        /* IRQ13 */
    MemoryRegion channel_io;
    MemoryRegion cont_io;
    MemoryRegion counth_io;
    MemoryRegion stop_io;
    MemoryRegion ext_mode_io;

    QEMUBH *dma_bh;
    bool dma_bh_scheduled;
    int running;
    PortioList portio_page;
    PortioList portio_pageh;
};

void i8257_dma_init(Object *parent, ISABus *bus, bool high_page_enable);

/* 82378/82374 scatter-gather, for the EISA DMA registers at 0x410 */
#define I8257_SG_ACTIVE     0x01
#define I8257_SG_CURRENT    0x04
#define I8257_SG_IRQ13      0x20
#define I8257_SG_LAST       0x80

void i8257_sg_command(I8257State *d, int ichan, uint8_t cmd);
uint8_t i8257_sg_status(I8257State *d, int ichan);
void i8257_sg_set_pointer(I8257State *d, int ichan, uint32_t ptr);
uint32_t i8257_sg_get_pointer(I8257State *d, int ichan);
uint8_t i8257_sg_irq_status(I8257State *d);

#endif
