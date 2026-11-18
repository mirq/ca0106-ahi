/*
 * CA0106 hardware layer for AmigaOS 68k + openpci.library.
 *
 * Shared by the test tools and ca0106.audio.  Chip programming follows the
 * Linux ALSA ca0106 driver (GPL-2.0-or-later); see ca0106_regs.h.
 */
#ifndef CA0106_HW_H
#define CA0106_HW_H

#include <exec/types.h>
#include <exec/interrupts.h>

struct pci_dev;

/* DMA allocation strategies, in the order ca_dma_alloc() tries them */
#define CA_DMA_NONE         0
#define CA_DMA_OPENPCI3     1   /* AllocateDMAMemoryForBoard (openpci v3+) */
#define CA_DMA_OPENPCI2     2   /* pci_allocdma_mem(MEM_PCI) */
#define CA_DMA_PROMETHEUS   3   /* prometheus.library Prm_AllocDMABuffer */

/* ca_dma_alloc() flags: restrict the strategy (0 = automatic) */
#define CA_DMAF_ONLY_OPENPCI    1
#define CA_DMAF_ONLY_PROMETHEUS 2

/* Inputs of the WM8775 ADC mux as presented to the user */
#define CA_INPUT_LINE   0
#define CA_INPUT_MIC    1
#define CA_INPUT_AUX    2
#define CA_INPUT_TAD    3
#define CA_INPUTS       4

/* Hardware sample rates */
#define CA_RATE_48000   0
#define CA_RATE_44100   1
#define CA_RATE_96000   2
#define CA_RATE_192000  3

#define CA_DMA_PAGE     4096UL

struct CAHw;
typedef void (*CAIrqFunc)(struct CAHw *hw, ULONG extstat, ULONG ipr);

struct CAHw
{
    struct pci_dev     *dev;
    volatile UBYTE     *io;             /* CPU address of the I/O BAR */
    ULONG               serial;         /* subsystem vendor/id, e.g. 0x100a1102 */
    UWORD               libver;         /* openpci.library version */
    UWORD               librev;
    UBYTE               owned;          /* pci_obtain_card() succeeded */
    UBYTE               dma_kind;       /* CA_DMA_xxx */
    UBYTE               irq_added;
    UBYTE               pad0;

    /* DMA block (one per hardware instance) */
    APTR                dma_raw;        /* pointer as returned by the allocator */
    ULONG               dma_rawsize;    /* size as passed to the allocator */
    UBYTE              *dma_cpu;        /* 4 KB aligned CPU address */
    ULONG               dma_pci;        /* matching PCI bus address */
    ULONG               dma_size;       /* usable bytes from dma_cpu */

    /* Interrupt */
    struct Interrupt    irq;
    CAIrqFunc           irq_func;       /* called from the hardware interrupt */
    APTR                irq_data;
    volatile ULONG      irq_count;      /* our interrupts */
    volatile ULONG      irq_foreign;    /* calls that were not ours */
    ULONG               inte;           /* shadow of CA0106_INTE */

    /* Codec shadow state */
    UWORD               spi_reg[16];
    UBYTE               input;          /* CA_INPUT_xxx */
    UBYTE               gain_l, gain_r; /* WM8775 attenuation codes */
    UBYTE               pad1;
};

/* ---- PCI / resources ---- */
extern struct Library *OpenPciBase;
extern struct Library *PrometheusBase;

/* Opens openpci.library, finds and claims the card, enables I/O + busmaster.
 * Returns NULL on success or a static error string. */
const char *ca_open(struct CAHw *hw, BOOL force_unknown_model);
void        ca_close(struct CAHw *hw);
const char *ca_model_name(ULONG serial);

/* One DMA block per instance; size is rounded up to whole pages. */
const char *ca_dma_alloc(struct CAHw *hw, ULONG size, ULONG flags);
void        ca_dma_free(struct CAHw *hw);
const char *ca_dma_kind_name(UBYTE kind);

/* Installs the PCI interrupt server.  func runs in interrupt context and is
 * called only for CA0106 audio interrupts (already acknowledged). */
BOOL        ca_irq_add(struct CAHw *hw, CAIrqFunc func, APTR data, const char *name);
void        ca_irq_rem(struct CAHw *hw);

/* ---- Register access ---- */
#define CA_INLINE static inline __attribute__((always_inline))

CA_INLINE ULONG ca_swap32(ULONG v)
{
    __asm__("ror.w #8,%0\n\tswap %0\n\tror.w #8,%0" : "+d"(v));
    return v;
}
CA_INLINE UWORD ca_swap16(UWORD v)
{
    __asm__("ror.w #8,%0" : "+d"(v));
    return v;
}
CA_INLINE ULONG ca_inl(struct CAHw *hw, ULONG off)
{
    return ca_swap32(*(volatile ULONG *)(hw->io + off));
}
CA_INLINE void ca_outl(struct CAHw *hw, ULONG off, ULONG val)
{
    *(volatile ULONG *)(hw->io + off) = ca_swap32(val);
}
CA_INLINE void ca_outw(struct CAHw *hw, ULONG off, UWORD val)
{
    *(volatile UWORD *)(hw->io + off) = ca_swap16(val);
}
CA_INLINE void ca_outb(struct CAHw *hw, ULONG off, UBYTE val)
{
    *(volatile UBYTE *)(hw->io + off) = val;
}
/* Unlocked indexed access: interrupt code, or callers holding Disable() */
CA_INLINE ULONG ca_ptr_read_nl(struct CAHw *hw, ULONG reg, ULONG ch)
{
    ca_outl(hw, 0x00, (reg << 16) | ch);
    return ca_inl(hw, 0x04);
}
CA_INLINE void ca_ptr_write_nl(struct CAHw *hw, ULONG reg, ULONG ch, ULONG val)
{
    ca_outl(hw, 0x00, (reg << 16) | ch);
    ca_outl(hw, 0x04, val);
}
/* Locked versions (Disable/Enable around the PTR/DATA pair) */
ULONG ca_ptr_read(struct CAHw *hw, ULONG reg, ULONG ch);
void  ca_ptr_write(struct CAHw *hw, ULONG reg, ULONG ch, ULONG val);

/* ---- Codecs ---- */
BOOL  ca_spi_write(struct CAHw *hw, UWORD data);
BOOL  ca_i2c_write(struct CAHw *hw, UBYTE reg, UWORD value);

/* ---- Chip ---- */
BOOL  ca_chip_is_running(struct CAHw *hw);   /* HCFG audio enabled */
const char *ca_chip_init(struct CAHw *hw);   /* full init, analog out, front DAC */
void  ca_chip_stop_dma(struct CAHw *hw);     /* stop channels, mask audio irqs */
void  ca_chip_shutdown(struct CAHw *hw);     /* stop everything incl. codecs */

BOOL  ca_load_state(struct CAHw *hw);
void  ca_debug_counters(struct CAHw *hw, ULONG underruns, ULONG overloads);  
ULONG ca_read_debug_counters(struct CAHw *hw);        /* input/gain mirror, see .c */
void  ca_set_input(struct CAHw *hw, UBYTE input);
void  ca_set_input_gain(struct CAHw *hw, UBYTE l, UBYTE r);     /* WM8775 codes */
void  ca_set_monitor(struct CAHw *hw, UBYTE att);               /* 0x30=0dB, 0xff=off */
void  ca_set_output(struct CAHw *hw, UBYTE att);                /* 0x30=0dB, 0xff=mute */
UBYTE ca_get_monitor(struct CAHw *hw);

/* ---- Streams (host channel 0, front) ---- */
/* single buffer at buf_off inside the DMA block, bytes <= 65535;
 * irq at half (EXTINT_PLAY_HALF) and end (EXTINT_PLAY_FULL) */
void  ca_play_setup(struct CAHw *hw, ULONG buf_off, ULONG bytes, UBYTE rate, BOOL s32);
void  ca_play_start(struct CAHw *hw);
void  ca_play_stop(struct CAHw *hw);
ULONG ca_play_pointer(struct CAHw *hw);      /* byte offset being played */

/* capture: buffer split into two halves, irq at half and full */
void  ca_cap_setup(struct CAHw *hw, ULONG buf_off, ULONG bytes, UBYTE rate);
void  ca_cap_start(struct CAHw *hw);
void  ca_cap_stop(struct CAHw *hw);
ULONG ca_cap_pointer(struct CAHw *hw);

/* Conversions between AHI Fixed values and register codes */
LONG  ca_fixed_to_qdb(LONG fixed);           /* 20*log10(f) in 1/4 dB, f>0 */
UBYTE ca_fixed_to_att(LONG fixed);           /* mixer attenuation code, 0 -> 0xff */
UBYTE ca_fixed_to_adcgain(LONG fixed);       /* WM8775 code */

#endif /* CA0106_HW_H */
