/*
 * caprobe - go/no-go test for the CA0106 (Audigy SE SB0570) on openpci.
 *
 * Checks, in order: card discovery and ownership, register access through
 * the I/O BAR, DMA memory (both openpci and prometheus.library paths),
 * codec acknowledges, PCI interrupt delivery, playback DMA (card reads
 * Radeon VRAM) and capture DMA (card writes Radeon VRAM).  Plays silence.
 *
 * Usage: caprobe [NOINIT] [NOIRQ] [DMA=AUTO|OPENPCI|PROMETHEUS] [FORCE]
 *
 * GPL-2.0-or-later (uses code derived from Linux sound/pci/ca0106).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include <libraries/openpci.h>
#include <inline/openpci.h>
#include <inline/prometheus_dma.h>

#include "ca0106_regs.h"
#include "ca0106_hw.h"

static const char vers[] = "$VER: caprobe 0.1 (18.11.2026)";

static struct CAHw hw;
static volatile ULONG play_irqs, cap_half, cap_full, other_irqs;

static void probe_irq(struct CAHw *h, ULONG ext, ULONG ipr)
{
    (void)h;
    if (ext & (EXTINT_PLAY_FULL | EXTINT_PLAY_HALF))
        play_irqs++;
    if (ext & EXTINT_CAP_HALF_BUFFER)
        cap_half++;
    if (ext & EXTINT_CAP_FULL_BUFFER)
        cap_full++;
    if (!(ext & (EXTINT_PLAY_FULL | EXTINT_PLAY_HALF | EXTINT_CAP_HALF_BUFFER | EXTINT_CAP_FULL_BUFFER)))
        other_irqs++;
    (void)ipr;
}

static void dump_play(const char *when)
{
    ULONG r;
    printf("  -- %s --\n  ch0:", when);
    for (r = 0; r <= 8; r++)
        printf(" %02lx=%08lx", r, ca_ptr_read(&hw, r, 0));
    printf("\n  40=%08lx 46=%08lx 65=%08lx 71=%08lx 75=%08lx 76=%08lx HCFG=%08lx IPR=%08lx\n",
           ca_ptr_read(&hw, BASIC_INTERRUPT, 0), ca_ptr_read(&hw, 0x46, 0),
           ca_ptr_read(&hw, PLAYBACK_MUTE, 0), ca_ptr_read(&hw, CAPTURE_CONTROL, 0),
           ca_ptr_read(&hw, EXTENDED_INT_MASK, 0), ca_ptr_read(&hw, EXTENDED_INT, 0),
           ca_inl(&hw, CA0106_HCFG), ca_inl(&hw, CA0106_IPR));
    printf("  table: %08lx %08lx %08lx %08lx  PCI_STATUS=$%04x\n",
           *(volatile ULONG *)hw.dma_cpu, *(volatile ULONG *)(hw.dma_cpu + 4),
           *(volatile ULONG *)(hw.dma_cpu + 8), *(volatile ULONG *)(hw.dma_cpu + 12),
           pci_read_config_word(PCI_STATUS, hw.dev));
}

static int fails;

static void result(const char *what, BOOL ok)
{
    printf("  %-44s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok)
        fails++;
}

/* Allocate with one strategy, test CPU access, report, free. */
static void dma_try(ULONG flags, const char *label)
{
    const char *err;
    ULONG i, bad = 0;
    volatile ULONG *p;

    err = ca_dma_alloc(&hw, 16384, flags);
    if (err)
    {
        printf("  %-20s: %s\n", label, err);
        return;
    }
    p = (volatile ULONG *)hw.dma_cpu;
    for (i = 0; i < 4096; i++)
        p[i] = i * 0x01010101UL ^ 0xa5a55a5aUL;
    for (i = 0; i < 4096; i++)
        if (p[i] != (i * 0x01010101UL ^ 0xa5a55a5aUL))
            bad++;
    printf("  %-20s: %s\n", label, ca_dma_kind_name(hw.dma_kind));
    printf("     raw=$%08lx size=%lu cpu=$%08lx pci=$%08lx  rw-test: %s (%lu bad)\n",
           (ULONG)hw.dma_raw, hw.dma_rawsize, (ULONG)hw.dma_cpu, hw.dma_pci,
           bad ? "FAIL" : "OK", bad);
    if (PrometheusBase)
        printf("     Prm_GetPhysicalAddress(cpu) = $%08lx\n",
               (ULONG)Prm_GetPhysicalAddress(hw.dma_cpu));
    ca_dma_free(&hw);
}

int main(void)
{
    LONG args[4] = { 0, 0, 0, 0 };
    struct RDArgs *rda;
    const char *err;
    ULONG dmaflags = 0, v, v2, i;
    BOOL noinit, noirq;

    rda = ReadArgs("NOINIT/S,NOIRQ/S,DMA/K,FORCE/S", args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "caprobe");
        return RETURN_FAIL;
    }
    noinit = args[0] != 0;
    noirq = args[1] != 0;
    if (args[2])
    {
        if (!strcasecmp((char *)args[2], "OPENPCI"))
            dmaflags = CA_DMAF_ONLY_OPENPCI;
        else if (!strcasecmp((char *)args[2], "PROMETHEUS"))
            dmaflags = CA_DMAF_ONLY_PROMETHEUS;
    }

    printf("caprobe 0.1 - CA0106 / Audigy SE go/no-go test\n\n");

    printf("[1] PCI\n");
    err = ca_open(&hw, args[3] != 0);
    if (OpenPciBase)
        printf("  openpci.library %u.%u, pci_bus()=$%04x\n",
               hw.libver, hw.librev, (unsigned)pci_bus());
    if (hw.dev)
    {
        printf("  device %04x:%04x devfn=$%02x class=$%06lx irq=%lu\n",
               hw.dev->vendor, hw.dev->device, hw.dev->devfn,
               hw.dev->devclass, hw.dev->irq);
        printf("  BAR0 $%08lx size-field $%08lx, subsystem $%08lx = %s\n",
               hw.dev->base_address[0], hw.dev->base_size[0], hw.serial,
               ca_model_name(hw.serial));
    }
    if (err)
    {
        printf("  ERROR: %s\n", err);
        ca_close(&hw);
        FreeArgs(rda);
        return RETURN_FAIL;
    }
    printf("  PCI_COMMAND=$%04x  latency=%u  int line/pin=%u/%u\n",
           pci_read_config_word(PCI_COMMAND, hw.dev),
           pci_read_config_byte(PCI_LATENCY_TIMER, hw.dev),
           pci_read_config_byte(PCI_INTERRUPT_LINE, hw.dev),
           pci_read_config_byte(PCI_INTERRUPT_PIN, hw.dev));
    result("card obtained (pci_obtain_card)", hw.owned);

    printf("\n[2] Registers (I/O BAR at $%08lx)\n", (ULONG)hw.io);
    printf("  IPR=$%08lx INTE=$%08lx HCFG=$%08lx GPIO=$%08lx\n",
           ca_inl(&hw, CA0106_IPR), ca_inl(&hw, CA0106_INTE),
           ca_inl(&hw, CA0106_HCFG), ca_inl(&hw, CA0106_GPIO));
    if (hw.libver < 3)
    {
        /* OpenPCI 2.1 pci_inl() on Prometheus returns the raw LE value */
        v = pci_inl((ULONG)hw.io + CA0106_HCFG);
        printf("  pci_inl(HCFG)=$%08lx (raw, cross-check of direct access)\n", v);
        result("direct access matches pci_inl()",
               v == ca_inl(&hw, CA0106_HCFG) || ca_swap32(v) == ca_inl(&hw, CA0106_HCFG));
    }
    v = ca_ptr_read(&hw, CHIP_VERSION, 0);
    printf("  CHIP_VERSION=$%08lx\n", v);
    ca_outl(&hw, CA0106_PTR, 0x00340002UL);
    v2 = ca_inl(&hw, CA0106_PTR);
    printf("  PTR write $00340002 -> read $%08lx\n", v2);
    result("PTR register read-back", (v2 & 0x00ff0003UL) == 0x00340002UL);
    v = ca_ptr_read(&hw, COUNTER77, 0);
    Delay(5);
    v2 = ca_ptr_read(&hw, COUNTER77, 0);
    printf("  COUNTER77 %lu -> %lu in 100 ms (expect ~19200)\n", v, v2);
    result("192 kHz counter running", v != v2);

    printf("\n[3] DMA memory\n");
    if (dmaflags != CA_DMAF_ONLY_PROMETHEUS)
        dma_try(CA_DMAF_ONLY_OPENPCI, "openpci");
    if (dmaflags != CA_DMAF_ONLY_OPENPCI)
        dma_try(CA_DMAF_ONLY_PROMETHEUS, "prometheus.library");
    err = ca_dma_alloc(&hw, 16384, dmaflags);
    if (err)
    {
        printf("  ERROR: %s\n", err);
        fails++;
        goto out;
    }
    printf("  using %s: cpu=$%08lx pci=$%08lx\n",
           ca_dma_kind_name(hw.dma_kind), (ULONG)hw.dma_cpu, hw.dma_pci);

    if (!noinit)
    {
        printf("\n[4] Chip and codec init\n");
        err = ca_chip_init(&hw);
        result(err ? err : "WM8775 (I2C) and WM8768 (SPI) acknowledged", err == NULL);
        printf("  HCFG=$%08lx GPIO=$%08lx SPI=$%08lx\n",
               ca_inl(&hw, CA0106_HCFG), ca_inl(&hw, CA0106_GPIO),
               ca_ptr_read(&hw, SPI, 0));
    }

    if (!noirq)
    {
        printf("\n[5] Interrupt + playback DMA (1 s of silence, 48 kHz)\n");
        if (!ca_irq_add(&hw, probe_irq, NULL, "caprobe"))
        {
            result("pci_add_intserver", FALSE);
            goto out;
        }
        result("pci_add_intserver", TRUE);
        /* layout: 4096 play (2 halves x 512 frames), 8192 capture */
        for (i = 0; i < 4096; i += 4)
            *(volatile ULONG *)(hw.dma_cpu + 4096 + i) = 0;
        ca_play_setup(&hw, 4096, 4096, CA_RATE_48000, FALSE);
        dump_play("after setup");
        ca_play_start(&hw);
        v = ca_ptr_read(&hw, PLAYBACK_POINTER, 0);
        Delay(5);
        dump_play("100 ms after start");
        Delay(45);
        v2 = ca_ptr_read(&hw, PLAYBACK_POINTER, 0);
        ca_play_stop(&hw);
        printf("  irqs: play=%lu other=%lu foreign=%lu total-ours=%lu\n",
               play_irqs, other_irqs, hw.irq_foreign, hw.irq_count);
        printf("  PLAYBACK_POINTER $%08lx -> $%08lx\n", v, v2);
        result("playback half/full interrupts (expect ~94)", play_irqs >= 60 && play_irqs <= 130);

        printf("\n[6] Capture DMA (0.5 s, 48 kHz, buffer pre-filled with $AAAAAAAA)\n");
        for (i = 0; i < 8192; i += 4)
            *(volatile ULONG *)(hw.dma_cpu + 8192 + i) = 0xaaaaaaaaUL;
        CacheClearE(hw.dma_cpu + 8192, 8192, CACRF_ClearD);
        ca_cap_setup(&hw, 8192, 8192, CA_RATE_48000);
        ca_cap_start(&hw);
        Delay(25);
        v = ca_cap_pointer(&hw);
        ca_cap_stop(&hw);
        CacheClearE(hw.dma_cpu + 8192, 8192, CACRF_ClearD);
        v2 = 0;
        for (i = 0; i < 8192; i += 4)
            if (*(volatile ULONG *)(hw.dma_cpu + 8192 + i) != 0xaaaaaaaaUL)
                v2++;
        printf("  irqs: half=%lu full=%lu, CAPTURE_POINTER=$%08lx, %lu/2048 longs written\n",
               cap_half, cap_full, v, v2);
        printf("  first samples: %08lx %08lx %08lx %08lx\n",
               *(volatile ULONG *)(hw.dma_cpu + 8192), *(volatile ULONG *)(hw.dma_cpu + 8196),
               *(volatile ULONG *)(hw.dma_cpu + 8200), *(volatile ULONG *)(hw.dma_cpu + 8204));
        result("capture interrupts (expect ~6 + ~6)", cap_half >= 3 && cap_full >= 3);
        result("card wrote capture data into DMA memory", v2 > 1024);
        ca_irq_rem(&hw);
    }

out:
    /* leave the codecs running (so monitor/loopback settings persist),
     * but stop all DMA before the memory goes away */
    if (hw.owned)
        ca_chip_stop_dma(&hw);
    ca_close(&hw);
    FreeArgs(rda);
    printf("\n%s (%d failure%s)\n", fails ? "PROBLEMS FOUND" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    (void)vers;
    return fails ? RETURN_WARN : RETURN_OK;
}
