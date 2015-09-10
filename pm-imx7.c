/*
 * Copyright (C) 2015 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/suspend.h>
#include <linux/slab.h>
#include <asm/suspend.h>
#include <asm/fncpy.h>

#include "common.h"

#define GPC_LPCR_A7_BSC		0x0
#define GPC_LPCR_A7_AD		0x4
#define GPC_LPCR_M4		0x8

#define GPC_SLPCR		0x14
#define GPC_PGC_ACK_SEL_A7	0x24

#define GPC_SLOTx_CFG(x) 	(0xb0 + 4 * (x))

#define GPC_PGC_CPU_MAPPING	0xec

#define GPC_PGC_C0		0x800
#define GPC_PGC_C1		0x840
#define GPC_PGC_SCU		0x880
#define GPC_PGC_SCU_TIMING	0x890
#define GPC_PGC_FM		0xa00
#define GPC_PGC_MIPI_PHY	0xc00
#define GPC_PGC_PCIE_PHY	0xc40
#define GPC_PGC_USB_OTG1_PHY	0xc80
#define GPC_PGC_USB_OTG2_PHY	0xcc0
#define GPC_PGC_USB_HSIC_PHY	0xd00

#define ANADIG_ARM_PLL		0x60
#define ANADIG_DDR_PLL		0x70
#define ANADIG_SYS_PLL		0xb0
#define ANADIG_ENET_PLL		0xe0
#define ANADIG_AUDIO_PLL	0xf0
#define ANADIG_VIDEO_PLL	0x130

#define BM_LPCR_A7_AD_L2PGE			(0x1 << 16)
#define BM_LPCR_A7_AD_EN_C1_PUP			(0x1 << 11)
#define BM_LPCR_A7_AD_EN_C1_IRQ_PUP		(0x1 << 10)
#define BM_LPCR_A7_AD_EN_C0_PUP			(0x1 << 9)
#define BM_LPCR_A7_AD_EN_C0_IRQ_PUP		(0x1 << 8)
#define BM_LPCR_A7_AD_EN_PLAT_PDN		(0x1 << 4)
#define BM_LPCR_A7_AD_EN_C1_PDN			(0x1 << 3)
#define BM_LPCR_A7_AD_EN_C1_WFI_PDN		(0x1 << 2)
#define BM_LPCR_A7_AD_EN_C0_PDN			(0x1 << 1)
#define BM_LPCR_A7_AD_EN_C0_WFI_PDN		(0x1)

#define BM_LPCR_A7_BSC_IRQ_SRC_A7_WAKEUP	(0x7 << 28)
#define BM_LPCR_A7_BSC_CPU_CLK_ON_LPM		(0x1 << 14)
#define BM_LPCR_A7_BSC_LPM1			(0x3 << 2)
#define BM_LPCR_A7_BSC_LPM0			(0x3)

#define BM_LPCR_M4_MASK_DSM_TRIGGER		(0x1 << 31)

#define BM_SLPCR_EN_DSM				(0x1 << 31)
#define BM_SLPCR_RBC_EN				(0x1 << 30)
#define BM_SLPCR_VSTBY				(0x1 << 2)
#define BM_SLPCR_SBYOS				(0x1 << 1)
#define BM_SLPCR_BYPASS_PMIC_READY		(0x1)

#define BM_GPC_PGC_ACK_SEL_A7_DUMMY_PUP		(0x1 << 31)
#define BM_GPC_PGC_ACK_SEL_A7_DUMMY_PDN		(0x1 << 15)

#define BM_ANADIG_ARM_PLL_OVERRIDE		(0x1 << 20)
#define BM_ANADIG_DDR_PLL_OVERRIDE		(0x1 << 19)
#define BM_ANADIG_SYS_PLL_PFDx_OVERRIDE		(0x1FF << 17)
#define BM_ANADIG_ENET_PLL_OVERRIDE		(0x1 << 13)
#define BM_ANADIG_AUDIO_PLL_OVERRIDE		(0x1 << 24)
#define BM_ANADIG_VIDEO_PLL_OVERRIDE		(0x1 << 24)

#define A7_LPM_WAIT		0x5
#define A7_LPM_STOP		0xa
#define GPC_MAX_SLOT_NUMBER	10

#define REG_SET			0x4
#define REG_CLR			0x8

#define MX7_MAX_DDRC_NUM		32
#define MX7_MAX_DDRC_PHY_NUM		16

#define READ_DATA_FROM_HARDWARE		0
#define MX7_SUSPEND_OCRAM_SIZE		0x1000

enum gpcv2_mode {
	GPC_WAIT_CLOCKED,
	GPC_WAIT_UNCLOCKED,
	GPC_STOP_POWER_ON,
	GPC_STOP_POWER_OFF,
};

/*
 * GPCv2 has the following power domains, and each domain can be power-up
 * and power-down via GPC settings.
 *
 * 	Core 0 of A7 power domain
 * 	Core1 of A7 power domain
 * 	SCU/L2 cache RAM of A7 power domain
 * 	Fastmix and megamix power domain
 * 	USB OTG1 PHY power domain
 * 	USB OTG2 PHY power domain
 * 	PCIE PHY power domain
 * 	USB HSIC PHY power domain
 *	Core 0 of M4 power domain
 */

enum gpcv2_slot {
	CORE0_A7,
	CORE1_A7,
	SCU_A7,
	FAST_MEGA_MIX,
	MIPI_PHY,
	PCIE_PHY,
	USB_OTG1_PHY,
	USB_OTG2_PHY,
	USB_HSIC_PHY,
	CORE0_M4,
};

struct imx_gpcv2;

struct imx_gpcv2_suspend {
	void (*set_mode)(struct imx_gpcv2 *, enum gpcv2_mode mode);
	void (*lpm_cpu_power_gate)(struct imx_gpcv2 *, u32, bool);
	void (*lpm_plat_power_gate)(struct imx_gpcv2 *, bool);
	void (*lpm_env_setup)(struct imx_gpcv2 *);
	void (*lpm_env_clean)(struct imx_gpcv2 *);

	void (*set_slot)(struct imx_gpcv2 *cd, u32 index,
			enum gpcv2_slot slot, bool mode);
	void (*set_act)(struct imx_gpcv2 *cd,
			enum gpcv2_slot slot, bool mode);
	void (*clear_slots)(struct imx_gpcv2 *);
	void (*lpm_enable_core)(struct imx_gpcv2 *,
			bool enable, u32 offset);

	void (*standby)(struct imx_gpcv2 *);
	void (*suspend)(struct imx_gpcv2 *);

	void (*suspend_fn_in_ocram)(void __iomem *ocram_vbase);
	void __iomem *ocram_vbase;
};

struct imx_gpcv2 {
	u32 *wakeupmix_mask;
	u32 *lpsrmix_mask;
	u32 *mfmix_mask;
	spinlock_t lock;

	struct imx_gpcv2_suspend *pm;
	struct regmap *anatop;
	struct regmap *gpcv2;

	u32 (*get_wakeup_source)(u32 **);
};

struct imx7_pm_base {
	phys_addr_t pbase;
	void __iomem *vbase;
};

struct imx7_pm_socdata {
	u32 ddr_type;
	const char *iomuxc_gpr_compat;
	const char *ddrc_phy_compat;
	const char *anatop_compat;
	const char *ddrc_compat;
	const char *ccm_compat;
	const char *gpc_compat;
	const char *src_compat;
	const u32 ddrc_num;
	const u32 (*ddrc_offset)[2];
	const u32 ddrc_phy_num;
	const u32 (*ddrc_phy_offset)[2];
};

/*
 * This structure is for passing necessary data for low level ocram
 * suspend code(arch/arm/mach-imx/suspend-imx7.S), if this struct
 * definition is changed, the offset definition in
 * arch/arm/mach-imx/suspend-imx7.S must be also changed accordingly,
 * otherwise, the suspend to ocram function will be broken!
 */

struct imx7_cpu_pm_info {
	u32 m4_reserve0;
	u32 m4_reserve1;
	u32 m4_reserve2;

	/* The physical address of pm_info. */
	phys_addr_t pbase;

	/* The physical resume address for asm code */
	phys_addr_t resume_addr;
	u32 ddr_type;

	u32 pm_info_size;

	struct imx7_pm_base ddrc_base;
	struct imx7_pm_base ddrc_phy_base;
	struct imx7_pm_base src_base;
	struct imx7_pm_base iomuxc_gpr_base;
	struct imx7_pm_base ccm_base;
	struct imx7_pm_base gpc_base;
	struct imx7_pm_base l2_base;
	struct imx7_pm_base anatop_base;

	u32 ttbr1;

	/* Number of DDRC which need saved/restored. */
	u32 ddrc_num;

	/* To save offset and value */
	u32 ddrc_val[MX7_MAX_DDRC_NUM][2];

	/* Number of DDRC which need saved/restored. */
	u32 ddrc_phy_num;

	/* To save offset and value */
	u32 ddrc_phy_val[MX7_MAX_DDRC_NUM][2];
} __aligned(8);

static const u32 imx7d_ddrc_ddr3_setting[][2] __initconst = {
	{ 0x0, READ_DATA_FROM_HARDWARE },
	{ 0x1a0, READ_DATA_FROM_HARDWARE },
	{ 0x1a4, READ_DATA_FROM_HARDWARE },
	{ 0x1a8, READ_DATA_FROM_HARDWARE },
	{ 0x64, READ_DATA_FROM_HARDWARE },
	{ 0x490, 0x00000001 },
	{ 0xd0, 0xc0020001 },
	{ 0xd4, READ_DATA_FROM_HARDWARE },
	{ 0xdc, READ_DATA_FROM_HARDWARE },
	{ 0xe0, READ_DATA_FROM_HARDWARE },
	{ 0xe4, READ_DATA_FROM_HARDWARE },
	{ 0xf4, READ_DATA_FROM_HARDWARE },
	{ 0x100, READ_DATA_FROM_HARDWARE },
	{ 0x104, READ_DATA_FROM_HARDWARE },
	{ 0x108, READ_DATA_FROM_HARDWARE },
	{ 0x10c, READ_DATA_FROM_HARDWARE },
	{ 0x110, READ_DATA_FROM_HARDWARE },
	{ 0x114, READ_DATA_FROM_HARDWARE },
	{ 0x120, 0x03030803 },
	{ 0x180, READ_DATA_FROM_HARDWARE },
	{ 0x190, READ_DATA_FROM_HARDWARE },
	{ 0x194, READ_DATA_FROM_HARDWARE },
	{ 0x200, READ_DATA_FROM_HARDWARE },
	{ 0x204, READ_DATA_FROM_HARDWARE },
	{ 0x214, READ_DATA_FROM_HARDWARE },
	{ 0x218, READ_DATA_FROM_HARDWARE },
	{ 0x240, 0x06000601 },
	{ 0x244, READ_DATA_FROM_HARDWARE },
};

static const u32 imx7d_ddrc_phy_ddr3_setting[][2] __initconst = {
	{ 0x0, READ_DATA_FROM_HARDWARE },
	{ 0x4, READ_DATA_FROM_HARDWARE },
	{ 0x10, READ_DATA_FROM_HARDWARE },
	{ 0x9c, READ_DATA_FROM_HARDWARE },
	{ 0x20, READ_DATA_FROM_HARDWARE },
	{ 0x30, READ_DATA_FROM_HARDWARE },
	{ 0x50, 0x01000010 },
	{ 0x50, 0x00000010 },
	{ 0xc0, 0x0e407304 },
	{ 0xc0, 0x0e447304 },
	{ 0xc0, 0x0e447306 },
	{ 0xc0, 0x0e447304 },
	{ 0xc0, 0x0e407306 },
};

static const struct imx7_pm_socdata imx7d_pm_data_ddr3 __initconst = {
	.iomuxc_gpr_compat = "fsl,imx7d-iomuxc",
	.ddrc_phy_compat = "fsl,imx7d-ddrc-phy",
	.anatop_compat = "fsl,imx7d-anatop",
	.ddrc_compat = "fsl,imx7d-ddrc",
	.ccm_compat = "fsl,imx7d-ccm",
	.gpc_compat = "fsl,imx7d-gpc",
	.src_compat = "fsl,imx7d-src",
	.ddrc_phy_num = ARRAY_SIZE(imx7d_ddrc_phy_ddr3_setting),
	.ddrc_phy_offset = imx7d_ddrc_phy_ddr3_setting,
	.ddrc_num = ARRAY_SIZE(imx7d_ddrc_ddr3_setting),
	.ddrc_offset = imx7d_ddrc_ddr3_setting,
};

static struct imx_gpcv2 *gpcv2_instance;

static void imx_gpcv2_lpm_clear_slots(struct imx_gpcv2 *gpc)
{
	int i;

	for (i = 0; i < GPC_MAX_SLOT_NUMBER; i++)
		regmap_write(gpc->gpcv2, GPC_SLOTx_CFG(i), 0x0);

	regmap_write(gpc->gpcv2, GPC_PGC_ACK_SEL_A7,
			BM_GPC_PGC_ACK_SEL_A7_DUMMY_PUP |
			BM_GPC_PGC_ACK_SEL_A7_DUMMY_PDN);
}

static void imx_gpcv2_lpm_enable_core(struct imx_gpcv2 *gpc,
			bool enable, u32 offset)
{
	regmap_write(gpc->gpcv2, offset, enable);
}

static void imx_gpcv2_lpm_slot_setup(struct imx_gpcv2 *gpc,
		u32 index, enum gpcv2_slot slot, bool powerup)
{
	u32 val;

	if (index >= GPC_MAX_SLOT_NUMBER) {
		pr_err("Invalid slot index!\n");
		return;
	}

	val = (powerup ? 0x2 : 0x1) << (slot * 2);
	regmap_write(gpc->gpcv2, GPC_SLOTx_CFG(index), val);
}

static void imx_gpcv2_lpm_set_ack(struct imx_gpcv2 *gpc,
		enum gpcv2_slot slot, bool powerup)
{
	u32 val;

	regmap_read(gpc->gpcv2, GPC_PGC_ACK_SEL_A7, &val);

	/* clear dummy ack */
	val &= ~(powerup ? BM_GPC_PGC_ACK_SEL_A7_DUMMY_PUP :
				BM_GPC_PGC_ACK_SEL_A7_DUMMY_PDN);

	val |= 1 << (slot + (powerup ? 16 : 0));
	regmap_write(gpc->gpcv2, GPC_PGC_ACK_SEL_A7, val);
}

static void imx_gpcv2_lpm_env_setup(struct imx_gpcv2 *gpc)
{
	/* PLL and PFDs overwrite set */
	regmap_write(gpc->anatop, ANADIG_ARM_PLL + REG_SET,
			BM_ANADIG_ARM_PLL_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_DDR_PLL + REG_SET,
			BM_ANADIG_DDR_PLL_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_SYS_PLL + REG_SET,
			BM_ANADIG_SYS_PLL_PFDx_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_ENET_PLL + REG_SET,
			BM_ANADIG_ENET_PLL_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_AUDIO_PLL + REG_SET,
			BM_ANADIG_AUDIO_PLL_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_VIDEO_PLL + REG_SET,
			BM_ANADIG_VIDEO_PLL_OVERRIDE);
}

static void imx_gpcv2_lpm_env_clean(struct imx_gpcv2 *gpc)
{
	/* PLL and PFDs overwrite clear */
	regmap_write(gpc->anatop, ANADIG_ARM_PLL + REG_CLR,
			BM_ANADIG_ARM_PLL_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_DDR_PLL + REG_CLR,
			BM_ANADIG_DDR_PLL_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_SYS_PLL + REG_CLR,
			BM_ANADIG_SYS_PLL_PFDx_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_ENET_PLL + REG_CLR,
			BM_ANADIG_ENET_PLL_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_AUDIO_PLL + REG_CLR,
			BM_ANADIG_AUDIO_PLL_OVERRIDE);
	regmap_write(gpc->anatop, ANADIG_VIDEO_PLL + REG_CLR,
			BM_ANADIG_VIDEO_PLL_OVERRIDE);
}

static void imx_gpcv2_lpm_set_mode(struct imx_gpcv2 *gpc,
		enum gpcv2_mode mode)
{
	u32 lpcr, slpcr;

	regmap_read(gpc->gpcv2, GPC_LPCR_A7_BSC, &lpcr);
	regmap_read(gpc->gpcv2, GPC_SLPCR, &slpcr);

	/* all cores' LPM settings must be same */
	lpcr &= ~(BM_LPCR_A7_BSC_LPM0 | BM_LPCR_A7_BSC_LPM1);
	lpcr |= BM_LPCR_A7_BSC_CPU_CLK_ON_LPM;

	slpcr &= ~(BM_SLPCR_EN_DSM | BM_SLPCR_VSTBY | BM_SLPCR_RBC_EN |
		BM_SLPCR_SBYOS | BM_SLPCR_BYPASS_PMIC_READY);

	switch (mode) {
	case GPC_WAIT_CLOCKED:
		break;
	case GPC_WAIT_UNCLOCKED:
		lpcr |= A7_LPM_WAIT;
		lpcr &= ~BM_LPCR_A7_BSC_CPU_CLK_ON_LPM;
		break;
	case GPC_STOP_POWER_ON:
		lpcr |= A7_LPM_STOP;
		lpcr &= ~BM_LPCR_A7_BSC_CPU_CLK_ON_LPM;
		slpcr |= (BM_SLPCR_EN_DSM | BM_SLPCR_RBC_EN |
			BM_SLPCR_BYPASS_PMIC_READY);
		break;
	case GPC_STOP_POWER_OFF:
		lpcr |= A7_LPM_STOP;
		lpcr &= ~BM_LPCR_A7_BSC_CPU_CLK_ON_LPM;
		slpcr |= (BM_SLPCR_EN_DSM | BM_SLPCR_RBC_EN |
			BM_SLPCR_SBYOS | BM_SLPCR_VSTBY  |
			BM_SLPCR_BYPASS_PMIC_READY);
		break;
	default:
		return;
	}
	regmap_write(gpc->gpcv2, GPC_LPCR_A7_BSC, lpcr);
	regmap_write(gpc->gpcv2, GPC_SLPCR, slpcr);
}

static void imx_gpcv2_lpm_cpu_power_gate(struct imx_gpcv2 *gpc,
				u32 cpu, bool engate)
{
	u32 val;

	const u32 val_pdn[2] = {
		BM_LPCR_A7_AD_EN_C0_PDN | BM_LPCR_A7_AD_EN_C0_PUP,
		BM_LPCR_A7_AD_EN_C1_PDN | BM_LPCR_A7_AD_EN_C1_PUP,
	};

	regmap_read(gpc->gpcv2, GPC_LPCR_A7_AD, &val);
	if (engate)
		val |= val_pdn[cpu];
	else
		val &= ~val_pdn[cpu];

	regmap_write(gpc->gpcv2, GPC_LPCR_A7_AD, val);
}

static void imx_lpm_plat_power_gate(struct imx_gpcv2 *gpc, bool engate)
{
	u32 val;

	regmap_read(gpc->gpcv2, GPC_LPCR_A7_AD, &val);
	val &= ~(BM_LPCR_A7_AD_EN_PLAT_PDN | BM_LPCR_A7_AD_L2PGE);
	if (engate)
		val |= BM_LPCR_A7_AD_EN_PLAT_PDN | BM_LPCR_A7_AD_L2PGE;

	regmap_write(gpc->gpcv2, GPC_LPCR_A7_AD, val);
}

static void imx_gpcv2_lpm_standby(struct imx_gpcv2 *gpc)
{
	struct imx_gpcv2_suspend *pm = gpc->pm;

	pm->lpm_env_setup(gpc);
	/* pm->set_mode(gpc, GPC_STOP_POWER_OFF); */
	pm->set_mode(gpc, GPC_WAIT_UNCLOCKED);

	/* Zzz ... */
	cpu_do_idle();

	pm->set_mode(gpc, GPC_WAIT_CLOCKED);
	pm->lpm_env_clean(gpc);
}

static int gpcv2_suspend_finish(unsigned long val)
{
	struct imx_gpcv2_suspend *pm = (struct imx_gpcv2_suspend *)val;

	if (!pm->suspend_fn_in_ocram) {
		cpu_do_idle();
	} else {
		/*
		 * call low level suspend function in ocram,
		 * as we need to float DDR IO.
		 */
		local_flush_tlb_all();
		pm->suspend_fn_in_ocram(pm->ocram_vbase);
	}

	return 0;
}

static void imx_gpcv2_lpm_suspend(struct imx_gpcv2 *gpc)
{
	struct imx_gpcv2_suspend *pm = gpc->pm;
	u32 *sources;
	int i, num;

	pm->lpm_env_setup(gpc);
	pm->set_mode(gpc, GPC_STOP_POWER_OFF);

	/* enable core0 power down/up with low power mode */
	pm->lpm_cpu_power_gate(gpc, 0, true);

	/* enable plat power down with low power mode */
	pm->lpm_plat_power_gate(gpc, true);

	/*
	 * To avoid confuse, we use slot 0~4 for power down,
	 * slot 5~9 for power up.
	 *
	 * Power down slot sequence:
	 * Slot0 -> CORE0
	 * Slot1 -> Mega/Fast MIX
	 * Slot2 -> SCU
	 *
	 * Power up slot sequence:
	 * Slot5 -> Mega/Fast MIX
	 * Slot6 -> SCU
	 * Slot7 -> CORE0
	 */
	pm->set_slot(gpc, 0, CORE0_A7, false);
	pm->set_slot(gpc, 2, SCU_A7, false);

	if (gpc->get_wakeup_source) {
		pm->set_slot(gpc, 1, FAST_MEGA_MIX, false);
		pm->set_slot(gpc, 5, FAST_MEGA_MIX, true);
		pm->lpm_enable_core(gpc, true, GPC_PGC_FM);
		num = gpc->get_wakeup_source(&sources);
		for (i = 0; i < num; i++) {
			if ((~sources[i] & gpc->mfmix_mask[i]) == 0)
				continue;
			pm->lpm_enable_core(gpc, false, GPC_PGC_FM);
			break;
		}
	}

	pm->set_slot(gpc, 6, SCU_A7, true);
	pm->set_slot(gpc, 7, CORE0_A7, true);

	/* Set Power down act */
	pm->set_act(gpc, SCU_A7, false);

	/* Set Power up act */
	pm->set_act(gpc, CORE0_A7, true);

	/* enable core0, scu */
	pm->lpm_enable_core(gpc, true, GPC_PGC_C0);
	pm->lpm_enable_core(gpc, true, GPC_PGC_SCU);

	cpu_suspend((unsigned long)pm, gpcv2_suspend_finish);

	pm->lpm_env_clean(gpc);
	pm->set_mode(gpc, GPC_WAIT_CLOCKED);
	pm->lpm_cpu_power_gate(gpc, 0, false);
	pm->lpm_plat_power_gate(gpc, false);

	pm->lpm_enable_core(gpc, false, GPC_PGC_C0);
	pm->lpm_enable_core(gpc, false, GPC_PGC_SCU);
	pm->lpm_enable_core(gpc, false, GPC_PGC_FM);
	pm->clear_slots(gpc);
}

static int imx_gpcv2_pm_enter(suspend_state_t state)
{
	struct imx_gpcv2_suspend *pm;

	BUG_ON(!gpcv2_instance);
	pm = gpcv2_instance->pm;

	switch (state) {
	case PM_SUSPEND_STANDBY:
		pm->standby(gpcv2_instance);
		break;

	case PM_SUSPEND_MEM:
		pm->suspend(gpcv2_instance);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int imx_gpcv2_pm_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM || state == PM_SUSPEND_STANDBY;
}

static int __init imx_get_base_from_node(struct device_node *node,
			struct imx7_pm_base *base)
{
	struct resource res;
	int ret = 0;

	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		goto put_node;

	base->pbase = res.start;
	base->vbase = ioremap(res.start, resource_size(&res));
	if (!base->vbase) {
		iounmap(base->vbase);
		ret = -ENOMEM;
	}
put_node:
	of_node_put(node);

	return ret;
}

static int __init imx_get_base_from_dt(struct imx7_pm_base *base,
				const char *compat)
{
	struct device_node *node;
	struct resource res;
	int ret = 0;

	node = of_find_compatible_node(NULL, NULL, compat);
	if (!node) {
		ret = -ENODEV;
		goto out;
	}

	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		goto put_node;

	base->pbase = res.start;
	base->vbase = ioremap(res.start, resource_size(&res));
	if (!base->vbase)
		ret = -ENOMEM;

put_node:
	of_node_put(node);
out:
	return ret;
}

static int __init imx_get_exec_base_from_dt(struct imx7_pm_base *base,
				const char *compat)
{
	struct device_node *node;
	struct resource res;
	int ret = 0;

	node = of_find_compatible_node(NULL, NULL, compat);
	if (!node) {
		ret = -ENODEV;
		goto out;
	}

	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		goto put_node;

	base->pbase = res.start;
	base->vbase = __arm_ioremap_exec(res.start, resource_size(&res), false);

	if (!base->vbase)
		ret = -ENOMEM;

put_node:
	of_node_put(node);
out:
	return ret;
}

static int __init imx_gpcv2_suspend_init(struct imx_gpcv2_suspend *pm,
			const struct imx7_pm_socdata *socdata)
{
	struct imx7_pm_base aips_base[3] = { {0, 0}, {0, 0}, {0, 0} };
	struct imx7_pm_base sram_base = {0, 0};
	struct imx7_cpu_pm_info *pm_info;
	struct device_node *node = NULL;
	int i, ret = 0;

	const u32 (*ddrc_phy_offset_array)[2];
	const u32 (*ddrc_offset_array)[2];

	if (!socdata || !pm) {
		pr_warn("%s: invalid argument!\n", __func__);
		return -EINVAL;
	}

	for (i = 0; i < 3; i++) {
		node = of_find_compatible_node(node, NULL, "fsl,aips-bus");
		if (!node) {
			pr_warn("%s: failed to find aips %d node!\n",
					__func__, i);
			break;
		}
		ret = imx_get_base_from_node(node, &aips_base[i]);
		if (ret) {
			pr_warn("%s: failed to get aips[%d] base %d!\n",
					__func__, i, ret);
		}
	}

	ret = imx_get_exec_base_from_dt(&sram_base, "fsl,lpm-sram");
	if (ret) {
		pr_warn("%s: failed to get lpm-sram base %d!\n",
				__func__, ret);
		goto lpm_sram_map_failed;
	}

	pm_info = sram_base.vbase;
	pm_info->pbase = sram_base.pbase;
	pm_info->resume_addr = virt_to_phys(ca7_cpu_resume);
	pm_info->pm_info_size = sizeof(*pm_info);

	ret = imx_get_base_from_dt(&pm_info->ccm_base, socdata->ccm_compat);
	if (ret) {
		pr_warn("%s: failed to get ccm base %d!\n", __func__, ret);
		goto ccm_map_failed;
	}

	ret = imx_get_base_from_dt(&pm_info->ddrc_base, socdata->ddrc_compat);
	if (ret) {
		pr_warn("%s: failed to get ddrc base %d!\n", __func__, ret);
		goto ddrc_map_failed;
	}

	ret = imx_get_base_from_dt(&pm_info->ddrc_phy_base,
				socdata->ddrc_phy_compat);
	if (ret) {
		pr_warn("%s: failed to get ddrc_phy base %d!\n", __func__, ret);
		goto ddrc_phy_map_failed;
	}

	ret = imx_get_base_from_dt(&pm_info->src_base, socdata->src_compat);
	if (ret) {
		pr_warn("%s: failed to get src base %d!\n", __func__, ret);
		goto src_map_failed;
	}

	ret = imx_get_base_from_dt(&pm_info->iomuxc_gpr_base,
				socdata->iomuxc_gpr_compat);
	if (ret) {
		pr_warn("%s: failed to get iomuxc_gpr base %d!\n",
					__func__, ret);
		goto iomuxc_gpr_map_failed;
	}

	ret = imx_get_base_from_dt(&pm_info->gpc_base, socdata->gpc_compat);
	if (ret) {
		pr_warn("%s: failed to get gpc base %d!\n", __func__, ret);
		goto gpc_map_failed;
	}

	ret = imx_get_base_from_dt(&pm_info->anatop_base,
				socdata->anatop_compat);
	if (ret) {
		pr_warn("%s: failed to get anatop base %d!\n", __func__, ret);
		goto anatop_map_failed;
	}

	pm_info->ddrc_num = socdata->ddrc_num;
	ddrc_offset_array = socdata->ddrc_offset;
	pm_info->ddrc_phy_num = socdata->ddrc_phy_num;
	ddrc_phy_offset_array = socdata->ddrc_phy_offset;

	/* initialize DDRC settings */
	for (i = 0; i < pm_info->ddrc_num; i++) {
		pm_info->ddrc_val[i][0] = ddrc_offset_array[i][0];
		if (ddrc_offset_array[i][1] == READ_DATA_FROM_HARDWARE)
			pm_info->ddrc_val[i][1] =
				readl_relaxed(pm_info->ddrc_base.vbase +
				ddrc_offset_array[i][0]);
		else
			pm_info->ddrc_val[i][1] = ddrc_offset_array[i][1];
	}

	/* initialize DDRC PHY settings */
	for (i = 0; i < pm_info->ddrc_phy_num; i++) {
		pm_info->ddrc_phy_val[i][0] =
			ddrc_phy_offset_array[i][0];
		if (ddrc_phy_offset_array[i][1] == READ_DATA_FROM_HARDWARE)
			pm_info->ddrc_phy_val[i][1] =
				readl_relaxed(pm_info->ddrc_phy_base.vbase +
				ddrc_phy_offset_array[i][0]);
		else
			pm_info->ddrc_phy_val[i][1] =
				ddrc_phy_offset_array[i][1];
	}

	pm->suspend_fn_in_ocram = fncpy(
		sram_base.vbase + sizeof(*pm_info),
		&imx7_suspend,
		MX7_SUSPEND_OCRAM_SIZE - sizeof(*pm_info));
	pm->ocram_vbase = sram_base.vbase;

	goto put_node;

anatop_map_failed:
	iounmap(pm_info->anatop_base.vbase);
gpc_map_failed:
	iounmap(pm_info->gpc_base.vbase);
iomuxc_gpr_map_failed:
	iounmap(pm_info->iomuxc_gpr_base.vbase);
src_map_failed:
	iounmap(pm_info->src_base.vbase);
ddrc_phy_map_failed:
	iounmap(pm_info->ddrc_phy_base.vbase);
ddrc_map_failed:
	iounmap(pm_info->ddrc_base.vbase);
ccm_map_failed:
	iounmap(pm_info->ccm_base.vbase);
lpm_sram_map_failed:
	iounmap(sram_base.vbase);
put_node:
	of_node_put(node);

	return ret;
}

static const struct platform_suspend_ops imx_gpcv2_pm_ops = {
	.enter = imx_gpcv2_pm_enter,
	.valid = imx_gpcv2_pm_valid,
};

static int __init imx_gpcv2_pm_init(void)
{
	struct imx_gpcv2_suspend *pm;
	struct imx_gpcv2 *gpc;
	int val, num;

	pm = kzalloc(sizeof(struct imx_gpcv2_suspend), GFP_KERNEL);
	if (!pm) {
		pr_warn("[GPCv2] %s init failed\r\n", __func__);
		return -ENOMEM;
	}

	gpc = kzalloc(sizeof(struct imx_gpcv2), GFP_KERNEL);
	if (!gpc) {
		pr_warn("[GPCv2] %s init failed\r\n", __func__);
		kfree(pm);
		return -ENOMEM;
	}

	imx_gpcv2_suspend_init(pm, &imx7d_pm_data_ddr3);

	pm->lpm_env_setup = imx_gpcv2_lpm_env_setup;
	pm->lpm_env_clean = imx_gpcv2_lpm_env_clean;

	pm->lpm_cpu_power_gate = imx_gpcv2_lpm_cpu_power_gate;
	pm->lpm_plat_power_gate = imx_lpm_plat_power_gate;
	pm->lpm_enable_core = imx_gpcv2_lpm_enable_core;
	pm->set_mode = imx_gpcv2_lpm_set_mode;

	pm->clear_slots = imx_gpcv2_lpm_clear_slots;
	pm->set_slot = imx_gpcv2_lpm_slot_setup;
	pm->set_act = imx_gpcv2_lpm_set_ack;

	pm->standby = imx_gpcv2_lpm_standby;
	pm->suspend = imx_gpcv2_lpm_suspend;

	gpc->anatop = syscon_regmap_lookup_by_compatible("fsl,imx6q-anatop");
	if (IS_ERR(gpc->anatop))
		goto error_exit;

	gpc->gpcv2 = syscon_regmap_lookup_by_compatible("fsl,imx7d-gpc");
	if (IS_ERR(gpc->gpcv2))
		goto error_exit;

	/* only external IRQs to wake up LPM and core 0/1 */
	regmap_read(gpc->gpcv2, GPC_LPCR_A7_BSC, &val);
	val |= BM_LPCR_A7_BSC_IRQ_SRC_A7_WAKEUP;
	regmap_write(gpc->gpcv2, GPC_LPCR_A7_BSC, val);
	/* mask m4 dsm trigger */
	regmap_read(gpc->gpcv2, GPC_LPCR_M4, &val);
	val |= BM_LPCR_M4_MASK_DSM_TRIGGER;
	regmap_write(gpc->gpcv2, GPC_LPCR_M4, val);

	/* set mega/fast mix in A7 domain */
	regmap_write(gpc->gpcv2, GPC_PGC_CPU_MAPPING, 0x1);
	/* set SCU timing */
	val = (0x59 << 10) | 0x5B | (0x51 << 20);
	regmap_write(gpc->gpcv2, GPC_PGC_SCU_TIMING, val);

	gpc->pm = pm;
	gpc->get_wakeup_source = imx_gpcv2_get_wakeup_source;
	gpcv2_instance = gpc;

	num = imx_gpcv2_get_wakeup_source(0);

	/*
	 * The IP blocks which may be the wakeup sources are allocated into
	 * several power domains. MFMIX, LPSRMX, and WAKEUPMIX are three of
	 * those power domains. If a bit is '1' in the mask, it means the IP
	 * block is inside the power domain. The mask will be used to decide
	 * if a power domain should be shutdown or not when system goes into
	 * suspend states.
	 */

	if (num)
		gpc->wakeupmix_mask = kzalloc(sizeof(u32)*num*3, GFP_KERNEL);

	if (!gpc->wakeupmix_mask)
		goto error_exit;

	/*
	 * Shutdown LPSRMIX and WAKEUP power domain has not been
	 * implemented in this patch yet.
	 */

	gpc->lpsrmix_mask = gpc->wakeupmix_mask + num;
	gpc->mfmix_mask = gpc->wakeupmix_mask + num*2;

	/* Mask the wakeup sources in M/F power domain */
	gpc->mfmix_mask[0] = 0x54010000;
	gpc->mfmix_mask[1] = 0xC00;
	gpc->mfmix_mask[2] = 0x0;
	gpc->mfmix_mask[3] = 0x400010;

	suspend_set_ops(&imx_gpcv2_pm_ops);
	return 0;

error_exit:
	kfree(pm);
	kfree(gpc);
	return -ENODEV;
}

device_initcall(imx_gpcv2_pm_init);
