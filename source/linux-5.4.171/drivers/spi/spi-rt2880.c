/*
 * spi-rt2880.c -- Ralink RT288x/RT305x SPI controller driver
 *
 * Copyright (C) 2011 Sergiy <piratfm@gmail.com>
 * Copyright (C) 2011-2013 Gabor Juhos <juhosg@openwrt.org>
 *
 * Some parts are based on spi-orion.c:
 *   Author: Shadi Ammouri <shadi@marvell.com>
 *   Copyright (C) 2007-2008 Marvell Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>

#define DRIVER_NAME			"spi-rt2880"

#define RAMIPS_SPI_STAT			0x00
#define RAMIPS_SPI_CFG			0x10
#define RAMIPS_SPI_CTL			0x14
#define RAMIPS_SPI_DATA			0x20
#define RAMIPS_SPI_ADDR			0x24
#define RAMIPS_SPI_BS			0x28
#define RAMIPS_SPI_USER			0x2C
#define RAMIPS_SPI_TXFIFO		0x30
#define RAMIPS_SPI_RXFIFO		0x34
#define RAMIPS_SPI_FIFO_STAT		0x38
#define RAMIPS_SPI_MODE			0x3C
#define RAMIPS_SPI_DEV_OFFSET		0x40
#define RAMIPS_SPI_DMA			0x80
#define RAMIPS_SPI_DMASTAT		0x84
#define RAMIPS_SPI_ARBITER		0xF0

/* SPISTAT register bit field */
#define SPISTAT_BUSY			BIT(0)

/* SPICFG register bit field */
#define SPICFG_ADDRMODE			BIT(12)
#define SPICFG_RXENVDIS			BIT(11)
#define SPICFG_RXCAP			BIT(10)
#define SPICFG_SPIENMODE		BIT(9)
#define SPICFG_MSBFIRST			BIT(8)
#define SPICFG_SPICLKPOL		BIT(6)
#define SPICFG_RXCLKEDGE_FALLING	BIT(5)
#define SPICFG_TXCLKEDGE_FALLING	BIT(4)
#define SPICFG_HIZSPI			BIT(3)
#define SPICFG_SPICLK_PRESCALE_MASK	0x7
#define SPICFG_SPICLK_DIV2		0
#define SPICFG_SPICLK_DIV4		1
#define SPICFG_SPICLK_DIV8		2
#define SPICFG_SPICLK_DIV16		3
#define SPICFG_SPICLK_DIV32		4
#define SPICFG_SPICLK_DIV64		5
#define SPICFG_SPICLK_DIV128		6
#define SPICFG_SPICLK_DISABLE		7

/* SPICTL register bit field */
#define SPICTL_START			BIT(4)
#define SPICTL_HIZSDO			BIT(3)
#define SPICTL_STARTWR			BIT(2)
#define SPICTL_STARTRD			BIT(1)
#define SPICTL_SPIENA			BIT(0)

/* SPIUSER register bit field */
#define SPIUSER_USERMODE		BIT(21)
#define SPIUSER_INSTR_PHASE		BIT(20)
#define SPIUSER_ADDR_PHASE_MASK		0x7
#define SPIUSER_ADDR_PHASE_OFFSET	17
#define SPIUSER_MODE_PHASE		BIT(16)
#define SPIUSER_DUMMY_PHASE_MASK	0x3
#define SPIUSER_DUMMY_PHASE_OFFSET	14
#define SPIUSER_DATA_PHASE_MASK		0x3
#define SPIUSER_DATA_PHASE_OFFSET	12
#define SPIUSER_DATA_READ		(BIT(0) << SPIUSER_DATA_PHASE_OFFSET)
#define SPIUSER_DATA_WRITE		(BIT(1) << SPIUSER_DATA_PHASE_OFFSET)
#define SPIUSER_ADDR_TYPE_OFFSET	9
#define SPIUSER_MODE_TYPE_OFFSET	6
#define SPIUSER_DUMMY_TYPE_OFFSET	3
#define SPIUSER_DATA_TYPE_OFFSET	0
#define SPIUSER_TRANSFER_MASK		0x7
#define SPIUSER_TRANSFER_SINGLE		BIT(0)
#define SPIUSER_TRANSFER_DUAL		BIT(1)
#define SPIUSER_TRANSFER_QUAD		BIT(2)

#define SPIUSER_TRANSFER_TYPE(type) ( \
	(type << SPIUSER_ADDR_TYPE_OFFSET) | \
	(type << SPIUSER_MODE_TYPE_OFFSET) | \
	(type << SPIUSER_DUMMY_TYPE_OFFSET) | \
	(type << SPIUSER_DATA_TYPE_OFFSET) \
)

/* SPIFIFOSTAT register bit field */
#define SPIFIFOSTAT_TXEMPTY		BIT(19)
#define SPIFIFOSTAT_RXEMPTY		BIT(18)
#define SPIFIFOSTAT_TXFULL		BIT(17)
#define SPIFIFOSTAT_RXFULL		BIT(16)
#define SPIFIFOSTAT_FIFO_MASK		0xff
#define SPIFIFOSTAT_TX_OFFSET		8
#define SPIFIFOSTAT_RX_OFFSET		0

#define SPI_FIFO_DEPTH			16

/* SPIMODE register bit field */
#define SPIMODE_MODE_OFFSET		24
#define SPIMODE_DUMMY_OFFSET		0

/* SPIARB register bit field */
#define SPICTL_ARB_EN			BIT(31)
#define SPICTL_CSCTL1			BIT(16)
#define SPI1_POR			BIT(1)
#define SPI0_POR			BIT(0)

#define RT2880_SPI_MODE_BITS	(SPI_CPOL | SPI_CPHA | SPI_LSB_FIRST | \
		SPI_CS_HIGH)

static atomic_t hw_reset_count = ATOMIC_INIT(0);

struct rt2880_spi {
	struct spi_master	*master;
	void __iomem		*base;
	u32			speed;
	u16			wait_loops;
	u16			mode;
	struct clk		*clk;
};

static inline struct rt2880_spi *spidev_to_rt2880_spi(struct spi_device *spi)
{
	return spi_master_get_devdata(spi->master);
}

static inline u32 rt2880_spi_read(struct rt2880_spi *rs, u32 reg)
{
	return ioread32(rs->base + reg);
}

static inline void rt2880_spi_write(struct rt2880_spi *rs, u32 reg,
		const u32 val)
{
	iowrite32(val, rs->base + reg);
}

static inline void rt2880_spi_setbits(struct rt2880_spi *rs, u32 reg, u32 mask)
{
	void __iomem *addr = rs->base + reg;

	iowrite32((ioread32(addr) | mask), addr);
}

static inline void rt2880_spi_clrbits(struct rt2880_spi *rs, u32 reg, u32 mask)
{
	void __iomem *addr = rs->base + reg;

	iowrite32((ioread32(addr) & ~mask), addr);
}

static u32 rt2880_spi_baudrate_get(struct spi_device *spi, unsigned int speed)
{
	struct rt2880_spi *rs = spidev_to_rt2880_spi(spi);
	u32 rate;
	u32 prescale;

	/*
	 * the supported rates are: 2, 4, 8, ... 128
	 * round up as we look for equal or less speed
	 */
	rate = DIV_ROUND_UP(clk_get_rate(rs->clk), speed);
	rate = roundup_pow_of_two(rate);

	/* Convert the rate to SPI clock divisor value.	*/
	prescale = ilog2(rate / 2);

	/* some tolerance. double and add 100 */
	rs->wait_loops = (8 * HZ * loops_per_jiffy) /
		(clk_get_rate(rs->clk) / rate);
	rs->wait_loops = (rs->wait_loops << 1) + 100;
	rs->speed = speed;

	dev_dbg(&spi->dev, "speed: %lu/%u, rate: %u, prescal: %u, loops: %hu\n",
			clk_get_rate(rs->clk) / rate, speed, rate, prescale,
			rs->wait_loops);

	return prescale;
}

static u32 get_arbiter_offset(struct spi_master *master)
{
	u32 offset;

	offset = RAMIPS_SPI_ARBITER;
	if (master->bus_num == 1)
		offset -= RAMIPS_SPI_DEV_OFFSET;

	return offset;
}

static void rt2880_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct rt2880_spi *rs = spidev_to_rt2880_spi(spi);

	if (enable)
		rt2880_spi_setbits(rs, RAMIPS_SPI_CTL, SPICTL_SPIENA);
	else
		rt2880_spi_clrbits(rs, RAMIPS_SPI_CTL, SPICTL_SPIENA);
}

static int rt2880_spi_wait_ready(struct rt2880_spi *rs, int len)
{
	int loop = rs->wait_loops * len;

	while ((rt2880_spi_read(rs, RAMIPS_SPI_STAT) & SPISTAT_BUSY) && --loop)
		cpu_relax();

	if (loop)
		return 0;

	return -ETIMEDOUT;
}

static void rt2880_dump_reg(struct spi_master *master)
{
	struct rt2880_spi *rs = spi_master_get_devdata(master);

	dev_dbg(&master->dev, "stat: %08x, cfg: %08x, ctl: %08x, " \
			"data: %08x, arb: %08x\n",
			rt2880_spi_read(rs, RAMIPS_SPI_STAT),
			rt2880_spi_read(rs, RAMIPS_SPI_CFG),
			rt2880_spi_read(rs, RAMIPS_SPI_CTL),
			rt2880_spi_read(rs, RAMIPS_SPI_DATA),
			rt2880_spi_read(rs, get_arbiter_offset(master)));
}

static int rt2880_spi_transfer_one(struct spi_master *master,
		struct spi_device *spi, struct spi_transfer *xfer)
{
	struct rt2880_spi *rs = spi_master_get_devdata(master);
	unsigned len;
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	int err = 0;

	/* change clock speed  */
	if (unlikely(rs->speed != xfer->speed_hz)) {
		u32 reg;
		reg = rt2880_spi_read(rs, RAMIPS_SPI_CFG);
		reg &= ~SPICFG_SPICLK_PRESCALE_MASK;
		reg |= rt2880_spi_baudrate_get(spi, xfer->speed_hz);
		rt2880_spi_write(rs, RAMIPS_SPI_CFG, reg);
	}

	if (tx) {
		len = xfer->len;
		while (len-- > 0) {
			rt2880_spi_write(rs, RAMIPS_SPI_DATA, *tx++);
			rt2880_spi_setbits(rs, RAMIPS_SPI_CTL, SPICTL_STARTWR);
			err = rt2880_spi_wait_ready(rs, 1);
			if (err) {
				dev_err(&spi->dev, "TX failed, err=%d\n", err);
				goto out;
			}
		}
	}

	if (rx) {
		len = xfer->len;
		while (len-- > 0) {
			rt2880_spi_setbits(rs, RAMIPS_SPI_CTL, SPICTL_STARTRD);
			err = rt2880_spi_wait_ready(rs, 1);
			if (err) {
				dev_err(&spi->dev, "RX failed, err=%d\n", err);
				goto out;
			}
			*rx++ = (u8) rt2880_spi_read(rs, RAMIPS_SPI_DATA);
		}
	}

out:
	return err;
}

/* copy from spi.c */
static void spi_set_cs(struct spi_device *spi, bool enable)
{
	if (spi->mode & SPI_CS_HIGH)
		enable = !enable;

	if (spi->cs_gpio >= 0)
		gpio_set_value(spi->cs_gpio, !enable);
	else if (spi->master->set_cs)
		spi->master->set_cs(spi, !enable);
}

static int rt2880_spi_setup(struct spi_device *spi)
{
	struct spi_master *master = spi->master;
	struct rt2880_spi *rs = spi_master_get_devdata(master);
	u32 reg, old_reg, arbit_off;

	if ((spi->max_speed_hz > master->max_speed_hz) ||
			(spi->max_speed_hz < master->min_speed_hz)) {
		dev_err(&spi->dev, "invalide requested speed %d Hz\n",
				spi->max_speed_hz);
		return -EINVAL;
	}

	if (!(master->bits_per_word_mask &
				BIT(spi->bits_per_word - 1))) {
		dev_err(&spi->dev, "invalide bits_per_word %d\n",
				spi->bits_per_word);
		return -EINVAL;
	}

	/* the hardware seems can't work on mode0 force it to mode3 */
	if ((spi->mode & (SPI_CPOL | SPI_CPHA)) == SPI_MODE_0) {
		dev_warn(&spi->dev, "force spi mode3\n");
		spi->mode |= SPI_MODE_3;
	}

	/* chip polarity */
	arbit_off = get_arbiter_offset(master);
	reg = old_reg = rt2880_spi_read(rs, arbit_off);
	if (spi->mode & SPI_CS_HIGH) {
		switch (master->bus_num) {
		case 1:
			reg |= SPI1_POR;
			break;
		default:
			reg |= SPI0_POR;
			break;
		}
	} else {
		switch (master->bus_num) {
		case 1:
			reg &= ~SPI1_POR;
			break;
		default:
			reg &= ~SPI0_POR;
			break;
		}
	}

	/* enable spi1 */
	if (master->bus_num == 1)
		reg |= SPICTL_ARB_EN;

	if (reg != old_reg)
		rt2880_spi_write(rs, arbit_off, reg);

	/* deselected the spi device */
	spi_set_cs(spi, false);

	rt2880_dump_reg(master);

	return 0;
}

static int rt2880_spi_prepare_message(struct spi_master *master,
		struct spi_message *msg)
{
	struct rt2880_spi *rs = spi_master_get_devdata(master);
	struct spi_device *spi = msg->spi;
	u32 reg;

	if ((rs->mode == spi->mode) && (rs->speed == spi->max_speed_hz))
		return 0;

#if 0
	/* set spido to tri-state */
	rt2880_spi_setbits(rs, RAMIPS_SPI_CTL, SPICTL_HIZSDO);
#endif

	reg = rt2880_spi_read(rs, RAMIPS_SPI_CFG);

	reg &= ~(SPICFG_MSBFIRST | SPICFG_SPICLKPOL |
			SPICFG_RXCLKEDGE_FALLING |
			SPICFG_TXCLKEDGE_FALLING |
			SPICFG_SPICLK_PRESCALE_MASK);

	/* MSB */
	if (!(spi->mode & SPI_LSB_FIRST))
		reg |= SPICFG_MSBFIRST;

	/* spi mode */
	switch (spi->mode & (SPI_CPOL | SPI_CPHA)) {
	case SPI_MODE_0:
		reg |= SPICFG_TXCLKEDGE_FALLING;
		break;
	case SPI_MODE_1:
		reg |= SPICFG_RXCLKEDGE_FALLING;
		break;
	case SPI_MODE_2:
		reg |= SPICFG_SPICLKPOL | SPICFG_RXCLKEDGE_FALLING;
		break;
	case SPI_MODE_3:
		reg |= SPICFG_SPICLKPOL | SPICFG_TXCLKEDGE_FALLING;
		break;
	}
	rs->mode = spi->mode;

#if 0
	/* set spiclk and spiena to tri-state */
	reg |= SPICFG_HIZSPI;
#endif

	/* clock divide */
	reg |= rt2880_spi_baudrate_get(spi, spi->max_speed_hz);

	rt2880_spi_write(rs, RAMIPS_SPI_CFG, reg);

	return 0;
}

static int rt2880_spi_probe(struct platform_device *pdev)
{
	struct spi_master *master;
	struct rt2880_spi *rs;
	void __iomem *base;
	struct resource *r;
	struct clk *clk;
	int ret;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "unable to get SYS clock\n");
		return PTR_ERR(clk);
	}

	ret = clk_prepare_enable(clk);
	if (ret)
		goto err_clk;

	master = spi_alloc_master(&pdev->dev, sizeof(*rs));
	if (master == NULL) {
		dev_dbg(&pdev->dev, "master allocation failed\n");
		ret = -ENOMEM;
		goto err_clk;
	}

	master->dev.of_node = pdev->dev.of_node;
	master->mode_bits = RT2880_SPI_MODE_BITS;
	master->bits_per_word_mask = SPI_BPW_MASK(8);
	master->min_speed_hz = clk_get_rate(clk) / 128;
	master->max_speed_hz = clk_get_rate(clk) / 2;
	master->flags = SPI_MASTER_HALF_DUPLEX;
	master->setup = rt2880_spi_setup;
	master->prepare_message = rt2880_spi_prepare_message;
	master->set_cs = rt2880_spi_set_cs;
	master->transfer_one = rt2880_spi_transfer_one,

	dev_set_drvdata(&pdev->dev, master);

	rs = spi_master_get_devdata(master);
	rs->master = master;
	rs->base = base;
	rs->clk = clk;

	if (atomic_inc_return(&hw_reset_count) == 1)
		device_reset(&pdev->dev);

	ret = devm_spi_register_master(&pdev->dev, master);
	if (ret < 0) {
		dev_err(&pdev->dev, "devm_spi_register_master error.\n");
		goto err_master;
	}

	return ret;

err_master:
	spi_master_put(master);
	kfree(master);
err_clk:
	clk_disable_unprepare(clk);

	return ret;
}

static int rt2880_spi_remove(struct platform_device *pdev)
{
	struct spi_master *master;
	struct rt2880_spi *rs;

	master = dev_get_drvdata(&pdev->dev);
	rs = spi_master_get_devdata(master);

	clk_disable_unprepare(rs->clk);
	atomic_dec(&hw_reset_count);

	return 0;
}

MODULE_ALIAS("platform:" DRIVER_NAME);

static const struct of_device_id rt2880_spi_match[] = {
	{ .compatible = "ralink,rt2880-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, rt2880_spi_match);

static struct platform_driver rt2880_spi_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = rt2880_spi_match,
	},
	.probe = rt2880_spi_probe,
	.remove = rt2880_spi_remove,
};

module_platform_driver(rt2880_spi_driver);

MODULE_DESCRIPTION("Ralink SPI driver");
MODULE_AUTHOR("Sergiy <piratfm@gmail.com>");
MODULE_AUTHOR("Gabor Juhos <juhosg@openwrt.org>");
MODULE_LICENSE("GPL");
