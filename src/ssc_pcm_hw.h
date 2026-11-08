/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Lantiq VR9 SSC "SPI-TDM" transport: controller engine.
 *
 * Facts and the register map: docs/hardware.md. The register
 * layout matches the mainline spi-lantiq-ssc controller for the same block.
 */
#ifndef _SSC_PCM_SSC_H
#define _SSC_PCM_SSC_H

#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "ssc_pcm.h"

/* VR9 SSC register map (physical base 0x1E100800 on the 7412). */
#define SSC_CLC		0x00	/* clock control */
#define SSC_PISEL	0x04	/* port input select */
#define SSC_ID		0x08	/* module / FIFO identification */
#define SSC_CON		0x10	/* control */
#define SSC_STATE	0x14	/* status */
#define SSC_WHBSTATE	0x18	/* write-modified control */
#define SSC_TB		0x20	/* transmit buffer */
#define SSC_RB		0x24	/* receive buffer */
#define SSC_RXFCON	0x30	/* RX FIFO control */
#define SSC_TXFCON	0x34	/* TX FIFO control */
#define SSC_FSTAT	0x38	/* FIFO status */
#define SSC_BR		0x40	/* baud-rate reload */
#define SSC_BRSTAT	0x44	/* baud-rate status */
#define SSC_SFCON	0x60	/* serial framing control */
#define SSC_SFSTAT	0x64	/* serial framing status */
#define SSC_GPOCON	0x70
#define SSC_GPOSTAT	0x74
#define SSC_WHBGPOSTAT	0x78
#define SSC_RXREQ	0x80
#define SSC_RXCNT	0x84
#define SSC_DMACON	0xec
#define SSC_IRN_EN	0xf4	/* interrupt node enable */
#define SSC_IRN_CR	0xf8	/* interrupt capture, written back to ack */

#define SSC_CLC_RMC_S	8
#define SSC_CLC_RMC_M	(0xff << SSC_CLC_RMC_S)
#define SSC_CLC_WR_PROTECT_DIS	BIT(4)

#define SSC_ID_RXFS_S	16
#define SSC_ID_TXFS_S	24

#define SSC_CON_TX_OFF		BIT(0)
#define SSC_CON_RX_OFF		BIT(1)
#define SSC_CON_MSB_FIRST	BIT(4)
#define SSC_CON_PH		BIT(5)
#define SSC_CON_PO		BIT(6)
#define SSC_CON_DATA_WIDTH_S	16
#define SSC_CON_ENABLE_BYTE_VALID BIT(22)
#define SSC_CON_WIDTH(w)	((((w) - 1) & 0x1f) << SSC_CON_DATA_WIDTH_S)

#define SSC_STATE_ENABLED	BIT(0)
#define SSC_STATE_SLAVE		BIT(2)
#define SSC_STATE_TX_OFL	BIT(8)
#define SSC_STATE_RX_OFL	BIT(9)
#define SSC_STATE_TX_UFL	BIT(11)
#define SSC_STATE_RX_UFL	BIT(12)
#define SSC_STATE_BUSY		BIT(13)

#define SSC_WHBSTATE_DISABLE	BIT(0)	/* -> configuration mode */
#define SSC_WHBSTATE_ENABLE	BIT(1)	/* -> run mode */
#define SSC_WHBSTATE_SLAVE_MODE	BIT(2)
#define SSC_WHBSTATE_MASTER_MODE BIT(3)
#define SSC_WHBSTATE_CLR_ALL_ERR 0x00000f50
/* error clear strobes sit at the same bit as the STATE flag they clear */
#define SSC_WHBSTATE_CLR_RX_OFL	BIT(9)
#define SSC_WHBSTATE_CLR_TX_UFL	BIT(11)

/* FIFO interrupt requests (observed on the 7412: bit 0 raises ICU line 23,
 * "spi_tx"; bit 1 raises line 22, "spi_rx").
 */
#define SSC_IRN_TX		BIT(0)
#define SSC_IRN_RX		BIT(1)

#define SSC_XFCON_ENABLE	BIT(0)
#define SSC_XFCON_FLUSH		BIT(1)
#define SSC_XFCON_ITL_S		8

#define SSC_FSTAT_RX_WORDS_M	0x3f
#define SSC_FSTAT_TX_WORDS_S	8
#define SSC_FSTAT_TX_WORDS_M	(0x3f << SSC_FSTAT_TX_WORDS_S)

#define SSC_MAX_FRAME_WORDS	32

struct ssc_pcm;

struct ssc_pcm *ssc_pcm_hw_create(struct device *dev, void __iomem *base,
				  int rx_irq, int tx_irq);
void ssc_pcm_hw_destroy(struct ssc_pcm *s);
void ssc_pcm_hw_teardown(struct ssc_pcm *s);

long ssc_pcm_hw_ioctl(struct ssc_pcm *s, unsigned int cmd, void __user *argp);
ssize_t ssc_pcm_hw_read(struct ssc_pcm *s, char __user *buf, size_t count,
			bool nonblock);
ssize_t ssc_pcm_hw_write(struct ssc_pcm *s, const char __user *buf,
			 size_t count, bool nonblock);
__poll_t ssc_pcm_hw_poll(struct ssc_pcm *s, struct file *file,
			 poll_table *wait);

/* Applied PCM configuration, for SSC_PCM_IOC_INFO. */
unsigned int ssc_pcm_hw_step(struct ssc_pcm *s);
unsigned int ssc_pcm_hw_slots(struct ssc_pcm *s);
unsigned int ssc_pcm_hw_frame_bytes(struct ssc_pcm *s);

#endif /* _SSC_PCM_SSC_H */
