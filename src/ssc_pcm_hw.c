// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lantiq VR9 SSC "SPI-TDM" transport: controller engine.
 *
 * Steps 1 (CLOCK) and 3 (TDM) program the SSC; step 2 (IFACE) is handled by
 * pinctrl. Step 4 (DMA) starts the engine. By default it is the VR9 central
 * DMA with one descriptor ring per direction, one frame per descriptor,
 * collected and re-armed from a 1 ms timer; it carries the 7412's 18-word
 * frames without loss. Without DMA (use_dma=0, or frames that are not a
 * multiple of 8 bytes) the engine runs from the SSC's own FIFO interrupts
 * ([CR A-0002]) or, failing those, an 8 kHz hrtimer; both lose words on a
 * fast bus. Every engine packs whole frames into blocks. Only words that
 * were really received count; a block is queued after frames_per_block
 * complete frames, never on a timer tick alone. Frames are not yet aligned
 * to the peer's frame sync, so the first frame may start mid-frame.
 *
 * Transmit mirrors this: write() queues whole frames, and the engine keeps
 * the TX FIFO topped up word by word from the queue, falling back to the
 * idle frame when it is empty, so the slave always has a word to shift. The
 * intended production path is a DMA ring . Do not treat as
 * production ready.
 *
 * Register access: the SoC is big-endian MIPS and the SSC registers are
 * native-endian, so all accesses use __raw_readl()/__raw_writel() on the
 * ioremap()ed base. readl()/writel() byte-swap on this target and silently
 * program the wrong bits (docs/7412-hardware-bringup.md).
 * Only ever access the documented 0x100-byte block: touching unmapped FPI
 * space raises a bus error that panics the kernel.
 */

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#include "ssc_pcm_hw.h"

#include <xway_dma.h>

#define SSC_PCM_DEFAULT_FRAMES_PER_BLOCK	32
#define SSC_PCM_DEFAULT_N_DESC		4
#define SSC_PCM_MAX_DESC			16
#define SSC_PCM_DEFAULT_SLOTS		8
#define SSC_PCM_FRAME_HZ			8000
#define SSC_PCM_TX_QUEUE_FRAMES		256	/* 32 ms at 8 kHz */

/*
 * The engine's timers: one per CPU on the DMA engine (ssc_start_timers()).
 */
#define SSC_TIMERS	2	/* the VR9 has two */

struct ssc_pcm;

struct ssc_pcm_timer {
	struct hrtimer t;
	ktime_t first;		/* delay of the first expiry */
	struct ssc_pcm *s;
	u64 last_ns;		/* this timer's last run */
};

struct ssc_pcm {
	struct device *dev;
	void __iomem *base;
	struct mutex *lock;

	unsigned int step;
	unsigned int slots;
	unsigned int frame_words;	/* 32-bit words per frame */
	unsigned int frame_bytes;
	unsigned int frames_per_block;
	unsigned int n_desc;
	unsigned int block_bytes;
	unsigned int tx_fifo_words;	/* SSC TX FIFO depth */
	unsigned int rx_fifo_words;	/* SSC RX FIFO depth */

	/*
	 * Receive ring: n_desc slots of block_bytes, each with its own header.
	 * ring_lock covers the ring, the descriptors and the counters; the
	 * engine takes it from the hrtimer (hard irq context).
	 */
	spinlock_t ring_lock;
	wait_queue_head_t wq;
	u8 *ring;
	struct ssc_pcm_rxdesc *desc;
	unsigned int head, tail;
	u32 seq;		/* blocks completed */
	u64 frames;		/* complete frames received */
	u64 dropped;		/* blocks overwritten before they were read */

	/* Reader side: one block is copied out of the ring under ring_lock */
	struct mutex read_lock;
	u8 *read_buf;

	/* Engine: SSC FIFO interrupts, or an hrtimer when there are none */
	int irq[2];			/* RX, TX; 0 if absent */
	spinlock_t engine_lock;		/* the two lines may fire on two CPUs */
	u32 overruns, tx_underruns;
	u32 irqs[2], polls;
	/*
	 * DMA engine: a ring of LTQ_DESC_NUM descriptors per direction, one
	 * frame each, re-armed from the 1 ms timer.
	 */
	bool dma;
	struct ltq_dma_channel rxch, txch;
	/*
	 * Ring health: the SSC's overflow flag does not report lost words
	 * (docs/7412-hardware-bringup.md), so a ring that ran full is the
	 * only sign of them. The ring holds LTQ_DESC_NUM frames; a run that
	 * finds every descriptor done, or comes later than that, may have
	 * let the FIFO overflow and shifted the frame by the lost words.
	 */
	u64 dma_last_ns;		/* last ring collection */
	u64 dma_max_gap_ns;		/* longest time between two */
	u32 rx_ring_full, tx_ring_empty;
	u8 *rxbuf, *txbuf;
	dma_addr_t rxbuf_phys, txbuf_phys;
	unsigned long storm_jiffy;	/* interrupt storm guard */
	unsigned int storm_count;
	bool storm;
	struct ssc_pcm_timer timer[SSC_TIMERS];
	unsigned int n_timers;
	ktime_t period;
	unsigned int word_fill;		/* words of the frame in progress */
	unsigned int block_fill;	/* complete frames in block_buf */
	u8 *block_buf;
	bool running;

	/*
	 * Transmit queue of whole frames. Single producer (write(), under
	 * write_lock) and single consumer (the engine), so the kfifo needs no
	 * further locking. tx_cur is the frame being shifted out.
	 */
	struct mutex write_lock;
	wait_queue_head_t tx_wq;
	struct kfifo txq;
	u8 tx_idle[SSC_PCM_MAX_FRAME_BYTES];
	u8 tx_cur[SSC_PCM_MAX_FRAME_BYTES];
	unsigned int tx_word;		/* next word of tx_cur to send */
	bool tx_cur_idle;
	u64 tx_frames;
	u64 tx_idle_frames;
};

struct ssc_pcm_rxdesc {
	u32 seq;
	u32 flags;
	u64 ts_ns;
};

static inline u32 ssc_rd(struct ssc_pcm *s, u32 off)
{
	u32 v = __raw_readl(s->base + off);

	/* the register read completes before whatever depends on it */
	rmb();
	return v;
}

static inline void ssc_wr(struct ssc_pcm *s, u32 off, u32 val)
{
	/* earlier memory writes are visible before the register write */
	wmb();
	__raw_writel(val, s->base + off);
}

static int ssc_set_run(struct ssc_pcm *s, bool run, unsigned int timeout_us)
{
	unsigned int waited = 0;

	/* WHBSTATE is write-modified and reads back as zero, so never
	 * read-modify-write it: write exactly the command bit.
	 */
	ssc_wr(s, SSC_WHBSTATE, run ? SSC_WHBSTATE_ENABLE :
	       SSC_WHBSTATE_DISABLE);
	/* the command reaches the SSC before STATE is polled */
	wmb();

	while (!!(ssc_rd(s, SSC_STATE) & SSC_STATE_ENABLED) != run) {
		if (waited >= timeout_us)
			return -ETIMEDOUT;
		udelay(100);
		waited += 100;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Steps                                                              */
/* ------------------------------------------------------------------ */
static int ssc_step_clock(struct ssc_pcm *s)
{
	u32 id, clc;
	unsigned int txfs;

	ssc_wr(s, SSC_CLC, 1 << SSC_CLC_RMC_S);
	id = ssc_rd(s, SSC_ID);
	clc = ssc_rd(s, SSC_CLC);
	dev_info(s->dev, "SSC id=0x%08x clc=0x%08x (rx_fifo=%u tx_fifo=%u)\n",
		 id, clc, (id >> SSC_ID_RXFS_S) & 0x3f,
		 (id >> SSC_ID_TXFS_S) & 0x3f);
	if (!id || id == 0xffffffff)
		return -ENODEV;
	txfs = (id >> SSC_ID_TXFS_S) & 0x3f;
	s->tx_fifo_words = txfs ?: 1;
	s->rx_fifo_words = ((id >> SSC_ID_RXFS_S) & 0x3f) ?: 1;
	/* RMC must read back, otherwise the module clock is not running. */
	if ((clc & SSC_CLC_RMC_M) != (1 << SSC_CLC_RMC_S)) {
		dev_err(s->dev, "CLC write did not land (clc=0x%08x)\n", clc);
		return -EIO;
	}
	return 0;
}

static uint rx_itl;
module_param(rx_itl, uint, 0644);
MODULE_PARM_DESC(rx_itl, "RX FIFO request level in words (0 = half the FIFO)");

/*
 * Central DMA (VR9 port 2, RX channel 12, TX channel 13, 2-word bursts,
 * [CR A-0006]). Stock never uses it; the FIFO engines lose words once the
 * peer's frame exceeds what an interrupt can drain in time (18 words per
 * 125 us on the 7412).
 */
static bool use_dma = true;
module_param(use_dma, bool, 0644);
MODULE_PARM_DESC(use_dma, "move SSC data with the central DMA (default; frames that are not a multiple of 8 bytes use the interrupt engine)");

static uint dma_swap;
module_param(dma_swap, uint, 0644);
MODULE_PARM_DESC(dma_swap, "byte order of DMA words in memory: 0 as read from the FIFO, 1 reversed");

#define SSC_DMA_PORT		2
#define SSC_DMA_RX_CH		12
#define SSC_DMA_TX_CH		13
#define SSC_DMA_BURST		2	/* words */
#define SSC_DMACON_DMAON	0x3	/* RX and TX requests */

static int ssc_step_tdm(struct ssc_pcm *s)
{
	/* RX request at half the FIFO, so the engine has half a FIFO of
	 * margin; a 7412 frame (8 words) would otherwise fill it entirely.
	 */
	u32 itl = clamp_t(u32, rx_itl ?: s->rx_fifo_words / 2, 1, 63);
	bool dma;

	if (ssc_set_run(s, false, 100000))
		dev_warn(s->dev, "SSC did not enter configuration mode\n");

	ssc_wr(s, SSC_WHBSTATE,
	       SSC_WHBSTATE_SLAVE_MODE | SSC_WHBSTATE_CLR_ALL_ERR);
	ssc_wr(s, SSC_BR, 1);

	ssc_wr(s, SSC_RXFCON,
	       SSC_XFCON_ENABLE | SSC_XFCON_FLUSH | (itl << SSC_XFCON_ITL_S));
	/* DMA wants room for a burst plus margin, [CR A-0006]: level 4 */
	dma = use_dma && !(s->frame_bytes % (SSC_DMA_BURST * 4));
	ssc_wr(s, SSC_TXFCON,
	       SSC_XFCON_ENABLE | SSC_XFCON_FLUSH |
	       ((dma ? s->tx_fifo_words / 2 : 1) << SSC_XFCON_ITL_S));

	ssc_wr(s, SSC_CON,
	       SSC_CON_WIDTH(32) | SSC_CON_MSB_FIRST | SSC_CON_PO |
	       SSC_CON_ENABLE_BYTE_VALID);
	ssc_wr(s, SSC_IRN_EN, 0);

	dev_info(s->dev,
		 "configured: CON=0x%08x RXFCON=0x%08x TXFCON=0x%08x STATE=0x%08x\n",
		 ssc_rd(s, SSC_CON), ssc_rd(s, SSC_RXFCON),
		 ssc_rd(s, SSC_TXFCON), ssc_rd(s, SSC_STATE));

	if (ssc_set_run(s, true, 100000)) {
		dev_err(s->dev, "SSC did not enter run mode (STATE=0x%08x)\n",
			ssc_rd(s, SSC_STATE));
		return -ETIMEDOUT;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Receive engine (placeholder)                                       */
/* ------------------------------------------------------------------ */
/* Queue block_buf as the newest block. Called with ring_lock held. */
static void ssc_enqueue_block(struct ssc_pcm *s)
{
	struct ssc_pcm_rxdesc *d = &s->desc[s->head];
	unsigned int next = (s->head + 1) % s->n_desc;

	memcpy(s->ring + (size_t)s->head * s->block_bytes, s->block_buf,
	       s->block_bytes);
	d->seq = s->seq++;
	d->flags = 0;
	d->ts_ns = ktime_get_ns();

	if (next == s->tail) {
		/* Full: drop the oldest block and flag the gap on the next. */
		s->tail = (s->tail + 1) % s->n_desc;
		s->desc[s->tail].flags |= SSC_PCM_BF_DROPPED;
		s->dropped++;
	}
	s->head = next;
}

/*
 * Move every word the RX FIFO holds into the frame in progress, re-reading
 * the fill level until it is empty: on a fast bus (the 7412 delivers 18
 * words per 125 us into an 8-word FIFO) words keep arriving while we drain.
 * Frames and blocks only advance on words actually read, so a silent bus
 * produces no blocks. Returns true if a block was queued.
 */
static bool ssc_rx_word(struct ssc_pcm *s, u32 v)
{
	put_unaligned_be32(v, s->block_buf +
			   (size_t)s->block_fill * s->frame_bytes +
			   s->word_fill * 4);
	if (++s->word_fill < s->frame_words)
		return false;
	s->word_fill = 0;
	s->frames++;
	if (++s->block_fill < s->frames_per_block)
		return false;
	s->block_fill = 0;
	ssc_enqueue_block(s);
	return true;
}

static bool ssc_rx_poll(struct ssc_pcm *s)
{
	unsigned int n = ssc_rd(s, SSC_FSTAT) & SSC_FSTAT_RX_WORDS_M;
	unsigned int budget = 4 * SSC_MAX_FRAME_WORDS;
	bool queued = false;

	spin_lock(&s->ring_lock);
	while (n && budget--) {
		queued |= ssc_rx_word(s, ssc_rd(s, SSC_RB));
		if (!--n)
			n = ssc_rd(s, SSC_FSTAT) & SSC_FSTAT_RX_WORDS_M;
	}
	spin_unlock(&s->ring_lock);
	return queued;
}

/*
 * Top the TX FIFO up, one word at a time from the current frame. A new
 * frame is taken from the queue, or the idle frame when the queue is empty.
 * Returns true if a queued frame was taken (queue space freed).
 */
static bool ssc_tx_fill(struct ssc_pcm *s)
{
	u32 fstat = ssc_rd(s, SSC_FSTAT);
	unsigned int used = (fstat & SSC_FSTAT_TX_WORDS_M) >>
			    SSC_FSTAT_TX_WORDS_S;
	unsigned int room = used < s->tx_fifo_words ?
			    s->tx_fifo_words - used : 0;
	bool took = false;

	while (room--) {
		if (s->tx_word == 0) {
			s->tx_cur_idle = kfifo_out(&s->txq, s->tx_cur,
						   s->frame_bytes) !=
					 s->frame_bytes;
			if (s->tx_cur_idle)
				memcpy(s->tx_cur, s->tx_idle, s->frame_bytes);
			else
				took = true;
		}
		ssc_wr(s, SSC_TB, get_unaligned_be32(s->tx_cur +
						     s->tx_word * 4));
		if (++s->tx_word < s->frame_words)
			continue;
		s->tx_word = 0;
		spin_lock(&s->ring_lock);
		if (s->tx_cur_idle)
			s->tx_idle_frames++;
		else
			s->tx_frames++;
		spin_unlock(&s->ring_lock);
	}
	return took;
}

/* Count and clear FIFO errors; each one shifts the frame by a word. */
static void ssc_check_errors(struct ssc_pcm *s)
{
	u32 st = ssc_rd(s, SSC_STATE), clr = 0;

	if (st & SSC_STATE_RX_OFL) {
		s->overruns++;
		clr |= SSC_WHBSTATE_CLR_RX_OFL;
	}
	if (st & SSC_STATE_TX_UFL) {
		s->tx_underruns++;
		clr |= SSC_WHBSTATE_CLR_TX_UFL;
	}
	if (clr)
		ssc_wr(s, SSC_WHBSTATE, clr);
}

static u32 ssc_dma_word(const u8 *p)
{
	return dma_swap ? get_unaligned_le32(p) : get_unaligned_be32(p);
}

static void ssc_dma_put_word(u8 *p, u32 v)
{
	if (dma_swap)
		put_unaligned_le32(v, p);
	else
		put_unaligned_be32(v, p);
}

/* Fill TX slot i with the next queued frame, or the idle frame. */
static bool ssc_dma_tx_frame(struct ssc_pcm *s, unsigned int i)
{
	u8 *dst = s->txbuf + (size_t)i * s->frame_bytes;
	unsigned int w;
	bool took;

	took = kfifo_out(&s->txq, s->tx_cur, s->frame_bytes) == s->frame_bytes;
	if (!took)
		memcpy(s->tx_cur, s->tx_idle, s->frame_bytes);
	for (w = 0; w < s->frame_words; w++)
		ssc_dma_put_word(dst + w * 4,
				 get_unaligned_be32(s->tx_cur + w * 4));
	spin_lock(&s->ring_lock);
	if (took)
		s->tx_frames++;
	else
		s->tx_idle_frames++;
	spin_unlock(&s->ring_lock);
	return took;
}

/*
 * Collect the frames the DMA has finished in each ring and hand the
 * descriptors back. Descriptors are taken in ring order; the DMA clears OWN
 * when it is done with one.
 */
static void ssc_dma_service(struct ssc_pcm *s, bool *tx, bool *rx)
{
	const u64 ring_ns = (u64)LTQ_DESC_NUM * NSEC_PER_SEC / SSC_PCM_FRAME_HZ;
	u64 now = ktime_get_ns(), gap = s->dma_last_ns ? now - s->dma_last_ns : 0;
	struct ltq_dma_desc *d;
	unsigned int n, w, n_rx;
	u8 *p;

	s->dma_last_ns = now;
	if (gap > s->dma_max_gap_ns)
		s->dma_max_gap_ns = gap;
	/* a 1 ms timer this late was held off: say when, and where */
	if (gap > 4 * NSEC_PER_MSEC)
		dev_info_ratelimited(s->dev,
				     "DMA: ring run %llu us after the last, on cpu %d (longest %llu us)\n",
				     div_u64(gap, NSEC_PER_USEC),
				     smp_processor_id(),
				     div_u64(s->dma_max_gap_ns, NSEC_PER_USEC));
	for (n = 0; n < LTQ_DESC_NUM; n++) {
		d = &s->rxch.desc_base[s->rxch.desc];
		if (d->ctl & LTQ_DMA_OWN)
			break;
		/* read the buffer only after seeing the DMA hand it back */
		rmb();
		p = s->rxbuf + (size_t)s->rxch.desc * s->frame_bytes;
		spin_lock(&s->ring_lock);
		for (w = 0; w < s->frame_words; w++)
			*rx |= ssc_rx_word(s, ssc_dma_word(p + w * 4));
		spin_unlock(&s->ring_lock);
		/* done with the buffer before the DMA owns it again */
		wmb();
		d->ctl = LTQ_DMA_OWN | s->frame_bytes;
		s->rxch.desc = (s->rxch.desc + 1) % LTQ_DESC_NUM;
	}
	n_rx = n;
	if (n_rx == LTQ_DESC_NUM || gap >= ring_ns) {
		s->rx_ring_full++;
		dev_warn_ratelimited(s->dev,
				     "DMA: rx ring ran full (%u frames, %llu us since the last run, ring %llu us): words may be lost\n",
				     n_rx, div_u64(gap, NSEC_PER_USEC),
				     div_u64(ring_ns, NSEC_PER_USEC));
	}
	for (n = 0; n < LTQ_DESC_NUM; n++) {
		d = &s->txch.desc_base[s->txch.desc];
		if (d->ctl & LTQ_DMA_OWN)
			break;
		*tx |= ssc_dma_tx_frame(s, s->txch.desc);
		/* the frame is in memory before the DMA owns its descriptor */
		wmb();
		d->ctl = LTQ_DMA_OWN | LTQ_DMA_SOP | LTQ_DMA_EOP |
			 s->frame_bytes;
		s->txch.desc = (s->txch.desc + 1) % LTQ_DESC_NUM;
	}
	if (n == LTQ_DESC_NUM) {
		s->tx_ring_empty++;
		dev_warn_ratelimited(s->dev,
				     "DMA: tx ring ran empty (%llu us since the last run): the peer may have missed words\n",
				     div_u64(gap, NSEC_PER_USEC));
	}
}

static void ssc_dma_free(struct ssc_pcm *s)
{
	size_t len = (size_t)LTQ_DESC_NUM * s->frame_bytes;

	ltq_dma_free(&s->rxch);
	ltq_dma_free(&s->txch);
	s->rxch.desc_base = NULL;
	s->txch.desc_base = NULL;
	if (s->rxbuf)
		dma_free_coherent(s->dev, len, s->rxbuf, s->rxbuf_phys);
	if (s->txbuf)
		dma_free_coherent(s->dev, len, s->txbuf, s->txbuf_phys);
	s->rxbuf = s->txbuf = NULL;
}

static int ssc_dma_start(struct ssc_pcm *s)
{
	size_t len = (size_t)LTQ_DESC_NUM * s->frame_bytes;
	unsigned int i;

	if (s->frame_bytes % (SSC_DMA_BURST * 4))
		return -EINVAL;
	s->rxbuf = dma_alloc_coherent(s->dev, len, &s->rxbuf_phys,
				      GFP_KERNEL);
	s->txbuf = dma_alloc_coherent(s->dev, len, &s->txbuf_phys,
				      GFP_KERNEL);
	if (!s->rxbuf || !s->txbuf)
		goto nomem;

	ltq_dma_init_port(SSC_DMA_PORT, SSC_DMA_BURST, SSC_DMA_BURST);

	memset(&s->rxch, 0, sizeof(s->rxch));
	s->rxch.nr = SSC_DMA_RX_CH;
	s->rxch.dev = s->dev;
	ltq_dma_alloc_rx(&s->rxch);
	memset(&s->txch, 0, sizeof(s->txch));
	s->txch.nr = SSC_DMA_TX_CH;
	s->txch.dev = s->dev;
	ltq_dma_alloc_tx(&s->txch);
	if (!s->rxch.desc_base || !s->txch.desc_base)
		goto nomem;
	/* The 1 ms timer collects the rings; no DMA interrupt is used. */
	ltq_dma_disable_irq(&s->rxch);
	ltq_dma_disable_irq(&s->txch);

	for (i = 0; i < LTQ_DESC_NUM; i++) {
		s->rxch.desc_base[i].addr = s->rxbuf_phys + i * s->frame_bytes;
		s->rxch.desc_base[i].ctl = LTQ_DMA_OWN | s->frame_bytes;
		ssc_dma_tx_frame(s, i);
		s->txch.desc_base[i].addr = s->txbuf_phys + i * s->frame_bytes;
		s->txch.desc_base[i].ctl = LTQ_DMA_OWN | LTQ_DMA_SOP |
					   LTQ_DMA_EOP | s->frame_bytes;
	}
	/* all descriptors are in memory before the channels start */
	wmb();
	ltq_dma_open(&s->rxch);
	ltq_dma_open(&s->txch);
	ssc_wr(s, SSC_DMACON, SSC_DMACON_DMAON);
	s->dma = true;
	dev_info(s->dev, "engine: DMA, %u descriptors of %u bytes\n",
		 LTQ_DESC_NUM, s->frame_bytes);
	return 0;

nomem:
	ssc_dma_free(s);
	return -ENOMEM;
}

static void ssc_dma_stop(struct ssc_pcm *s)
{
	if (!s->dma)
		return;
	ssc_wr(s, SSC_DMACON, 0);
	ltq_dma_close(&s->rxch);
	ltq_dma_close(&s->txch);
	/*
	 * The ltq_dma API reports no channel idle state, so give a burst in
	 * flight time to land: with the requests off, two frame times (one
	 * descriptor each) is ample before the rings and buffers are freed.
	 */
	usleep_range(2 * USEC_PER_SEC / SSC_PCM_FRAME_HZ,
		     4 * USEC_PER_SEC / SSC_PCM_FRAME_HZ);
	ssc_dma_free(s);
	s->dma = false;
}

/* One engine run: called from hard irq context (interrupt or hrtimer). */
static void ssc_service(struct ssc_pcm *s, bool timer)
{
	unsigned long flags;
	bool tx = false, rx = false;

	spin_lock_irqsave(&s->engine_lock, flags);
	if (timer)
		s->polls++;
	if (s->dma) {
		ssc_dma_service(s, &tx, &rx);
		spin_unlock_irqrestore(&s->engine_lock, flags);
		goto wake;
	}
	ssc_check_errors(s);
	tx = ssc_tx_fill(s);
	rx = ssc_rx_poll(s);
	spin_unlock_irqrestore(&s->engine_lock, flags);
wake:
	if (tx)
		wake_up_interruptible(&s->tx_wq);
	if (rx)
		wake_up_interruptible(&s->wq);
}

static enum hrtimer_restart ssc_timer_fn(struct hrtimer *t)
{
	struct ssc_pcm_timer *pt = container_of(t, struct ssc_pcm_timer, t);
	u64 now = ktime_get_ns();

	/* held off on this CPU; with a timer on the other the ring went on */
	if (pt->s->n_timers > 1 && pt->last_ns &&
	    now - pt->last_ns > 4 * NSEC_PER_MSEC)
		dev_info_ratelimited(pt->s->dev,
				     "timer on cpu %d held off %llu us\n",
				     smp_processor_id(),
				     div_u64(now - pt->last_ns, NSEC_PER_USEC));
	pt->last_ns = now;
	ssc_service(pt->s, true);
	hrtimer_forward_now(t, pt->s->period);
	return HRTIMER_RESTART;
}

static void ssc_start_timer_here(void *data)
{
	struct ssc_pcm_timer *pt = data;

	hrtimer_start(&pt->t, pt->first, HRTIMER_MODE_REL_PINNED);
}

/*
 * Start the engine's timer: with per_cpu, one on each online CPU (up to
 * SSC_TIMERS), spread over the period, all running the same engine under
 * engine_lock. On the 7412, ubihealthd's flash check keeps interrupts off
 * for about 27 ms every 120 s on whichever CPU it runs on, longer than the
 * DMA ring lasts; a single timer there let the ring overflow and the frame
 * shift by the lost words. With one per CPU the other keeps the ring going
 * (bench 2026-10-29).
 */
static void ssc_start_timers(struct ssc_pcm *s, u64 period_ns, bool per_cpu)
{
	unsigned int cpu, n = 0;

	s->period = ns_to_ktime(period_ns);
	for_each_online_cpu(cpu) {
		struct ssc_pcm_timer *pt = &s->timer[n];

		if (!per_cpu || n == SSC_TIMERS)
			break;
		hrtimer_init(&pt->t, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		pt->t.function = ssc_timer_fn;
		pt->s = s;
		pt->last_ns = 0;
		pt->first = ns_to_ktime(period_ns + n * period_ns / SSC_TIMERS);
		if (!smp_call_function_single(cpu, ssc_start_timer_here, pt, 1))
			n++;
	}
	if (!n) {
		struct ssc_pcm_timer *pt = &s->timer[0];

		hrtimer_init(&pt->t, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		pt->t.function = ssc_timer_fn;
		pt->s = s;
		pt->last_ns = 0;
		pt->first = s->period;
		ssc_start_timer_here(pt);
		n = 1;
	}
	s->n_timers = n;
}

/*
 * More than 100 kHz worth of runs within one jiffy means the request is not
 * being cleared (a 2 Mbit/s bus needs about 16 kHz). Switch the SSC
 * interrupts off and fall back to the timer rather than lock up the CPU.
 */
static bool ssc_irq_storm(struct ssc_pcm *s)
{
	if (time_after(jiffies, s->storm_jiffy)) {
		s->storm_jiffy = jiffies;
		s->storm_count = 0;
	}
	if (++s->storm_count < 100000 / HZ)
		return false;
	ssc_wr(s, SSC_IRN_EN, 0);
	if (s->irq[0] > 0)
		disable_irq_nosync(s->irq[0]);
	if (s->irq[1] > 0)
		disable_irq_nosync(s->irq[1]);
	s->storm = true;
	/* the backstop timer keeps running; make it the engine */
	s->period = ns_to_ktime(NSEC_PER_SEC / SSC_PCM_FRAME_HZ);
	return true;
}

static irqreturn_t ssc_irq_fn(int irq, void *data)
{
	struct ssc_pcm *s = data;

	/* The capture register is acknowledged by writing it back. */
	ssc_wr(s, SSC_IRN_CR, ssc_rd(s, SSC_IRN_CR));
	s->irqs[irq == s->irq[1]]++;
	if (s->storm || ssc_irq_storm(s))
		return IRQ_HANDLED;
	ssc_service(s, false);
	return IRQ_HANDLED;
}

/* The RX request alone paces the engine; it also tops up TX. */
static uint irn_en = SSC_IRN_RX;
module_param(irn_en, uint, 0644);
MODULE_PARM_DESC(irn_en, "SSC IRN_EN mask used by the interrupt engine");

static bool ssc_has_irqs(struct ssc_pcm *s)
{
	return s->irq[0] > 0 || s->irq[1] > 0;
}

static int ssc_engine_start(struct ssc_pcm *s)
{
	unsigned int i;

	if (!s->block_buf)
		return -ENOMEM;
	s->word_fill = 0;
	s->block_fill = 0;
	s->tx_word = 0;
	s->overruns = 0;
	s->tx_underruns = 0;
	s->irqs[0] = s->irqs[1] = s->polls = 0;
	s->dma_last_ns = 0;
	s->dma_max_gap_ns = 0;
	s->rx_ring_full = 0;
	s->tx_ring_empty = 0;
	s->storm = false;
	s->storm_count = 0;
	s->running = true;

	if (use_dma && !(s->frame_bytes % (SSC_DMA_BURST * 4))) {
		int ret = ssc_dma_start(s);

		if (ret) {
			s->running = false;
			return ret;
		}
		/* 192 frames of slack: a 1 ms collector on each CPU */
		ssc_start_timers(s, NSEC_PER_MSEC, true);
		return 0;
	}

	if (ssc_has_irqs(s)) {
		ssc_wr(s, SSC_IRN_CR, ssc_rd(s, SSC_IRN_CR));
		for (i = 0; i < 2; i++)
			if (s->irq[i] > 0)
				enable_irq(s->irq[i]);
		ssc_wr(s, SSC_IRN_EN, irn_en);
		/*
		 * The FIFO request fires on crossing its level: if one is
		 * missed, the FIFO stays full and no request follows. A slow
		 * backstop timer drains it and gets the requests going again.
		 */
		ssc_start_timers(s, NSEC_PER_MSEC, false);
		ssc_service(s, false);
		return 0;
	}

	ssc_start_timers(s, NSEC_PER_SEC / SSC_PCM_FRAME_HZ, false);
	return 0;
}

static void ssc_engine_stop(struct ssc_pcm *s)
{
	unsigned int i;

	if (!s->running)
		return;
	for (i = 0; i < s->n_timers; i++)
		hrtimer_cancel(&s->timer[i].t);
	if (s->dma) {
		dev_info(s->dev,
			 "DMA: longest gap between ring runs %llu us; rx ring full %u, tx ring empty %u\n",
			 div_u64(s->dma_max_gap_ns, NSEC_PER_USEC),
			 s->rx_ring_full, s->tx_ring_empty);
		ssc_dma_stop(s);
	} else if (ssc_has_irqs(s)) {
		ssc_wr(s, SSC_IRN_EN, 0);
		/* after a storm the lines are already disabled */
		for (i = 0; i < 2; i++)
			if (s->irq[i] > 0) {
				if (s->storm)
					synchronize_irq(s->irq[i]);
				else
					disable_irq(s->irq[i]);
			}
	}
	s->running = false;
	wake_up_interruptible(&s->wq);
	wake_up_interruptible(&s->tx_wq);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */
static void ssc_free_buffers(struct ssc_pcm *s)
{
	kfree(s->ring);
	kfree(s->desc);
	kfree(s->block_buf);
	kfree(s->read_buf);
	kfifo_free(&s->txq);
	s->ring = NULL;
	s->desc = NULL;
	s->block_buf = NULL;
	s->read_buf = NULL;
}

struct ssc_pcm *ssc_pcm_hw_create(struct device *dev, void __iomem *base,
				  int rx_irq, int tx_irq)
{
	static const char * const names[] = { "ssc-pcm rx", "ssc-pcm tx" };
	struct ssc_pcm *s;
	unsigned int i;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);

	s->dev = dev;
	s->base = base;
	s->slots = SSC_PCM_DEFAULT_SLOTS;
	s->frame_words = s->slots / 2;
	s->frame_bytes = s->frame_words * 4;

	spin_lock_init(&s->ring_lock);
	spin_lock_init(&s->engine_lock);
	mutex_init(&s->read_lock);
	mutex_init(&s->write_lock);
	init_waitqueue_head(&s->wq);
	init_waitqueue_head(&s->tx_wq);

	s->irq[0] = rx_irq;
	s->irq[1] = tx_irq;
	for (i = 0; i < 2; i++) {
		if (s->irq[i] <= 0)
			continue;
		ret = devm_request_irq(dev, s->irq[i], ssc_irq_fn,
				       IRQF_NO_AUTOEN, names[i], s);
		if (ret) {
			dev_warn(dev, "irq %d: %d, using the timer\n",
				 s->irq[i], ret);
			s->irq[0] = s->irq[1] = 0;
			break;
		}
	}
	dev_info(dev, "engine: %s (rx irq %d, tx irq %d)\n",
		 ssc_has_irqs(s) ? "FIFO interrupts" : "8 kHz timer",
		 s->irq[0], s->irq[1]);
	return s;
}

void ssc_pcm_hw_destroy(struct ssc_pcm *s)
{
	if (IS_ERR_OR_NULL(s))
		return;
	ssc_engine_stop(s);
}

void ssc_pcm_hw_teardown(struct ssc_pcm *s)
{
	if (IS_ERR_OR_NULL(s))
		return;
	ssc_engine_stop(s);
	if (s->base)
		ssc_set_run(s, false, 20000);
	/* Readers and writers woken by ssc_engine_stop() leave first. */
	mutex_lock(&s->read_lock);
	mutex_lock(&s->write_lock);
	ssc_free_buffers(s);
	s->head = s->tail = 0;
	s->seq = 0;
	s->frames = 0;
	s->dropped = 0;
	s->tx_frames = 0;
	s->tx_idle_frames = 0;
	s->step = SSC_PCM_STEP_OFF;
	mutex_unlock(&s->write_lock);
	mutex_unlock(&s->read_lock);
}

unsigned int ssc_pcm_hw_step(struct ssc_pcm *s) { return s->step; }
unsigned int ssc_pcm_hw_slots(struct ssc_pcm *s) { return s->slots; }
unsigned int ssc_pcm_hw_frame_bytes(struct ssc_pcm *s) { return s->frame_bytes; }

/* ------------------------------------------------------------------ */
/* ioctl                                                              */
/* ------------------------------------------------------------------ */
static int ssc_config(struct ssc_pcm *s, struct ssc_pcm_config *c)
{
	unsigned int slots = c->slots ?: SSC_PCM_DEFAULT_SLOTS;
	unsigned int fpb = c->frames_per_block ?: SSC_PCM_DEFAULT_FRAMES_PER_BLOCK;
	unsigned int ndesc = c->n_desc ?: SSC_PCM_DEFAULT_N_DESC;

	if (s->step != SSC_PCM_STEP_OFF)
		return -EBUSY;
	if (slots < 2 || slots > SSC_MAX_FRAME_WORDS * 2 || (slots & 1))
		return -EINVAL;
	if (fpb < 1 || fpb > 4095 / (slots / 2 * 4))
		return -EINVAL;
	if (ndesc < 2 || ndesc > SSC_PCM_MAX_DESC)
		return -EINVAL;

	s->slots = slots;
	s->frames_per_block = fpb;
	s->n_desc = ndesc;
	s->frame_words = slots / 2;
	s->frame_bytes = s->frame_words * 4;
	s->block_bytes = fpb * s->frame_bytes;

	ssc_free_buffers(s);
	s->ring = kzalloc((size_t)s->n_desc * s->block_bytes, GFP_KERNEL);
	s->desc = kcalloc(s->n_desc, sizeof(*s->desc), GFP_KERNEL);
	s->block_buf = kzalloc(s->block_bytes, GFP_KERNEL);
	s->read_buf = kzalloc(s->block_bytes, GFP_KERNEL);
	if (!s->ring || !s->desc || !s->block_buf || !s->read_buf ||
	    kfifo_alloc(&s->txq, SSC_PCM_TX_QUEUE_FRAMES * s->frame_bytes,
			GFP_KERNEL)) {
		ssc_free_buffers(s);
		return -ENOMEM;
	}

	c->slots = slots;
	c->frames_per_block = fpb;
	c->n_desc = ndesc;
	return 0;
}

static int ssc_step(struct ssc_pcm *s, unsigned int n)
{
	int ret;

	if (n == SSC_PCM_STEP_OFF) {
		ssc_pcm_hw_teardown(s);
		return 0;
	}
	if (n != s->step + 1)
		return -EINVAL;

	switch (n) {
	case SSC_PCM_STEP_CLOCK:
		ret = ssc_step_clock(s);
		break;
	case SSC_PCM_STEP_IFACE:
		ret = 0;	/* pinctrl is applied by the core */
		break;
	case SSC_PCM_STEP_TDM:
		ret = ssc_step_tdm(s);
		break;
	case SSC_PCM_STEP_DMA:
		ret = ssc_engine_start(s);
		break;
	default:
		ret = -EINVAL;
	}
	if (!ret)
		s->step = n;
	return ret;
}

static int ssc_regs(struct ssc_pcm *s, struct ssc_pcm_regs *r)
{
	memset(r, 0, sizeof(*r));
	r->step = s->step;
	r->valid = 1;
	r->reg[0] = ssc_rd(s, SSC_CON);
	r->reg[1] = ssc_rd(s, SSC_WHBSTATE);
	r->reg[2] = ssc_rd(s, SSC_STATE);
	r->reg[3] = ssc_rd(s, SSC_FSTAT);
	r->reg[4] = ssc_rd(s, SSC_ID);
	r->reg[5] = ssc_rd(s, SSC_RXFCON);
	r->reg[6] = ssc_rd(s, SSC_TXFCON);
	r->reg[7] = ssc_rd(s, SSC_SFCON);
	return 0;
}

long ssc_pcm_hw_ioctl(struct ssc_pcm *s, unsigned int cmd, void __user *argp)
{
	struct ssc_pcm_config cfg;
	struct ssc_pcm_regs regs;
	struct ssc_pcm_stats st;
	struct ssc_pcm_tx_idle idle;
	unsigned long flags;
	__u32 n;
	int ret;

	switch (cmd) {
	case SSC_PCM_IOC_CONFIG:
		if (copy_from_user(&cfg, argp, sizeof(cfg)))
			return -EFAULT;
		ret = ssc_config(s, &cfg);
		if (ret)
			return ret;
		return copy_to_user(argp, &cfg, sizeof(cfg)) ? -EFAULT : 0;
	case SSC_PCM_IOC_STEP:
		if (copy_from_user(&n, argp, sizeof(n)))
			return -EFAULT;
		return ssc_step(s, n);
	case SSC_PCM_IOC_REGS:
		ret = ssc_regs(s, &regs);
		if (ret)
			return ret;
		return copy_to_user(argp, &regs, sizeof(regs)) ? -EFAULT : 0;
	case SSC_PCM_IOC_STATS:
		memset(&st, 0, sizeof(st));
		spin_lock_irqsave(&s->ring_lock, flags);
		st.frames = s->frames;
		st.blocks = s->seq;
		st.dropped = s->dropped;
		st.tx_frames = s->tx_frames;
		st.tx_idle = s->tx_idle_frames;
		st.overruns = s->overruns;
		st.tx_underruns = s->tx_underruns;
		st.rx_irqs = s->irqs[0];
		st.tx_irqs = s->irqs[1];
		st.polls = s->polls;
		spin_unlock_irqrestore(&s->ring_lock, flags);
		if (s->frame_bytes && kfifo_initialized(&s->txq))
			st.tx_queued = kfifo_len(&s->txq) / s->frame_bytes;
		st.step = s->step;
		st.block_bytes = s->block_bytes;
		return copy_to_user(argp, &st, sizeof(st)) ? -EFAULT : 0;
	case SSC_PCM_IOC_TX_IDLE:
		if (copy_from_user(&idle, argp, sizeof(idle)))
			return -EFAULT;
		/* Taken at the next frame boundary; a torn idle frame is
		 * harmless, so no lock against the engine.
		 */
		memcpy(s->tx_idle, idle.frame, sizeof(s->tx_idle));
		return 0;
	default:
		return -ENOTTY;
	}
}

/* ------------------------------------------------------------------ */
/* read / poll                                                        */
/* ------------------------------------------------------------------ */
ssize_t ssc_pcm_hw_read(struct ssc_pcm *s, char __user *buf, size_t count,
			bool nonblock)
{
	struct ssc_pcm_block hdr;
	struct ssc_pcm_rxdesc *d;
	unsigned long flags;
	size_t need;
	int ret;

	if (s->step != SSC_PCM_STEP_DMA)
		return -ENODATA;

	if (mutex_lock_interruptible(&s->read_lock))
		return -ERESTARTSYS;
	for (;;) {
		if (!s->running || !s->ring) {
			ret = -ENODATA;
			goto out;
		}
		/* the block size can only change while read_lock is free */
		need = sizeof(hdr) + s->block_bytes;
		if (count < need) {
			ret = -EINVAL;
			goto out;
		}
		spin_lock_irqsave(&s->ring_lock, flags);
		if (s->tail != s->head)
			break;
		spin_unlock_irqrestore(&s->ring_lock, flags);
		if (nonblock) {
			ret = -EAGAIN;
			goto out;
		}
		mutex_unlock(&s->read_lock);
		ret = wait_event_interruptible(s->wq, s->tail != s->head ||
					       !s->running);
		if (ret)
			return ret;
		if (mutex_lock_interruptible(&s->read_lock))
			return -ERESTARTSYS;
	}

	/* Copy out under the lock so the engine cannot overwrite the slot. */
	d = &s->desc[s->tail];
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = SSC_PCM_BLOCK_MAGIC;
	hdr.seq = d->seq;
	hdr.flags = d->flags;
	hdr.len = s->block_bytes;
	hdr.ts_ns = d->ts_ns;
	memcpy(s->read_buf, s->ring + (size_t)s->tail * s->block_bytes,
	       s->block_bytes);
	s->tail = (s->tail + 1) % s->n_desc;
	spin_unlock_irqrestore(&s->ring_lock, flags);

	if (copy_to_user(buf, &hdr, sizeof(hdr)) ||
	    copy_to_user(buf + sizeof(hdr), s->read_buf, s->block_bytes))
		ret = -EFAULT;
	else
		ret = need;
out:
	mutex_unlock(&s->read_lock);
	return ret;
}

static unsigned int ssc_tx_room(struct ssc_pcm *s)
{
	return kfifo_avail(&s->txq) / s->frame_bytes;
}

ssize_t ssc_pcm_hw_write(struct ssc_pcm *s, const char __user *buf,
			 size_t count, bool nonblock)
{
	u8 frame[SSC_PCM_MAX_FRAME_BYTES];
	unsigned int frames, i;
	ssize_t ret;

	if (s->step != SSC_PCM_STEP_DMA)
		return -ENODATA;
	if (!count || count % s->frame_bytes)
		return -EINVAL;

	if (mutex_lock_interruptible(&s->write_lock))
		return -ERESTARTSYS;
	for (;;) {
		if (!s->running) {
			ret = -ENODATA;
			goto out;
		}
		/* the frame size can only change while write_lock is free */
		if (count % s->frame_bytes) {
			ret = -EINVAL;
			goto out;
		}
		if (ssc_tx_room(s))
			break;
		if (nonblock) {
			ret = -EAGAIN;
			goto out;
		}
		mutex_unlock(&s->write_lock);
		ret = wait_event_interruptible(s->tx_wq, !s->running ||
					       ssc_tx_room(s));
		if (ret)
			return ret;
		if (mutex_lock_interruptible(&s->write_lock))
			return -ERESTARTSYS;
	}

	/*
	 * Whole frames only, through a bounce buffer: kfifo_from_user() would
	 * queue a partial frame on a fault and shift every later frame.
	 */
	frames = min_t(size_t, ssc_tx_room(s), count / s->frame_bytes);
	for (i = 0; i < frames; i++) {
		if (copy_from_user(frame, buf + (size_t)i * s->frame_bytes,
				   s->frame_bytes))
			break;
		kfifo_in(&s->txq, frame, s->frame_bytes);
	}
	ret = i ? (ssize_t)i * s->frame_bytes : -EFAULT;
out:
	mutex_unlock(&s->write_lock);
	return ret;
}

__poll_t ssc_pcm_hw_poll(struct ssc_pcm *s, struct file *file,
			 poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &s->wq, wait);
	poll_wait(file, &s->tx_wq, wait);
	if (s->step != SSC_PCM_STEP_DMA || !s->running)
		return 0;
	if (s->tail != s->head)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (ssc_tx_room(s))
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}
