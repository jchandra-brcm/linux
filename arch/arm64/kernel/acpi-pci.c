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

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/pci-ecam.h>
#include <linux/slab.h>

/* ACPI info for generic ACPI PCI controller */
struct acpi_pci_generic_root_info {
	struct acpi_pci_root_info	common;
	struct pci_config_window	*cfg;	/* config space mapping */
};

/*
 * Lookup the bus range for the domain in MCFG, and set up config space
 * mapping.
 */
static int pci_acpi_setup_ecam_mapping(struct acpi_pci_root *root,
				       struct acpi_pci_generic_root_info *ri)
{
	struct resource *bus_res = &root->secondary;
	struct acpi_device *adev = root->device;
	struct pci_config_window *cfg;
	struct resource cfgres;
	int err;

	err = pci_mcfg_lookup(root, &cfgres, bus_res);
	if (err)
		return err;

	cfg = pci_ecam_create(&adev->dev, &cfgres, bus_res,
						&pci_generic_ecam_ops);
	if (IS_ERR(cfg)) {
		pr_err("%04x:%pR error %ld mapping CAM\n",
			root->segment, bus_res, PTR_ERR(cfg));
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

int pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	struct pci_config_window *cfg = bridge->bus->sysdata;
	struct acpi_device *adev = to_acpi_device(cfg->parent);

	if (adev)
		ACPI_COMPANION_SET(&bridge->dev, adev);

	return 0;
}

void pcibios_add_bus(struct pci_bus *bus)
{
	acpi_pci_add_bus(bus);
}

void pcibios_remove_bus(struct pci_bus *bus)
{
	acpi_pci_remove_bus(bus);
}
