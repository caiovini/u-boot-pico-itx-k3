// SPDX-License-Identifier: GPL-2.0+
/*
 * Spacemit K1 PCIe host controller driver
 *
 * Copyright (c) 2023, Spacemit Corporation.
 * Copyright (c) 2026, RISCstar Ltd.
 *
 */
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm-generic/gpio.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <dm/read.h>
#include <generic-phy.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <log.h>
#include <pci.h>
#include <power-domain.h>
#include <reset.h>

#include "pcie_dw_common.h"
#include "pcie_dw_spacemit.h"

DECLARE_GLOBAL_DATA_PTR;

struct pcie_dw_spacemit {
	/* Must be first member of the struct */
	struct pcie_dw dw;
	void __iomem		*apmu_base;
	u32			apmu_offset;
	void __iomem		*pcie_mgmt;
	void __iomem		*phy_ahb;	/* DT "link" or "phy_ahb" */
	int			port_id;
	u32			max_link_speed;
	u32			num_lanes;
	bool			is_k3;

	/* reset, clock, phy resources */
	struct clk_bulk		clks;
	struct reset_ctl_bulk	rsts;
	struct phy_bulk		phys;
	struct gpio_desc		pwr_on_gpio;
	int			power_on_status;
};

static inline u32 spacemit_pcie_readl(struct pcie_dw_spacemit *pcie, u32 offset)
{
	return readl(pcie->apmu_base + pcie->apmu_offset + offset);
}

static inline void spacemit_pcie_writel(struct pcie_dw_spacemit *pcie,
					u32 offset, u32 value)
{
	writel(value, pcie->apmu_base + pcie->apmu_offset + offset);
}

static inline u32 spacemit_pcie_phy_ahb_readl(struct pcie_dw_spacemit *pcie, u32 offset)
{
	return readl(pcie->phy_ahb + offset);
}

static void pcie_dw_configure(struct pcie_dw_spacemit *pci, u32 cap_speed)
{
	u32 val;

	dw_pcie_dbi_write_enable(&pci->dw, true);

	val = readl(pci->dw.dbi_base + PCIE_LINK_CAPABILITY);
	val &= ~TARGET_LINK_SPEED_MASK;
	val |= cap_speed;
	writel(val, pci->dw.dbi_base + PCIE_LINK_CAPABILITY);

	val = readl(pci->dw.dbi_base + PCIE_LINK_CTL_2);
	val &= ~TARGET_LINK_SPEED_MASK;
	val |= cap_speed;
	writel(val, pci->dw.dbi_base + PCIE_LINK_CTL_2);

	if (pci->num_lanes)
		dw_pcie_link_set_max_link_width(&pci->dw, pci->num_lanes);

	dw_pcie_dbi_write_enable(&pci->dw, false);
}

static int is_link_up(struct pcie_dw_spacemit *pci)
{
	u32 val;

	if (pci->is_k3 && pci->phy_ahb) {
		val = spacemit_pcie_phy_ahb_readl(pci, K1X_PHY_AHB_LINK_STS);
		return ((val & (RDLH_LINK_UP | SMLH_LINK_UP)) ==
			(RDLH_LINK_UP | SMLH_LINK_UP));
	}

	val = readl(pci->dw.dbi_base + PCIE_PORT_DEBUG0);
	val &= PORT_LOGIC_LTSSM_STATE_MASK;

	return (val == PORT_LOGIC_LTSSM_STATE_L0);
}

static int wait_link_up(struct pcie_dw_spacemit *pci)
{
	unsigned long timeout;

	timeout = get_timer(0) + PCIE_LINK_UP_TIMEOUT_MS;
	while (!is_link_up(pci)) {
		if (get_timer(0) > timeout) {
			dev_dbg(pci->dw.dev, "LTSSM link training timeout\n");
			return 0;
		}
	}

	return 1;
}

static int pcie_dw_spacemit_pcie_link_up(struct pcie_dw_spacemit *pci, u32 cap_speed)
{
	u32 reg;

	if (is_link_up(pci)) {
		printf("PCI Link already up before configuration!\n");
		return 1;
	}

	/* DW pre link configurations */
	pcie_dw_configure(pci, cap_speed);

	/* Initiate link training */
	reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg |= LTSSM_EN;
	reg &= ~APP_HOLD_PHY_RST;
	spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	/* Check that link was established */
	if (!wait_link_up(pci))
		return 0;

	/*
	 * Link can be established in Gen 1, still need to wait
	 * until MAC negotiation is completed
	 */
	udelay(100);

	return 1;
}

static int pcie_set_mode(struct pcie_dw_spacemit *pci,
			 enum dw_pcie_device_mode mode)
{
	u32 reg;

	switch (mode) {
	case DW_PCIE_RC_TYPE:
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg |= DEVICE_TYPE_RC | PCIE_AUX_PWR_DET;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		reg = spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
		reg |= PCIE_IGNORE_PERSTN;
		if (pci->is_k3)
			reg |= K3_PCIE_IGNORE_PERSTN;
		spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg);
		break;
	case DW_PCIE_EP_TYPE:
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg &= ~DEVICE_TYPE_RC;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);
		break;
	default:
		dev_err(pci->dw.dev, "INVALID device type %d\n", mode);
		return -EINVAL;
	}

	return 0;
}

static int spacemit_pcie_host_init(struct pcie_dw_spacemit *pci)
{
	u32 reg;

	mdelay(100);
	/* set Perst# gpio high state*/
	reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg &= ~PCIE_RC_PERST;
	spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	if (pci->is_k3) {
		reg = spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
		reg |= PCIE_PERSTN_OE | PCIE_PERSTN_OUT;
		spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg);
	}

	return 0;
}

static int pcie_dw_init_id(struct pcie_dw_spacemit *pci)
{
	u16 dev_id = pci->is_k3 ? SPACEMIT_K3_PCIE_DEVICE_ID :
				  SPACEMIT_PCIE_DEVICE_ID;

	dw_pcie_dbi_write_enable(&pci->dw, true);
	writew(SPACEMIT_PCIE_VENDOR_ID, pci->dw.dbi_base + PCI_VENDOR_ID);
	writew(dev_id, pci->dw.dbi_base + PCI_DEVICE_ID);
	dw_pcie_dbi_write_enable(&pci->dw, false);

	return 0;
}

static int k3_pcie_map_lanes(struct pcie_dw_spacemit *pci)
{
	void __iomem *mgmt = pci->pcie_mgmt;
	u32 mask = 0, set = 0, val;

	if (!mgmt && pci->apmu_base)
		mgmt = pci->apmu_base + K3_APMU_PCIE_MGMT_OFFSET;
	if (!mgmt)
		return 0;

	switch (pci->port_id) {
	case 0:
		mask = 0x18;
		if (pci->num_lanes == 8)
			set = 0x00;
		else if (pci->num_lanes == 4)
			set = 0x10;
		else if (pci->num_lanes == 2)
			set = 0x18;
		else
			return -EINVAL;
		break;
	case 1:
		if (pci->num_lanes != 2)
			return -EINVAL;
		mask = 0x18;
		set = 0x18;
		break;
	case 2:
		mask = 0x16;
		if (pci->num_lanes == 2)
			set = 0x10;
		else if (pci->num_lanes == 1)
			set = 0x12;
		else
			return -EINVAL;
		break;
	case 3:
		if (pci->num_lanes != 1)
			return -EINVAL;
		mask = 0x11;
		set = 0x10;
		break;
	case 4:
		if (pci->num_lanes != 1)
			return -EINVAL;
		mask = 0x10;
		set = 0x10;
		break;
	default:
		return -EINVAL;
	}

	val = readl(mgmt);
	val = (val & ~mask) | set;
	writel(val, mgmt);

	return 0;
}

static int k1x_power_on(struct pcie_dw_spacemit *pci, int on)
{
	struct pcie_dw *dwp = &pci->dw;
	int gpio_val = 0;

	if (on) {
		if (pci->power_on_status)
			gpio_val = 1;
		else
			gpio_val = 0;
	} else {
		if (pci->power_on_status)
			gpio_val = 0;
		else
			gpio_val = 1;
	}

	if (dm_gpio_is_valid(&pci->pwr_on_gpio)) {
		dev_info(dwp->dev, "PCIe interface power %s, set gpio %d to %d\n",
			 on ? "on" : "off", pci->pwr_on_gpio.offset, gpio_val);
		dm_gpio_set_value(&pci->pwr_on_gpio, gpio_val);
	}

	return 0;
}

static int pcie_dw_spacemit_probe(struct udevice *dev)
{
	struct pcie_dw_spacemit *pci = dev_get_priv(dev);
	struct udevice *ctlr = pci_get_controller(dev);
	struct pci_controller *hose = dev_get_uclass_priv(ctlr);
	struct phy phy0 = {0};
	ofnode port;
	int ret, i;
	u32 reg;

	pci->dw.first_busno = dev_seq(dev);
	pci->dw.dev = dev;

	/* enable pcie clk and deassert resets */
	clk_enable_bulk(&pci->clks);
	reset_deassert_bulk(&pci->rsts);

	if (pci->is_k3) {
		/*
		 * Release APP_HOLD_PHY_RST (BIT(30), which defaults to 1 out of reset)
		 * in both Port A (0x1f0) and Port B (0x1d0, when 4-lane Port 0 bonds
		 * phy1) BEFORE initializing the PHYs so the PHY PLL can lock!
		 */
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg &= ~APP_HOLD_PHY_RST;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		if (pci->port_id == 0 && pci->num_lanes >= 4 && pci->apmu_base) {
			/* Also enable Port B clocks/resets & release phy1 APP_HOLD_PHY_RST at 0x1d0 */
			u32 reg_b = readl(pci->apmu_base + 0x1d0);

			reg_b |= 0x3f;
			reg_b &= ~APP_HOLD_PHY_RST;
			writel(reg_b, pci->apmu_base + 0x1d0);
		}

		ret = k3_pcie_map_lanes(pci);
		if (ret) {
			dev_err(dev, "failed to map lanes: %d\n", ret);
			return ret;
		}

		if (pci->phys.count > 0) {
			for (i = 0; i < pci->phys.count; i++) {
				ret = generic_phy_init(&pci->phys.phys[i]);
				if (ret) {
					dev_err(dev, "failed to init phy %d: %d\n", i, ret);
					return ret;
				}
				ret = generic_phy_power_on(&pci->phys.phys[i]);
				if (ret) {
					dev_err(dev, "failed to power on phy %d: %d\n", i, ret);
					return ret;
				}
			}
		} else {
			port = ofnode_first_subnode(dev_ofnode(dev));
			if (!ofnode_valid(port)) {
				dev_err(dev, "failed to find PCIe port node\n");
				return -ENODEV;
			}
			for (i = 0; ; i++) {
				struct phy cphy = {0};

				ret = generic_phy_get_by_index_nodev(port, i, &cphy);
				if (ret) {
					if (i == 0) {
						dev_err(dev, "failed to get pcie-phy: %d\n", ret);
						return ret;
					}
					break;
				}
				ret = generic_phy_init(&cphy);
				if (ret) {
					dev_err(dev, "failed to init PHY %d: %d\n", i, ret);
					return ret;
				}
				ret = generic_phy_power_on(&cphy);
				if (ret) {
					dev_err(dev, "failed to power on PHY %d: %d\n", i, ret);
					return ret;
				}
			}
		}

		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg &= ~LTSSM_EN;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg |= PCIE_AUX_PWR_DET;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		printf("%s %s: pcie config device cmd in rc mode: 0x%x\n",
		       dev->driver->name, dev->name, reg);

		/* Configure K3 PERST# in PCIE_CTRL_LOGIC (offset 0x04) */
		reg = spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
		reg |= K3_PCIE_IGNORE_PERSTN;
		spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg);

		reg = spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
		reg |= PCIE_PERSTN_OE | PCIE_PERSTN_OUT;
		spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg);

		reg = spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
		reg &= ~PCIE_PERSTN_OUT;
		spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg);

		udelay(100000);

		reg = spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
		reg |= PCIE_PERSTN_OUT;
		spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg);

		/* Configure GEN3_RELATED_OFF (0x8a8) */
		reg = readl(pci->dw.dbi_base + GEN3_RELATED_OFF);
		reg = (reg & 0xff0000ff) | BIT(15);
		writel(reg, pci->dw.dbi_base + GEN3_RELATED_OFF);
	} else {
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg &= ~LTSSM_EN;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		/* set Perst# (fundamental reset) gpio low state*/
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg |= PCIE_RC_PERST;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		/* The PHY is described on the root port child node */
		port = ofnode_first_subnode(dev_ofnode(dev));
		if (!ofnode_valid(port)) {
			dev_err(dev, "failed to find PCIe port node\n");
			return -ENODEV;
		}

		/* Ensure PHY clocks/gates and reset hold are released before PHY probe */
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg |= DEVICE_TYPE_RC | PCIE_AUX_PWR_DET | APP_HOLD_PHY_RST |
		       PCIE_PHY_LANE_CTRL_MASK;
		reg &= ~GLOBAL_PHY_RST;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);
		reg &= ~(u32)APP_HOLD_PHY_RST;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		ret = generic_phy_get_by_index_nodev(port, 0, &phy0);
		if (!ret) {
			ret = generic_phy_init(&phy0);
			if (ret) {
				dev_err(dev, "failed to init PHY: %d\n", ret);
				return ret;
			}

			ret = generic_phy_power_on(&phy0);
			if (ret) {
				dev_err(dev, "failed to power on PHY: %d\n", ret);
				return ret;
			}
		} else {
			dev_err(dev, "failed to get pcie-phy: %d\n", ret);
			return ret;
		}

		pcie_set_mode(pci, DW_PCIE_RC_TYPE);

		/* power on the interface */
		k1x_power_on(pci, 1);

		spacemit_pcie_host_init(pci);
	}

	pcie_dw_setup_host(&pci->dw);
	pcie_dw_init_id(pci);

	if (!pcie_dw_spacemit_pcie_link_up(pci, pci->max_link_speed)) {
		printf("PCIE-%d: Link down\n", dev_seq(dev));
		if (!pci->is_k3)
			pcie_dw_prog_outbound_atu_unroll(&pci->dw,
							 PCIE_ATU_REGION_INDEX0,
							 PCIE_ATU_TYPE_MEM,
							 pci->dw.mem.phys_start,
							 pci->dw.mem.bus_start,
							 pci->dw.mem.size);
		return -ENODEV;
	}

	printf("PCIE-%d: Link up (Gen%d-x%d, Bus%d)\n", dev_seq(dev),
	       pcie_dw_get_link_speed(&pci->dw),
	       pcie_dw_get_link_width(&pci->dw),
	       hose->first_busno);

	ret = pcie_dw_prog_outbound_atu_unroll(&pci->dw, PCIE_ATU_REGION_INDEX0,
					       PCIE_ATU_TYPE_MEM,
					       pci->dw.mem.phys_start,
					       pci->dw.mem.bus_start,
					       pci->dw.mem.size);

	return 0;
}

static int pcie_dw_spacemit_of_to_plat(struct udevice *dev)
{
	int ret = 0;
	struct pcie_dw_spacemit *pcie = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	fdt_addr_t dbi_addr;

	/* Get the controller base address */
	pcie->dw.dbi_base = (void *)dev_read_addr_name(dev, "dbi");
	if ((fdt_addr_t)pcie->dw.dbi_base == FDT_ADDR_T_NONE)
		return -EINVAL;

	dbi_addr = (fdt_addr_t)pcie->dw.dbi_base;

	/* Get the config space base address and size */
	pcie->dw.cfg_base = (void *)dev_read_addr_size_name(dev, "config",
							 &pcie->dw.cfg_size);
	if ((fdt_addr_t)pcie->dw.cfg_base == FDT_ADDR_T_NONE)
		return -EINVAL;

	/* Get the iATU base address and size */
	pcie->dw.atu_base = (void *)dev_read_addr_name(dev, "atu");
	if ((fdt_addr_t)pcie->dw.atu_base == FDT_ADDR_T_NONE)
		return -EINVAL;

	pcie->is_k3 = device_is_compatible(dev, "spacemit,k3-pcie");

	/* Get the PHY AHB base address (called "link" in Linux DTS, optional on K3) */
	pcie->phy_ahb = (void *)dev_read_addr_name(dev, "link");
	if ((fdt_addr_t)pcie->phy_ahb == FDT_ADDR_T_NONE) {
		pcie->phy_ahb = (void *)dev_read_addr_name(dev, "phy_ahb");
		if ((fdt_addr_t)pcie->phy_ahb == FDT_ADDR_T_NONE) {
			if (!pcie->is_k3)
				return -EINVAL;
			pcie->phy_ahb = NULL;
		}
	}

	if (dev_read_u32(dev, "max-link-speed", &pcie->max_link_speed) ||
	    pcie->max_link_speed < LINK_SPEED_GEN_1 ||
	    pcie->max_link_speed > LINK_SPEED_GEN_3) {
		pcie->max_link_speed = pcie->is_k3 ? LINK_SPEED_GEN_3 :
						     LINK_SPEED_GEN_1;
	}

	pcie->num_lanes = dev_read_u32_default(dev, "num-lanes", 1);
	if (pcie->num_lanes == 0 || pcie->num_lanes > 8)
		pcie->num_lanes = 1;

	/* Get APMU regmap from spacemit,apmu property or "app" reg */
	ret = dev_read_phandle_with_args(dev, "spacemit,apmu", NULL, 1, 0,
					 &args);
	if (!ret) {
		pcie->apmu_base = (void __iomem *)ofnode_get_addr(args.node);
		if (!pcie->apmu_base)
			return -EINVAL;
		pcie->apmu_offset = args.args[0];
	} else {
		fdt_addr_t app_addr = dev_read_addr_name(dev, "app");

		if (app_addr == FDT_ADDR_T_NONE)
			return ret;
		pcie->apmu_base = (void __iomem *)app_addr;
		pcie->apmu_offset = 0;
	}

	{
		u32 mgmt_addr = 0;

		if (!dev_read_u32(dev, "spacemit,pcie-mgmt", &mgmt_addr) && mgmt_addr)
			pcie->pcie_mgmt = (void __iomem *)(uintptr_t)mgmt_addr;
		else if (pcie->is_k3 && pcie->apmu_offset)
			pcie->pcie_mgmt = pcie->apmu_base + K3_APMU_PCIE_MGMT_OFFSET;
	}

	/* Derive port ID from DT or DBI base address */
	if (dev_read_u32(dev, "spacemit,pcie-port", (u32 *)&pcie->port_id)) {
		if (dbi_addr >= 0x80000000 && dbi_addr < 0x82000000)
			pcie->port_id = (dbi_addr - 0x80000000) / SPACEMIT_PCIE_DBI_STRIDE;
		else
			pcie->port_id = (dbi_addr - SPACEMIT_PCIE_DBI_BASE) / SPACEMIT_PCIE_DBI_STRIDE;
	}

	generic_phy_get_bulk(dev, &pcie->phys);

	ret = clk_get_bulk(dev, &pcie->clks);
	if (ret) {
		dev_warn(dev, "failed to get clocks: %d\n", ret);
		return ret;
	}

	ret = reset_get_bulk(dev, &pcie->rsts);
	if (ret) {
		dev_warn(dev, "failed to get resets: %d\n", ret);
		return ret;
	}

	gpio_request_by_name(dev, "k1x,pwr_on", 0, &pcie->pwr_on_gpio,
			     GPIOD_IS_OUT);
	if (!dm_gpio_is_valid(&pcie->pwr_on_gpio))
		dev_info(dev, "has no power on gpio.\n");

	ret = dev_read_u32(dev, "power-on-status", &pcie->power_on_status);
	if (ret) {
		dev_info(dev, "has no power-on-status flag, use default.\n");
		pcie->power_on_status = 1;
	}

	return 0;
}

static const struct dm_pci_ops pcie_dw_spacemit_ops = {
	.read_config	= pcie_dw_read_config,
	.write_config	= pcie_dw_write_config,
};

static const struct udevice_id pcie_dw_spacemit_ids[] = {
	{ .compatible = "spacemit,k1-pcie" },
	{ .compatible = "spacemit,k3-pcie" },
	{ }
};

U_BOOT_DRIVER(pcie_dw_spacemit) = {
	.name			= "pcie_dw_spacemit",
	.id			= UCLASS_PCI,
	.of_match		= pcie_dw_spacemit_ids,
	.ops			= &pcie_dw_spacemit_ops,
	.of_to_plat	= pcie_dw_spacemit_of_to_plat,
	.probe			= pcie_dw_spacemit_probe,
	.priv_auto	= sizeof(struct pcie_dw_spacemit),
};
