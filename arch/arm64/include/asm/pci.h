#ifndef __ASM_PCI_H
#define __ASM_PCI_H
#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/acpi.h>
#include <linux/pci-ecam.h>

#include <asm/io.h>

#define PCIBIOS_MIN_IO		0x1000
#define PCIBIOS_MIN_MEM		0

/*
 * Set to 1 if the kernel should re-assign all PCI bus numbers
 */
#define pcibios_assign_all_busses() \
	(pci_has_flag(PCI_REASSIGN_ALL_BUS))

/*
 * PCI address space differs from physical memory address space
 */
#define PCI_DMA_BUS_IS_PHYS	(0)

extern int isa_dma_bridge_buggy;

#ifdef CONFIG_PCI
static inline int pci_get_legacy_ide_irq(struct pci_dev *dev, int channel)
{
	/* no legacy IRQ on arm64 */
	return -ENODEV;
}

static inline int pci_proc_domain(struct pci_bus *bus)
{
	return 1;
}

#ifdef CONFIG_ACPI
static inline int pci_domain_nr(struct pci_bus *bus)
{
	if (acpi_disabled)
		return bus->domain_nr;
	else
		return ((struct pci_config_window *)bus->sysdata)->domain;
}
#endif

#endif  /* CONFIG_PCI */

#endif  /* __KERNEL__ */
#endif  /* __ASM_PCI_H */
