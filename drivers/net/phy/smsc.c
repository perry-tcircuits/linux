// SPDX-License-Identifier: GPL-2.0+
/*
 * drivers/net/phy/smsc.c
 *
 * Driver for SMSC PHYs
 *
 * Author: Herbert Valerio Riedel
 *
 * Copyright (c) 2006 Herbert Valerio Riedel <hvr@gnu.org>
 *
 * Support added for SMSC LAN8187 and LAN8700 by steve.glendinning@shawell.net
 *
 */

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/netdevice.h>
#include <linux/smscphy.h>

/* Vendor-specific PHY Definitions */
/* EDPD NLP / crossover time configuration */
#define PHY_EDPD_CONFIG			16
#define PHY_EDPD_CONFIG_EXT_CROSSOVER_	0x0001

/* Control/Status Indication Register */
#define SPECIAL_CTRL_STS		27
#define SPECIAL_CTRL_STS_OVRRD_AMDIX_	0x8000
#define SPECIAL_CTRL_STS_AMDIX_ENABLE_	0x4000
#define SPECIAL_CTRL_STS_AMDIX_STATE_	0x2000

#define EDPD_MAX_WAIT_DFLT_MS		640
/* interval between phylib state machine runs in ms */
#define PHY_STATE_MACH_MS		1000

struct smsc_hw_stat {
	const char *string;
	u8 reg;
	u8 bits;
};

static struct smsc_hw_stat smsc_hw_stats[] = {
	{ "phy_symbol_errors", 26, 16},
};

struct smsc_phy_priv {
	unsigned int edpd_enable:1;
	unsigned int edpd_mode_set_by_user:1;
	unsigned int edpd_max_wait_ms;
	bool wol_arp;
	u16 intmask;
	bool energy_enable;
	bool wakeup_enable;
};

static int smsc_phy_ack_interrupt(struct phy_device *phydev)
{
	int rc = phy_read(phydev, MII_LAN83C185_ISF);

	return rc < 0 ? rc : 0;
}

int smsc_phy_config_intr(struct phy_device *phydev)
{
	struct smsc_phy_priv *priv = phydev->priv;
	int rc;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		rc = smsc_phy_ack_interrupt(phydev);
		if (rc)
			return rc;

		if (priv->wakeup_enable)
			priv->intmask |= MII_LAN83C185_ISF_INT8;
		rc = phy_write(phydev, MII_LAN83C185_IM, priv->intmask);
	} else {
		priv->intmask = 0;

		rc = phy_write(phydev, MII_LAN83C185_IM, 0);
		if (rc)
			return rc;

		rc = smsc_phy_ack_interrupt(phydev);
	}

	return rc < 0 ? rc : 0;
}
EXPORT_SYMBOL_GPL(smsc_phy_config_intr);

static int smsc_phy_config_edpd(struct phy_device *phydev)
{
	struct smsc_phy_priv *priv = phydev->priv;

	if (priv->edpd_enable)
		return phy_set_bits(phydev, MII_LAN83C185_CTRL_STATUS,
				    MII_LAN83C185_EDPWRDOWN);
	else
		return phy_clear_bits(phydev, MII_LAN83C185_CTRL_STATUS,
				      MII_LAN83C185_EDPWRDOWN);
}

irqreturn_t smsc_phy_handle_interrupt(struct phy_device *phydev)
{
	struct smsc_phy_priv *priv = phydev->priv;
	int irq_status;

	irq_status = phy_read(phydev, MII_LAN83C185_ISF);
	if (irq_status < 0) {
		if (irq_status != -ENODEV)
			phy_error(phydev);

		return IRQ_NONE;
	}

	if (!(irq_status &  priv->intmask))
		return IRQ_NONE;

	phy_trigger_machine(phydev);

	return IRQ_HANDLED;
}
EXPORT_SYMBOL_GPL(smsc_phy_handle_interrupt);

static int smsc_phy_config_wol(struct phy_device *phydev)
{
	int i, wol_ctrl, wol_filter;
	u16 pwd[3] = {0, 0, 0};

	/* Write @MAC in LAN8742_MMD3_MAC_ADDRA/B/C registers */
	const u8 *mac_addr = phydev->attached_dev->dev_addr;
	/* Store the device address for the magic packet */
	for (i = 0; i < ARRAY_SIZE(pwd); i++)
		pwd[i] = mac_addr[5 - i * 2] << 8 | mac_addr[5 - (i * 2 + 1)];

	phy_write_mmd(phydev, 3, LAN8742_MMD3_MAC_ADDRA,
		      pwd[0]);

	phy_write_mmd(phydev, 3, LAN8742_MMD3_MAC_ADDRB,
		      pwd[1]);

	phy_write_mmd(phydev, 3, LAN8742_MMD3_MAC_ADDRC,
		      pwd[2]);

	/* Configure WoL */
	wol_ctrl = phy_read_mmd(phydev, 3, LAN8742_MMD3_WAKEUP_CTRL);

	/* Configure LED2 functions as nPME, WoL Configured, Magic Packet Enable */
	wol_ctrl |= (LAN8742_MMD3_WUCSR_LED2_AS_NPME | LAN8742_MMD3_WUCSR_WOL | LAN8742_MMD3_WUCSR_MPEN);
	phy_write_mmd(phydev, 3, LAN8742_MMD3_WAKEUP_CTRL,
		      wol_ctrl);

	wol_filter = phy_read_mmd(phydev, 3, LAN8742_MMD3_WAKEUP_FILTER);

	/* Configure Filter enabled, Address Match Enable */
	wol_filter |= (LAN8742_MMD3_WUF_CFGA_FE | LAN8742_MMD3_WUF_CFGA_AME);
	phy_write_mmd(phydev, 3, LAN8742_MMD3_WAKEUP_FILTER,
		      wol_filter);

	return 0;
}

int smsc_phy_config_init(struct phy_device *phydev)
{
	struct smsc_phy_priv *priv = phydev->priv;

	if (!priv)
		return 0;

	if (priv->wakeup_enable)
		smsc_phy_config_wol(phydev);

	/* don't use EDPD in irq mode except overridden by user */
	if (!priv->edpd_mode_set_by_user && phydev->irq != PHY_POLL)
		priv->edpd_enable = false;

	return smsc_phy_config_edpd(phydev);
}
EXPORT_SYMBOL_GPL(smsc_phy_config_init);

static int smsc_phy_reset(struct phy_device *phydev)
{
	int rc = phy_read(phydev, MII_LAN83C185_SPECIAL_MODES);
	if (rc < 0)
		return rc;

	/* If the SMSC PHY is in power down mode, then set it
	 * in all capable mode before using it.
	 */
	if ((rc & MII_LAN83C185_MODE_MASK) == MII_LAN83C185_MODE_POWERDOWN) {
		/* set "all capable" mode */
		rc |= MII_LAN83C185_MODE_ALL;
		phy_write(phydev, MII_LAN83C185_SPECIAL_MODES, rc);
	}

	/* reset the phy */
	return genphy_soft_reset(phydev);
}

static int smsc_phy_suspend(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;

	/* do not power down PHY when PHY enable power/wakeup */
	if (!device_may_wakeup(dev))
		return genphy_suspend(phydev);

	return 0;
}

static int lan87xx_config_aneg(struct phy_device *phydev)
{
	int rc;
	int val;

	switch (phydev->mdix_ctrl) {
	case ETH_TP_MDI:
		val = SPECIAL_CTRL_STS_OVRRD_AMDIX_;
		break;
	case ETH_TP_MDI_X:
		val = SPECIAL_CTRL_STS_OVRRD_AMDIX_ |
			SPECIAL_CTRL_STS_AMDIX_STATE_;
		break;
	case ETH_TP_MDI_AUTO:
		val = SPECIAL_CTRL_STS_AMDIX_ENABLE_;
		break;
	default:
		return genphy_config_aneg(phydev);
	}

	rc = phy_read(phydev, SPECIAL_CTRL_STS);
	if (rc < 0)
		return rc;

	rc &= ~(SPECIAL_CTRL_STS_OVRRD_AMDIX_ |
		SPECIAL_CTRL_STS_AMDIX_ENABLE_ |
		SPECIAL_CTRL_STS_AMDIX_STATE_);
	rc |= val;
	phy_write(phydev, SPECIAL_CTRL_STS, rc);

	phydev->mdix = phydev->mdix_ctrl;
	return genphy_config_aneg(phydev);
}

static int lan95xx_config_aneg_ext(struct phy_device *phydev)
{
	if (phydev->phy_id == 0x0007c0f0) { /* LAN9500A or LAN9505A */
		/* Extend Manual AutoMDIX timer */
		int rc = phy_set_bits(phydev, PHY_EDPD_CONFIG,
				      PHY_EDPD_CONFIG_EXT_CROSSOVER_);

		if (rc < 0)
			return rc;
	}

	return lan87xx_config_aneg(phydev);
}

/*
 * The LAN87xx suffers from rare absence of the ENERGYON-bit when Ethernet cable
 * plugs in while LAN87xx is in Energy Detect Power-Down mode. This leads to
 * unstable detection of plugging in Ethernet cable.
 * This workaround disables Energy Detect Power-Down mode and waiting for
 * response on link pulses to detect presence of plugged Ethernet cable.
 * The Energy Detect Power-Down mode is enabled again in the end of procedure to
 * save approximately 220 mW of power if cable is unplugged.
 * The workaround is only applicable to poll mode. Energy Detect Power-Down may
 * not be used in interrupt mode lest link change detection becomes unreliable.
 */
int lan87xx_read_status(struct phy_device *phydev)
{
	struct smsc_phy_priv *priv = phydev->priv;
	int err;

	err = genphy_read_status(phydev);
	if (err)
		return err;

	if (!phydev->link && priv && priv->edpd_enable &&
	    priv->edpd_max_wait_ms) {
		unsigned int max_wait = priv->edpd_max_wait_ms * 1000;
		int rc;

		/* Disable EDPD to wake up PHY */
		rc = phy_read(phydev, MII_LAN83C185_CTRL_STATUS);
		if (rc < 0)
			return rc;

		rc = phy_write(phydev, MII_LAN83C185_CTRL_STATUS,
			       rc & ~MII_LAN83C185_EDPWRDOWN);
		if (rc < 0)
			return rc;

		/* Wait max 640 ms to detect energy and the timeout is not
		 * an actual error.
		 */
		read_poll_timeout(phy_read, rc,
				  rc & MII_LAN83C185_ENERGYON || rc < 0,
				  10000, max_wait, true, phydev,
				  MII_LAN83C185_CTRL_STATUS);
		if (rc < 0)
			return rc;

		/* Re-enable EDPD */
		rc = phy_read(phydev, MII_LAN83C185_CTRL_STATUS);
		if (rc < 0)
			return rc;

		rc = phy_write(phydev, MII_LAN83C185_CTRL_STATUS,
			       rc | MII_LAN83C185_EDPWRDOWN);
		if (rc < 0)
			return rc;
	}

	if (priv->wakeup_enable) {
		/* Check status of WUCSR bits 7:4 : Perfect DA Frame, Remote Wakeup
		 * Frame, Magic Packet, Broadcast Frame Received, if one of these bits
		 * are 1, clearing them*/
		int wol_ctrl = phy_read_mmd(phydev, 3, LAN8742_MMD3_WAKEUP_CTRL);

		if ((wol_ctrl & (LAN8742_MMD3_WUCSR_PFDA_FR | LAN8742_MMD3_WUCSR_WUFR |
				 LAN8742_MMD3_WUCSR_MPR | LAN8742_MMD3_WUCSR_BCAST_FR)) > 0) {
			wol_ctrl |= (LAN8742_MMD3_WUCSR_PFDA_FR | LAN8742_MMD3_WUCSR_WUFR |
				     LAN8742_MMD3_WUCSR_MPR | LAN8742_MMD3_WUCSR_BCAST_FR);
			phy_write_mmd(phydev, 3, LAN8742_MMD3_WAKEUP_CTRL,
				      wol_ctrl);
		}
	}
	return err;
}
EXPORT_SYMBOL_GPL(lan87xx_read_status);

static int smsc_get_sset_count(struct phy_device *phydev)
{
	return ARRAY_SIZE(smsc_hw_stats);
}

static void smsc_get_strings(struct phy_device *phydev, u8 *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(smsc_hw_stats); i++) {
		strncpy(data + i * ETH_GSTRING_LEN,
		       smsc_hw_stats[i].string, ETH_GSTRING_LEN);
	}
}

static u64 smsc_get_stat(struct phy_device *phydev, int i)
{
	struct smsc_hw_stat stat = smsc_hw_stats[i];
	int val;
	u64 ret;

	val = phy_read(phydev, stat.reg);
	if (val < 0)
		ret = U64_MAX;
	else
		ret = val;

	return ret;
}

static void smsc_get_stats(struct phy_device *phydev,
			   struct ethtool_stats *stats, u64 *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(smsc_hw_stats); i++)
		data[i] = smsc_get_stat(phydev, i);
}

static int smsc_phy_get_edpd(struct phy_device *phydev, u16 *edpd)
{
	struct smsc_phy_priv *priv = phydev->priv;

	if (!priv)
		return -EOPNOTSUPP;

	if (!priv->edpd_enable)
		*edpd = ETHTOOL_PHY_EDPD_DISABLE;
	else if (!priv->edpd_max_wait_ms)
		*edpd = ETHTOOL_PHY_EDPD_NO_TX;
	else
		*edpd = PHY_STATE_MACH_MS + priv->edpd_max_wait_ms;

	return 0;
}

static int smsc_phy_set_edpd(struct phy_device *phydev, u16 edpd)
{
	struct smsc_phy_priv *priv = phydev->priv;

	if (!priv)
		return -EOPNOTSUPP;

	switch (edpd) {
	case ETHTOOL_PHY_EDPD_DISABLE:
		priv->edpd_enable = false;
		break;
	case ETHTOOL_PHY_EDPD_NO_TX:
		priv->edpd_enable = true;
		priv->edpd_max_wait_ms = 0;
		break;
	case ETHTOOL_PHY_EDPD_DFLT_TX_MSECS:
		edpd = PHY_STATE_MACH_MS + EDPD_MAX_WAIT_DFLT_MS;
		fallthrough;
	default:
		if (phydev->irq != PHY_POLL)
			return -EOPNOTSUPP;
		if (edpd < PHY_STATE_MACH_MS || edpd > PHY_STATE_MACH_MS + 1000)
			return -EINVAL;
		priv->edpd_enable = true;
		priv->edpd_max_wait_ms = edpd - PHY_STATE_MACH_MS;
	}

	priv->edpd_mode_set_by_user = true;

	return smsc_phy_config_edpd(phydev);
}

int smsc_phy_get_tunable(struct phy_device *phydev,
			 struct ethtool_tunable *tuna, void *data)
{
	switch (tuna->id) {
	case ETHTOOL_PHY_EDPD:
		return smsc_phy_get_edpd(phydev, data);
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(smsc_phy_get_tunable);

int smsc_phy_set_tunable(struct phy_device *phydev,
			 struct ethtool_tunable *tuna, const void *data)
{
	switch (tuna->id) {
	case ETHTOOL_PHY_EDPD:
		return smsc_phy_set_edpd(phydev, *(u16 *)data);
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(smsc_phy_set_tunable);

int smsc_phy_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct smsc_phy_priv *priv;
	struct clk *refclk;

	struct device_node *of_node = dev->of_node;
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->edpd_enable = true;
	priv->edpd_max_wait_ms = EDPD_MAX_WAIT_DFLT_MS;
	priv->energy_enable = true;
	priv->wakeup_enable = false;

	if (device_property_present(dev, "smsc,disable-energy-detect"))
		priv->edpd_enable = false;

	if (of_property_read_bool(of_node, "wakeup-source")) {
		device_set_wakeup_capable(dev, true);
		priv->wakeup_enable = true;
	}

	phydev->priv = priv;

	/* Make clk optional to keep DTB backward compatibility. */
	refclk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(refclk))
		return dev_err_probe(dev, PTR_ERR(refclk),
				     "Failed to request clock\n");

	return clk_set_rate(refclk, 50 * 1000 * 1000);
}
EXPORT_SYMBOL_GPL(smsc_phy_probe);

static struct phy_driver smsc_phy_driver[] = {
{
	.phy_id		= 0x0007c0a0, /* OUI=0x00800f, Model#=0x0a */
	.phy_id_mask	= 0xfffffff0,
	.name		= "SMSC LAN83C185",

	/* PHY_BASIC_FEATURES */

	.probe		= smsc_phy_probe,

	/* basic functions */
	.config_init	= smsc_phy_config_init,
	.soft_reset	= smsc_phy_reset,

	/* IRQ related */
	.config_intr	= smsc_phy_config_intr,
	.handle_interrupt = smsc_phy_handle_interrupt,

	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
}, {
	.phy_id		= 0x0007c0b0, /* OUI=0x00800f, Model#=0x0b */
	.phy_id_mask	= 0xfffffff0,
	.name		= "SMSC LAN8187",

	/* PHY_BASIC_FEATURES */

	.probe		= smsc_phy_probe,

	/* basic functions */
	.config_init	= smsc_phy_config_init,
	.soft_reset	= smsc_phy_reset,

	/* IRQ related */
	.config_intr	= smsc_phy_config_intr,
	.handle_interrupt = smsc_phy_handle_interrupt,

	/* Statistics */
	.get_sset_count = smsc_get_sset_count,
	.get_strings	= smsc_get_strings,
	.get_stats	= smsc_get_stats,

	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
}, {
	/* This covers internal PHY (phy_id: 0x0007C0C3) for
	 * LAN9500 (PID: 0x9500), LAN9514 (PID: 0xec00), LAN9505 (PID: 0x9505)
	 */
	.phy_id		= 0x0007c0c0, /* OUI=0x00800f, Model#=0x0c */
	.phy_id_mask	= 0xfffffff0,
	.name		= "SMSC LAN8700",

	/* PHY_BASIC_FEATURES */

	.probe		= smsc_phy_probe,

	/* basic functions */
	.read_status	= lan87xx_read_status,
	.config_init	= smsc_phy_config_init,
	.soft_reset	= smsc_phy_reset,
	.config_aneg	= lan87xx_config_aneg,

	/* IRQ related */
	.config_intr	= smsc_phy_config_intr,
	.handle_interrupt = smsc_phy_handle_interrupt,

	/* Statistics */
	.get_sset_count = smsc_get_sset_count,
	.get_strings	= smsc_get_strings,
	.get_stats	= smsc_get_stats,

	.get_tunable	= smsc_phy_get_tunable,
	.set_tunable	= smsc_phy_set_tunable,

	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
}, {
	.phy_id		= 0x0007c0d0, /* OUI=0x00800f, Model#=0x0d */
	.phy_id_mask	= 0xfffffff0,
	.name		= "SMSC LAN911x Internal PHY",

	/* PHY_BASIC_FEATURES */

	.probe		= smsc_phy_probe,

	/* IRQ related */
	.config_intr	= smsc_phy_config_intr,
	.handle_interrupt = smsc_phy_handle_interrupt,

	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
}, {
	/* This covers internal PHY (phy_id: 0x0007C0F0) for
	 * LAN9500A (PID: 0x9E00), LAN9505A (PID: 0x9E01)
	 */
	.phy_id		= 0x0007c0f0, /* OUI=0x00800f, Model#=0x0f */
	.phy_id_mask	= 0xfffffff0,
	.name		= "SMSC LAN8710/LAN8720",

	/* PHY_BASIC_FEATURES */

	.probe		= smsc_phy_probe,

	/* basic functions */
	.read_status	= lan87xx_read_status,
	.config_init	= smsc_phy_config_init,
	.soft_reset	= smsc_phy_reset,
	.config_aneg	= lan95xx_config_aneg_ext,

	/* IRQ related */
	.config_intr	= smsc_phy_config_intr,
	.handle_interrupt = smsc_phy_handle_interrupt,

	/* Statistics */
	.get_sset_count = smsc_get_sset_count,
	.get_strings	= smsc_get_strings,
	.get_stats	= smsc_get_stats,

	.get_tunable	= smsc_phy_get_tunable,
	.set_tunable	= smsc_phy_set_tunable,

	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
}, {
	.phy_id		= 0x0007c110,
	.phy_id_mask	= 0xfffffff0,
	.name		= "SMSC LAN8740",

	/* PHY_BASIC_FEATURES */
	.flags		= PHY_RST_AFTER_CLK_EN,

	.probe		= smsc_phy_probe,

	/* basic functions */
	.read_status	= lan87xx_read_status,
	.config_init	= smsc_phy_config_init,
	.soft_reset	= smsc_phy_reset,

	/* IRQ related */
	.config_intr	= smsc_phy_config_intr,
	.handle_interrupt = smsc_phy_handle_interrupt,

	/* Statistics */
	.get_sset_count = smsc_get_sset_count,
	.get_strings	= smsc_get_strings,
	.get_stats	= smsc_get_stats,

	.get_tunable	= smsc_phy_get_tunable,
	.set_tunable	= smsc_phy_set_tunable,

	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
}, {
	.phy_id		= 0x0007c130,	/* 0x0007c130 and 0x0007c131 */
	/* This mask (0xfffffff2) is to differentiate from
	 * LAN88xx (phy_id 0x0007c132)
	 * and allows future phy_id revisions.
	 */
	.phy_id_mask	= 0xfffffff2,
	.name		= "Microchip LAN8742",

	/* PHY_BASIC_FEATURES */
	.flags		= PHY_RST_AFTER_CLK_EN,

	.probe		= smsc_phy_probe,

	/* basic functions */
	.read_status	= lan87xx_read_status,
	.config_init	= smsc_phy_config_init,
	.soft_reset	= smsc_phy_reset,

	/* IRQ related */
	.config_intr	= smsc_phy_config_intr,
	.handle_interrupt = smsc_phy_handle_interrupt,

	/* Statistics */
	.get_sset_count = smsc_get_sset_count,
	.get_strings	= smsc_get_strings,
	.get_stats	= smsc_get_stats,

	.get_tunable	= smsc_phy_get_tunable,
	.set_tunable	= smsc_phy_set_tunable,

	.suspend	= smsc_phy_suspend,
	.resume		= genphy_resume,
} };

module_phy_driver(smsc_phy_driver);

MODULE_DESCRIPTION("SMSC PHY driver");
MODULE_AUTHOR("Herbert Valerio Riedel");
MODULE_LICENSE("GPL");

static struct mdio_device_id __maybe_unused smsc_tbl[] = {
	{ 0x0007c0a0, 0xfffffff0 },
	{ 0x0007c0b0, 0xfffffff0 },
	{ 0x0007c0c0, 0xfffffff0 },
	{ 0x0007c0d0, 0xfffffff0 },
	{ 0x0007c0f0, 0xfffffff0 },
	{ 0x0007c110, 0xfffffff0 },
	{ 0x0007c130, 0xfffffff2 },
	{ }
};

MODULE_DEVICE_TABLE(mdio, smsc_tbl);
