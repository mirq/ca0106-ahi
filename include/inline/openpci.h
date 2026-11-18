/*
 * Hand-written GCC inline stubs for openpci.library.
 *
 * Covers the subset used by the CA0106 driver.  The LVOs of the shared
 * functions are identical in OpenPCI 2.1 (Vernoux) and the new
 * openpci.library 3.x/40.x (Richter); the *ForBoard functions exist only
 * in version 3 and later and must not be called on older libraries.
 */
#ifndef _INLINE_OPENPCI_H
#define _INLINE_OPENPCI_H

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif

#ifndef OPENPCI_BASE_NAME
#define OPENPCI_BASE_NAME OpenPciBase
#endif

struct pci_dev;
struct Interrupt;

#define pci_bus() \
	LP0(0x1e, UWORD, pci_bus, , OPENPCI_BASE_NAME)

#define pci_inl(address) \
	LP1(0x3c, ULONG, pci_inl, ULONG, address, a0, , OPENPCI_BASE_NAME)

#define pci_outl(value, address) \
	LP2NR(0x42, pci_outl, ULONG, value, d0, ULONG, address, a0, , OPENPCI_BASE_NAME)

#define pci_find_device(vendor, device, pcidev) \
	LP3(0x5a, struct pci_dev *, pci_find_device, UWORD, vendor, d0, UWORD, device, d1, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_read_config_byte(reg, pcidev) \
	LP2(0x6c, UBYTE, pci_read_config_byte, UBYTE, reg, d0, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_read_config_word(reg, pcidev) \
	LP2(0x72, UWORD, pci_read_config_word, UBYTE, reg, d0, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_read_config_long(reg, pcidev) \
	LP2(0x78, ULONG, pci_read_config_long, UBYTE, reg, d0, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_write_config_byte(reg, val, pcidev) \
	LP3NR(0x7e, pci_write_config_byte, UBYTE, reg, d0, UBYTE, val, d1, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_write_config_word(reg, val, pcidev) \
	LP3NR(0x84, pci_write_config_word, UBYTE, reg, d0, UWORD, val, d1, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_write_config_long(reg, val, pcidev) \
	LP3NR(0x8a, pci_write_config_long, UBYTE, reg, d0, ULONG, val, d1, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_set_master(pcidev) \
	LP1NR(0x90, pci_set_master, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_add_intserver(intr, pcidev) \
	LP2(0x96, BOOL, pci_add_intserver, struct Interrupt *, intr, a0, struct pci_dev *, pcidev, a1, , OPENPCI_BASE_NAME)

#define pci_rem_intserver(intr, pcidev) \
	LP2NR(0x9c, pci_rem_intserver, struct Interrupt *, intr, a0, struct pci_dev *, pcidev, a1, , OPENPCI_BASE_NAME)

#define pci_allocdma_mem(size, flags) \
	LP2(0xa2, APTR, pci_allocdma_mem, ULONG, size, d0, ULONG, flags, d1, , OPENPCI_BASE_NAME)

#define pci_freedma_mem(buffer, size) \
	LP2NR(0xa8, pci_freedma_mem, APTR, buffer, a0, ULONG, size, d0, , OPENPCI_BASE_NAME)

#define pci_logic_to_physic_addr(addr, pcidev) \
	LP2(0xae, APTR, pci_logic_to_physic_addr, APTR, addr, a0, struct pci_dev *, pcidev, a1, , OPENPCI_BASE_NAME)

#define pci_obtain_card(pcidev) \
	LP1(0xba, BOOL, pci_obtain_card, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

#define pci_release_card(pcidev) \
	LP1NR(0xc0, pci_release_card, struct pci_dev *, pcidev, a0, , OPENPCI_BASE_NAME)

/* openpci.library v3+ only */
#define AllocateDMAMemoryForBoard(pcidev, size, flags) \
	LP3(0xde, APTR, AllocateDMAMemoryForBoard, struct pci_dev *, pcidev, a0, ULONG, size, d0, ULONG, flags, d1, , OPENPCI_BASE_NAME)

#define ReleaseDMAMemoryForBoard(pcidev, addr, size) \
	LP3NR(0xe4, ReleaseDMAMemoryForBoard, struct pci_dev *, pcidev, a0, APTR, addr, a1, ULONG, size, d0, , OPENPCI_BASE_NAME)

#endif /* _INLINE_OPENPCI_H */
