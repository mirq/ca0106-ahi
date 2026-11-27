/*
 * caprobe - go/no-go test for the CA0106 (Audigy SE SB0570) on openpci.
 *
 * Checks, in order: card discovery and ownership, register access through
 * the I/O BAR, DMA memory (both openpci and prometheus.library paths),
 * codec acknowledges, PCI interrupt delivery, playback DMA (card reads
 * Radeon VRAM) and capture DMA (card writes Radeon VRAM).  Plays silence.
 *
 * Usage: caprobe [NOINIT] [NOIRQ] [DMA=AUTO|OPENPCI|PROMETHEUS] [FORCE] [TRACE]
 *
 * TRACE clears the PCI error flags, runs a bus stability test and prints
 * the card registers and PCI status after every chip setup step and codec
 * write, flushing each line, to find where a setup goes wrong.
 *
 * The DMA tests [5] and [6] are skipped when the chip is not set up
 * (NOINIT on a cold card, or a failed init): starting the engines then
 * proves nothing and was seen to hang a machine.
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
#include <utility/tagitem.h>

/* openpci v3+ GetBoardAttrs() tags (same values as prometheus.library) */
#define PRM_SubsysVendor      0x6EDA000AUL
#define PRM_SubsysID          0x6EDA000BUL
#define PRM_PCIToHostOffset   0x6EDA000FUL
#define PRM_LegacyIOSpace     0x6EDA000EUL
#define PRM_MemoryAddr0       0x6EDA0010UL
#define PRM_InterruptLine     0x6EDA001DUL
#define PRM_MemorySize0       0x6EDA0020UL
#define PRM_MemoryFlags0      0x6EDA0030UL
#define PRM_PCIMemWindowLow   0x6EDA0050UL
#define PRM_PCIMemWindowHigh  0x6EDA0051UL

#include "ca0106_regs.h"
#include "ca0106_hw.h"

static const char vers[] __attribute__((used)) = "$VER: caprobe 0.3 (27.11.2026)";

static struct CAHw hw;

/* as much as ca0106.audio allocates (DMA_BYTES in driver/driver.h) */
#define DMA_TEST_BYTES  (16384UL + 30720UL)
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
static void result(const char *what, BOOL ok);

/* PCI_STATUS error bits: parity, SERR, master/target abort, data parity */
#define PCI_STATUS_ERRORS   0xf900

static UWORD trace_status;

/* hw.trace hook: one entry per chip setup step or codec write */
static void trace_step(struct CAHw *h, const char *stage, ULONG arg)
{
    ULONG hcfg = ca_inl(h, CA0106_HCFG);
    ULONG gpio = ca_inl(h, CA0106_GPIO);
    ULONG ipr = ca_inl(h, CA0106_IPR);
    UWORD st = pci_read_config_word(PCI_STATUS, h->dev);

    printf("  %-40s", stage);
    if (!strncmp(stage, "I2C", 3))
        printf(" reg $%02lx val $%03lx try %lu polls %lu I2C_A=$%08lx",
               arg >> 16, arg & 0xffff, h->i2c_tries, h->i2c_polls, h->i2c_status);
    else if (!strncmp(stage, "SPI", 3))
        printf(" data $%04lx polls %lu", arg & 0xffff, h->spi_polls);
    printf("\n    HCFG=%08lx GPIO=%08lx IPR=%08lx PCI_STATUS=$%04x%s%s\n",
           hcfg, gpio, ipr, st,
           (hcfg == 0xffffffffUL || gpio == 0xffffffffUL) ? "  <-- reads all ones" : "",
           (st & ~trace_status & PCI_STATUS_ERRORS) ? "  <-- new PCI error" : "");
    trace_status = st;
    fflush(stdout);
}

/* TRACE: repeated register access, looking for lost or corrupted cycles */
static void bus_test(void)
{
    ULONG i, v, ref, bad = 0, ones = 0, ptrbad = 0, databad = 0, save, mask;

    printf("\n[2b] Bus stability\n");
    ref = ca_inl(&hw, CA0106_HCFG);
    for (i = 0; i < 20000; i++)
    {
        v = ca_inl(&hw, CA0106_HCFG);
        if (v != ref)
        {
            bad++;
            if (v == 0xffffffffUL)
                ones++;
        }
    }
    printf("  20000 HCFG reads: %lu differ from $%08lx (%lu all ones)\n", bad, ref, ones);
    result("HCFG reads stable", bad == 0);

    for (i = 0; i < 5000; i++)
    {
        ULONG p = ((i & 0x7fUL) << 16) | (i & 3);
        ca_outl(&hw, CA0106_PTR, p);
        v = ca_inl(&hw, CA0106_PTR);
        if ((v & 0x007f0003UL) != p)
            ptrbad++;
    }
    printf("  5000 PTR write/read pairs: %lu wrong\n", ptrbad);
    result("PTR write/read stable", ptrbad == 0);

    /* PLAYBACK_DMA_ADDR of channel 3, which is never started: find the
     * bits it keeps, then write/read patterns through PTR/DATA */
    if (ca_ptr_read(&hw, BASIC_INTERRUPT, 0) & 0x8)
    {
        printf("  channel 3 playback is running, indexed register test skipped\n");
        goto bus_done;
    }
    save = ca_ptr_read(&hw, PLAYBACK_DMA_ADDR, 3);
    ca_ptr_write(&hw, PLAYBACK_DMA_ADDR, 3, 0xffffffffUL);
    mask = ca_ptr_read(&hw, PLAYBACK_DMA_ADDR, 3);
    ca_ptr_write(&hw, PLAYBACK_DMA_ADDR, 3, 0);
    mask &= ~ca_ptr_read(&hw, PLAYBACK_DMA_ADDR, 3);
    printf("  test register 0x%02x ch3 keeps bits $%08lx\n", (unsigned)PLAYBACK_DMA_ADDR, mask);
    for (i = 0; i < 5000; i++)
    {
        ULONG pat = ((i * 0x9e3779b1UL) ^ (i << 7)) & mask;
        ca_ptr_write(&hw, PLAYBACK_DMA_ADDR, 3, pat);
        if ((ca_ptr_read(&hw, PLAYBACK_DMA_ADDR, 3) & mask) != pat)
            databad++;
    }
    ca_ptr_write(&hw, PLAYBACK_DMA_ADDR, 3, save);
    printf("  5000 indexed register write/read pairs: %lu wrong\n", databad);
    result("indexed register write/read stable", databad == 0 && mask != 0);
bus_done:
    printf("  PCI_STATUS=$%04x after the bus test\n", pci_read_config_word(PCI_STATUS, hw.dev));
    fflush(stdout);
}

static void result(const char *what, BOOL ok)
{
    printf("  %-44s %s\n", what, ok ? "OK" : "FAIL");
    if (!ok)
        fails++;
}

static void mem_kind(const char *label, APTR addr)
{
    ULONG t = TypeOfMem(addr);
    APTR bus = pci_logic_to_physic_addr(addr, hw.dev);

    printf("     %-14s $%08lx: TypeOfMem=$%04lx%s%s%s, PCI bus address $%08lx%s\n",
           label, (ULONG)addr, t,
           t & MEMF_CHIP ? " CHIP" : "", t & MEMF_FAST ? " FAST" : "",
           t ? "" : " (not exec RAM)", (ULONG)bus,
           bus ? "" : " (none)");
}

/* Bridge and address map as openpci sees it */
static void bridge_info(void)
{
    UWORD bus = pci_bus();
    APTR bar0 = NULL;
    ULONG size0 = 0, flags0 = 0, svend = 0, sid = 0, wlo = 0, whi = 0, iline = 0;
    LONG hostoff = 0;
    APTR legacy = NULL;
    ULONG n;

    printf("  bridge:%s%s%s%s%s\n",
           bus & 0x01 ? " Mediator-A1200" : "", bus & 0x02 ? " Mediator-Z4" : "",
           bus & 0x04 ? " Prometheus/Firestorm" : "", bus & 0x18 ? " G-REX" : "",
           bus ? "" : " (none reported)");
    if (hw.libver < 3)
        return;
    {
        struct TagItem tags[] =
        {
            { PRM_MemoryAddr0, (ULONG)&bar0 },
            { PRM_MemorySize0, (ULONG)&size0 },
            { PRM_MemoryFlags0, (ULONG)&flags0 },
            { PRM_SubsysVendor, (ULONG)&svend },
            { PRM_SubsysID, (ULONG)&sid },
            { PRM_PCIToHostOffset, (ULONG)&hostoff },
            { PRM_LegacyIOSpace, (ULONG)&legacy },
            { PRM_PCIMemWindowLow, (ULONG)&wlo },
            { PRM_PCIMemWindowHigh, (ULONG)&whi },
            { PRM_InterruptLine, (ULONG)&iline },
            { TAG_DONE, 0 }
        };
        n = GetBoardAttrsA(hw.dev, tags);
    }
    printf("  GetBoardAttrs (%lu tags): BAR0 $%08lx size $%lx flags $%lx, subsys %04lx:%04lx\n",
           n, (ULONG)bar0, size0, flags0, svend, sid);
    printf("    PCIToHostOffset $%08lx, legacy I/O $%08lx, PCI mem window $%08lx-$%08lx, int line %lu\n",
           (ULONG)hostoff, (ULONG)legacy, wlo, whi, iline);
    if (bar0 && ((ULONG)bar0 & ~3UL) != ((ULONG)hw.io))
        printf("    NOTE: GetBoardAttrs BAR0 differs from pci_dev base_address[0]\n");
}

/* Allocate with one strategy, show where the memory is, free. */
static void dma_try(ULONG flags, const char *label)
{
    const char *err;
    ULONG size = DMA_TEST_BYTES;

    err = ca_dma_alloc(&hw, size, flags);
    printf("  %-20s: %s\n", label, err ? err : ca_dma_kind_name(hw.dma_kind));
    if (hw.dma_info & (CA_DMAI_PAGES_BAD | CA_DMAI_RW_BAD))
        printf("     check failed at offset $%05lx: %s $%08lx\n", hw.dma_badoff,
               hw.dma_info & CA_DMAI_PAGES_BAD ? "page is on the bus at" : "read back",
               hw.dma_badval);
    if (err)
        return;
    printf("     raw=$%08lx size=%lu cpu=$%08lx pci=$%08lx %s\n",
           (ULONG)hw.dma_raw, hw.dma_rawsize, (ULONG)hw.dma_cpu, hw.dma_pci,
           hw.dma_kind == CA_DMA_OPENPCI3 ?
               (hw.dma_info & CA_DMAI_NONCACHE ? "(non-cacheable)" : "(CACHEABLE: MEM_NONCACHEABLE refused)") : "");
    printf("     page map and CPU write/cache push/read back over %lu bytes: OK\n", hw.dma_size);
    if (hw.dma_kind != CA_DMA_PROMETHEUS)
    {
        APTR back = pci_physic_to_logic_addr((APTR)hw.dma_pci, hw.dev);
        printf("     pci_physic_to_logic_addr(pci) = $%08lx %s\n", (ULONG)back,
               back == (APTR)hw.dma_cpu ? "(round trip OK)" : "(DIFFERS)");
    }
    mem_kind("DMA block", hw.dma_cpu);
    if (PrometheusBase)
        printf("     Prm_GetPhysicalAddress(cpu) = $%08lx\n",
               (ULONG)Prm_GetPhysicalAddress(hw.dma_cpu));
    ca_dma_free(&hw);
}

int main(void)
{
    LONG args[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    const char *err;
    ULONG dmaflags = 0, v, v2, i;
    BOOL noinit, noirq, trace;

    rda = ReadArgs("NOINIT/S,NOIRQ/S,DMA/K,FORCE/S,TRACE/S", args, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), "caprobe");
        return RETURN_FAIL;
    }
    noinit = args[0] != 0;
    noirq = args[1] != 0;
    trace = args[4] != 0;
    if (args[2])
    {
        if (!strcasecmp((char *)args[2], "OPENPCI"))
            dmaflags = CA_DMAF_ONLY_OPENPCI;
        else if (!strcasecmp((char *)args[2], "PROMETHEUS"))
            dmaflags = CA_DMAF_ONLY_PROMETHEUS;
    }

    printf("caprobe 0.3 - CA0106 / Audigy SE go/no-go test%s\n\n", trace ? " (TRACE)" : "");

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
    bridge_info();
    if (trace)
    {
        UWORD st = pci_read_config_word(PCI_STATUS, hw.dev);
        pci_write_config_word(PCI_STATUS, PCI_STATUS_ERRORS, hw.dev);  /* write 1 to clear */
        trace_status = pci_read_config_word(PCI_STATUS, hw.dev);
        printf("  PCI_STATUS=$%04x, error flags cleared -> $%04x\n", st, trace_status);
    }
    fflush(stdout);

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
    if (trace)
        bus_test();

    printf("\n[3] DMA memory (%lu bytes, as the driver uses)\n", DMA_TEST_BYTES);
    {
        /* can the card reach host RAM at all? (no on Prometheus/Mediator) */
        APTR f = AllocVec(4096, MEMF_FAST);
        APTR c = AllocVec(4096, MEMF_CHIP);
        if (f) mem_kind("Fast RAM", f);
        if (c) mem_kind("Chip RAM", c);
        FreeVec(f);
        FreeVec(c);
    }
    if (dmaflags != CA_DMAF_ONLY_PROMETHEUS)
        dma_try(CA_DMAF_ONLY_OPENPCI, "openpci");
    if (dmaflags != CA_DMAF_ONLY_OPENPCI)
        dma_try(CA_DMAF_ONLY_PROMETHEUS, "prometheus.library");
    err = ca_dma_alloc(&hw, 16384, dmaflags);
    if (err)
    {
        printf("  ERROR: %s\n", err);
        result("usable DMA memory", FALSE);
        goto out;
    }
    printf("  using %s: cpu=$%08lx pci=$%08lx\n",
           ca_dma_kind_name(hw.dma_kind), (ULONG)hw.dma_cpu, hw.dma_pci);

    if (!noinit)
    {
        printf("\n[4] Chip and codec init\n");
        fflush(stdout);
        if (trace)
        {
            trace_status = pci_read_config_word(PCI_STATUS, hw.dev);
            hw.trace = trace_step;
        }
        err = ca_chip_init(&hw);
        hw.trace = NULL;
        result(err ? err : "WM8775 (I2C) and WM8768 (SPI) acknowledged", err == NULL);
        printf("  HCFG=$%08lx GPIO=$%08lx SPI=$%08lx\n",
               ca_inl(&hw, CA0106_HCFG), ca_inl(&hw, CA0106_GPIO),
               ca_ptr_read(&hw, SPI, 0));
    }

    if (!noirq && !ca_chip_is_running(&hw))
    {
        printf("\n[5]/[6] skipped: the chip is not set up (HCFG=$%08lx), DMA tests need it\n",
               ca_inl(&hw, CA0106_HCFG));
        noirq = TRUE;
    }
    fflush(stdout);
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
    if (trace && hw.dev)
        printf("\n  PCI_STATUS=$%04x at the end\n", pci_read_config_word(PCI_STATUS, hw.dev));
    if (hw.owned)
        ca_chip_stop_dma(&hw);
    ca_close(&hw);
    FreeArgs(rda);
    printf("\n%s (%d failure%s)\n", fails ? "PROBLEMS FOUND" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? RETURN_WARN : RETURN_OK;
}
