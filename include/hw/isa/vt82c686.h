#ifndef HW_VT82C686_H
#define HW_VT82C686_H

#include "hw/pci/pci_device.h"
#include "qemu/audio.h"

#define TYPE_VT82C686B_ISA "vt82c686b-isa"
#define TYPE_VT82C686B_USB_UHCI "vt82c686b-usb-uhci"
#define TYPE_VT8231_ISA "vt8231-isa"
#define TYPE_VIA_AC97 "via-ac97"
#define TYPE_VIA_IDE "via-ide"
#define TYPE_VIA_MC97 "via-mc97"

typedef struct {
    uint8_t stat;
    uint8_t type;
    uint32_t base;
    uint32_t curr;
    uint32_t addr;
    uint32_t clen;
} ViaAC97SGDChannel;

/* SGD channels: playback at 0x00, capture at 0x10, FM at 0x20 */
#define VIA_AC97_SGD_PLAY     0
#define VIA_AC97_SGD_REC      1
#define VIA_AC97_SGD_CHANNELS 3

OBJECT_DECLARE_SIMPLE_TYPE(ViaAC97State, VIA_AC97);

struct ViaAC97State {
    PCIDevice dev;
    AudioBackend *audio_be;
    MemoryRegion sgd;
    MemoryRegion fm;
    MemoryRegion midi;
    SWVoiceOut *vo;
    SWVoiceIn *vi;
    ViaAC97SGDChannel sgd_ch[VIA_AC97_SGD_CHANNELS];
    uint32_t gpi_intr;
    uint16_t codec_regs[128];
    uint32_t ac97_cmd;
};

void via_isa_set_irq(PCIDevice *d, int n, int level);

#endif
