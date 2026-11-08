// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lantiq VR9 SSC "SPI-TDM" transport driver.
 *
 * Exposes the Lantiq VR9 Synchronous Serial Controller used as a PCM/TDM
 * slave (peer-supplied bit clock and frame sync) through /dev/ssc-pcm0. This
 * is a generic transport: it does not know which device drives the bus.
 *
 * Facts and the bring-up sequence: docs/hardware.md. The register
 * layout matches the mainline spi-lantiq-ssc controller for the same block.
 *
 * Bring-up fallback: the OpenWrt FRITZ!Box 7412 device tree keeps the SSC
 * disabled and describes no TDM user, so nothing binds this driver on such a
 * system. For that case only, the driver instantiates itself from the machine
 * compatible, taking the module clock from the SoC's clkdev entry. The proper
 * upstream path is a device tree node with compatible "lantiq,xrx200-ssc-pcm".
 *
 * Optional "enable" GPIO: a board-level enable for the bus clock (on the
 * FRITZ!Box 7412, GPIO7 gates the peer's PCM bit clock). It is driven high at
 * probe, so it is already asserted when a consumer releases the peer from
 * reset; asserting it later leaves the bus without a bit clock.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "ssc_pcm.h"
#include "ssc_pcm_hw.h"

#define SSC_PCM_DRV_NAME		"lantiq-ssc-pcm"

/* Board facts for the bring-up fallback (docs/cross-model-7520-vs-7412.md). */
#define SSC_PCM_7412_SSC_PHY	0x1e100800
#define SSC_PCM_7412_SSC_SIZE	0x100
#define SSC_PCM_7412_PINMUX_DEV	"1e100b10.pinmux"
#define SSC_PCM_7412_GPIO_CHIP	"gpio-xway"
#define SSC_PCM_7412_CLK_EN_GPIO	7	/* peer PCM clock enable */
/* clkdev name of the SoC's SSC module gate (PMU SPI bit) */
#define SSC_PCM_SSC_CLK_DEV	"1e100800.spi"
/* the SoC's (disabled) SPI-master node for the same block */
#define SSC_PCM_SSC_SPI_COMPAT	"lantiq,xrx200-spi"

static bool use_irq = true;
module_param(use_irq, bool, 0444);
MODULE_PARM_DESC(use_irq, "run the engine from the SSC FIFO interrupts (default) or, if 0, from a timer");

struct lantiq_ssc_pcm {
	struct device *dev;
	struct ssc_pcm *ssc;
	struct mutex lock;
	struct miscdevice misc;
	bool opened;
};

/* ------------------------------------------------------------------ */
/* file operations                                                    */
/* ------------------------------------------------------------------ */
/* There is a single SSC on this SoC, so a single instance pointer is
 * enough; it is set at probe time.
 */
static struct lantiq_ssc_pcm *ssc_pcm_cur;

static int ssc_pcm_dev_open(struct inode *inode, struct file *file)
{
	struct lantiq_ssc_pcm *lp = ssc_pcm_cur;
	int ret = 0;

	if (!lp)
		return -ENODEV;
	if (mutex_lock_interruptible(&lp->lock))
		return -ERESTARTSYS;
	if (lp->opened)
		ret = -EBUSY;
	else {
		lp->opened = true;
		file->private_data = lp;
	}
	mutex_unlock(&lp->lock);
	return ret;
}

static int ssc_pcm_dev_release(struct inode *inode, struct file *file)
{
	struct lantiq_ssc_pcm *lp = file->private_data;

	mutex_lock(&lp->lock);
	if (lp->ssc)
		ssc_pcm_hw_teardown(lp->ssc);
	lp->opened = false;
	mutex_unlock(&lp->lock);
	return 0;
}

static ssize_t ssc_pcm_dev_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct lantiq_ssc_pcm *lp = file->private_data;

	return ssc_pcm_hw_read(lp->ssc, buf, count, file->f_flags & O_NONBLOCK);
}

static ssize_t ssc_pcm_dev_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct lantiq_ssc_pcm *lp = file->private_data;

	return ssc_pcm_hw_write(lp->ssc, buf, count,
				file->f_flags & O_NONBLOCK);
}

static __poll_t ssc_pcm_dev_poll(struct file *file, poll_table *wait)
{
	struct lantiq_ssc_pcm *lp = file->private_data;

	return ssc_pcm_hw_poll(lp->ssc, file, wait);
}

static long ssc_pcm_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lantiq_ssc_pcm *lp = file->private_data;
	struct ssc_pcm_info info;
	long ret;

	switch (cmd) {
	case SSC_PCM_IOC_INFO:
		memset(&info, 0, sizeof(info));
		info.version = SSC_PCM_IOC_VERSION;
		info.step = ssc_pcm_hw_step(lp->ssc);
		info.slots = ssc_pcm_hw_slots(lp->ssc);
		info.frame_bytes = ssc_pcm_hw_frame_bytes(lp->ssc);
		strscpy(info.name, SSC_PCM_DRV_NAME, sizeof(info.name));
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ?
			-EFAULT : 0;
	case SSC_PCM_IOC_CONFIG:
	case SSC_PCM_IOC_STEP:
	case SSC_PCM_IOC_REGS:
	case SSC_PCM_IOC_STATS:
	case SSC_PCM_IOC_TX_IDLE:
		if (mutex_lock_interruptible(&lp->lock))
			return -ERESTARTSYS;
		ret = ssc_pcm_hw_ioctl(lp->ssc, cmd, (void __user *)arg);
		mutex_unlock(&lp->lock);
		return ret;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ssc_pcm_fops = {
	.owner		= THIS_MODULE,
	.open		= ssc_pcm_dev_open,
	.release	= ssc_pcm_dev_release,
	.read		= ssc_pcm_dev_read,
	.write		= ssc_pcm_dev_write,
	.poll		= ssc_pcm_dev_poll,
	.unlocked_ioctl	= ssc_pcm_dev_ioctl,
	.llseek		= noop_llseek,
};

/* ------------------------------------------------------------------ */
/* probe / remove                                                     */
/* ------------------------------------------------------------------ */
static void ssc_pcm_clk_off(void *clk)
{
	clk_disable_unprepare(clk);
}

static int ssc_pcm_probe(struct platform_device *pdev)
{
	struct lantiq_ssc_pcm *lp;
	void __iomem *base;
	struct gpio_desc *en;
	struct clk *clk;
	int ret;

	lp = devm_kzalloc(&pdev->dev, sizeof(*lp), GFP_KERNEL);
	if (!lp)
		return -ENOMEM;

	lp->dev = &pdev->dev;
	mutex_init(&lp->lock);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* the optional DMA engine allocates its rings for this device */
	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/* Module clock. With a DT node named spi@e100800 the xway clkdev entry
	 * resolves it; the bring-up fallback enables it itself.
	 */
	clk = devm_clk_get_optional(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk),
				     "module clock\n");
	if (clk) {
		/* Not devm_clk_get_optional_enabled(): the xway legacy
		 * clk_enable() fails on a NULL clock.
		 */
		ret = clk_prepare_enable(clk);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "module clock enable\n");
		ret = devm_add_action_or_reset(&pdev->dev, ssc_pcm_clk_off,
					       clk);
		if (ret)
			return ret;
	} else if (pdev->dev.of_node) {
		dev_warn(&pdev->dev, "no module clock, SSC may be gated\n");
	}

	en = devm_gpiod_get_optional(&pdev->dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(en))
		return dev_err_probe(&pdev->dev, PTR_ERR(en),
				     "enable gpio\n");

	/* RX and TX FIFO interrupts, in that order; without them the engine
	 * falls back to a timer.
	 */
	lp->ssc = ssc_pcm_hw_create(&pdev->dev, base,
				    use_irq ? platform_get_irq_optional(pdev, 0) : 0,
				    use_irq ? platform_get_irq_optional(pdev, 1) : 0);
	if (IS_ERR(lp->ssc))
		return PTR_ERR(lp->ssc);

	lp->misc.name = "ssc-pcm0";
	lp->misc.minor = MISC_DYNAMIC_MINOR;
	lp->misc.fops = &ssc_pcm_fops;
	lp->misc.mode = 0600;
	lp->misc.parent = &pdev->dev;
	ret = misc_register(&lp->misc);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, lp);
	ssc_pcm_cur = lp;
	dev_info(&pdev->dev, "VR9 SSC PCM/TDM transport ready\n");
	return 0;
}

static void ssc_pcm_remove(struct platform_device *pdev)
{
	struct lantiq_ssc_pcm *lp = platform_get_drvdata(pdev);

	ssc_pcm_cur = NULL;
	misc_deregister(&lp->misc);
	if (lp->ssc)
		ssc_pcm_hw_teardown(lp->ssc);
}

static const struct of_device_id ssc_pcm_of_match[] = {
	{ .compatible = "lantiq,xrx200-ssc-pcm" },
	{ .compatible = "lantiq,vr9-ssc-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, ssc_pcm_of_match);

static struct platform_driver ssc_pcm_driver = {
	.probe		= ssc_pcm_probe,
	.remove		= ssc_pcm_remove,
	.driver		= {
		.name		= SSC_PCM_DRV_NAME,
		.of_match_table	= ssc_pcm_of_match,
		/*
		 * No unbind through sysfs: the instance is devm-allocated, and
		 * an fd still open on /dev/ssc-pcm0 would outlive it.
		 */
		.suppress_bind_attrs = true,
	},
};

/* ------------------------------------------------------------------ */
/* Bring-up fallback (no device tree node)                            */
/* ------------------------------------------------------------------ */
/*
 * pinctrl-xway pad config "output" (param 4, packed as param << 16 | arg),
 * as in the lantiq,output DT property. The mux alone leaves the pad
 * direction as it was.
 */
#define SSC_PCM_XWAY_OUTPUT(on)	((4UL << 16) | (on))

static unsigned long ssc_pcm_pad_out[] = { SSC_PCM_XWAY_OUTPUT(1) };
static unsigned long ssc_pcm_pad_in[] = { SSC_PCM_XWAY_OUTPUT(0) };

/*
 * pinctrl-xway names the pins for master mode. As a slave the SSC
 * transmits on spi_di (GPIO16) and receives on spi_do (GPIO17); the peer
 * drives the bit clock (GPIO18). GPIO10 carries the SSC's chip select 4;
 * the stock firmware muxes it the same way and makes the pad an output,
 * so we do too (docs/hardware.md, section 5).
 */
static const struct pinctrl_map ssc_pcm_fallback_pinmux[] = {
	PIN_MAP_MUX_GROUP(SSC_PCM_DRV_NAME, "default", SSC_PCM_7412_PINMUX_DEV,
			  "spi_di", "spi"),
	PIN_MAP_MUX_GROUP(SSC_PCM_DRV_NAME, "default", SSC_PCM_7412_PINMUX_DEV,
			  "spi_do", "spi"),
	PIN_MAP_MUX_GROUP(SSC_PCM_DRV_NAME, "default", SSC_PCM_7412_PINMUX_DEV,
			  "spi_clk", "spi"),
	PIN_MAP_MUX_GROUP(SSC_PCM_DRV_NAME, "default", SSC_PCM_7412_PINMUX_DEV,
			  "spi_cs4", "spi"),
	PIN_MAP_CONFIGS_PIN(SSC_PCM_DRV_NAME, "default",
			    SSC_PCM_7412_PINMUX_DEV, "io16", ssc_pcm_pad_out),
	PIN_MAP_CONFIGS_PIN(SSC_PCM_DRV_NAME, "default",
			    SSC_PCM_7412_PINMUX_DEV, "io17", ssc_pcm_pad_in),
	PIN_MAP_CONFIGS_PIN(SSC_PCM_DRV_NAME, "default",
			    SSC_PCM_7412_PINMUX_DEV, "io18", ssc_pcm_pad_in),
	PIN_MAP_CONFIGS_PIN(SSC_PCM_DRV_NAME, "default",
			    SSC_PCM_7412_PINMUX_DEV, "io10", ssc_pcm_pad_out),
};

static struct gpiod_lookup_table ssc_pcm_fallback_gpios = {
	.dev_id = SSC_PCM_DRV_NAME,
	.table = {
		GPIO_LOOKUP(SSC_PCM_7412_GPIO_CHIP, SSC_PCM_7412_CLK_EN_GPIO,
			    "enable", GPIO_ACTIVE_HIGH),
		{ }
	},
};

static struct platform_device *ssc_pcm_fallback_pdev;
static struct clk *ssc_pcm_fallback_clk;
static bool ssc_pcm_fallback_maps;

static int __init ssc_pcm_fallback_init(void)
{
	struct resource res = {
		.flags	= IORESOURCE_MEM,
		.start	= SSC_PCM_7412_SSC_PHY,
		.end	= SSC_PCM_7412_SSC_PHY + SSC_PCM_7412_SSC_SIZE - 1,
	};
	struct resource irqs[2] = { };
	struct device_node *np;
	unsigned int nres = 1, i;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "lantiq,xrx200-ssc-pcm");
	if (np) {
		of_node_put(np);
		return 0;	/* described by the device tree */
	}
	if (!of_machine_is_compatible("avm,fritz7412"))
		return 0;

	/* The platform device below is not named after the SSC, so take the
	 * module gate by its clkdev name instead of in probe.
	 */
	ssc_pcm_fallback_clk = clk_get_sys(SSC_PCM_SSC_CLK_DEV, NULL);
	if (IS_ERR(ssc_pcm_fallback_clk)) {
		ret = PTR_ERR(ssc_pcm_fallback_clk);
		pr_err(SSC_PCM_DRV_NAME ": no SSC module clock: %d\n", ret);
		ssc_pcm_fallback_clk = NULL;
		return ret;
	}
	ret = clk_prepare_enable(ssc_pcm_fallback_clk);
	if (ret) {
		pr_err(SSC_PCM_DRV_NAME ": SSC module clock enable: %d\n", ret);
		goto err_clk;
	}

	ret = pinctrl_register_mappings(ssc_pcm_fallback_pinmux,
					ARRAY_SIZE(ssc_pcm_fallback_pinmux));
	if (ret) {
		pr_err(SSC_PCM_DRV_NAME ": pinctrl mapping failed: %d\n", ret);
		goto err_disable;
	}
	gpiod_add_lookup_table(&ssc_pcm_fallback_gpios);
	ssc_pcm_fallback_maps = true;

	/* The disabled SPI-master node still describes the SSC interrupts. */
	np = of_find_compatible_node(NULL, NULL, SSC_PCM_SSC_SPI_COMPAT);
	for (i = 0; np && i < 2; i++) {
		int irq = irq_of_parse_and_map(np, i);

		if (irq <= 0)
			break;
		irqs[i] = (struct resource)DEFINE_RES_IRQ(irq);
		nres++;
	}
	of_node_put(np);

	{
		struct resource all[3] = { res, irqs[0], irqs[1] };

		ssc_pcm_fallback_pdev =
			platform_device_register_simple(SSC_PCM_DRV_NAME, -1,
							all, nres);
	}
	if (IS_ERR(ssc_pcm_fallback_pdev)) {
		ret = PTR_ERR(ssc_pcm_fallback_pdev);
		ssc_pcm_fallback_pdev = NULL;
		goto err_maps;
	}
	pr_info(SSC_PCM_DRV_NAME ": bring-up fallback device registered\n");
	return 0;

err_maps:
	gpiod_remove_lookup_table(&ssc_pcm_fallback_gpios);
	pinctrl_unregister_mappings(ssc_pcm_fallback_pinmux);
	ssc_pcm_fallback_maps = false;
err_disable:
	clk_disable_unprepare(ssc_pcm_fallback_clk);
err_clk:
	clk_put(ssc_pcm_fallback_clk);
	ssc_pcm_fallback_clk = NULL;
	return ret;
}

static void ssc_pcm_fallback_exit(void)
{
	if (ssc_pcm_fallback_pdev) {
		platform_device_unregister(ssc_pcm_fallback_pdev);
		ssc_pcm_fallback_pdev = NULL;
	}
	if (ssc_pcm_fallback_maps) {
		gpiod_remove_lookup_table(&ssc_pcm_fallback_gpios);
		pinctrl_unregister_mappings(ssc_pcm_fallback_pinmux);
		ssc_pcm_fallback_maps = false;
	}
	if (ssc_pcm_fallback_clk) {
		clk_disable_unprepare(ssc_pcm_fallback_clk);
		clk_put(ssc_pcm_fallback_clk);
		ssc_pcm_fallback_clk = NULL;
	}
}

static int __init ssc_pcm_init(void)
{
	int ret = platform_driver_register(&ssc_pcm_driver);

	if (ret)
		return ret;
	ret = ssc_pcm_fallback_init();
	if (ret)
		pr_warn(SSC_PCM_DRV_NAME ": bring-up fallback failed: %d\n",
			ret);
	return 0;
}

static void __exit ssc_pcm_exit(void)
{
	ssc_pcm_fallback_exit();
	platform_driver_unregister(&ssc_pcm_driver);
}

module_init(ssc_pcm_init);
module_exit(ssc_pcm_exit);

MODULE_AUTHOR("FritzBox 7412 open DECT project");
MODULE_DESCRIPTION("Lantiq VR9 SSC PCM/TDM (\"SPI-TDM\") transport");
MODULE_LICENSE("GPL");
