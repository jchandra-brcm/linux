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

#define PREFIX	"ACPI: "

/*
 * Structure to hold entries from the MCFG table, these need to be
 * mapped until a pci host bridge claims them for raw_pci_read/write
 * to work
 */
struct mcfg_entry {
	phys_addr_t		addr;
	u16			segment;
	u8			bus_start;
	u8			bus_end;
};

/*
 * Global to save mcfg entries
 */
static struct {
	struct mcfg_entry	*cfg;
	int			size;
} mcfgsav;

/* List of all ACPI PCI roots, needed for raw operations */
static struct list_head gen_acpi_pci_roots;

/* lock for global table AND list above */
static struct mutex gen_acpi_pci_lock;

/* ACPI info for generic ACPI PCI controller */
struct acpi_pci_generic_root_info {
	struct acpi_pci_root_info	common;
	struct pci_config_window	*cfg;	/* config space mapping */
	struct list_head		node;	/* node in acpi_pci_roots */
};

/* Call generic map_bus after getting cfg pointer */
static void __iomem *gen_acpi_map_cfg_bus(struct pci_bus *bus,
					  unsigned int devfn, int where)
{
	struct acpi_pci_generic_root_info *ri = bus->sysdata;

	return pci_generic_map_bus(ri->cfg, bus->number, devfn, where);
}

static struct pci_ops acpi_pci_ops = {
	.map_bus	= gen_acpi_map_cfg_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

/* find the entry in mcfgsav.cfg which contains range bus_start..bus_end */
static int mcfg_lookup(u16 seg, u8 bus_start, u8 bus_end)
{
	struct mcfg_entry *e;
	int i;

	for (i = 0, e = mcfgsav.cfg; i < mcfgsav.size; i++, e++) {
		if (seg != e->segment)
			continue;
		if (bus_start >= e->bus_start && bus_start <= e->bus_end)
			return (bus_end <= e->bus_end) ? i : -EINVAL;
		else if (bus_end >= e->bus_start && bus_end <= e->bus_end)
			return -EINVAL;
	}
	return -ENOENT;
}

/*
 * init_info - lookup the bus range for the domain in MCFG, and set up
 * config space mapping.
 */
static int pci_acpi_generic_init_info(struct acpi_pci_root_info *ci)
{
	struct acpi_pci_generic_root_info *ri;
	struct acpi_pci_root *root = ci->root;
	u16 seg = root->segment;
	u8 bus_start = root->secondary.start;
	u8 bus_end = root->secondary.end;
	phys_addr_t addr = root->mcfg_addr;
	struct mcfg_entry *e;
	int ret;

	ri = container_of(ci, struct acpi_pci_generic_root_info, common);

	mutex_lock(&gen_acpi_pci_lock);
	ret = mcfg_lookup(seg, bus_start, bus_end);
	switch (ret) {
	case -ENOENT:
		if (addr != 0)	/* use address from _CBA */
			break;
		pr_err("%04x:%02x-%02x mcfg lookup failed\n", seg,
		       bus_start, bus_end);
		goto err_out;
	case -EINVAL:
		pr_err("%04x:%02x-%02x bus range error\n", seg, bus_start,
		       bus_end);
		goto err_out;
	default:
		e = &mcfgsav.cfg[ret];
		if (addr == 0)
			addr = e->addr;
		if (bus_start != e->bus_start) {
			pr_err("%04x:%02x-%02x bus range mismatch %02x\n",
			       seg, bus_start, bus_end, e->bus_start);
			goto err_out;
		}
		if (addr != e->addr) {
			pr_warn("%04x:%02x-%02x addr mismatch, ignoring MCFG\n",
				seg, bus_start, bus_end);
		} else if (bus_end != e->bus_end) {
			pr_warn("%04x:%02x-%02x bus end mismatch %02x\n",
				seg, bus_start, bus_end, e->bus_end);
			bus_end = min(bus_end, e->bus_end);
		}
		break;
	}

	ri->cfg = pci_generic_map_config(addr, bus_start, bus_end, 20, 12);
	if (IS_ERR(ri->cfg)) {
		ret = PTR_ERR(ri->cfg);
		pr_err("%04x:%02x-%02x error %d mapping CFG\n", seg, bus_start,
		       bus_end, ret);
		goto err_out;
	}
	list_add_tail(&ri->node, &gen_acpi_pci_roots);
err_out:
	mutex_unlock(&gen_acpi_pci_lock);
	return ret;
}

/* release_info: free resrouces allocated by init_info */
static void pci_acpi_generic_release_info(struct acpi_pci_root_info *ci)
{
	struct acpi_pci_generic_root_info *ri;

	ri = container_of(ci, struct acpi_pci_generic_root_info, common);

	mutex_lock(&gen_acpi_pci_lock);
	list_del(&ri->node);
	pci_generic_unmap_config(ri->cfg);
	mutex_unlock(&gen_acpi_pci_lock);

	kfree(ri);
}

static struct acpi_pci_root_ops acpi_pci_root_ops = {
	.pci_ops = &acpi_pci_ops,
	.init_info = pci_acpi_generic_init_info,
	.release_info = pci_acpi_generic_release_info,
};

/* Interface called from ACPI code to setup PCI host controller */
struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	int node = acpi_get_node(root->device->handle);
	struct acpi_pci_generic_root_info *ri;
	struct pci_bus *bus, *child;

	ri = kzalloc_node(sizeof(*ri), GFP_KERNEL, node);
	if (!ri)
		return NULL;

	bus = acpi_pci_root_create(root, &acpi_pci_root_ops, &ri->common, ri);
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
	struct mcfg_entry *e, *arr;
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

	arr = kcalloc(n, sizeof(*arr), GFP_KERNEL);
	if (!arr)
		return -ENOMEM;

	for (i = 0, e = arr; i < n; i++, mptr++, e++) {
		e->segment = mptr->pci_segment;
		e->addr =  mptr->address;
		e->bus_start = mptr->start_bus_number;
		e->bus_end = mptr->end_bus_number;
	}

	mcfgsav.cfg = arr;
	mcfgsav.size = n;
	return 0;
}

/* Interface called by ACPI - parse and save MCFG table */
void __init pci_mmcfg_late_init(void)
{
	int err;

	mutex_init(&gen_acpi_pci_lock);
	INIT_LIST_HEAD(&gen_acpi_pci_roots);
	err = acpi_sfi_table_parse(ACPI_SIG_MCFG, handle_mcfg);
	if (err) {
		pr_err(PREFIX " Failed to parse MCFG (%d)\n", err);
		mcfgsav.size = 0;
	} else {
		pr_info(PREFIX " MCFG table at %p, %d entries.\n",
			mcfgsav.cfg, mcfgsav.size);
	}
}
