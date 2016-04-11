/*
 * Copyright 2016 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation (the "GPL").
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 (GPLv2) for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 (GPLv2) along with this source code.
 */
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/sfi_acpi.h>
#include <linux/slab.h>

#include "../pci/ecam.h"

#define PREFIX	"ACPI: "

/*
 * Array of config windows from MCFG table, parsed and created by
 * pci_mmcfg_late_init() at boot
 */
static struct saved_mcfg {
	int	nentries;
	struct saved_mcfg_entry {
		phys_addr_t	addr;
		u16		domain;
		u8		bus_start;
		u8		bus_end;
	} *entries;
} saved_mcfg;

/* ACPI info for generic ACPI PCI controller */
struct acpi_pci_generic_root_info {
	struct acpi_pci_root_info	common;
	struct pci_config_window	*cfg;	/* config space mapping */
};

/* find the entry in cfgarr which contains range bus_start..bus_end */
static int mcfg_lookup(u16 seg, u8 bus_start, u8 bus_end)
{
	struct saved_mcfg_entry *mcfg;
	int i;

	for (i = 0; i < saved_mcfg.nentries; i++) {
		mcfg = &saved_mcfg.entries[i];
		if (seg != mcfg->domain)
			continue;
		if (bus_start >= mcfg->bus_start && bus_start <= mcfg->bus_end)
			return (bus_end <= mcfg->bus_end) ? i : -EINVAL;
		else if (bus_end >= mcfg->bus_start && bus_end <= mcfg->bus_end)
			return -EINVAL;
	}
	return -ENOENT;
}

/*
 * create a new mapping
 */
static struct pci_config_window *pci_acpi_ecam_create(struct device *dev,
			phys_addr_t addr, u16 seg, u8 bus_start, u8 bus_end)
{
	unsigned int bsz = 1 << pci_generic_ecam_ops.bus_shift;
	struct resource cfgres = {
		.start	= addr + bus_start * bsz,
		.end	= addr + (bus_end + 1) * bsz - 1,
		.flags	= IORESOURCE_MEM,
	};
	struct resource busr = {
		.start	= bus_start,
		.end	= bus_end,
		.flags	= IORESOURCE_BUS,
	};

	return pci_ecam_create(dev, &cfgres, &busr, &pci_generic_ecam_ops);
}

/*
 * Lookup the bus range for the domain in MCFG, and set up config space
 * mapping.
 */
static int pci_acpi_setup_ecam_mapping(struct acpi_pci_root *root,
				       struct acpi_pci_generic_root_info *ri)
{
	u16 seg = root->segment;
	u8 bus_start = root->secondary.start;
	u8 bus_end = root->secondary.end;
	phys_addr_t addr = root->mcfg_addr;
	struct saved_mcfg_entry *mcfg;
	struct acpi_device *adev = root->device;
	struct pci_config_window *cfg;
	int ret;

	ret = mcfg_lookup(seg, bus_start, bus_end);
	if (ret == -ENOENT) {
		if (addr == 0) {
			pr_err("%04x:%02x-%02x mcfg lookup failed\n", seg,
				bus_start, bus_end);
			return ret;
		}
	} else if (ret < 0) {
		pr_err("%04x:%02x-%02x bus range error (%d)\n", seg, bus_start,
		       bus_end, ret);
		return ret;
	} else {
		mcfg = &saved_mcfg.entries[ret];
		if (addr == 0)
			addr = mcfg->addr;
		if (bus_start != mcfg->bus_start) {
			pr_err("%04x:%02x-%02x bus range mismatch %02x\n",
			       seg, bus_start, bus_end, mcfg->bus_start);
			return -EINVAL;
		}
		if (addr != mcfg->addr) {
			pr_warn("%04x:%02x-%02x addr mismatch, ignoring MCFG\n",
				seg, bus_start, bus_end);
		} else if (bus_end != mcfg->bus_end) {
			pr_warn("%04x:%02x-%02x bus end mismatch using %02x\n",
				seg, bus_start, bus_end, mcfg->bus_end);
			bus_end = mcfg->bus_end;
		}
	}

	cfg = pci_acpi_ecam_create(&adev->dev, addr, seg, bus_start, bus_end);
	if (IS_ERR(cfg)) {
		pr_err("%04x:%02x-%02x error %ld mapping CAM\n", seg,
			bus_start, bus_end, PTR_ERR(cfg));
		return PTR_ERR(cfg);
	}

	ri->cfg = cfg;
	return 0;
}

/* release_info: free resrouces allocated by init_info */
static void pci_acpi_generic_release_info(struct acpi_pci_root_info *ci)
{
	struct acpi_pci_generic_root_info *ri;

	ri = container_of(ci, struct acpi_pci_generic_root_info, common);
	pci_ecam_free(ri->cfg);
	kfree(ri);
}

static struct acpi_pci_root_ops acpi_pci_root_ops = {
	.release_info = pci_acpi_generic_release_info,
};

/* Interface called from ACPI code to setup PCI host controller */
struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	int node = acpi_get_node(root->device->handle);
	struct acpi_pci_generic_root_info *ri;
	struct pci_bus *bus, *child;
	int err;

	ri = kzalloc_node(sizeof(*ri), GFP_KERNEL, node);
	if (!ri)
		return NULL;

	err = pci_acpi_setup_ecam_mapping(root, ri);
	if (err)
		return NULL;

	acpi_pci_root_ops.pci_ops = &ri->cfg->ops->pci_ops;
	bus = acpi_pci_root_create(root, &acpi_pci_root_ops, &ri->common,
				   ri->cfg);
	if (!bus)
		return NULL;

	pci_bus_size_bridges(bus);
	pci_bus_assign_resources(bus);

	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child);

	return bus;
}

/* handle MCFG table entries */
static __init int handle_mcfg(struct acpi_table_header *header)
{
	struct acpi_table_mcfg *mcfg;
	struct acpi_mcfg_allocation *mptr;
	struct saved_mcfg_entry *e;
	int i, n;

	if (!header)
		return -EINVAL;

	mcfg = (struct acpi_table_mcfg *)header;
	mptr = (struct acpi_mcfg_allocation *) &mcfg[1];
	n = (header->length - sizeof(*mcfg)) / sizeof(*mptr);
	if (n <= 0 || n > 255) {
		pr_err(PREFIX " MCFG has incorrect entries (%d).\n", n);
		return -EINVAL;
	}

	e = kcalloc(n, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	saved_mcfg.entries = e;
	saved_mcfg.nentries = n;
	for (i = 0; i < n; i++, e++) {
		e->addr = mptr->address;
		e->domain = mptr->pci_segment;
		e->bus_start = mptr->start_bus_number;
		e->bus_end = mptr->end_bus_number;
	}

	return 0;
}

/* Interface called by ACPI - parse and save MCFG table */
void __init pci_mmcfg_late_init(void)
{
	int err;

	err = acpi_sfi_table_parse(ACPI_SIG_MCFG, handle_mcfg);
	if (err)
		pr_err(PREFIX " Failed to parse MCFG (%d)\n", err);
	else if (!saved_mcfg.nentries)
		pr_err(PREFIX " Failed to parse MCFG, no valid entries.\n");
	else
		pr_info(PREFIX " MCFG table loaded, %d entries\n",
			saved_mcfg.nentries);
}

/* Raw operations */
int raw_pci_read(unsigned int domain, unsigned int busn, unsigned int devfn,
		 int reg, int len, u32 *val)
{
	struct pci_bus *bus = pci_find_bus(domain, busn);

	if (!bus)
		return PCIBIOS_DEVICE_NOT_FOUND;
	return bus->ops->read(bus, devfn, reg, len, val);
}

int raw_pci_write(unsigned int domain, unsigned int busn, unsigned int devfn,
		  int reg, int len, u32 val)
{
	struct pci_bus *bus = pci_find_bus(domain, busn);

	if (!bus)
		return PCIBIOS_DEVICE_NOT_FOUND;
	return bus->ops->write(bus, devfn, reg, len, val);
}
