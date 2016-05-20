/*
 * Code borrowed from powerpc/kernel/pci-common.c
 *
 * Copyright (C) 2003 Anton Blanchard <anton@au.ibm.com>, IBM
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/kernel.h>

/*
 * raw_pci_read/write - Platform-specific PCI config space access.
 */
int raw_pci_read(unsigned int domain, unsigned int bus,
		  unsigned int devfn, int reg, int len, u32 *val)
{
	return -ENXIO;
}

int raw_pci_write(unsigned int domain, unsigned int bus,
		unsigned int devfn, int reg, int len, u32 val)
{
	return -ENXIO;
}

/* Root bridge scanning */
struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	/* TODO: Should be revisited when implementing PCI on ACPI */
	return NULL;
}
