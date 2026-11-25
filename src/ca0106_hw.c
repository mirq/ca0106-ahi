/*
 * CA0106 hardware layer for AmigaOS 68k + openpci.library.
 *
 * Chip and codec programming is ported from the Linux ALSA driver
 * sound/pci/ca0106 (Copyright (c) 2004 James Courtier-Dutton and others),
 * GPL-2.0-or-later.  This file is distributed under the same license.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>

#include <libraries/openpci.h>
#include <inline/openpci.h>
#include <inline/prometheus_dma.h>

#include "ca0106_regs.h"
#include "ca0106_hw.h"

#ifndef MEM_PCI
#define MEM_PCI             0x1
#endif
#ifndef MEM_NONCACHEABLE
#define MEM_NONCACHEABLE    0x2
#endif

struct Library *OpenPciBase;
struct Library *PrometheusBase;

/* ------------------------------------------------------------------------ */
/* Interrupt glue: exec wants the Z flag cleared when the server handled    */
/* the interrupt, which C cannot express, hence this stub.                  */
/* ------------------------------------------------------------------------ */

ULONG ca_irq_entry(struct CAHw *hw) __attribute__((used));

__asm__(
"	.text\n"
"	.even\n"
"	.globl	_ca_irq_stub\n"
"_ca_irq_stub:\n"
"	move.l	a1,-(sp)\n"
"	jsr	_ca_irq_entry\n"
"	addq.l	#4,sp\n"
"	tst.l	d0\n"
"	rts\n");

extern void ca_irq_stub(void);

ULONG ca_irq_entry(struct CAHw *hw)
{
    ULONG ipr, ext = 0;

    ipr = ca_inl(hw, CA0106_IPR);
    if (ipr == 0xffffffffUL || (ipr & hw->inte) == 0)
    {
        hw->irq_foreign++;
        return 0;
    }
    ipr &= hw->inte;
    if (ipr & IPR_AI)
    {
        ext = ca_ptr_read_nl(hw, EXTENDED_INT, 0);
        ca_ptr_write_nl(hw, EXTENDED_INT, 0, ext);
    }
    ca_outl(hw, CA0106_IPR, ipr);
    hw->irq_count++;
    if (hw->irq_func)
        hw->irq_func(hw, ext, ipr);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Locked indexed register access                                           */
/* ------------------------------------------------------------------------ */

ULONG ca_ptr_read(struct CAHw *hw, ULONG reg, ULONG ch)
{
    ULONG v;
    Disable();
    v = ca_ptr_read_nl(hw, reg, ch);
    Enable();
    return v;
}

void ca_ptr_write(struct CAHw *hw, ULONG reg, ULONG ch, ULONG val)
{
    Disable();
    ca_ptr_write_nl(hw, reg, ch, val);
    Enable();
}

static void ca_ptr_modify(struct CAHw *hw, ULONG reg, ULONG ch, ULONG clr, ULONG set)
{
    Disable();
    ca_ptr_write_nl(hw, reg, ch, (ca_ptr_read_nl(hw, reg, ch) & ~clr) | set);
    Enable();
}

static void ca_io_modify(struct CAHw *hw, ULONG off, ULONG clr, ULONG set)
{
    Disable();
    ca_outl(hw, off, (ca_inl(hw, off) & ~clr) | set);
    Enable();
}

/* ------------------------------------------------------------------------ */
/* PCI                                                                      */
/* ------------------------------------------------------------------------ */

const char *ca_model_name(ULONG serial)
{
    switch (serial)
    {
        case SERIAL_SB0570:  return "Audigy SE [SB0570]";
        case SERIAL_SB0570A: return "Audigy SE OEM [SB0570a]";
        default:             return "unknown CA0106 board";
    }
}

const char *ca_open(struct CAHw *hw, BOOL force_unknown_model)
{
    UWORD cmd;
    ULONG bar;

    if (!OpenPciBase)
    {
        OpenPciBase = OpenLibrary("openpci.library", 2);
        if (!OpenPciBase)
            return "cannot open openpci.library v2+";
    }
    hw->libver = OpenPciBase->lib_Version;
    hw->librev = OpenPciBase->lib_Revision;

    hw->dev = pci_find_device(CA0106_VENDOR, CA0106_DEVICE, NULL);
    if (!hw->dev)
        return "no CA0106 (1102:0007) found on the PCI bus";

    hw->serial = pci_read_config_long(PCI_SUBSYSTEM_VENDOR_ID, hw->dev);
    if (hw->serial != SERIAL_SB0570 && hw->serial != SERIAL_SB0570A &&
        !force_unknown_model)
        return "card is not an Audigy SE (SB0570); unsupported codec setup";

    bar = hw->dev->base_address[0];
    if (!bar)
        return "CA0106 BAR0 is not mapped";
    hw->io = (volatile UBYTE *)(bar & ~3UL);

    if (!pci_obtain_card(hw->dev))
        return "card is in use by another driver (pci_obtain_card failed)";
    hw->owned = 1;

    cmd = pci_read_config_word(PCI_COMMAND, hw->dev);
    cmd |= PCI_COMMAND_IO | PCI_COMMAND_MASTER;
    pci_write_config_word(PCI_COMMAND, cmd, hw->dev);
    pci_set_master(hw->dev);
    return NULL;
}

void ca_close(struct CAHw *hw)
{
    ca_irq_rem(hw);
    ca_dma_free(hw);
    if (hw->owned && OpenPciBase)
        pci_release_card(hw->dev);
    hw->owned = 0;
    hw->dev = NULL;
    if (PrometheusBase)
    {
        CloseLibrary(PrometheusBase);
        PrometheusBase = NULL;
    }
    if (OpenPciBase)
    {
        CloseLibrary(OpenPciBase);
        OpenPciBase = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* DMA memory                                                               */
/* ------------------------------------------------------------------------ */

const char *ca_dma_kind_name(UBYTE kind)
{
    switch (kind)
    {
        case CA_DMA_OPENPCI3:   return "openpci AllocateDMAMemoryForBoard";
        case CA_DMA_OPENPCI2:   return "openpci pci_allocdma_mem(MEM_PCI)";
        case CA_DMA_PROMETHEUS: return "prometheus Prm_AllocDMABuffer";
        default:                return "none";
    }
}

static BOOL ca_dma_finish(struct CAHw *hw, APTR raw, ULONG rawsize, ULONG need, UBYTE kind)
{
    ULONG cpu = ((ULONG)raw + CA_DMA_PAGE - 1) & ~(CA_DMA_PAGE - 1);
    ULONG phys;

    if (cpu + need > (ULONG)raw + rawsize)
        return FALSE;
    if (kind == CA_DMA_PROMETHEUS)
        phys = (ULONG)Prm_GetPhysicalAddress((APTR)cpu);
    else
        phys = (ULONG)pci_logic_to_physic_addr((APTR)cpu, hw->dev);
    if (!phys)
        return FALSE;

    hw->dma_raw = raw;
    hw->dma_rawsize = rawsize;
    hw->dma_cpu = (UBYTE *)cpu;
    hw->dma_pci = phys;
    hw->dma_size = need;
    hw->dma_kind = kind;
    return TRUE;
}

static void ca_dma_release(struct CAHw *hw, UBYTE kind, APTR raw, ULONG rawsize)
{
    switch (kind)
    {
        case CA_DMA_OPENPCI3:
            ReleaseDMAMemoryForBoard(hw->dev, raw, rawsize);
            break;
        case CA_DMA_OPENPCI2:
            pci_freedma_mem(raw, rawsize);
            break;
        case CA_DMA_PROMETHEUS:
            Prm_FreeDMABuffer(raw, rawsize);
            break;
    }
}

const char *ca_dma_alloc(struct CAHw *hw, ULONG size, ULONG flags)
{
    ULONG need = (size + CA_DMA_PAGE - 1) & ~(CA_DMA_PAGE - 1);
    APTR raw;
    ULONG i;

    if (hw->dma_kind != CA_DMA_NONE)
        return "DMA memory already allocated";

    if (!(flags & CA_DMAF_ONLY_PROMETHEUS))
    {
        if (hw->libver >= 3)
        {
            /* new openpci: no alignment guarantee, so over-allocate */
            raw = AllocateDMAMemoryForBoard(hw->dev, need + CA_DMA_PAGE,
                                            MEM_PCI | MEM_NONCACHEABLE);
            if (!raw)
                raw = AllocateDMAMemoryForBoard(hw->dev, need + CA_DMA_PAGE, MEM_PCI);
            if (raw)
            {
                if (ca_dma_finish(hw, raw, need + CA_DMA_PAGE, need, CA_DMA_OPENPCI3))
                    goto done;
                ReleaseDMAMemoryForBoard(hw->dev, raw, need + CA_DMA_PAGE);
            }
        }
        else
        {
            /* OpenPCI 2.x: on Prometheus this is expected to land in the
             * page-aligned Prometheus.card arena; fall back to over-allocation */
            raw = pci_allocdma_mem(need, MEM_PCI);
            if (raw)
            {
                if (ca_dma_finish(hw, raw, need, need, CA_DMA_OPENPCI2))
                    goto done;
                pci_freedma_mem(raw, need);
            }
            raw = pci_allocdma_mem(need + CA_DMA_PAGE, MEM_PCI);
            if (raw)
            {
                if (ca_dma_finish(hw, raw, need + CA_DMA_PAGE, need, CA_DMA_OPENPCI2))
                    goto done;
                pci_freedma_mem(raw, need + CA_DMA_PAGE);
            }
        }
    }

    if (!(flags & CA_DMAF_ONLY_OPENPCI) && hw->libver < 3)
    {
        /* The new openpci disables the vendor libraries, so only try this
         * next to OpenPCI 2.x. */
        if (!PrometheusBase)
            PrometheusBase = OpenLibrary("prometheus.library", 2);
        if (PrometheusBase)
        {
            raw = Prm_AllocDMABuffer(need);
            if (raw)
            {
                if (ca_dma_finish(hw, raw, need, need, CA_DMA_PROMETHEUS))
                    goto done;
                Prm_FreeDMABuffer(raw, need);
            }
        }
    }
    return "no PCI DMA memory available (check Prometheus DMASIZE tooltype)";

done:
    for (i = 0; i < hw->dma_size; i += 4)
        *(volatile ULONG *)(hw->dma_cpu + i) = 0;
    CacheClearE(hw->dma_cpu, hw->dma_size, CACRF_ClearD);
    return NULL;
}

void ca_dma_free(struct CAHw *hw)
{
    if (hw->dma_kind == CA_DMA_NONE)
        return;
    ca_dma_release(hw, hw->dma_kind, hw->dma_raw, hw->dma_rawsize);
    hw->dma_kind = CA_DMA_NONE;
    hw->dma_raw = NULL;
    hw->dma_cpu = NULL;
    hw->dma_pci = 0;
    hw->dma_size = 0;
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                               */
/* ------------------------------------------------------------------------ */

BOOL ca_irq_add(struct CAHw *hw, CAIrqFunc func, APTR data, const char *name)
{
    if (hw->irq_added)
        return TRUE;
    hw->irq_func = func;
    hw->irq_data = data;
    hw->inte = 0;
    ca_outl(hw, CA0106_INTE, 0);
    ca_outl(hw, CA0106_IPR, 0xffffffffUL);

    hw->irq.is_Node.ln_Type = NT_INTERRUPT;
    hw->irq.is_Node.ln_Pri = 0;
    hw->irq.is_Node.ln_Name = (char *)name;
    hw->irq.is_Data = hw;
    hw->irq.is_Code = (void (*)(void))ca_irq_stub;
    if (!pci_add_intserver(&hw->irq, hw->dev))
        return FALSE;
    hw->irq_added = 1;

    hw->inte = INTE_AI | INTE_PCI;
    ca_outl(hw, CA0106_INTE, hw->inte);
    return TRUE;
}

void ca_irq_rem(struct CAHw *hw)
{
    if (!hw->irq_added)
        return;
    ca_ptr_write(hw, EXTENDED_INT_MASK, 0, 0);
    hw->inte = 0;
    ca_outl(hw, CA0106_INTE, 0);
    ca_outl(hw, CA0106_IPR, 0xffffffffUL);
    pci_rem_intserver(&hw->irq, hw->dev);
    hw->irq_added = 0;
    hw->irq_func = NULL;
}

/* ------------------------------------------------------------------------ */
/* Codecs                                                                   */
/* ------------------------------------------------------------------------ */

/* WM8768 DAC through the CA0106 SPI master */
BOOL ca_spi_write(struct CAHw *hw, UWORD data)
{
    ULONG tmp, reset, set;
    int n;

    tmp = ca_ptr_read(hw, SPI, 0);
    reset = (tmp & ~0x3ffffUL) | 0x20000UL;
    set = reset | 0x10000UL;
    ca_ptr_write(hw, SPI, 0, reset | data);
    (void)ca_ptr_read(hw, SPI, 0);
    ca_ptr_write(hw, SPI, 0, set | data);
    /* Linux polls 100 x 10 us; one PCI read costs ~1 us here */
    for (n = 0; n < 2000; n++)
    {
        if (!(ca_ptr_read(hw, SPI, 0) & 0x10000UL))
            break;
    }
    if (n == 2000)
        return FALSE;
    ca_ptr_write(hw, SPI, 0, reset | data);
    (void)ca_ptr_read(hw, SPI, 0);
    return TRUE;
}

/* WM8775 ADC (write only) */
BOOL ca_i2c_write(struct CAHw *hw, UBYTE reg, UWORD value)
{
    ULONG status = 0;
    int retry, timeout;

    if (reg > 0x7f || value > 0x1ff)
        return FALSE;
    ca_ptr_write(hw, I2C_D1, 0, ((ULONG)reg << 25) | ((ULONG)value << 16));
    for (retry = 0; retry < 10; retry++)
    {
        ca_ptr_write(hw, I2C_A, 0, I2C_A_ADC_LAST | I2C_A_ADC_START | I2C_A_ADC_ADD);
        for (timeout = 0; timeout < 1000; timeout++)
        {
            status = ca_ptr_read(hw, I2C_A, 0);
            if (!(status & I2C_A_ADC_START))
                break;
        }
        /* still busy after the poll: not a success, try again */
        if (status & I2C_A_ADC_START)
            continue;
        if (!(status & I2C_A_ADC_ABORT))
            return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------ */
/* Chip                                                                     */
/* ------------------------------------------------------------------------ */

static const UWORD spi_dac_init[] =
{
    SPI_REG(SPI_LDA1_REG,   SPI_DA_BIT_0dB),
    SPI_REG(SPI_RDA1_REG,   SPI_DA_BIT_0dB),
    SPI_REG(SPI_PL_REG,     SPI_PL_BIT_L_L | SPI_PL_BIT_R_R | SPI_IZD_BIT),
    SPI_REG(SPI_FMT_REG,    SPI_FMT_BIT_I2S | SPI_IWL_BIT_24),
    SPI_REG(SPI_LDA2_REG,   SPI_DA_BIT_0dB),
    SPI_REG(SPI_RDA2_REG,   SPI_DA_BIT_0dB),
    SPI_REG(SPI_LDA3_REG,   SPI_DA_BIT_0dB),
    SPI_REG(SPI_RDA3_REG,   SPI_DA_BIT_0dB),
    SPI_REG(SPI_MASTDA_REG, SPI_DA_BIT_0dB),
    SPI_REG(9,              0x00),
    SPI_REG(SPI_MS_REG,     SPI_DACD0_BIT | SPI_DACD1_BIT | SPI_DACD2_BIT),
    SPI_REG(12,             0x00),
    SPI_REG(SPI_LDA4_REG,   SPI_DA_BIT_0dB),
    SPI_REG(SPI_RDA4_REG,   SPI_DA_BIT_0dB | SPI_DA_BIT_UPDATE),
    SPI_REG(SPI_DACD4_REG,  SPI_DACD4_BIT),
};

static const UBYTE i2c_adc_init[][2] =
{
    { ADC_RESET,      0x00 },
    { ADC_TIMEOUT,    0x00 },
    { ADC_IFC_CTRL,   0x22 },
    { ADC_MASTER,     0x22 },
    { ADC_POWER,      0x08 },
    { ADC_ATTEN_ADCL, ADC_GAIN_0DB },
    { ADC_ATTEN_ADCR, ADC_GAIN_0DB },
    { ADC_ALC_CTRL1,  0x7b },   /* ALC off: plain gain from ATTEN regs */
    { ADC_ALC_CTRL2,  0x00 },
    { ADC_ALC_CTRL3,  0x32 },
    { ADC_NOISE_CTRL, 0x00 },
    { ADC_LIMIT_CTRL, 0xa6 },
};

/* SB0570 GPIO (from the 4Front OSS driver): GPO0 analog out, GPO1 output
 * enable, GPO2 (bit 10) mic bias + mic path, GPO3 SPDIF-IO disable.
 * The front jack is shared between Line and Mic. */
static const UBYTE adc_mux_bits[CA_INPUTS] =
{
    ADC_MUX_LINEIN, ADC_MUX_MIC, ADC_MUX_AUX, ADC_MUX_PHONE
};

BOOL ca_chip_is_running(struct CAHw *hw)
{
    return (ca_inl(hw, CA0106_HCFG) & HCFG_AUDIOENABLE) != 0;
}

const char *ca_chip_init(struct CAHw *hw)
{
    ULONG ch, n;
    const char *err = NULL;
    UBYTE mon = 0xff;

    /* keep the monitor (loopback) level across a re-init */
    if (ca_chip_is_running(hw))
        mon = ca_get_monitor(hw);

    hw->inte &= ~INTE_AI;
    ca_outl(hw, CA0106_INTE, hw->inte);
    ca_ptr_write(hw, BASIC_INTERRUPT, 0, 0);
    ca_ptr_write(hw, EXTENDED_INT_MASK, 0, 0);

    ca_ptr_write(hw, SPCS1, 0, SPCS_DEFAULT);
    ca_ptr_write(hw, SPCS0, 0, SPCS_DEFAULT);
    ca_ptr_write(hw, SPCS2, 0, SPCS_DEFAULT);
    ca_ptr_write(hw, SPCS3, 0, SPCS_DEFAULT);

    ca_ptr_write(hw, PLAYBACK_MUTE, 0, 0x00fc0000UL);
    ca_ptr_write(hw, CAPTURE_MUTE, 0, 0x00fc0000UL);

    /* mute the (absent) AC97 record gain */
    ca_outb(hw, CA0106_AC97ADDRESS, AC97_REC_GAIN);
    ca_outw(hw, CA0106_AC97DATA, 0x8000);

    /* analog outputs */
    ca_ptr_write(hw, SPDIF_SELECT1, 0, 0xf);
    ca_ptr_write(hw, SPDIF_SELECT2, 0, 0x000f0000UL);

    /* record sources 0/1 from I2S in (the WM8775), as OSS does for the
     * codec-less SB0570 */
    ca_ptr_write(hw, CAPTURE_CONTROL, 0, 0x50c81000UL);
    ca_ptr_write(hw, CAPTURE_CONTROL, 1, 0xffffffffUL);     /* monitor muted */
    ca_ptr_write(hw, CAPTURE_CONTROL, 2, 0x30300000UL);
    ca_ptr_write(hw, CAPTURE_CONTROL, 3, 0x00700000UL);

    ca_ptr_write(hw, PLAYBACK_ROUTING1, 0, 0x32765410UL);
    ca_ptr_write(hw, PLAYBACK_ROUTING2, 0, 0x76767676UL);
    ca_ptr_write(hw, CAPTURE_ROUTING1, 0, 0x32765410UL);
    ca_ptr_write(hw, CAPTURE_ROUTING2, 0, 0x76767676UL);

    for (ch = 0; ch < 4; ch++)
    {
        ca_ptr_write(hw, CAPTURE_VOLUME1, ch, 0x30303030UL);
        ca_ptr_write(hw, CAPTURE_VOLUME2, ch, 0x30303030UL);
        ca_ptr_write(hw, PLAYBACK_VOLUME1, ch, 0x30303030UL);  /* 0 dB */
        ca_ptr_write(hw, PLAYBACK_VOLUME2, ch, 0x30303030UL);  /* 0 dB */
    }

    /* record source: I2S input (WM8775) on all capture channels */
    ca_ptr_write(hw, CAPTURE_SOURCE, 0, 0x333300e4UL);

    /* GPIO for SB0410/SB0413/SB0570: analog jack mode */
    ca_outl(hw, CA0106_GPIO, 0);
    ca_outl(hw, CA0106_GPIO, 0x005f5301UL);   /* GPO0|GPO1|GPO4|GPO6 */

    ca_outl(hw, CA0106_HCFG, HCFG_AC97 | HCFG_AUDIOENABLE);

    for (n = 0; n < sizeof(i2c_adc_init) / sizeof(i2c_adc_init[0]); n++)
        if (!ca_i2c_write(hw, i2c_adc_init[n][0], i2c_adc_init[n][1]) && !err)
            err = "WM8775 ADC did not acknowledge I2C writes";
    hw->gain_l = hw->gain_r = ADC_GAIN_0DB;
    ca_set_input(hw, hw->input);

    for (n = 0; n < 16; n++)
        hw->spi_reg[n] = 0;
    for (n = 0; n < sizeof(spi_dac_init) / sizeof(spi_dac_init[0]); n++)
    {
        UWORD reg = spi_dac_init[n] >> SPI_REG_SHIFT;
        if (!ca_spi_write(hw, spi_dac_init[n]) && !err)
            err = "WM8768 DAC SPI write timed out";
        if (reg < 16)
            hw->spi_reg[reg] = spi_dac_init[n];
    }
    /* power up the front DAC (DAC4 on the SB0570, spi_dac=0x4021) */
    hw->spi_reg[SPI_DACD4_REG] &= ~SPI_DACD4_BIT;
    if (!ca_spi_write(hw, hw->spi_reg[SPI_DACD4_REG]) && !err)
        err = "WM8768 DAC SPI write timed out";

    ca_set_monitor(hw, mon);

    /* HCFG_AUDIOENABLE is what ca_chip_is_running() trusts; drop it on a
     * failed codec setup so the next open runs the full init again
     * instead of skipping it and leaving the card silent. */
    if (err)
        ca_io_modify(hw, CA0106_HCFG, HCFG_AUDIOENABLE, 0);

    if (hw->irq_added)
    {
        hw->inte = INTE_AI | INTE_PCI;
        ca_outl(hw, CA0106_INTE, hw->inte);
    }
    return err;
}

void ca_chip_stop_dma(struct CAHw *hw)
{
    Disable();
    ca_ptr_write_nl(hw, BASIC_INTERRUPT, 0,
                    ca_ptr_read_nl(hw, BASIC_INTERRUPT, 0) & ~0x00000f0fUL);
    ca_ptr_write_nl(hw, EXTENDED_INT_MASK, 0, 0);
    ca_ptr_write_nl(hw, EXTENDED_INT, 0, 0xffffffffUL);
    Enable();
}

void ca_chip_shutdown(struct CAHw *hw)
{
    ca_chip_stop_dma(hw);
    hw->inte = 0;
    ca_outl(hw, CA0106_INTE, 0);
    ca_outl(hw, CA0106_HCFG, 0);
}

/*
 * The WM8775 is write-only, so input and gain are mirrored in an unused
 * indexed register (0x0f, channel 3; "0x9 to 0xf unused") to let cactl and
 * ca0106.audio share them.  Format: $CA << 24 | input << 16 | gl << 8 | gr.
 */
#define STATE_REG   0x0f
#define STATE_CH    3

static void ca_save_state(struct CAHw *hw)
{
    ca_ptr_write(hw, STATE_REG, STATE_CH, 0xca000000UL | ((ULONG)hw->input << 16) |
                 ((ULONG)hw->gain_l << 8) | hw->gain_r);
}

/* driver statistics for cactl STATUS: reg 0x0e ch3 = underruns<<16 | overloads */
void ca_debug_counters(struct CAHw *hw, ULONG underruns, ULONG overloads)
{
    ca_ptr_write(hw, 0x0e, STATE_CH, ((underruns & 0xffff) << 16) | (overloads & 0xffff));
}

ULONG ca_read_debug_counters(struct CAHw *hw)
{
    return ca_ptr_read(hw, 0x0e, STATE_CH);
}

BOOL ca_load_state(struct CAHw *hw)
{
    ULONG v = ca_ptr_read(hw, STATE_REG, STATE_CH);
    if ((v >> 24) != 0xca || ((v >> 16) & 0xff) >= CA_INPUTS)
        return FALSE;
    hw->input = (v >> 16) & 0xff;
    hw->gain_l = (v >> 8) & 0xff;
    hw->gain_r = v & 0xff;
    return TRUE;
}

void ca_set_input(struct CAHw *hw, UBYTE input)
{
    if (input >= CA_INPUTS)
        input = CA_INPUT_LINE;
    hw->input = input;
    ca_i2c_write(hw, ADC_MUX, 0);   /* mute while switching */
    /* shared Mic/Line jack: GPIO bit 10 selects mic (SB0410/SB0413 style) */
    if (input == CA_INPUT_MIC)
        ca_io_modify(hw, CA0106_GPIO, 0, 0x400);
    else
        ca_io_modify(hw, CA0106_GPIO, 0x400, 0);
    ca_i2c_write(hw, ADC_ATTEN_ADCL, hw->gain_l);
    ca_i2c_write(hw, ADC_ATTEN_ADCR, hw->gain_r);
    ca_i2c_write(hw, ADC_MUX, adc_mux_bits[input]);
    ca_save_state(hw);
}

void ca_set_input_gain(struct CAHw *hw, UBYTE l, UBYTE r)
{
    hw->gain_l = l ? l : 1;
    hw->gain_r = r ? r : 1;
    ca_i2c_write(hw, ADC_ATTEN_ADCL, hw->gain_l);
    ca_i2c_write(hw, ADC_ATTEN_ADCR, hw->gain_r);
    ca_save_state(hw);
}

/* I2S-in (ADC) fed back into the playback mixer: hardware loopback */
void ca_set_monitor(struct CAHw *hw, UBYTE att)
{
    ULONG v = att;
    ca_ptr_write(hw, CAPTURE_CONTROL, 1, v | (v << 8) | (v << 16) | (v << 24));
}

UBYTE ca_get_monitor(struct CAHw *hw)
{
    return (UBYTE)(ca_ptr_read(hw, CAPTURE_CONTROL, 1) & 0xff);
}

void ca_set_output(struct CAHw *hw, UBYTE att)
{
    ULONG v = att;
    v = v | (v << 8) | (v << 16) | (v << 24);
    ca_ptr_write(hw, PLAYBACK_VOLUME2, 0, v);   /* front analog */
    ca_ptr_write(hw, PLAYBACK_VOLUME1, 0, v);   /* front digital mixer */
}

/* ------------------------------------------------------------------------ */
/* Streams                                                                  */
/* ------------------------------------------------------------------------ */

static const ULONG play_rate40[4] = { 0, 0x10000UL, 0x20000UL, 0x30000UL };
static const ULONG play_rate71[4] = { 0, 0x01010000UL, 0x02020000UL, 0x03030000UL };
static const ULONG cap_rate71[4]  = { 0, 0x4000UL, 0x8000UL, 0xc000UL };

/*
 * Playback uses the chip's single-buffer mode (as the 4Front OSS driver
 * does): no period list, the engine loops over one buffer and raises
 * EXTINT_PLAY_HALF at the middle and EXTINT_PLAY_FULL at the end.
 *
 * The Linux period-list mode does not work behind the Prometheus bridge:
 * the list entry fetched from Radeon VRAM arrives with its two dwords in
 * the wrong order and the engine never starts.
 */
void ca_play_setup(struct CAHw *hw, ULONG buf_off, ULONG bytes, UBYTE rate, BOOL s32)
{
    rate &= 3;
    ca_io_modify(hw, CA0106_HCFG, HCFG_PLAYBACK_S32_LE, s32 ? HCFG_PLAYBACK_S32_LE : 0);
    ca_ptr_modify(hw, BASIC_INTERRUPT, 0, 0x30000UL, play_rate40[rate]);
    ca_ptr_modify(hw, CAPTURE_CONTROL, 0, 0x03030000UL, play_rate71[rate]);

    ca_ptr_write(hw, PLAYBACK_LIST_ADDR, 0, 0);
    ca_ptr_write(hw, PLAYBACK_LIST_SIZE, 0, 0);
    ca_ptr_write(hw, PLAYBACK_LIST_PTR, 0, 0);
    ca_ptr_write(hw, PLAYBACK_POINTER, 0, 0);
    ca_ptr_write(hw, PLAYBACK_PERIOD_END_ADDR, 0, 0);
    ca_ptr_write(hw, PLAYBACK_FIFO_OFFSET_ADDRESS, 0, 0);
    ca_ptr_write(hw, PLAYBACK_DMA_ADDR, 0, hw->dma_pci + buf_off);
    ca_ptr_write(hw, PLAYBACK_PERIOD_SIZE, 0, bytes << 16);
    ca_ptr_write(hw, PLAYBACK_MUTE, 0, 0);
}

void ca_play_start(struct CAHw *hw)
{
    Disable();
    ca_ptr_write_nl(hw, EXTENDED_INT, 0, EXTINT_PLAY_FULL | EXTINT_PLAY_HALF);
    ca_ptr_write_nl(hw, EXTENDED_INT_MASK, 0,
                    ca_ptr_read_nl(hw, EXTENDED_INT_MASK, 0) |
                    EXTINT_PLAY_FULL | EXTINT_PLAY_HALF);
    ca_ptr_write_nl(hw, BASIC_INTERRUPT, 0,
                    ca_ptr_read_nl(hw, BASIC_INTERRUPT, 0) | 0x1);
    Enable();
}

void ca_play_stop(struct CAHw *hw)
{
    Disable();
    ca_ptr_write_nl(hw, BASIC_INTERRUPT, 0,
                    ca_ptr_read_nl(hw, BASIC_INTERRUPT, 0) & ~0x1UL);
    ca_ptr_write_nl(hw, EXTENDED_INT_MASK, 0,
                    ca_ptr_read_nl(hw, EXTENDED_INT_MASK, 0) &
                    ~(EXTINT_PLAY_FULL | EXTINT_PLAY_HALF));
    Enable();
}

ULONG ca_play_pointer(struct CAHw *hw)
{
    return ca_ptr_read(hw, PLAYBACK_POINTER, 0);
}

void ca_cap_setup(struct CAHw *hw, ULONG buf_off, ULONG bytes, UBYTE rate)
{
    rate &= 3;
    ca_io_modify(hw, CA0106_HCFG, HCFG_CAPTURE_S32_LE, 0);
    ca_ptr_modify(hw, CAPTURE_CONTROL, 0, 0xc000UL, cap_rate71[rate]);
    ca_i2c_write(hw, ADC_MASTER, rate >= CA_RATE_96000 ? 0xa : 0x2);

    ca_ptr_write(hw, CAPTURE_FIFO_OFFSET_ADDRESS, 0, 0);
    ca_ptr_write(hw, CAPTURE_DMA_ADDR, 0, hw->dma_pci + buf_off);
    ca_ptr_write(hw, CAPTURE_BUFFER_SIZE, 0, bytes << 16);
    ca_ptr_write(hw, CAPTURE_POINTER, 0, 0);
}

void ca_cap_start(struct CAHw *hw)
{
    Disable();
    ca_ptr_write_nl(hw, EXTENDED_INT, 0, EXTINT_CAP_HALF_BUFFER | EXTINT_CAP_FULL_BUFFER);
    ca_ptr_write_nl(hw, EXTENDED_INT_MASK, 0,
                    ca_ptr_read_nl(hw, EXTENDED_INT_MASK, 0) |
                    EXTINT_CAP_HALF_BUFFER | EXTINT_CAP_FULL_BUFFER);
    ca_ptr_write_nl(hw, BASIC_INTERRUPT, 0,
                    ca_ptr_read_nl(hw, BASIC_INTERRUPT, 0) | 0x100);
    Enable();
}

void ca_cap_stop(struct CAHw *hw)
{
    Disable();
    ca_ptr_write_nl(hw, BASIC_INTERRUPT, 0,
                    ca_ptr_read_nl(hw, BASIC_INTERRUPT, 0) & ~0x100UL);
    ca_ptr_write_nl(hw, EXTENDED_INT_MASK, 0,
                    ca_ptr_read_nl(hw, EXTENDED_INT_MASK, 0) &
                    ~(EXTINT_CAP_HALF_BUFFER | EXTINT_CAP_FULL_BUFFER));
    Enable();
}

ULONG ca_cap_pointer(struct CAHw *hw)
{
    return ca_ptr_read(hw, CAPTURE_POINTER, 0);
}

/* ------------------------------------------------------------------------ */
/* Fixed <-> dB helpers (no libm)                                           */
/* ------------------------------------------------------------------------ */

/* log2(x) in 16.16, x > 0 */
static LONG ca_log2_fix16(ULONG x)
{
    LONG ip = 31;
    ULONG m, frac = 0;
    int i;

    while (!(x & 0x80000000UL))
    {
        x <<= 1;
        ip--;
    }
    m = x >> 16;                        /* 1.15 mantissa in [1,2) */
    for (i = 0; i < 16; i++)
    {
        m = (m * m) >> 15;
        frac <<= 1;
        if (m >= 0x10000UL)
        {
            m >>= 1;
            frac |= 1;
        }
    }
    return (ip << 16) | (LONG)frac;
}

LONG ca_fixed_to_qdb(LONG fixed)
{
    LONG l;
    if (fixed <= 0)
        return -100000;
    l = ca_log2_fix16((ULONG)fixed) - (16L << 16);  /* log2(fixed/1.0) */
    /* 80*log10(2) = 24.0824; 24.0824 * 2^7 = 3082.5 */
    return ((l >> 4) * 3083L) / (1L << 19);
}

UBYTE ca_fixed_to_att(LONG fixed)
{
    LONG att;
    if (fixed <= 0)
        return 0xff;
    att = 0x30 - ca_fixed_to_qdb(fixed);
    if (att < 0)
        att = 0;
    if (att > 0xfe)
        att = 0xfe;
    return (UBYTE)att;
}

UBYTE ca_fixed_to_adcgain(LONG fixed)
{
    LONG code;
    if (fixed <= 0)
        return 0x01;
    code = ADC_GAIN_0DB + ca_fixed_to_qdb(fixed) / 2;
    if (code < 1)
        code = 1;
    if (code > 0xff)
        code = 0xff;
    return (UBYTE)code;
}
