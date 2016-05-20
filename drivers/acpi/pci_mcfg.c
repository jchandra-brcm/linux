/*
 * Copyright (C) 2016 Broadcom
 *	Author: Jayachandran C <jchandra@broadcom.com>
 * Copyright (C) 2016 Semihalf
 * 	Author: Tomasz Nowicki <tn@semihalf.com>
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

/* Copy of the MCFG entries */
static struct {
	struct acpi_mcfg_allocation *entries;
	int nentries;
} saved_mcfg;

int pci_mcfg_lookup(struct acpi_pci_root *root, struct resource *res,
		    struct resource *busr)
{
	struct acpi_mcfg_allocation *mptr = saved_mcfg.entries;
	phys_addr_t cfgaddr = root->mcfg_addr;
	u16 seg = root->segment;
	int i;

	/* look up in saved MCFG copy */
	for (i = 0; i < saved_mcfg.nentries; i++, mptr++)
		if (mptr->pci_segment == seg &&
		    mptr->start_bus_number == busr->start)
			break;

	/* not found, use _CBA if available, else error */
	if (i == saved_mcfg.nentries) {
		if (!cfgaddr) {
			pr_err("%04x:%pR MCFG lookup failed\n", seg, busr);
			return -ENOENT;
		}
		goto done;
	}

	/* found, check address against _CBA if available */
	if (!cfgaddr) {
		cfgaddr = mptr->address;
	} else {
		if (mptr->address != cfgaddr) {
			pr_warn("%04x:%pR CBA %pa != MCFG %lx using CBA\n",
				seg, busr, &cfgaddr, (unsigned long)mptr->address);
			goto done;
		}
	}

	/* bus range check */
	if (mptr->end_bus_number != busr->end) {
		resource_size_t bus_end = min_t(resource_size_t,
					mptr->end_bus_number, busr->end);
		pr_warn("%04x:%pR bus end mismatch, using %02lx\n", seg, busr,
			 (unsigned long)bus_end);
		busr->end = bus_end;
	}
done:
	res->start = cfgaddr + (busr->start << 20);
	res->end = cfgaddr + ((busr->end + 1) << 20) - 1;
	res->flags = IORESOURCE_MEM;
	return 0;
}

static __init int pci_mcfg_parse(struct acpi_table_header *header)
{
	struct acpi_table_mcfg *mcfg;
	struct acpi_mcfg_allocation *mnew, *mptr;
	int n;

	mcfg = (struct acpi_table_mcfg *)header;
	n = (mcfg->header.length - sizeof(*mcfg)) / sizeof(*mptr);
	if (n <= 0 || n > 255) {
		pr_err("ACPI: MCFG has incorrect entries (%d).\n", n);
		return -EINVAL;
	}

	mptr = (struct acpi_mcfg_allocation *)&mcfg[1];
	mnew = kcalloc(n, sizeof(*mnew), GFP_KERNEL);
	if (mnew == NULL)
		return -ENOMEM;
	memcpy(mnew, mptr, n * sizeof(*mnew));

	saved_mcfg.entries = mnew;
	saved_mcfg.nentries = n;
	pr_info("ACPI: MCFG table loaded, %d entries saved\n", n);
	return 0;
}

/* Interface called by ACPI - parse and save MCFG table */
void __init pci_mmcfg_late_init(void)
{
	int err = acpi_table_parse(ACPI_SIG_MCFG, pci_mcfg_parse);
	if (err)
		pr_err("ACPI Failed to parse MCFG (%d)\n", err);
}
