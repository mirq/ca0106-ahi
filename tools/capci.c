/*
 * capci - dump CA0106 PCI config space; "capci RESET" does a PCI power
 * management D3hot -> D0 cycle (chip reset) and restores BARs/command.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include <libraries/openpci.h>
#include <inline/openpci.h>

#include "ca0106_regs.h"
#include "ca0106_hw.h"

static struct CAHw hw;

static void dump(void)
{
    int i;
    for (i = 0; i < 0x80; i += 16)
        printf("  %02x: %08lx %08lx %08lx %08lx\n", i,
               pci_read_config_long(i, hw.dev), pci_read_config_long(i + 4, hw.dev),
               pci_read_config_long(i + 8, hw.dev), pci_read_config_long(i + 12, hw.dev));
}

int main(int argc, char **argv)
{
    const char *err = ca_open(&hw, FALSE);
    UBYTE cap;
    int pm = 0, guard = 0;

    if (err)
    {
        printf("error: %s\n", err);
        ca_close(&hw);
        return 10;
    }
    dump();
    cap = pci_read_config_byte(PCI_CAPABILITIES_PTR, hw.dev);
    while (cap && guard++ < 16)
    {
        UBYTE id = pci_read_config_byte(cap, hw.dev);
        printf("  capability $%02x at $%02x\n", id, cap);
        if (id == 1)
            pm = cap;
        cap = pci_read_config_byte(cap + 1, hw.dev);
    }
    if (pm)
        printf("  PM: PMC=$%04x PMCSR=$%04x\n", pci_read_config_word(pm + 2, hw.dev),
               pci_read_config_word(pm + 4, hw.dev));

    if (argc > 1 && !strcasecmp(argv[1], "RESET") && pm)
    {
        ULONG bar0 = pci_read_config_long(PCI_BASE_ADDRESS_0, hw.dev);
        UWORD cmd = pci_read_config_word(PCI_COMMAND, hw.dev);
        UBYTE il = pci_read_config_byte(PCI_INTERRUPT_LINE, hw.dev);

        ca_outl(&hw, CA0106_INTE, 0);
        Disable();
        pci_write_config_word(pm + 4, (pci_read_config_word(pm + 4, hw.dev) & ~3) | 3, hw.dev);
        Enable();
        Delay(2);
        Disable();
        pci_write_config_word(pm + 4, pci_read_config_word(pm + 4, hw.dev) & ~3, hw.dev);
        Enable();
        Delay(2);
        printf("after D3->D0: BAR0=$%08lx CMD=$%04x PMCSR=$%04x\n",
               pci_read_config_long(PCI_BASE_ADDRESS_0, hw.dev),
               pci_read_config_word(PCI_COMMAND, hw.dev),
               pci_read_config_word(pm + 4, hw.dev));
        pci_write_config_long(PCI_BASE_ADDRESS_0, bar0, hw.dev);
        pci_write_config_byte(PCI_INTERRUPT_LINE, il, hw.dev);
        pci_write_config_word(PCI_COMMAND, cmd, hw.dev);
        printf("restored:     BAR0=$%08lx CMD=$%04x HCFG=$%08lx\n",
               pci_read_config_long(PCI_BASE_ADDRESS_0, hw.dev),
               pci_read_config_word(PCI_COMMAND, hw.dev), ca_inl(&hw, CA0106_HCFG));
        dump();
    }
    ca_close(&hw);
    return 0;
}
