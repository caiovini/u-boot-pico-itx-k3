// SPDX-License-Identifier: GPL-2.0+
/*
 * rtl8169.c : U-Boot driver for the RealTek RTL8169
 *
 * Masami Komiya (mkomiya@sonare.it)
 *
 * Most part is taken from r8169.c of etherboot
 *
 */

/**************************************************************************
*    r8169.c: Etherboot device driver for the RealTek RTL-8169 Gigabit
*    Written 2003 by Timothy Legge <tlegge@rogers.com>
*
*    Portions of this code based on:
*	r8169.c: A RealTek RTL-8169 Gigabit Ethernet driver
*		for Linux kernel 2.4.x.
*
*    Written 2002 ShuChen <shuchen@realtek.com.tw>
*	  See Linux Driver for full information
*
*    Linux Driver Version 1.27a, 10.02.2002
*
*    Thanks to:
*	Jean Chen of RealTek Semiconductor Corp. for
*	providing the evaluation NIC used to develop
*	this driver.  RealTek's support for Etherboot
*	is appreciated.
*
*    REVISION HISTORY:
*    ================
*
*    v1.0	11-26-2003	timlegge	Initial port of Linux driver
*    v1.5	01-17-2004	timlegge	Initial driver output cleanup
*
*    Indent Options: indent -kr -i8
***************************************************************************/
/*
 * 26 August 2006 Mihai Georgian <u-boot@linuxnotincluded.org.uk>
 * Modified to use le32_to_cpu and cpu_to_le32 properly
 */
#include <cpu_func.h>
#include <dm.h>
#include <errno.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <net.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <pci.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include "rtl8127a_fw.h"

#undef DEBUG_RTL8169
#undef DEBUG_RTL8169_TX
#undef DEBUG_RTL8169_RX

#define drv_version "v1.5"
#define drv_date "01-17-2004"

static unsigned long ioaddr;

/* Condensed operations for readability. */
#define currticks()	get_timer(0)

/* media options */
#define MAX_UNITS 8
static int media[MAX_UNITS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

/* MAC address length*/
#define MAC_ADDR_LEN	6

/* max supported gigabit ethernet frame size -- must be at least (dev->mtu+14+4).*/
#define MAX_ETH_FRAME_SIZE	1536

#define TX_FIFO_THRESH 256	/* In bytes */

#define RX_FIFO_THRESH	7	/* 7 means NO threshold, Rx buffer level before first PCI xfer.	 */
#define RX_DMA_BURST	6	/* Maximum PCI burst, '6' is 1024 */
#define TX_DMA_BURST	6	/* Maximum PCI burst, '6' is 1024 */
#define EarlyTxThld	0x3F	/* 0x3F means NO early transmit */
#define RxPacketMaxSize 0x0800	/* Maximum size supported is 16K-1 */
#define InterFrameGap	0x03	/* 3 means InterFrameGap = the shortest one */

#define NUM_TX_DESC	1024	/* Number of Tx descriptor registers */
#define NUM_RX_DESC	512	/* Number of Rx descriptor registers */
#define RX_BUF_SIZE	1536	/* Rx Buffer size */
#define RX_BUF_LEN	8192

#define RTL_MIN_IO_SIZE 0x80
#define TX_TIMEOUT  (6*HZ)

/* write/read MMIO register. Notice: {read,write}[wl] do the necessary swapping */
#define RTL_W8(reg, val8)	writeb((val8), (void *)(ioaddr + (reg)))
#define RTL_W16(reg, val16)	writew((val16), (void *)(ioaddr + (reg)))
#define RTL_W32(reg, val32)	writel((val32), (void *)(ioaddr + (reg)))
#define RTL_R8(reg)		readb((void *)(ioaddr + (reg)))
#define RTL_R16(reg)		readw((void *)(ioaddr + (reg)))
#define RTL_R32(reg)		readl((void *)(ioaddr + (reg)))

#define bus_to_phys(a)	pci_mem_to_phys((pci_dev_t)(unsigned long)dev->priv, \
	(pci_addr_t)(unsigned long)a)
#define phys_to_bus(a)	pci_phys_to_mem((pci_dev_t)(unsigned long)dev->priv, \
	(phys_addr_t)a)

enum RTL8169_registers {
	MAC0 = 0,		/* Ethernet hardware address. */
	MAR0 = 8,		/* Multicast filter. */
	TxDescStartAddrLow = 0x20,
	TxDescStartAddrHigh = 0x24,
	TxHDescStartAddrLow = 0x28,
	TxHDescStartAddrHigh = 0x2c,
	FLASH = 0x30,
	ERSR = 0x36,
	ChipCmd = 0x37,
	TxPoll_8169 = 0x38,
	IntrMask_8169 = 0x3C,
	IntrStatus_8169 = 0x3E,
	TxConfig = 0x40,
	RxConfig = 0x44,
	RxMissed = 0x4C,
	Cfg9346 = 0x50,
	Config0 = 0x51,
	Config1 = 0x52,
	Config2 = 0x53,
	Config3 = 0x54,
	Config4 = 0x55,
	Config5 = 0x56,
	MultiIntr = 0x5C,
	PHYAR = 0x60,
	TBICSR = 0x64,
	TBI_ANAR = 0x68,
	TBI_LPAR = 0x6A,
	PHYstatus = 0x6C,
	RxMaxSize = 0xDA,
	CPlusCmd = 0xE0,
	RxDescStartAddrLow = 0xE4,
	RxDescStartAddrHigh = 0xE8,
	EarlyTxThres = 0xEC,
	FuncEvent = 0xF0,
	FuncEventMask = 0xF4,
	FuncPresetState = 0xF8,
	FuncForceEvent = 0xFC,
};

enum RTL8125_registers {
	IntrMask_8125 = 0x38,
	IntrStatus_8125 = 0x3C,
	TxPoll_8125 = 0x90,
	RX_DESC_MODE = 0xd8,
	MACOCP = 0xb0,
	GPHY_OCP = 0xb8,
	RADMFIFO_PROTECT = 0x0402,
	TX_DESC_MODE = 0xeb58,
};

/* MAC / GPHY OCP indirect access via MACOCP / GPHY_OCP registers */
#define RTL8125_OCP_WRITE	0x80000000
#define RTL8125_OCP_ADDR_SHIFT	16
#define GPHY_OCP_FLAG		0x80000000
#define FIBER_MODE_RTL8127ATF	0x07
#define RX_DESC_CRC_DROP	BIT(0)
#define RX_DESC_TYPE		BIT(1)

/* TX_DESC_MODE (MAC OCP 0xeb58): TX descriptor format select, bits [1:0] */
#define TX_DESC_FMT_MASK	GENMASK(1, 0)
#define TX_DESC_FMT_STD		BIT(0)
#define TX_DESC_FMT_V3		BIT(1)
#define RX_PAUSE_SLOT_ON	BIT(11)

enum RTL8169_register_content {
	/*InterruptStatusBits */
	SYSErr = 0x8000,
	PCSTimeout = 0x4000,
	SWInt = 0x0100,
	TxDescUnavail = 0x80,
	RxFIFOOver = 0x40,
	RxUnderrun = 0x20,
	RxOverflow = 0x10,
	TxErr = 0x08,
	TxOK = 0x04,
	RxErr = 0x02,
	RxOK = 0x01,

	/*RxStatusDesc */
	RxRES = 0x00200000,
	RxCRC = 0x00080000,
	RxRUNT = 0x00100000,
	RxRWT = 0x00400000,

	/*ChipCmdBits */
	CmdReset = 0x10,
	CmdRxEnb = 0x08,
	CmdTxEnb = 0x04,
	RxBufEmpty = 0x01,

	/*Cfg9346Bits */
	Cfg9346_Lock = 0x00,
	Cfg9346_Unlock = 0xC0,

	/*rx_mode_bits */
	AcceptErr = 0x20,
	AcceptRunt = 0x10,
	AcceptBroadcast = 0x08,
	AcceptMulticast = 0x04,
	AcceptMyPhys = 0x02,
	AcceptAllPhys = 0x01,

	/*RxConfigBits */
	RxCfgFIFOShift = 13,
	RxCfgDMAShift = 8,

	/*TxConfigBits */
	TxInterFrameGapShift = 24,
	TxDMAShift = 8,		/* DMA burst value (0-7) is shift this many bits */

	/*rtl8169_PHYstatus */
	TBI_Enable = 0x80,
	TxFlowCtrl = 0x40,
	RxFlowCtrl = 0x20,
	_1000bpsF = 0x10,
	_100bps = 0x08,
	_10bps = 0x04,
	LinkStatus = 0x02,
	FullDup = 0x01,

	/*GIGABIT_PHY_registers */
	PHY_CTRL_REG = 0,
	PHY_STAT_REG = 1,
	PHY_AUTO_NEGO_REG = 4,
	PHY_1000_CTRL_REG = 9,

	/*GIGABIT_PHY_REG_BIT */
	PHY_Restart_Auto_Nego = 0x0200,
	PHY_Enable_Auto_Nego = 0x1000,

	/* PHY_STAT_REG = 1; */
	PHY_Auto_Nego_Comp = 0x0020,

	/* PHY_AUTO_NEGO_REG = 4; */
	PHY_Cap_10_Half = 0x0020,
	PHY_Cap_10_Full = 0x0040,
	PHY_Cap_100_Half = 0x0080,
	PHY_Cap_100_Full = 0x0100,

	/* PHY_1000_CTRL_REG = 9; */
	PHY_Cap_1000_Full = 0x0200,

	PHY_Cap_Null = 0x0,

	/*_MediaType*/
	_10_Half = 0x01,
	_10_Full = 0x02,
	_100_Half = 0x04,
	_100_Full = 0x08,
	_1000_Full = 0x10,

	/*_TBICSRBit*/
	TBILinkOK = 0x02000000,

	/* FuncEvent/Misc */
	RxDv_Gated_En = 0x80000,
};

static struct {
	const char *name;
	u8 version;		/* depend on RTL8169 docs */
	u32 RxConfigMask;	/* should clear the bits supported by this chip */
} rtl_chip_info[] = {
	{"RTL-8169", 0x00, 0xff7e1880,},
	{"RTL-8169", 0x04, 0xff7e1880,},
	{"RTL-8169", 0x00, 0xff7e1880,},
	{"RTL-8169s/8110s",	0x02, 0xff7e1880,},
	{"RTL-8169s/8110s",	0x04, 0xff7e1880,},
	{"RTL-8169sb/8110sb",	0x10, 0xff7e1880,},
	{"RTL-8169sc/8110sc",	0x18, 0xff7e1880,},
	{"RTL-8168b/8111sb",	0x30, 0xff7e1880,},
	{"RTL-8168b/8111sb",	0x38, 0xff7e1880,},
	{"RTL-8168c/8111c",	0x3c, 0xff7e1880,},
	{"RTL-8168d/8111d",	0x28, 0xff7e1880,},
	{"RTL-8168evl/8111evl",	0x2e, 0xff7e1880,},
	{"RTL-8168/8111g",	0x4c, 0xff7e1880,},
	{"RTL-8101e",		0x34, 0xff7e1880,},
	{"RTL-8100e",		0x32, 0xff7e1880,},
	{"RTL-8168h/8111h",	0x54, 0xff7e1880,},
	{"RTL-8125B",		0x64, 0xff7e1880,},
	{"RTL-8125d",		0x6a, 0xff7e5880,},
	{"RTL-8126A",		0x66, 0xff7e5880,},
	{"RTL-8127A",		0x6e, 0xff7e5880,},
};

enum _DescStatusBit {
	OWNbit = 0x80000000,
	EORbit = 0x40000000,
	FSbit = 0x20000000,
	LSbit = 0x10000000,
};

struct TxDesc {
	u32 status;
	u32 vlan_tag;
	u32 buf_addr;
	u32 buf_Haddr;
};

struct RxDesc {
	u32 status;
	u32 vlan_tag;
	u32 buf_addr;
	u32 buf_Haddr;
};

static unsigned char rxdata[RX_BUF_LEN];

#define RTL8169_DESC_SIZE 16

#if ARCH_DMA_MINALIGN > 256
#  define RTL8169_ALIGN ARCH_DMA_MINALIGN
#else
#  define RTL8169_ALIGN 256
#endif

/*
 * Warn if the cache-line size is larger than the descriptor size. In such
 * cases the driver will likely fail because the CPU needs to flush the cache
 * when requeuing RX buffers, therefore descriptors written by the hardware
 * may be discarded.
 *
 * This can be fixed by defining CONFIG_SYS_NONCACHED_MEMORY which will cause
 * the driver to allocate descriptors from a pool of non-cached memory.
 *
 * Hardware maintain D-cache coherency in RISC-V architecture.
 */
#if RTL8169_DESC_SIZE < ARCH_DMA_MINALIGN
#if !defined(CONFIG_SYS_NONCACHED_MEMORY) && \
	!CONFIG_IS_ENABLED(SYS_DCACHE_OFF) && !defined(CONFIG_X86) && !defined(CONFIG_RISCV)
#warning cache-line size is larger than descriptor size
#endif
#endif

/*
 * Create a static buffer of size RX_BUF_SZ for each TX Descriptor. All
 * descriptors point to a part of this buffer.
 */
DEFINE_ALIGN_BUFFER(u8, txb, NUM_TX_DESC * RX_BUF_SIZE, RTL8169_ALIGN);

/*
 * Create a static buffer of size RX_BUF_SZ for each RX Descriptor. All
 * descriptors point to a part of this buffer.
 */
DEFINE_ALIGN_BUFFER(u8, rxb, NUM_RX_DESC * RX_BUF_SIZE, RTL8169_ALIGN);
DEFINE_ALIGN_BUFFER(u64, rtl_tally_buf, 32, RTL8169_ALIGN);

struct rtl8169_private {
	ulong iobase;
	void *mmio_addr;	/* memory map physical address */
	int chipset;
	u8 pci_rev;
	unsigned long cur_rx;	/* Index into the Rx descriptor buffer of next Rx pkt. */
	unsigned long cur_tx;	/* Index into the Tx descriptor buffer of next Rx pkt. */
	unsigned long dirty_tx;
	struct TxDesc *TxDescArray;	/* Index of 256-alignment Tx Descriptor buffer */
	struct RxDesc *RxDescArray;	/* Index of 256-alignment Rx Descriptor buffer */
	unsigned char *RxBufferRings;	/* Index of Rx Buffer  */
	unsigned char *RxBufferRing[NUM_RX_DESC];	/* Index of Rx Buffer array */
	unsigned char *Tx_skbuff[NUM_TX_DESC];
} tpx;

static struct rtl8169_private *tpc;

static const unsigned int rtl8169_rx_config =
    (RX_FIFO_THRESH << RxCfgFIFOShift) | (RX_DMA_BURST << RxCfgDMAShift);

static struct pci_device_id supported[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8125) },
	{ PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8126) },
	{ PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8127) },
	{ PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8161) },
	{ PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8167) },
	{ PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8168) },
	{ PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8169) },
	{}
};

void mdio_write(int RegAddr, int value)
{
	int i;

	RTL_W32(PHYAR, 0x80000000 | (RegAddr & 0xFF) << 16 | value);
	udelay(1000);

	for (i = 2000; i > 0; i--) {
		/* Check if the RTL8169 has completed writing to the specified MII register */
		if (!(RTL_R32(PHYAR) & 0x80000000)) {
			break;
		} else {
			udelay(100);
		}
	}
}

int mdio_read(int RegAddr)
{
	int i, value = -1;

	RTL_W32(PHYAR, 0x0 | (RegAddr & 0xFF) << 16);
	udelay(1000);

	for (i = 2000; i > 0; i--) {
		/* Check if the RTL8169 has completed retrieving data from the specified MII register */
		if (RTL_R32(PHYAR) & 0x80000000) {
			value = (int) (RTL_R32(PHYAR) & 0xFFFF);
			break;
		} else {
			udelay(100);
		}
	}
	return value;
}

/* RTL8125/8126/8127 share the same register layout and quirks */
static bool rtl_is_8125(struct pci_child_plat *pplat)
{
	return pplat->device == 0x8125 || pplat->device == 0x8126 ||
	       pplat->device == 0x8127;
}

static void rtl_mac_ocp_write(u16 reg_addr, u16 value)
{
	u32 data32;

	data32 = (u32)(reg_addr / 2) << RTL8125_OCP_ADDR_SHIFT;
	data32 += value;
	data32 |= RTL8125_OCP_WRITE;
	RTL_W32(MACOCP, data32);
}

static u16 rtl_mac_ocp_read(u16 reg_addr)
{
	u32 data32;

	data32 = (u32)(reg_addr / 2) << RTL8125_OCP_ADDR_SHIFT;
	RTL_W32(MACOCP, data32);

	return (u16)RTL_R32(MACOCP);
}

static void rtl_mac_ocp_modify(u16 reg_addr, u16 mask, u16 set)
{
	u16 data;

	if (mask == 0xffff) {
		rtl_mac_ocp_write(reg_addr, set);
		return;
	}

	data = rtl_mac_ocp_read(reg_addr);
	rtl_mac_ocp_write(reg_addr, (data & ~mask) | set);
}

static void rtl_csi_mod(u16 addr, u32 mask, u32 set)
{
	u32 val;
	int i;

	RTL_W32(0x68, 0x0000f000 | (addr & 0xfff));
	for (i = 0; i < 100; i++) {
		if (RTL_R32(0x68) & BIT(31))
			break;
		udelay(10);
	}
	val = RTL_R32(0x64);
	RTL_W32(0x64, (val & ~mask) | set);
	RTL_W32(0x68, 0x8000f000 | (addr & 0xfff));
	for (i = 0; i < 100; i++) {
		if (!(RTL_R32(0x68) & BIT(31)))
			break;
		udelay(10);
	}
}

static void rtl_ephy_write_raw(int addr, int data)
{
	int i;

	RTL_W32(0x80, 0x80000000U | ((u32)(addr & 0x7f) << 16) | (u32)(data & 0xffff));
	for (i = 0; i < 100; i++) {
		udelay(10);
		if (!(RTL_R32(0x80) & 0x80000000U))
			break;
	}
	udelay(20);
}

static void rtl_ephy_write(u16 addr, u16 data)
{
	rtl_ephy_write_raw(0x0ffe, addr >> 12);
	rtl_ephy_write_raw(addr & 0xfff, data);
}

static u16 rtl_sds_phy_read(u16 index, u16 page, u16 reg)
{
	int i;

	RTL_W16(0x234a, (index << 11) | (page << 5) | reg);
	RTL_W16(0x2348, BIT(0));
	for (i = 0; i < 100; i++) {
		if (!(RTL_R16(0x2348) & BIT(0)))
			return RTL_R16(0x234e);
		udelay(1);
	}
	return 0xffff;
}

static void rtl_sds_phy_write(u16 index, u16 page, u16 reg, u16 val)
{
	int i;

	RTL_W16(0x234c, val);
	RTL_W16(0x234a, (index << 11) | (page << 5) | reg);
	RTL_W16(0x2348, BIT(1) | BIT(0));
	for (i = 0; i < 100; i++) {
		if (!(RTL_R16(0x2348) & BIT(0)))
			break;
		udelay(1);
	}
}

static void rtl_sds_phy_modify(u16 index, u16 page, u16 reg, u16 clearmask, u16 setmask)
{
	u16 val = rtl_sds_phy_read(index, page, reg);

	val = (val & ~clearmask) | setmask;
	rtl_sds_phy_write(index, page, reg, val);
}

static bool rtl8127_phy_configured;

static bool rtl8127_is_fiber_mode(void)
{
	u8 mode;

	if (rtl_chip_info[tpc->chipset].version != 0x6e)
		return false;

	mode = (u8)(rtl_mac_ocp_read(0xd006) & 0xff);
	return mode == 0x07 || mode == 0x08;
}

static void rtl8127_switch_mac_mcu_page(u16 page)
{
	rtl_mac_ocp_modify(0xe446, 0x0003, page & 0x0003);
}

static void rtl8127_load_mac_mcu_fiber(void)
{
	const u16 *code;
	size_t cnt;
	u64 ver = 0;
	u16 reg;
	size_t i;

	if (tpc->pci_rev == 0x09) {
		code = rtl8127afa_1_mac_mcu_code;
		cnt = ARRAY_SIZE(rtl8127afa_1_mac_mcu_code);
	} else {
		code = rtl8127a_1_mac_mcu_code;
		cnt = ARRAY_SIZE(rtl8127a_1_mac_mcu_code);
	}

	/* Disable UPS and GPHY isolation (r8127_n.c:rtl8127_hw_init) */
	RTL_W8(0xf1, RTL_R8(0xf1) & ~BIT(7));
	rtl_mac_ocp_modify(0xd40a, BIT(4), 0x0000);

	/* Disable MAC MCU breakpoints before writing RAM code */
	rtl_mac_ocp_write(0xfc48, 0x0000);
	for (reg = 0xfc28; reg < 0xfc48; reg += 2)
		rtl_mac_ocp_write(reg, 0x0000);
	mdelay(3);
	rtl_mac_ocp_write(0xfc26, 0x0000);

	/* Write MAC MCU patch code in 256-word pages */
	for (i = 0; i < cnt; i++) {
		u16 offset = (u16)(i % 256);

		if (offset == 0)
			rtl8127_switch_mac_mcu_page((u16)(i / 256));
		rtl_mac_ocp_write(0xf800 + offset * 2, code[i]);
	}
	rtl8127_switch_mac_mcu_page(0);

	/* Record 64-bit patch version in page 2 (0xf9f8..0xf9fe) */
	for (i = 0; i < 4; i++)
		ver = (ver << 16) | code[cnt - 4 + i];
	rtl8127_switch_mac_mcu_page(2);
	for (i = 0; i < 8; i += 2) {
		rtl_mac_ocp_write((u16)(0xf9f8 + 6 - i), (u16)ver);
		ver >>= 16;
	}
	rtl8127_switch_mac_mcu_page(0);

	if (tpc->pci_rev == 0x09) {
		rtl_mac_ocp_write(0xfc26, 0x8000);
		rtl_mac_ocp_write(0xfc28, 0x0862);
		rtl_mac_ocp_write(0xfc2a, 0x1a44);
		rtl_mac_ocp_write(0xfc2c, 0x11a0);
		rtl_mac_ocp_write(0xfc2e, 0x0148);
		rtl_mac_ocp_write(0xfc32, 0x0166);
		rtl_mac_ocp_write(0xfc34, 0x1a6a);
		rtl_mac_ocp_write(0xfc36, 0x1a2c);
		rtl_mac_ocp_write(0xfc38, 0x51ca);
		rtl_mac_ocp_write(0xfc3a, 0x41e2);
		rtl_mac_ocp_write(0xfc3e, 0x027c);
		rtl_mac_ocp_write(0xfc40, 0x2610);
		rtl_mac_ocp_write(0xfc48, 0x18ef);
	} else {
		/* Exact RTL8127A rev.a (mcfg == 2, rev 0x08) breakpoints from Bianbu r8127.ko */
		rtl_mac_ocp_write(0xfc26, 0x8000);
		rtl_mac_ocp_write(0xfc28, 0x1520);
		rtl_mac_ocp_write(0xfc2a, 0x41e0);
		rtl_mac_ocp_write(0xfc2c, 0x508c);
		rtl_mac_ocp_write(0xfc2e, 0x50f6);
		rtl_mac_ocp_write(0xfc30, 0x34fa);
		rtl_mac_ocp_write(0xfc32, 0x0166);
		rtl_mac_ocp_write(0xfc34, 0x1a6a);
		rtl_mac_ocp_write(0xfc36, 0x1a2c);
		rtl_mac_ocp_write(0xfc48, 0x00ff);
	}

	rtl_csi_mod(0x108, 0x00000000, 0x00100000);
	rtl_mac_ocp_modify(0xc0b6, 0x0000, BIT(0));
}

static void rtl8127_hw_ephy_config(void)
{
	static const struct {
		u16 addr;
		u16 val;
	} ephy_ops[] = {
		{ 0x8088, 0x0064 }, { 0x8488, 0x0064 }, { 0x8888, 0x0064 }, { 0x8c88, 0x0064 },
		{ 0x8188, 0x0064 }, { 0x8588, 0x0064 }, { 0x8988, 0x0064 }, { 0x8d88, 0x0064 },
		{ 0x808c, 0x09b0 }, { 0x848c, 0x09b0 }, { 0x888c, 0x0f90 }, { 0x8c8c, 0x0f90 },
		{ 0x818c, 0x09b0 }, { 0x858c, 0x09b0 }, { 0x898c, 0x0f90 }, { 0x8d8c, 0x0f90 },
		{ 0x808a, 0x09b8 }, { 0x848a, 0x09b8 }, { 0x888a, 0x0f98 }, { 0x8c8a, 0x0f98 },
		{ 0x818a, 0x09b8 }, { 0x858a, 0x09b8 }, { 0x898a, 0x0f98 }, { 0x8d8a, 0x0f98 },
		{ 0x9020, 0x0080 }, { 0x9420, 0x0080 }, { 0x9820, 0x0080 }, { 0x9c20, 0x0080 },
		{ 0x901e, 0x0190 }, { 0x941e, 0x0190 }, { 0x981e, 0x0140 }, { 0x9c1e, 0x0140 },
		{ 0x901c, 0x0190 }, { 0x941c, 0x0190 }, { 0x981c, 0x0140 }, { 0x9c1c, 0x0140 },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(ephy_ops); i++)
		rtl_ephy_write(ephy_ops[i].addr, ephy_ops[i].val);

	rtl_ephy_write_raw(0x0ffe, 0x0000);
}

static void rtl8127_exit_oob(void)
{
	int i;

	RTL_W32(RxConfig, RTL_R32(RxConfig) &
		~(AcceptErr | AcceptRunt | AcceptBroadcast |
		  AcceptMulticast | AcceptMyPhys | AcceptAllPhys));
	rtl_mac_ocp_write(0xc0bc, 0x00ff);

	RTL_W32(FuncEvent, RTL_R32(FuncEvent) | RxDv_Gated_En);
	RTL_W8(ChipCmd, RTL_R8(ChipCmd) & ~(CmdTxEnb | CmdRxEnb));
	mdelay(1);

	RTL_W8(ChipCmd, CmdReset);
	for (i = 1000; i > 0; i--) {
		if (!(RTL_R8(ChipCmd) & CmdReset))
			break;
		udelay(10);
	}

	RTL_W8(0xd3, RTL_R8(0xd3) & ~0x80);
	rtl_mac_ocp_modify(0xe8de, BIT(14), 0x0000);
	for (i = 0; i < 50; i++) {
		if (RTL_R8(0xd3) & BIT(1))
			break;
		udelay(100);
	}

	/* Exact FIFO thresholds from Realtek r8127 (mtu 1500) */
	rtl_mac_ocp_write(0xc0a6, 0x0493);
	rtl_mac_ocp_write(0xc0aa, 0x2c4a);
	rtl_mac_ocp_write(0xc01e, 0x5555);
	for (i = 0; i < 50; i++) {
		if (RTL_R8(0xd3) & BIT(1))
			break;
		udelay(100);
	}
}

static void rtl8127_sds_phy_reset(void)
{
	/* Exact rtl8127_sds_phy_reset_8127 from Bianbu r8127.ko (0x1a166..0x1a238) */
	RTL_W8(0x2350, RTL_R8(0x2350) & ~BIT(0));
	udelay(1);

	RTL_W16(0x233a, 0x801f);
	RTL_W8(0x2350, RTL_R8(0x2350) | BIT(0));
	udelay(10);
}

static void rtl8127_hw_sds_phy_config(void)
{
	u16 val;

	/* Exact rtl8127_hw_fiber_phy_config from Bianbu r8127.ko (0x19ec0..0x1a070) */
	val = RTL_R16(0x2400);
	val = (val | BIT(13)) & ~BIT(8);
	RTL_W16(0x2400, val);

	/* rtl8127_sds_phy_exit_1g_8127 */
	rtl_sds_phy_modify(0, 1, 31, BIT(3), 0x0000);
	rtl_sds_phy_modify(0, 2, 0, BIT(13) | BIT(12) | BIT(6), BIT(6));
	rtl8127_sds_phy_reset();

	/* rtl8127_set_sds_phy_caps_10g_8127 */
	RTL_W16(0x233a, 0x801a);
	val = RTL_R16(0x233e);
	val = (val & ~(BIT(13) | BIT(12) | BIT(1) | BIT(0))) | BIT(12);
	RTL_W16(0x233e, val);
}

static void rtl8127_disable_eee(void)
{
	/* In Fiber Mode (HwFiberModeVer != 0), r8127.ko only touches MACOCP 0xe040 & 0xe052 */
	rtl_mac_ocp_modify(0xe040, BIT(1) | BIT(0), 0x0000);
	rtl_mac_ocp_modify(0xe052, BIT(0), 0x0000);
}

/*
 * Initialize Realtek RTL8127ATF 10GbE Fiber (SFP+) EPHY & SerDes PHY.
 * Exact sequence from working SpacemiT Bianbu r8127.ko (rtl8127_up -> r8127_fiber.c).
 */
static void rtl8127_hw_fiber_phy_config(void)
{
	/* 1. rtl8127_powerup_pll (skips copper PHYOCP when HwFiberModeVer != 0) */
	RTL_W8(0x6f, RTL_R8(0x6f) | BIT(7) | BIT(6));

	/* 2. rtl8127_hw_ephy_config_8127_1 */
	rtl8127_hw_ephy_config();

	/* 3. rtl8127_hw_fiber_phy_config */
	rtl8127_hw_sds_phy_config();

	/* 4. rtl8127_disable_eee */
	rtl8127_disable_eee();

	rtl8127_phy_configured = true;
}

static int rtl8169_init_board(unsigned long dev_iobase, const char *name)
{
	int i;
	u32 tmp;

#ifdef DEBUG_RTL8169
	printf("%s\n", __func__);
#endif
	ioaddr = dev_iobase;
	rtl8127_phy_configured = false;

	/* Identify chip attached to board before reset (matches Linux rtl_init_one) */
	tmp = RTL_R32(TxConfig);
	tmp = ((tmp & 0x7c000000) + ((tmp & 0x00800000) << 2)) >> 24;

	tpc->chipset = 0;
	for (i = ARRAY_SIZE(rtl_chip_info) - 1; i >= 0; i--) {
		if (tmp == rtl_chip_info[i].version) {
			tpc->chipset = i;
			break;
		}
	}
	if (i < 0) {
		printf("PCI device %s: unknown chip version, assuming RTL-8169\n",
		       name);
		printf("PCI device: TxConfig = 0x%lX\n", (unsigned long)RTL_R32(TxConfig));
	}

	if (rtl_chip_info[tpc->chipset].version == 0x6e)
		rtl8127_exit_oob();

	/* Soft reset the chip. */
	RTL_W8(ChipCmd, CmdReset);

	/* Check that the chip has finished the reset. */
	for (i = 1000; i > 0; i--)
		if ((RTL_R8(ChipCmd) & CmdReset) == 0)
			break;
		else
			udelay(10);

	return 0;
}

/*
 * TX and RX descriptors are 16 bytes. This causes problems with the cache
 * maintenance on CPUs where the cache-line size exceeds the size of these
 * descriptors. What will happen is that when the driver receives a packet
 * it will be immediately requeued for the hardware to reuse. The CPU will
 * therefore need to flush the cache-line containing the descriptor, which
 * will cause all other descriptors in the same cache-line to be flushed
 * along with it. If one of those descriptors had been written to by the
 * device those changes (and the associated packet) will be lost.
 *
 * To work around this, we make use of non-cached memory if available. If
 * descriptors are mapped uncached there's no need to manually flush them
 * or invalidate them.
 *
 * Note that this only applies to descriptors. The packet data buffers do
 * not have the same constraints since they are 1536 bytes large, so they
 * are unlikely to share cache-lines.
 */
static void *rtl_alloc_descs(unsigned int num)
{
	size_t size = num * RTL8169_DESC_SIZE;

#ifdef CONFIG_SYS_NONCACHED_MEMORY
	return (void *)noncached_alloc(size, RTL8169_ALIGN);
#else
	return memalign(RTL8169_ALIGN, size);
#endif
}

/*
 * Cache maintenance functions. These are simple wrappers around the more
 * general purpose flush_cache() and invalidate_dcache_range() functions.
 */

static void rtl_inval_rx_desc(struct RxDesc *desc)
{
#ifndef CONFIG_SYS_NONCACHED_MEMORY
	unsigned long start = (unsigned long)desc & ~(ARCH_DMA_MINALIGN - 1);
	unsigned long end = ALIGN(start + sizeof(*desc), ARCH_DMA_MINALIGN);

	invalidate_dcache_range(start, end);
#endif
}

static void rtl_flush_rx_desc(struct RxDesc *desc)
{
#ifndef CONFIG_SYS_NONCACHED_MEMORY
	flush_cache((unsigned long)desc, sizeof(*desc));
#endif
}

static void rtl_inval_tx_desc(struct TxDesc *desc)
{
#ifndef CONFIG_SYS_NONCACHED_MEMORY
	unsigned long start = (unsigned long)desc & ~(ARCH_DMA_MINALIGN - 1);
	unsigned long end = ALIGN(start + sizeof(*desc), ARCH_DMA_MINALIGN);

	invalidate_dcache_range(start, end);
#endif
}

static void rtl_flush_tx_desc(struct TxDesc *desc)
{
#ifndef CONFIG_SYS_NONCACHED_MEMORY
	flush_cache((unsigned long)desc, sizeof(*desc));
#endif
}

static void rtl_inval_buffer(void *buf, size_t size)
{
	unsigned long start = (unsigned long)buf & ~(ARCH_DMA_MINALIGN - 1);
	unsigned long end = ALIGN(start + size, ARCH_DMA_MINALIGN);

	invalidate_dcache_range(start, end);
}

static void rtl_flush_buffer(void *buf, size_t size)
{
	flush_cache((unsigned long)buf, size);
}

/**************************************************************************
RECV - Receive a frame
***************************************************************************/
static int rtl_recv_common(struct udevice *dev, unsigned long dev_iobase,
			   uchar **packetp)
{
	/* return true if there's an ethernet packet ready to read */
	/* nic->packet should contain data on return */
	/* nic->packetlen should contain length of data */
	struct pci_child_plat *pplat = dev_get_parent_plat(dev);
	int cur_rx;
	int length = 0;

#ifdef DEBUG_RTL8169_RX
	printf("%s\n", __func__);
#endif
	ioaddr = dev_iobase;

	cur_rx = tpc->cur_rx;

	rtl_inval_rx_desc(&tpc->RxDescArray[cur_rx]);

	if ((le32_to_cpu(tpc->RxDescArray[cur_rx].status) & OWNbit) == 0) {
		if (!(le32_to_cpu(tpc->RxDescArray[cur_rx].status) & RxRES)) {
			length = (int) (le32_to_cpu(tpc->RxDescArray[cur_rx].
						status) & 0x00001FFF) - 4;

			rtl_inval_buffer(tpc->RxBufferRing[cur_rx], length);
			memcpy(rxdata, tpc->RxBufferRing[cur_rx], length);
			*packetp = rxdata;
		} else {
			puts("Error Rx");
			length = -EIO;
		}

		/*
		 * Re-arm RX descriptors in groups of 4 (one 64-byte cache line)
		 * once all 4 descriptors in the cache line have been consumed,
		 * avoiding CPU cache writeback races against concurrent NIC DMA
		 * writes to adjacent 16-byte descriptors in the same cache line.
		 */
		if ((cur_rx & 3) == 3) {
			int base = cur_rx & ~3;
			int j;

			for (j = 0; j < 4; j++) {
				int idx = base + j;
				phys_addr_t rx_phys = (phys_addr_t)(uintptr_t)
					tpc->RxBufferRing[idx];

				tpc->RxDescArray[idx].buf_addr =
					cpu_to_le32((u32)rx_phys);
				tpc->RxDescArray[idx].buf_Haddr =
					cpu_to_le32((u32)((u64)rx_phys >> 32));
				if (idx == NUM_RX_DESC - 1)
					tpc->RxDescArray[idx].status =
						cpu_to_le32((OWNbit | EORbit) + RX_BUF_SIZE);
				else
					tpc->RxDescArray[idx].status =
						cpu_to_le32(OWNbit + RX_BUF_SIZE);
			}
			rtl_flush_rx_desc(&tpc->RxDescArray[base]);
		}

		cur_rx = (cur_rx + 1) % NUM_RX_DESC;
		tpc->cur_rx = cur_rx;
		return length;

	} else {
		u32 sts;

		if (rtl_is_8125(pplat)) {
			sts = RTL_R32(IntrStatus_8125);
			RTL_W32(IntrStatus_8125, sts & ~(TxErr | RxErr | SYSErr));
		} else {
			sts = RTL_R16(IntrStatus_8169);
			RTL_W16(IntrStatus_8169, sts & ~(TxErr | RxErr | SYSErr));
		}
	}
	tpc->cur_rx = cur_rx;
	return (0);		/* initially as this is called to flush the input */
}

int rtl8169_eth_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct rtl8169_private *priv = dev_get_priv(dev);

	return rtl_recv_common(dev, priv->iobase, packetp);
}

#define HZ 1000
/**************************************************************************
SEND - Transmit a frame
***************************************************************************/
static int rtl_send_common(struct udevice *dev, unsigned long dev_iobase,
			   void *packet, int length)
{
	/* send the packet to destination */

	struct pci_child_plat *pplat = dev_get_parent_plat(dev);
	phys_addr_t tx_phys;
	u32 to;
	u8 *ptxb;
	bool is_8127 = rtl_chip_info[tpc->chipset].version == 0x6e;
	int entry = tpc->cur_tx % NUM_TX_DESC;
	int desc_idx = is_8127 ? (entry * 2) : entry;
	u32 len = length;
	int ret;

#ifdef DEBUG_RTL8169_TX
	int stime = currticks();
	printf("%s\n", __func__);
	printf("sending %d bytes\n", len);
#endif

	ioaddr = dev_iobase;

	/* point to the current txb incase multiple tx_rings are used */
	ptxb = tpc->Tx_skbuff[entry];
	memcpy(ptxb, (char *)packet, (int)length);

	while (len < ETH_ZLEN)
		ptxb[len++] = '\0';

	rtl_flush_buffer(ptxb, ALIGN(len, RTL8169_ALIGN));

	tx_phys = (phys_addr_t)(uintptr_t)ptxb;
	if (is_8127)
		memset(&tpc->TxDescArray[desc_idx + 1], 0, sizeof(struct TxDesc));
	tpc->TxDescArray[desc_idx].vlan_tag = 0;
	tpc->TxDescArray[desc_idx].buf_Haddr = cpu_to_le32((u32)((u64)tx_phys >> 32));
	tpc->TxDescArray[desc_idx].buf_addr = cpu_to_le32((u32)tx_phys);
	if (entry != (NUM_TX_DESC - 1)) {
		tpc->TxDescArray[desc_idx].status =
			cpu_to_le32((OWNbit | FSbit | LSbit) |
				    ((len > ETH_ZLEN) ? len : ETH_ZLEN));
	} else {
		tpc->TxDescArray[desc_idx].status =
			cpu_to_le32((OWNbit | EORbit | FSbit | LSbit) |
				    ((len > ETH_ZLEN) ? len : ETH_ZLEN));
	}
	rtl_flush_tx_desc(&tpc->TxDescArray[desc_idx]);
	if (is_8127)
		RTL_W32(TxPoll_8125, BIT(0));
	else if (rtl_is_8125(pplat))
		RTL_W16(TxPoll_8125, 0x0001);	/* set queue 0 polling bit */
	else
		RTL_W8(TxPoll_8169, 0x40);	/* set polling bit */

	tpc->cur_tx++;
	to = currticks() + TX_TIMEOUT;
	do {
		rtl_inval_tx_desc(&tpc->TxDescArray[desc_idx]);
	} while ((le32_to_cpu(tpc->TxDescArray[desc_idx].status) & OWNbit)
				&& (currticks() < to));	/* wait */

	if (currticks() >= to) {
		ret = -ETIMEDOUT;
	} else {
		ret = 0;
	}

	return ret;
}

int rtl8169_eth_send(struct udevice *dev, void *packet, int length)
{
	struct rtl8169_private *priv = dev_get_priv(dev);

	return rtl_send_common(dev, priv->iobase, packet, length);
}

static void rtl8169_set_rx_mode(void)
{
	u32 mc_filter[2];	/* Multicast hash filter */
	int rx_mode;
	u32 tmp = 0;

#ifdef DEBUG_RTL8169
	printf("%s\n", __func__);
#endif

	/* IFF_ALLMULTI */
	/* Too many to filter perfectly -- accept all multicasts. */
	rx_mode = AcceptBroadcast | AcceptMulticast | AcceptMyPhys | AcceptAllPhys;
	mc_filter[1] = mc_filter[0] = 0xffffffff;

	if (rtl_chip_info[tpc->chipset].version == 0x6e) {
		/* Exact RCR_Cfg from Realtek r8127 (Fetch_8 | Close_Multiple | Pause | DMA_512) */
		tmp = BIT(30) | BIT(21) | RX_PAUSE_SLOT_ON |
		      (5U << RxCfgDMAShift) | rx_mode |
		      (RTL_R32(RxConfig) & rtl_chip_info[tpc->chipset].RxConfigMask);
	} else if (rtl_chip_info[tpc->chipset].version >= 0x64) {
		tmp = (8U << 27) | RX_PAUSE_SLOT_ON |
		      (7U << RxCfgDMAShift) | rx_mode;
	} else {
		tmp = rtl8169_rx_config | rx_mode | (RTL_R32(RxConfig) &
					   rtl_chip_info[tpc->chipset].RxConfigMask);
	}

	RTL_W32(RxConfig, tmp);
	RTL_W32(MAR0 + 0, mc_filter[0]);
	RTL_W32(MAR0 + 4, mc_filter[1]);
}

static void rtl8169_init_ring(struct udevice *dev);

static void rtl8127_hw_config_mac(struct udevice *dev)
{
	struct eth_pdata *plat = dev_get_plat(dev);
	phys_addr_t tx_desc_phys = (phys_addr_t)(uintptr_t)tpc->TxDescArray;
	phys_addr_t rx_desc_phys = (phys_addr_t)(uintptr_t)tpc->RxDescArray;
	phys_addr_t tally_phys = (phys_addr_t)(uintptr_t)rtl_tally_buf;
	u32 reg, i;

	/* Disable RX filter and soft-reset MAC (r8127_n.c:rtl8127_hw_config) */
	RTL_W32(RxConfig, RTL_R32(RxConfig) &
		~(AcceptErr | AcceptRunt | AcceptBroadcast |
		  AcceptMulticast | AcceptMyPhys | AcceptAllPhys));

	RTL_W32(FuncEvent, RTL_R32(FuncEvent) | RxDv_Gated_En);
	RTL_W8(ChipCmd, RTL_R8(ChipCmd) & ~(CmdTxEnb | CmdRxEnb));
	mdelay(1);
	RTL_W8(ChipCmd, CmdReset);
	for (i = 1000; i > 0; i--) {
		if (!(RTL_R8(ChipCmd) & CmdReset))
			break;
		udelay(10);
	}

	RTL_W8(Cfg9346, Cfg9346_Unlock);

	/* Re-program hardware MAC after CmdReset */
	RTL_W32(MAC0, (u32)plat->enetaddr[0] | ((u32)plat->enetaddr[1] << 8) |
		      ((u32)plat->enetaddr[2] << 16) | ((u32)plat->enetaddr[3] << 24));
	RTL_W32(MAC0 + 4, (u32)plat->enetaddr[4] | ((u32)plat->enetaddr[5] << 8));

	RTL_W8(0xf1, RTL_R8(0xf1) & ~BIT(7));
	RTL_W8(0x34, 0x00);
	RTL_W8(Config5, RTL_R8(Config5) & ~BIT(0));

	rtl_mac_ocp_modify(0xc0b6, 0xfffe, 0x0000);
	for (reg = 0xa00; reg < 0xb00; reg += 4)
		RTL_W32(reg, 0x00000000);
	RTL_W16(0x7a, 0x0000);

	RTL_W32(0x14, (u32)((u64)tally_phys >> 32));
	RTL_W32(0x10, (u32)tally_phys);
	rtl_mac_ocp_modify(0xea84, 0x0000, 0x0003);

	RTL_W32(TxDescStartAddrLow, (u32)tx_desc_phys);
	RTL_W32(TxDescStartAddrHigh, (u32)((u64)tx_desc_phys >> 32));
	RTL_W32(RxDescStartAddrLow, (u32)rx_desc_phys);
	RTL_W32(RxDescStartAddrHigh, (u32)((u64)rx_desc_phys >> 32));

	RTL_W32(TxConfig, (7U << TxDMAShift) | (InterFrameGap << TxInterFrameGapShift));
	RTL_W16(0x1000, 0x0000);
	rtl_csi_mod(0x70c, 0xff000000, 0x27000000);

	RTL_W8(Config3, RTL_R8(Config3) & ~BIT(1));
	RTL_W16(0x0382, 0x221b);
	RTL_W32(0x4500, 0x00000000);
	RTL_W16(0x4800, 0x0000);
	RTL_W32(0x0818, 0x00000000);
	rtl_mac_ocp_modify(0xd40a, 0x0010, 0x0000);
	RTL_W8(Config1, RTL_R8(Config1) & ~0x10);
	rtl_mac_ocp_write(0xc140, 0xffff);
	rtl_mac_ocp_write(0xc142, 0xffff);
	rtl_mac_ocp_modify(0xd3e2, 0x0fff, 0x03a9);
	rtl_mac_ocp_modify(0xd3e4, 0x00ff, 0x0000);
	rtl_mac_ocp_modify(0xe860, 0x0000, 0x0080);

	/* Use 32-byte TxDesc format (TX_DESC_FMT_STD = BIT(0)) with TxPoll_8125 (0x90) */
	rtl_mac_ocp_modify(TX_DESC_MODE, TX_DESC_FMT_MASK, TX_DESC_FMT_STD);
	RTL_W8(0x20e4, RTL_R8(0x20e4) & ~BIT(2));
	RTL_W8(RX_DESC_MODE,
	       RTL_R8(RX_DESC_MODE) & ~(RX_DESC_CRC_DROP | RX_DESC_TYPE));

	rtl_mac_ocp_modify(0xe00c, BIT(12), 0x0000);
	rtl_mac_ocp_modify(0xc0c2, BIT(6), 0x0000);
	rtl_mac_ocp_modify(0xe614, 0x0f00, 0x0f00);
	rtl_mac_ocp_modify(0xe63e, 0x0c30, 0x0020);

	/* Enable MAC MCU (0 -> 1) and set BIT(3)|BIT(2) */
	rtl_mac_ocp_modify(0xc0b4, BIT(0), 0x0000);
	rtl_mac_ocp_modify(0xc0b4, 0x0000, BIT(0) | BIT(2) | BIT(3));

	rtl_mac_ocp_modify(0xeb6a, 0x00ff, 0x0033);
	rtl_mac_ocp_modify(0xeb50, 0x03e0, 0x0040);
	rtl_mac_ocp_modify(0xe056, 0x00f0, 0x0000);
	RTL_W8(0x57, 0x10);
	rtl_mac_ocp_modify(0xe040, 0x1003, 0x0000);
	rtl_mac_ocp_write(0xe0c0, 0x4400);
	rtl_mac_ocp_modify(0xe052, BIT(3) | BIT(7), BIT(6) | BIT(5));
	rtl_mac_ocp_modify(0xd430, 0x0fff, 0x045f);
	RTL_W8(0xd0, RTL_R8(0xd0) | BIT(6) | BIT(7));
	RTL_W8(0xf2, RTL_R8(0xf2) & ~BIT(3));
	rtl_mac_ocp_modify(0xe080, BIT(1), 0x0000);
	rtl_mac_ocp_modify(0xea1c, 0x0307, 0x0001);
	rtl_mac_ocp_modify(0xeb54, 0x0000, 0x0001);
	udelay(2);
	rtl_mac_ocp_modify(0xeb54, 0x0001, 0x0000);
	RTL_W16(0x1880, RTL_R16(0x1880) & ~0x0030);
	rtl_mac_ocp_modify(0xd40c, 0xe038, 0x8020);
	rtl_mac_ocp_write(0xe098, 0xc302);
	rtl_mac_ocp_modify(0xe032, 0x0003, 0x0000);
	rtl_mac_ocp_modify(0xe092, 0x00ff, 0x0000);
	for (i = 0; i < 10; i++) {
		if (!(rtl_mac_ocp_read(0xe00e) & BIT(13)))
			break;
		udelay(1000);
	}
	rtl_mac_ocp_modify(0xc0ac, 0x0000, 0x1f80);
	RTL_W16(0xe2, 0x0000);

	if (rtl8127_is_fiber_mode()) {
		rtl_mac_ocp_modify(0xd3f0, BIT(0), 0x0000);
		rtl_mac_ocp_modify(0xd3f2, BIT(0), 0x0000);
		rtl_mac_ocp_modify(0xe85a, BIT(6), 0x0000);
		rtl_mac_ocp_modify(0xdc42, BIT(3), 0x0000);
	}

	RTL_W16(CPlusCmd, 0x0000);
	RTL_W16(RxMaxSize, RX_BUF_SIZE | 0x8000);
	RTL_W32(FuncEvent, RTL_R32(FuncEvent) & ~RxDv_Gated_En);
	rtl8169_set_rx_mode();
	RTL_W16(RADMFIFO_PROTECT, 0x2001);
	RTL_W8(Cfg9346, Cfg9346_Lock);
	udelay(10);
}

static void rtl8169_hw_start(struct udevice *dev)
{
	u8 version = rtl_chip_info[tpc->chipset].version;
	u16 tx_desc_mode;
	u32 i;

#ifdef DEBUG_RTL8169
	int stime = currticks();
	printf("%s\n", __func__);
#endif

	if (version == 0x6e && rtl8127_is_fiber_mode()) {
		u32 phystat;

		if (!rtl8127_phy_configured) {
			/* 1. Exact rtl8127_open -> rtl8127_up sequence (r8127 11.015.00) */
			rtl8127_exit_oob();
			RTL_W8(0x6f, RTL_R8(0x6f) | BIT(7) | BIT(6));
			rtl8127_load_mac_mcu_fiber();
			RTL_W8(ChipCmd, CmdReset);
			for (i = 1000; i > 0; i--) {
				if (!(RTL_R8(ChipCmd) & CmdReset))
					break;
				udelay(10);
			}
			rtl8127_hw_fiber_phy_config();
			rtl8127_hw_config_mac(dev);
		}

		/* 2. Wait for 10G SerDes link up */
		for (i = 0; i < 2000; i++) {
			if (RTL_R32(PHYstatus) & LinkStatus)
				break;
			udelay(1000);
		}
		mdelay(50);

		/* 3. Exact rtl8127_link_on_patch in live 10G clock domain */
		rtl8169_init_ring(dev);
		rtl8127_hw_config_mac(dev);
		{
			phys_addr_t tpaddr = (phys_addr_t)(uintptr_t)rtl_tally_buf;

			RTL_W32(0x14, (u32)((u64)tpaddr >> 32));
			RTL_W32(0x10, (u32)tpaddr | BIT(0));
			for (i = 0; i < 100; i++) {
				if (!(RTL_R32(0x10) & BIT(0)))
					break;
				udelay(10);
			}
		}
		phystat = RTL_R32(PHYstatus);
		if (phystat & (BIT(2) | BIT(3) | BIT(4) | BIT(9) | BIT(19)))
			RTL_W16(RADMFIFO_PROTECT, 0x2001);
		else
			RTL_W16(RADMFIFO_PROTECT, 0x0000);

		RTL_W8(ChipCmd, CmdTxEnb | CmdRxEnb);
		RTL_W32(TxConfig, (7U << TxDMAShift) | (InterFrameGap << TxInterFrameGapShift));
		rtl8169_set_rx_mode();
		RTL_W32(IntrMask_8125, 0x00000000);
		RTL_W32(0x0d0c, 0x00000000);

		printf("rtl8169: 10G SFP+ rev=0x%02x PHYstatus=0x%08x sfp2400=0x%04x sds=0x%04x/0x%04x tgr5=%04x/%04x tgr6_2=0x%04x fib2_1=%04x/%04x sds0_0=%04x mac=%08x:%04x\n",
		       tpc->pci_rev, phystat, RTL_R16(0x2400),
		       RTL_R16(0x233a), RTL_R16(0x233e),
		       rtl_sds_phy_read(0, 5, 0), rtl_sds_phy_read(0, 5, 1),
		       rtl_sds_phy_read(0, 6, 2),
		       rtl_sds_phy_read(0, 2, 1), rtl_sds_phy_read(0, 2, 1),
		       rtl_sds_phy_read(0, 0, 0),
		       RTL_R32(MAC0), RTL_R16(MAC0 + 4));
		return;
	}

	RTL_W8(Cfg9346, Cfg9346_Unlock);

	if (version >= 0x64)
		RTL_W32(FuncEvent, RTL_R32(FuncEvent) | RxDv_Gated_En);

	RTL_W16(CPlusCmd, 0x0000);

	/*
	 * RTL8125D/8126A/8127A require explicit descriptor-type and CRC-drop setup.
	 * RTL8125B (0x64) is intentionally excluded to avoid regressing working hardware.
	 */
	switch (version) {
	case 0x6a:
	case 0x66:
	case 0x6e:
		if (version == 0x6e) {
			rtl8127_hw_config_mac(dev);
		} else {
			/* Reg 0xD8: Disable CRC drop and force legacy 16-byte RX desc */
			RTL_W8(RX_DESC_MODE,
			       RTL_R8(RX_DESC_MODE) & ~(RX_DESC_CRC_DROP | RX_DESC_TYPE));

			/* Single queue / RSS off (rtl_hw_start_8125_common) */
			RTL_W32(0x0818, 0x00000000);
			RTL_W16(0x4800, 0x0000);
			rtl_mac_ocp_write(0xc140, 0xffff);
			rtl_mac_ocp_write(0xc142, 0xffff);
			rtl_mac_ocp_write(0xe860, rtl_mac_ocp_read(0xe860) | 0x0080);

			/* RxConfig 0x44: enable RX pause slot (RTL8125B and later) */
			RTL_W32(RxConfig, RTL_R32(RxConfig) | RX_PAUSE_SLOT_ON);

			/* MAC OCP 0xEB58: clear bits [1:0] to use legacy 16-byte struct TxDesc */
			tx_desc_mode = rtl_mac_ocp_read(TX_DESC_MODE) & ~TX_DESC_FMT_MASK;
			rtl_mac_ocp_write(TX_DESC_MODE, tx_desc_mode);
		}
		break;
	default:
		break;
	}

	tpc->cur_rx = 0;

	{
		phys_addr_t tx_desc_phys = (phys_addr_t)(uintptr_t)tpc->TxDescArray;
		phys_addr_t rx_desc_phys = (phys_addr_t)(uintptr_t)tpc->RxDescArray;

		RTL_W32(TxDescStartAddrLow, (u32)tx_desc_phys);
		RTL_W32(TxDescStartAddrHigh, (u32)((u64)tx_desc_phys >> 32));
		RTL_W32(RxDescStartAddrLow, (u32)rx_desc_phys);
		RTL_W32(RxDescStartAddrHigh, (u32)((u64)rx_desc_phys >> 32));
	}

	/*
	 * Enable TX and RX in ChipCmd BEFORE writing TxConfig/RxConfig:
	 * on RTL8168/8125/8126/8127, writes to TxConfig are ignored while CmdTxEnb == 0.
	 */
	RTL_W8(ChipCmd, CmdTxEnb | CmdRxEnb);

	if (version < 0x64)
		RTL_W8(EarlyTxThres, EarlyTxThld);

	RTL_W16(RxMaxSize, RxPacketMaxSize);

	/* Set DMA burst size, Interframe Gap Time, and TXCFG_AUTO_FIFO (BIT(7)) on 8125+ */
	RTL_W32(TxConfig, (TX_DMA_BURST << TxDMAShift) |
			  (InterFrameGap << TxInterFrameGapShift) |
			  (version >= 0x64 ? BIT(7) : 0));

	RTL_W32(RxMissed, 0);

	rtl8169_set_rx_mode();

	/* no early-rx interrupts */
	RTL_W16(MultiIntr, RTL_R16(MultiIntr) & 0xF000);

	RTL_W8(Cfg9346, Cfg9346_Lock);

	/* Un-gate RX/TX packet FIFO after rings and TxConfig/RxConfig are armed */
	if (version >= 0x64) {
		RTL_W32(FuncEvent, RTL_R32(FuncEvent) & ~RxDv_Gated_En);
		udelay(100);
	} else {
		udelay(10);
	}

#ifdef DEBUG_RTL8169
	printf("%s elapsed time : %lu\n", __func__, currticks()-stime);
#endif
}

static void rtl8169_init_ring(struct udevice *dev)
{
	int i;

#ifdef DEBUG_RTL8169
	int stime = currticks();
	printf("%s\n", __func__);
#endif

	tpc->cur_rx = 0;
	tpc->cur_tx = 0;
	tpc->dirty_tx = 0;
	memset(tpc->TxDescArray, 0x0, (NUM_TX_DESC * 2) * sizeof(struct TxDesc));
	memset(tpc->RxDescArray, 0x0, NUM_RX_DESC * sizeof(struct RxDesc));

	for (i = 0; i < NUM_TX_DESC; i++) {
		tpc->Tx_skbuff[i] = &txb[i * RX_BUF_SIZE];
	}

	for (i = 0; i < NUM_RX_DESC; i++) {
		phys_addr_t rx_buf_phys;

		if (i == (NUM_RX_DESC - 1))
			tpc->RxDescArray[i].status =
				cpu_to_le32((OWNbit | EORbit) + RX_BUF_SIZE);
		else
			tpc->RxDescArray[i].status =
				cpu_to_le32(OWNbit + RX_BUF_SIZE);

		tpc->RxBufferRing[i] = &rxb[i * RX_BUF_SIZE];
		rx_buf_phys = (phys_addr_t)(uintptr_t)tpc->RxBufferRing[i];
		tpc->RxDescArray[i].buf_addr = cpu_to_le32((u32)rx_buf_phys);
		tpc->RxDescArray[i].buf_Haddr = cpu_to_le32((u32)((u64)rx_buf_phys >> 32));
		rtl_flush_rx_desc(&tpc->RxDescArray[i]);
	}

#ifdef DEBUG_RTL8169
	printf("%s elapsed time : %lu\n", __func__, currticks()-stime);
#endif
}

static void rtl8169_common_start(struct udevice *dev, unsigned char *enetaddr,
				 unsigned long dev_iobase)
{
	int i;

#ifdef DEBUG_RTL8169
	int stime = currticks();
	printf("%s\n", __func__);
#endif

	ioaddr = dev_iobase;

	/* Program 6-byte hardware MAC using 32-bit RAR writes required by RTL8125/8126/8127 */
	RTL_W8(Cfg9346, Cfg9346_Unlock);
	RTL_W32(MAC0, (u32)enetaddr[0] | ((u32)enetaddr[1] << 8) |
		      ((u32)enetaddr[2] << 16) | ((u32)enetaddr[3] << 24));
	RTL_W32(MAC0 + 4, (u32)enetaddr[4] | ((u32)enetaddr[5] << 8));
	RTL_W8(Cfg9346, Cfg9346_Lock);

	rtl8169_init_ring(dev);
	rtl8169_hw_start(dev);
	/* Construct a perfect filter frame with the mac address as first match
	 * and broadcast for all others */
	for (i = 0; i < 192; i++)
		txb[i] = 0xFF;

	txb[0] = enetaddr[0];
	txb[1] = enetaddr[1];
	txb[2] = enetaddr[2];
	txb[3] = enetaddr[3];
	txb[4] = enetaddr[4];
	txb[5] = enetaddr[5];

#ifdef DEBUG_RTL8169
	printf("%s elapsed time : %lu\n", __func__, currticks()-stime);
#endif
}

static int rtl8169_eth_start(struct udevice *dev)
{
	struct eth_pdata *plat = dev_get_plat(dev);
	struct rtl8169_private *priv = dev_get_priv(dev);

	rtl8169_common_start(dev, plat->enetaddr, priv->iobase);

	return 0;
}

static void rtl_halt_common(struct udevice *dev)
{
	struct rtl8169_private *priv = dev_get_priv(dev);
	struct pci_child_plat *pplat = dev_get_parent_plat(dev);
	int i;

#ifdef DEBUG_RTL8169
	printf("%s\n", __func__);
#endif

	ioaddr = priv->iobase;

	/* Stop the chip's Tx and Rx DMA processes. */
	RTL_W8(ChipCmd, 0x00);

	/* Disable interrupts by clearing the interrupt mask. */
	if (rtl_is_8125(pplat))
		RTL_W32(IntrMask_8125, 0x00000000);
	else
		RTL_W16(IntrMask_8169, 0x0000);

	RTL_W32(RxMissed, 0);

	for (i = 0; i < NUM_RX_DESC; i++) {
		tpc->RxBufferRing[i] = NULL;
	}
}

void rtl8169_eth_stop(struct udevice *dev)
{
	rtl_halt_common(dev);
}

static int rtl8169_write_hwaddr(struct udevice *dev)
{
	struct rtl8169_private *priv = dev_get_priv(dev);
	struct eth_pdata *plat = dev_get_plat(dev);

	ioaddr = priv->iobase;
	RTL_W8(Cfg9346, Cfg9346_Unlock);
	RTL_W32(MAC0, (u32)plat->enetaddr[0] | ((u32)plat->enetaddr[1] << 8) |
		      ((u32)plat->enetaddr[2] << 16) | ((u32)plat->enetaddr[3] << 24));
	RTL_W32(MAC0 + 4, (u32)plat->enetaddr[4] | ((u32)plat->enetaddr[5] << 8));
	RTL_W8(Cfg9346, Cfg9346_Lock);

	return 0;
}

/**************************************************************************
INIT - Look for an adapter, this routine's visible to the outside
***************************************************************************/

#define board_found 1
#define valid_link 0
static int rtl_init(unsigned long dev_ioaddr, const char *name,
		    unsigned char *enetaddr)
{
	static int board_idx = -1;
	int i, rc;
	int option = -1, Cap10_100 = 0, Cap1000 = 0;

#ifdef DEBUG_RTL8169
	printf("%s\n", __func__);
#endif
	ioaddr = dev_ioaddr;

	board_idx++;

	/* point to private storage */
	tpc = &tpx;

	rc = rtl8169_init_board(ioaddr, name);
	if (rc)
		return rc;

	/* Get MAC address.  FIXME: read EEPROM */
	for (i = 0; i < MAC_ADDR_LEN; i++)
		enetaddr[i] = RTL_R8(MAC0 + i);

#ifdef DEBUG_RTL8169
	printf("chipset = %d\n", tpc->chipset);
	printf("MAC Address");
	for (i = 0; i < MAC_ADDR_LEN; i++)
		printf(":%02x", enetaddr[i]);
	putc('\n');
#endif

#ifdef DEBUG_RTL8169
	/* Print out some hardware info */
	printf("%s: at ioaddr 0x%lx\n", name, ioaddr);
#endif

	/* RTL8127ATF 10GbE Fiber (SFP+) configures SerDes in rtl8169_hw_start() */
	if (rtl8127_is_fiber_mode()) {
		udelay(10);
	} else if (!(RTL_R8(PHYstatus) & TBI_Enable)) {
		int val = mdio_read(PHY_AUTO_NEGO_REG);

		option = (board_idx >= MAX_UNITS) ? 0 : media[board_idx];
		/* Force RTL8169 in 10/100/1000 Full/Half mode. */
		if (option > 0) {
#ifdef DEBUG_RTL8169
			printf("%s: Force-mode Enabled.\n", name);
#endif
			Cap10_100 = 0, Cap1000 = 0;
			switch (option) {
			case _10_Half:
				Cap10_100 = PHY_Cap_10_Half;
				Cap1000 = PHY_Cap_Null;
				break;
			case _10_Full:
				Cap10_100 = PHY_Cap_10_Full;
				Cap1000 = PHY_Cap_Null;
				break;
			case _100_Half:
				Cap10_100 = PHY_Cap_100_Half;
				Cap1000 = PHY_Cap_Null;
				break;
			case _100_Full:
				Cap10_100 = PHY_Cap_100_Full;
				Cap1000 = PHY_Cap_Null;
				break;
			case _1000_Full:
				Cap10_100 = PHY_Cap_Null;
				Cap1000 = PHY_Cap_1000_Full;
				break;
			default:
				break;
			}
			mdio_write(PHY_AUTO_NEGO_REG, Cap10_100 | (val & 0x1F));	/* leave PHY_AUTO_NEGO_REG bit4:0 unchanged */
			mdio_write(PHY_1000_CTRL_REG, Cap1000);
		} else {
#ifdef DEBUG_RTL8169
			printf("%s: Auto-negotiation Enabled.\n",
			       name);
#endif
			/* enable 10/100 Full/Half Mode, leave PHY_AUTO_NEGO_REG bit4:0 unchanged */
			mdio_write(PHY_AUTO_NEGO_REG,
				   PHY_Cap_10_Half | PHY_Cap_10_Full |
				   PHY_Cap_100_Half | PHY_Cap_100_Full |
				   (val & 0x1F));

			/* enable 1000 Full Mode */
			mdio_write(PHY_1000_CTRL_REG, PHY_Cap_1000_Full);

		}

		/* Enable auto-negotiation and restart auto-nigotiation */
		mdio_write(PHY_CTRL_REG,
			   PHY_Enable_Auto_Nego | PHY_Restart_Auto_Nego);
		udelay(100);

		/* wait for auto-negotiation process */
		for (i = 10000; i > 0; i--) {
			/* check if auto-negotiation complete */
			if (mdio_read(PHY_STAT_REG) & PHY_Auto_Nego_Comp) {
				udelay(100);
				option = RTL_R8(PHYstatus);
				if (option & _1000bpsF) {
#ifdef DEBUG_RTL8169
					printf("%s: 1000Mbps Full-duplex operation.\n",
					       name);
#endif
				} else {
#ifdef DEBUG_RTL8169
					printf("%s: %sMbps %s-duplex operation.\n",
					       name,
					       (option & _100bps) ? "100" :
					       "10",
					       (option & FullDup) ? "Full" :
					       "Half");
#endif
				}
				break;
			} else {
				udelay(100);
			}
		}		/* end for-loop to wait for auto-negotiation process */

	} else {
		udelay(100);
#ifdef DEBUG_RTL8169
		printf
		    ("%s: 1000Mbps Full-duplex operation, TBI Link %s!\n",
		     name,
		     (RTL_R32(TBICSR) & TBILinkOK) ? "OK" : "Failed");
#endif
	}

	tpc->RxDescArray = rtl_alloc_descs(NUM_RX_DESC);
	if (!tpc->RxDescArray)
		return -ENOMEM;

	/* Allocate 2x 16-byte slots for RTL8127 32-byte TX descriptors (TX_DESC_FMT_STD) */
	tpc->TxDescArray = rtl_alloc_descs(NUM_TX_DESC * 2);
	if (!tpc->TxDescArray)
		return -ENOMEM;

	return 0;
}

static int rtl8169_eth_probe(struct udevice *dev)
{
	struct pci_child_plat *pplat = dev_get_parent_plat(dev);
	struct rtl8169_private *priv = dev_get_priv(dev);
	struct eth_pdata *plat = dev_get_plat(dev);
	int region;
	int ret;

	switch (pplat->device) {
	case 0x8125:
	case 0x8126:
	case 0x8127:
	case 0x8161:
	case 0x8168:
		region = 2;
		break;
	default:
		region = 1;
		break;
	}

	dm_pci_clrset_config16(dev, PCI_COMMAND, 0,
			       PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	dm_pci_read_config8(dev, PCI_REVISION_ID, &tpx.pci_rev);

	priv->iobase = (ulong)dm_pci_map_bar(dev,
					     PCI_BASE_ADDRESS_0 + region * 4,
					     0, 0,
					     PCI_REGION_TYPE, PCI_REGION_MEM);

	debug("rtl8169: REALTEK RTL8169 @0x%lx\n", priv->iobase);
	ret = rtl_init(priv->iobase, dev->name, plat->enetaddr);
	if (ret < 0) {
		printf(pr_fmt("failed to initialize card: %d\n"), ret);
		return ret;
	}

	debug("rtl8169: %s (%s, ver=0x%02x, rev=0x%02x, fiber=%d, PHYstatus=0x%08x) @ 0x%lx\n",
	      dev->name, rtl_chip_info[tpc->chipset].name,
	      rtl_chip_info[tpc->chipset].version, tpc->pci_rev,
	      rtl8127_is_fiber_mode() ? 1 : 0,
	      RTL_R32(PHYstatus), priv->iobase);

	/*
	 * WAR for DHCP failure after rebooting from kernel.
	 * Clear RxDv_Gated_En bit which was set by kernel driver.
	 * Without this, U-Boot can't get an IP via DHCP.
	 * Register (FuncEvent, aka MISC) and RXDV_GATED_EN bit are from
	 * the r8169.c kernel driver.
	 */

	u32 val = RTL_R32(FuncEvent);
	debug("%s: FuncEvent/Misc (0xF0) = 0x%08X\n", __func__, val);
	val &= ~RxDv_Gated_En;
	RTL_W32(FuncEvent, val);

	return 0;
}

static const struct eth_ops rtl8169_eth_ops = {
	.start	= rtl8169_eth_start,
	.send	= rtl8169_eth_send,
	.recv	= rtl8169_eth_recv,
	.stop	= rtl8169_eth_stop,
	.write_hwaddr = rtl8169_write_hwaddr,
};

static const struct udevice_id rtl8169_eth_ids[] = {
	{ .compatible = "realtek,rtl8169" },
	{ }
};

U_BOOT_DRIVER(eth_rtl8169) = {
	.name	= "eth_rtl8169",
	.id	= UCLASS_ETH,
	.of_match = rtl8169_eth_ids,
	.probe	= rtl8169_eth_probe,
	.ops	= &rtl8169_eth_ops,
	.priv_auto	= sizeof(struct rtl8169_private),
	.plat_auto	= sizeof(struct eth_pdata),
};

U_BOOT_PCI_DEVICE(eth_rtl8169, supported);
