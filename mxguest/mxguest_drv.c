// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "mxga.h"
#include "mxio.h"

#define MXGUEST_NAME "mxguest-agent"
#define MXGUEST_RING_HEADER 4u
#define MXGUEST_AVAIL_IDX 2u
#define MXGUEST_USED_IDX 2u
#define MXGUEST_ISR_QUEUE 0x01u
#define MXGUEST_ISR_CONFIG 0x02u
#define MXGUEST_CHUNK_BYTES (64u * 1024u)
#define MXGUEST_CHAIN_CHUNKS 16u
#define MXGUEST_CHAIN_BYTES (MXGUEST_CHUNK_BYTES * MXGUEST_CHAIN_CHUNKS)
#define MXGUEST_RX_CHAINS 2u
#define MXGUEST_RX_LIST_MAX 64u
#define MXGUEST_TX_TIMEOUT (5 * HZ)
#define MXGUEST_RESET_TIMEOUT_US 100000u
#define MXGUEST_MSIX_VECTORS 3

static_assert(MXGUEST_CHAIN_BYTES >= MXGA_MAX_FRAME_BYTES);
static_assert(MXGUEST_CHAIN_CHUNKS * MXGUEST_RX_CHAINS <= MXIO_AGENT_QUEUE_MAX_SIZE);

struct mxguest_chunk {
	void *cpu;
	dma_addr_t dma;
};

struct mxguest_queue {
	void *desc, *avail, *used;
	dma_addr_t desc_dma, avail_dma, used_dma;
	void __iomem *notify;
	u16 avail_idx, last_used;
};

struct mxguest_rx_frame {
	struct list_head node;
	u32 len;
	u8 data[];
};

struct mxguest_dev {
	struct pci_dev *pdev;
	struct miscdevice misc;
	struct kref ref;
	void __iomem *common, *notify, *isr;
	u32 notify_multiplier, notify_len;
	bool msix;
	unsigned int irq_count;
	struct mxguest_queue queue[MXIO_AGENT_QUEUE_COUNT];
	struct mxguest_chunk tx[MXGUEST_CHAIN_CHUNKS];
	struct mxguest_chunk rx[MXGUEST_RX_CHAINS][MXGUEST_CHAIN_CHUNKS];

	struct mutex tx_lock;
	u16 tx_posted;

	struct mutex rx_lock;
	struct work_struct rx_work;
	bool rx_posted[MXGUEST_RX_CHAINS];
	u32 rx_len[MXGUEST_RX_CHAINS];
	u8 rx_done[MXGUEST_RX_CHAINS];
	unsigned int rx_done_n;

	spinlock_t list_lock;
	struct list_head frames;
	unsigned int frame_count;

	wait_queue_head_t wait;
	atomic_t opened;
	bool live, broken, removed;
};

static void mxguest_mark_broken(struct mxguest_dev *dev)
{
	WRITE_ONCE(dev->broken, true);
	wake_up_interruptible_all(&dev->wait);
}

static void mxguest_check_status(struct mxguest_dev *dev)
{
	u8 status;

	if (!READ_ONCE(dev->live))
		return;
	status = readb(dev->common + MXIO_REG_STATUS);
	if (status & (MXIO_STATUS_NEEDS_RESET | MXIO_STATUS_FAILED))
		mxguest_mark_broken(dev);
}

static irqreturn_t mxguest_irq_intx(int irq, void *data)
{
	struct mxguest_dev *dev = data;
	u8 isr = readb(dev->isr);

	if (!isr)
		return IRQ_NONE;
	if (isr & MXGUEST_ISR_CONFIG)
		mxguest_check_status(dev);
	if (isr & MXGUEST_ISR_QUEUE)
		schedule_work(&dev->rx_work);
	wake_up_interruptible_all(&dev->wait);
	return IRQ_HANDLED;
}

static irqreturn_t mxguest_irq_config(int irq, void *data)
{
	struct mxguest_dev *dev = data;

	mxguest_check_status(dev);
	wake_up_interruptible_all(&dev->wait);
	return IRQ_HANDLED;
}

static irqreturn_t mxguest_irq_tx(int irq, void *data)
{
	struct mxguest_dev *dev = data;

	wake_up_interruptible_all(&dev->wait);
	return IRQ_HANDLED;
}

static irqreturn_t mxguest_irq_rx(int irq, void *data)
{
	struct mxguest_dev *dev = data;

	schedule_work(&dev->rx_work);
	return IRQ_HANDLED;
}

static u16 mxguest_used_idx(const struct mxguest_queue *q)
{
	const __le16 *idx = q->used + MXGUEST_USED_IDX;

	return le16_to_cpu(READ_ONCE(*idx));
}

static int mxguest_desc_set(struct mxguest_queue *q, u16 slot, dma_addr_t addr, u32 len,
			    u16 flags, u16 next)
{
	struct mxio_descriptor desc = {
		.address = addr, .length = len, .flags = flags, .next = next,
	};
	u8 raw[MXIO_DESCRIPTOR_BYTES];

	if (mxio_descriptor_encode(&desc, MXIO_AGENT_QUEUE_MAX_SIZE, raw, sizeof(raw)) != MXIO_OK)
		return -EINVAL;
	memcpy(q->desc + (size_t)slot * MXIO_DESCRIPTOR_BYTES, raw, sizeof(raw));
	return 0;
}

static int mxguest_publish(struct mxguest_queue *q, u16 head)
{
	u8 raw[sizeof(u16)];
	__le16 *idx = q->avail + MXGUEST_AVAIL_IDX;

	if (mxio_available_encode(head, MXIO_AGENT_QUEUE_MAX_SIZE, raw, sizeof(raw)) != MXIO_OK)
		return -EINVAL;
	memcpy(q->avail + MXGUEST_RING_HEADER +
	       (size_t)(q->avail_idx % MXIO_AGENT_QUEUE_MAX_SIZE) * sizeof(raw), raw, sizeof(raw));
	dma_wmb();
	q->avail_idx++;
	WRITE_ONCE(*idx, cpu_to_le16(q->avail_idx));
	return 0;
}

static int mxguest_used_get(const struct mxguest_queue *q, u16 pos, struct mxio_used_element *el)
{
	const void *slot = q->used + MXGUEST_RING_HEADER +
			   (size_t)(pos % MXIO_AGENT_QUEUE_MAX_SIZE) * MXIO_USED_ELEMENT_BYTES;

	if (mxio_used_decode(slot, MXIO_USED_ELEMENT_BYTES, MXIO_AGENT_QUEUE_MAX_SIZE, el) != MXIO_OK)
		return -EIO;
	return 0;
}

static void mxguest_notify(struct mxguest_queue *q, u16 index)
{
	mb();
	writew(index, q->notify);
}

static void mxguest_drop_frames(struct mxguest_dev *dev)
{
	struct mxguest_rx_frame *frame, *tmp;
	LIST_HEAD(drop);

	spin_lock(&dev->list_lock);
	list_splice_init(&dev->frames, &drop);
	dev->frame_count = 0;
	spin_unlock(&dev->list_lock);
	list_for_each_entry_safe(frame, tmp, &drop, node) {
		list_del(&frame->node);
		kvfree(frame);
	}
}

static bool mxguest_opcode_allowed(u16 opcode)
{
	switch (opcode) {
	case MXGA_OP_HELLO:
	case MXGA_OP_HEARTBEAT:
	case MXGA_OP_CLIPBOARD_CHANGED:
	case MXGA_OP_COMMAND_RESULT:
	case MXGA_OP_SYSTEM_STATS:
	case MXGA_OP_INTEGRATION_STATUS:
	case MXGA_OP_NETWORK_INFO:
	case MXGA_OP_SHARE_STATUS:
	case MXGA_OP_FS_REQUEST:
		return true;
	default:
		return false;
	}
}

static int mxguest_reset(struct mxguest_dev *dev)
{
	u8 status;

	writeb(0, dev->common + MXIO_REG_STATUS);
	return readb_poll_timeout(dev->common + MXIO_REG_STATUS, status, !status, 100,
				  MXGUEST_RESET_TIMEOUT_US);
}

static int mxguest_negotiate(struct mxguest_dev *dev)
{
	void __iomem *c = dev->common;
	u64 features;
	u32 lo, hi;

	writel(0, c + MXIO_REG_DEVICE_FEATURE_SELECT);
	lo = readl(c + MXIO_REG_DEVICE_FEATURE);
	writel(1, c + MXIO_REG_DEVICE_FEATURE_SELECT);
	hi = readl(c + MXIO_REG_DEVICE_FEATURE);
	features = (u64)hi << 32 | lo;
	if (!(features & MXIO_FEATURE_VERSION_1))
		return -ENODEV;
	writel(0, c + MXIO_REG_DRIVER_FEATURE_SELECT);
	writel(0, c + MXIO_REG_DRIVER_FEATURE);
	writel(1, c + MXIO_REG_DRIVER_FEATURE_SELECT);
	writel(upper_32_bits(MXIO_FEATURE_VERSION_1), c + MXIO_REG_DRIVER_FEATURE);
	return 0;
}

static void mxguest_write_addr(void __iomem *reg, dma_addr_t addr)
{
	writel(lower_32_bits(addr), reg);
	writel(upper_32_bits(addr), reg + 4);
}

static int mxguest_queue_enable(struct mxguest_dev *dev, u16 index)
{
	struct mxguest_queue *q = &dev->queue[index];
	void __iomem *c = dev->common;
	u16 off;

	writew(index, c + MXIO_REG_QUEUE_SELECT);
	writew(MXIO_AGENT_QUEUE_MAX_SIZE, c + MXIO_REG_QUEUE_SIZE);
	writew(dev->msix ? index + 1 : MXIO_NO_VECTOR, c + MXIO_REG_QUEUE_VECTOR);
	mxguest_write_addr(c + MXIO_REG_QUEUE_DESCRIPTOR, q->desc_dma);
	mxguest_write_addr(c + MXIO_REG_QUEUE_AVAILABLE, q->avail_dma);
	mxguest_write_addr(c + MXIO_REG_QUEUE_USED, q->used_dma);
	off = readw(c + MXIO_REG_QUEUE_NOTIFY_OFFSET);
	if ((u64)off * dev->notify_multiplier + sizeof(u16) > dev->notify_len)
		return -ENODEV;
	q->notify = dev->notify + (size_t)off * dev->notify_multiplier;
	writew(1, c + MXIO_REG_QUEUE_ENABLE);
	return 0;
}

static int mxguest_rx_post(struct mxguest_dev *dev, unsigned int chain)
{
	struct mxguest_queue *q = &dev->queue[MXIO_AGENT_QUEUE_FROM_HOST];
	u16 head = chain * MXGUEST_CHAIN_CHUNKS;
	unsigned int i;
	int ret;

	for (i = 0; i < MXGUEST_CHAIN_CHUNKS; i++) {
		bool more = i + 1 < MXGUEST_CHAIN_CHUNKS;

		ret = mxguest_desc_set(q, head + i, dev->rx[chain][i].dma, MXGUEST_CHUNK_BYTES,
				       MXIO_DESCRIPTOR_WRITE | (more ? MXIO_DESCRIPTOR_NEXT : 0),
				       more ? head + i + 1 : 0);
		if (ret)
			return ret;
	}
	ret = mxguest_publish(q, head);
	if (ret)
		return ret;
	dev->rx_posted[chain] = true;
	return 0;
}

static int mxguest_start(struct mxguest_dev *dev)
{
	void __iomem *c = dev->common;
	u8 status;
	unsigned int i;
	int ret;

	WRITE_ONCE(dev->live, false);
	ret = mxguest_reset(dev);
	if (ret)
		goto fail;
	WRITE_ONCE(dev->broken, false);
	mxguest_drop_frames(dev);
	dev->tx_posted = 0;
	dev->rx_done_n = 0;
	memset(dev->rx_posted, 0, sizeof(dev->rx_posted));
	for (i = 0; i < MXIO_AGENT_QUEUE_COUNT; i++) {
		struct mxguest_queue *q = &dev->queue[i];

		memset(q->desc, 0, PAGE_SIZE);
		memset(q->avail, 0, PAGE_SIZE);
		memset(q->used, 0, PAGE_SIZE);
		q->avail_idx = 0;
		q->last_used = 0;
	}

	status = MXIO_STATUS_ACKNOWLEDGE;
	writeb(status, c + MXIO_REG_STATUS);
	status |= MXIO_STATUS_DRIVER;
	writeb(status, c + MXIO_REG_STATUS);
	ret = mxguest_negotiate(dev);
	if (ret)
		goto fail;
	status |= MXIO_STATUS_FEATURES_OK;
	writeb(status, c + MXIO_REG_STATUS);
	if (!(readb(c + MXIO_REG_STATUS) & MXIO_STATUS_FEATURES_OK)) {
		ret = -ENODEV;
		goto fail;
	}
	writew(dev->msix ? 0 : MXIO_NO_VECTOR, c + MXIO_REG_CONFIG_VECTOR);
	if (readw(c + MXIO_REG_QUEUE_COUNT) < MXIO_AGENT_QUEUE_COUNT) {
		ret = -ENODEV;
		goto fail;
	}
	for (i = 0; i < MXIO_AGENT_QUEUE_COUNT; i++) {
		ret = mxguest_queue_enable(dev, i);
		if (ret)
			goto fail;
	}
	for (i = 0; i < MXGUEST_RX_CHAINS; i++) {
		ret = mxguest_rx_post(dev, i);
		if (ret)
			goto fail;
	}
	WRITE_ONCE(dev->live, true);
	writeb(status | MXIO_STATUS_DRIVER_OK, c + MXIO_REG_STATUS);
	if (readb(c + MXIO_REG_STATUS) & (MXIO_STATUS_NEEDS_RESET | MXIO_STATUS_FAILED)) {
		ret = -EIO;
		goto fail;
	}
	mxguest_notify(&dev->queue[MXIO_AGENT_QUEUE_FROM_HOST], MXIO_AGENT_QUEUE_FROM_HOST);
	return 0;

fail:
	WRITE_ONCE(dev->live, false);
	WRITE_ONCE(dev->broken, true);
	mxguest_reset(dev);
	return ret;
}

static int mxguest_restart(struct mxguest_dev *dev)
{
	int ret;

	mutex_lock(&dev->tx_lock);
	mutex_lock(&dev->rx_lock);
	if (READ_ONCE(dev->removed))
		ret = -ENODEV;
	else
		ret = mxguest_start(dev);
	mutex_unlock(&dev->rx_lock);
	mutex_unlock(&dev->tx_lock);
	wake_up_interruptible_all(&dev->wait);
	return ret;
}

static struct mxguest_rx_frame *mxguest_rx_copy(struct mxguest_dev *dev, unsigned int chain,
						u32 len)
{
	struct mxguest_rx_frame *frame;
	struct mxga_frame parsed;
	u32 off = 0;
	unsigned int i;

	if (len < MXGA_HEADER_BYTES || len > MXGUEST_CHAIN_BYTES) {
		dev_warn_ratelimited(&dev->pdev->dev, "dropping frame with length %u\n", len);
		return NULL;
	}
	frame = kvmalloc(struct_size(frame, data, len), GFP_KERNEL);
	if (!frame) {
		dev_warn_ratelimited(&dev->pdev->dev, "dropping frame, no memory\n");
		return NULL;
	}
	for (i = 0; off < len; i++) {
		u32 n = min_t(u32, len - off, MXGUEST_CHUNK_BYTES);

		memcpy(frame->data + off, dev->rx[chain][i].cpu, n);
		off += n;
	}
	if (mxga_decode_frame(frame->data, len, &parsed) != MXGA_OK) {
		dev_warn_ratelimited(&dev->pdev->dev, "dropping invalid frame\n");
		kvfree(frame);
		return NULL;
	}
	frame->len = len;
	return frame;
}

static int mxguest_rx_harvest(struct mxguest_dev *dev)
{
	struct mxguest_queue *q = &dev->queue[MXIO_AGENT_QUEUE_FROM_HOST];
	u16 used_idx = mxguest_used_idx(q);

	dma_rmb();
	while (q->last_used != used_idx) {
		struct mxio_used_element el;
		unsigned int chain;

		if (mxguest_used_get(q, q->last_used, &el))
			return -EIO;
		chain = el.id / MXGUEST_CHAIN_CHUNKS;
		if (el.id % MXGUEST_CHAIN_CHUNKS || chain >= MXGUEST_RX_CHAINS ||
		    !dev->rx_posted[chain])
			return -EIO;
		dev->rx_posted[chain] = false;
		dev->rx_len[chain] = el.length;
		dev->rx_done[dev->rx_done_n++] = chain;
		q->last_used++;
	}
	return 0;
}

static void mxguest_rx_drain(struct mxguest_dev *dev)
{
	bool kick = false;

	while (dev->rx_done_n && !READ_ONCE(dev->broken)) {
		struct mxguest_rx_frame *frame;
		unsigned int chain;
		bool full;

		spin_lock(&dev->list_lock);
		full = dev->frame_count >= MXGUEST_RX_LIST_MAX;
		spin_unlock(&dev->list_lock);
		if (full)
			break;
		chain = dev->rx_done[0];
		dev->rx_done_n--;
		dev->rx_done[0] = dev->rx_done[1];
		frame = mxguest_rx_copy(dev, chain, dev->rx_len[chain]);
		if (frame) {
			spin_lock(&dev->list_lock);
			list_add_tail(&frame->node, &dev->frames);
			dev->frame_count++;
			spin_unlock(&dev->list_lock);
			wake_up_interruptible_all(&dev->wait);
		}
		if (mxguest_rx_post(dev, chain)) {
			mxguest_mark_broken(dev);
			break;
		}
		kick = true;
	}
	if (kick)
		mxguest_notify(&dev->queue[MXIO_AGENT_QUEUE_FROM_HOST], MXIO_AGENT_QUEUE_FROM_HOST);
}

static void mxguest_rx_work(struct work_struct *work)
{
	struct mxguest_dev *dev = container_of(work, struct mxguest_dev, rx_work);

	mutex_lock(&dev->rx_lock);
	if (READ_ONCE(dev->live) && !READ_ONCE(dev->removed) && !READ_ONCE(dev->broken)) {
		if (mxguest_rx_harvest(dev))
			mxguest_mark_broken(dev);
		else
			mxguest_rx_drain(dev);
	}
	mutex_unlock(&dev->rx_lock);
}

static bool mxguest_tx_busy(struct mxguest_dev *dev)
{
	return READ_ONCE(dev->tx_posted) != mxguest_used_idx(&dev->queue[MXIO_AGENT_QUEUE_TO_HOST]);
}

static int mxguest_tx_wait(struct mxguest_dev *dev)
{
	long left;

	left = wait_event_interruptible_timeout(dev->wait,
						!mxguest_tx_busy(dev) ||
						READ_ONCE(dev->broken) ||
						READ_ONCE(dev->removed),
						MXGUEST_TX_TIMEOUT);
	if (left < 0)
		return -ERESTARTSYS;
	if (READ_ONCE(dev->removed))
		return -ENODEV;
	if (READ_ONCE(dev->broken))
		return -EIO;
	if (!left && mxguest_tx_busy(dev)) {
		mxguest_mark_broken(dev);
		return -EIO;
	}
	return 0;
}

static int mxguest_tx_reap(struct mxguest_dev *dev)
{
	struct mxguest_queue *q = &dev->queue[MXIO_AGENT_QUEUE_TO_HOST];
	struct mxio_used_element el;
	u16 used_idx = mxguest_used_idx(q);

	if (q->last_used == used_idx)
		return 0;
	dma_rmb();
	if ((u16)(used_idx - q->last_used) != 1 || mxguest_used_get(q, q->last_used, &el) ||
	    el.id != 0) {
		mxguest_mark_broken(dev);
		return -EIO;
	}
	q->last_used++;
	return 0;
}

static int mxguest_tx_ready(struct mxguest_dev *dev, bool nonblock)
{
	int ret;

	if (READ_ONCE(dev->removed))
		return -ENODEV;
	mxguest_check_status(dev);
	if (READ_ONCE(dev->broken))
		return -EIO;
	if (mxguest_tx_busy(dev)) {
		if (nonblock)
			return -EAGAIN;
		ret = mxguest_tx_wait(dev);
		if (ret)
			return ret;
	}
	return mxguest_tx_reap(dev);
}

static int mxguest_tx_post(struct mxguest_dev *dev, size_t count)
{
	struct mxguest_queue *q = &dev->queue[MXIO_AGENT_QUEUE_TO_HOST];
	unsigned int n = DIV_ROUND_UP(count, MXGUEST_CHUNK_BYTES);
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		bool more = i + 1 < n;
		u32 len = min_t(size_t, count - (size_t)i * MXGUEST_CHUNK_BYTES,
				MXGUEST_CHUNK_BYTES);

		ret = mxguest_desc_set(q, i, dev->tx[i].dma, len,
				       more ? MXIO_DESCRIPTOR_NEXT : 0, more ? i + 1 : 0);
		if (ret)
			return ret;
	}
	ret = mxguest_publish(q, 0);
	if (ret)
		return ret;
	WRITE_ONCE(dev->tx_posted, q->avail_idx);
	mxguest_notify(q, MXIO_AGENT_QUEUE_TO_HOST);
	return 0;
}

static ssize_t mxguest_write_locked(struct mxguest_dev *dev, const char __user *buf,
				    size_t count, bool nonblock)
{
	struct mxga_frame frame;
	size_t off, n;
	unsigned int i;
	int ret;

	ret = mxguest_tx_ready(dev, nonblock);
	if (ret)
		return ret;
	n = min_t(size_t, count, MXGUEST_CHUNK_BYTES);
	if (copy_from_user(dev->tx[0].cpu, buf, n))
		return -EFAULT;
	if (mxga_decode_frame(dev->tx[0].cpu, count, &frame) != MXGA_OK ||
	    !mxguest_opcode_allowed(frame.opcode))
		return -EINVAL;
	for (off = n, i = 1; off < count; off += n, i++) {
		n = min_t(size_t, count - off, MXGUEST_CHUNK_BYTES);
		if (copy_from_user(dev->tx[i].cpu, buf + off, n))
			return -EFAULT;
	}
	ret = mxguest_tx_post(dev, count);
	if (ret) {
		mxguest_mark_broken(dev);
		return -EIO;
	}
	if (nonblock)
		return count;
	ret = mxguest_tx_wait(dev);
	if (ret == -ERESTARTSYS)
		return count;
	if (!ret)
		ret = mxguest_tx_reap(dev);
	return ret ? ret : count;
}

static ssize_t mxguest_write(struct file *file, const char __user *buf, size_t count,
			     loff_t *ppos)
{
	struct mxguest_dev *dev = file->private_data;
	bool nonblock = file->f_flags & O_NONBLOCK;
	ssize_t ret;

	if (count < MXGA_HEADER_BYTES || count > MXGA_MAX_FRAME_BYTES)
		return -EINVAL;
	if (nonblock) {
		if (!mutex_trylock(&dev->tx_lock))
			return -EAGAIN;
	} else if (mutex_lock_interruptible(&dev->tx_lock)) {
		return -ERESTARTSYS;
	}
	ret = mxguest_write_locked(dev, buf, count, nonblock);
	mutex_unlock(&dev->tx_lock);
	wake_up_interruptible_all(&dev->wait);
	return ret;
}

static bool mxguest_rx_ready(struct mxguest_dev *dev)
{
	bool ready;

	spin_lock(&dev->list_lock);
	ready = !list_empty(&dev->frames);
	spin_unlock(&dev->list_lock);
	return ready || READ_ONCE(dev->broken) || READ_ONCE(dev->removed);
}

static ssize_t mxguest_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct mxguest_dev *dev = file->private_data;
	struct mxguest_rx_frame *frame;
	bool was_full = false;
	size_t len;
	int ret;

	for (;;) {
		if (READ_ONCE(dev->removed))
			return -ENODEV;
		if (READ_ONCE(dev->broken))
			return -EIO;
		spin_lock(&dev->list_lock);
		frame = list_first_entry_or_null(&dev->frames, struct mxguest_rx_frame, node);
		if (frame) {
			if (count < frame->len) {
				spin_unlock(&dev->list_lock);
				return -EMSGSIZE;
			}
			list_del(&frame->node);
			was_full = dev->frame_count >= MXGUEST_RX_LIST_MAX;
			dev->frame_count--;
			spin_unlock(&dev->list_lock);
			break;
		}
		spin_unlock(&dev->list_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(dev->wait, mxguest_rx_ready(dev));
		if (ret)
			return ret;
	}

	len = frame->len;
	if (copy_to_user(buf, frame->data, len)) {
		spin_lock(&dev->list_lock);
		list_add(&frame->node, &dev->frames);
		dev->frame_count++;
		spin_unlock(&dev->list_lock);
		return -EFAULT;
	}
	kvfree(frame);
	if (was_full)
		schedule_work(&dev->rx_work);
	return len;
}

static __poll_t mxguest_poll(struct file *file, poll_table *wait)
{
	struct mxguest_dev *dev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &dev->wait, wait);
	if (READ_ONCE(dev->removed))
		return EPOLLERR | EPOLLHUP;
	if (READ_ONCE(dev->broken))
		return EPOLLERR;
	spin_lock(&dev->list_lock);
	if (!list_empty(&dev->frames))
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock(&dev->list_lock);
	if (mutex_trylock(&dev->tx_lock)) {
		if (!READ_ONCE(dev->removed) && !mxguest_tx_busy(dev))
			mask |= EPOLLOUT | EPOLLWRNORM;
		mutex_unlock(&dev->tx_lock);
	}
	return mask;
}

static void mxguest_dev_free(struct kref *ref)
{
	struct mxguest_dev *dev = container_of(ref, struct mxguest_dev, ref);

	cancel_work_sync(&dev->rx_work);
	mxguest_drop_frames(dev);
	kfree(dev);
}

static int mxguest_open(struct inode *inode, struct file *file)
{
	struct mxguest_dev *dev = container_of(file->private_data, struct mxguest_dev, misc);
	int ret;

	if (READ_ONCE(dev->removed))
		return -ENODEV;
	if (atomic_cmpxchg(&dev->opened, 0, 1))
		return -EBUSY;
	kref_get(&dev->ref);
	if (READ_ONCE(dev->broken)) {
		ret = mxguest_restart(dev);
		if (ret) {
			atomic_set(&dev->opened, 0);
			kref_put(&dev->ref, mxguest_dev_free);
			return ret;
		}
	}
	file->private_data = dev;
	return nonseekable_open(inode, file);
}

static int mxguest_release(struct inode *inode, struct file *file)
{
	struct mxguest_dev *dev = file->private_data;

	mxguest_drop_frames(dev);
	atomic_set(&dev->opened, 0);
	if (!READ_ONCE(dev->removed))
		schedule_work(&dev->rx_work);
	kref_put(&dev->ref, mxguest_dev_free);
	return 0;
}

static const struct file_operations mxguest_fops = {
	.owner = THIS_MODULE,
	.open = mxguest_open,
	.release = mxguest_release,
	.read = mxguest_read,
	.write = mxguest_write,
	.poll = mxguest_poll,
	.llseek = noop_llseek,
};

static int mxguest_read_caps(struct mxguest_dev *dev, struct mxio_capabilities *caps)
{
	u64 bars[6] = { pci_resource_len(dev->pdev, 0) };
	u8 config[MXIO_PCI_CONFIG_BYTES];
	unsigned int off;

	for (off = 0; off < sizeof(config); off += sizeof(u32)) {
		u32 value;

		if (pci_read_config_dword(dev->pdev, off, &value))
			return -EIO;
		put_unaligned_le32(value, config + off);
	}
	if (mxio_capabilities_decode(config, sizeof(config), bars, caps) != MXIO_OK)
		return -ENODEV;
	return 0;
}

static int mxguest_alloc_dma(struct mxguest_dev *dev)
{
	struct device *d = &dev->pdev->dev;
	struct mxio_ring_sizes sizes;
	unsigned int i, j;

	if (mxio_ring_sizes(MXIO_AGENT_QUEUE_MAX_SIZE, &sizes) != MXIO_OK ||
	    sizes.descriptors > PAGE_SIZE || sizes.available > PAGE_SIZE ||
	    sizes.used > PAGE_SIZE)
		return -EINVAL;
	for (i = 0; i < MXIO_AGENT_QUEUE_COUNT; i++) {
		struct mxguest_queue *q = &dev->queue[i];

		q->desc = dmam_alloc_coherent(d, PAGE_SIZE, &q->desc_dma, GFP_KERNEL);
		q->avail = dmam_alloc_coherent(d, PAGE_SIZE, &q->avail_dma, GFP_KERNEL);
		q->used = dmam_alloc_coherent(d, PAGE_SIZE, &q->used_dma, GFP_KERNEL);
		if (!q->desc || !q->avail || !q->used)
			return -ENOMEM;
	}
	for (i = 0; i < MXGUEST_CHAIN_CHUNKS; i++) {
		dev->tx[i].cpu = dmam_alloc_coherent(d, MXGUEST_CHUNK_BYTES, &dev->tx[i].dma,
						     GFP_KERNEL);
		if (!dev->tx[i].cpu)
			return -ENOMEM;
	}
	for (i = 0; i < MXGUEST_RX_CHAINS; i++) {
		for (j = 0; j < MXGUEST_CHAIN_CHUNKS; j++) {
			dev->rx[i][j].cpu = dmam_alloc_coherent(d, MXGUEST_CHUNK_BYTES,
								&dev->rx[i][j].dma, GFP_KERNEL);
			if (!dev->rx[i][j].cpu)
				return -ENOMEM;
		}
	}
	return 0;
}

static void mxguest_free_irqs(struct mxguest_dev *dev)
{
	while (dev->irq_count) {
		dev->irq_count--;
		free_irq(pci_irq_vector(dev->pdev, dev->irq_count), dev);
	}
	pci_free_irq_vectors(dev->pdev);
}

static int mxguest_setup_irqs(struct mxguest_dev *dev)
{
	static const struct {
		irq_handler_t handler;
		const char *name;
	} msix[MXGUEST_MSIX_VECTORS] = {
		{ mxguest_irq_config, "mxguest-config" },
		{ mxguest_irq_tx, "mxguest-tx" },
		{ mxguest_irq_rx, "mxguest-rx" },
	};
	struct pci_dev *pdev = dev->pdev;
	int ret;

	if (pci_alloc_irq_vectors(pdev, MXGUEST_MSIX_VECTORS, MXGUEST_MSIX_VECTORS,
				  PCI_IRQ_MSIX) == MXGUEST_MSIX_VECTORS) {
		dev->msix = true;
		while (dev->irq_count < MXGUEST_MSIX_VECTORS) {
			ret = request_irq(pci_irq_vector(pdev, dev->irq_count),
					  msix[dev->irq_count].handler, 0,
					  msix[dev->irq_count].name, dev);
			if (ret) {
				mxguest_free_irqs(dev);
				return ret;
			}
			dev->irq_count++;
		}
		return 0;
	}
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_INTX);
	if (ret < 0)
		return ret;
	ret = request_irq(pci_irq_vector(pdev, 0), mxguest_irq_intx, IRQF_SHARED, "mxguest", dev);
	if (ret) {
		pci_free_irq_vectors(pdev);
		return ret;
	}
	dev->irq_count = 1;
	return 0;
}

static int mxguest_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mxio_capabilities caps;
	struct mxguest_dev *dev;
	void __iomem *base;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->pdev = pdev;
	kref_init(&dev->ref);
	mutex_init(&dev->tx_lock);
	mutex_init(&dev->rx_lock);
	spin_lock_init(&dev->list_lock);
	INIT_LIST_HEAD(&dev->frames);
	INIT_WORK(&dev->rx_work, mxguest_rx_work);
	init_waitqueue_head(&dev->wait);
	atomic_set(&dev->opened, 0);
	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = MXGUEST_NAME;
	dev->misc.fops = &mxguest_fops;
	dev->misc.mode = 0600;
	pci_set_drvdata(pdev, dev);

	ret = pcim_enable_device(pdev);
	if (ret)
		goto err_put;
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
		ret = -ENODEV;
		goto err_put;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_put;
	base = pcim_iomap_region(pdev, 0, MXGUEST_NAME);
	if (IS_ERR(base)) {
		ret = PTR_ERR(base);
		goto err_put;
	}
	ret = mxguest_read_caps(dev, &caps);
	if (ret)
		goto err_put;
	dev->common = base + caps.common.offset;
	dev->notify = base + caps.notify.offset;
	dev->isr = base + caps.isr.offset;
	dev->notify_multiplier = caps.notify_multiplier;
	dev->notify_len = caps.notify.length;
	ret = mxguest_alloc_dma(dev);
	if (ret)
		goto err_put;
	pci_set_master(pdev);
	ret = mxguest_setup_irqs(dev);
	if (ret)
		goto err_master;
	ret = mxguest_restart(dev);
	if (ret)
		goto err_irqs;
	ret = misc_register(&dev->misc);
	if (ret)
		goto err_reset;
	return 0;

err_reset:
	mxguest_reset(dev);
err_irqs:
	mxguest_free_irqs(dev);
err_master:
	pci_clear_master(pdev);
err_put:
	kref_put(&dev->ref, mxguest_dev_free);
	return ret;
}

static void mxguest_remove(struct pci_dev *pdev)
{
	struct mxguest_dev *dev = pci_get_drvdata(pdev);

	misc_deregister(&dev->misc);
	WRITE_ONCE(dev->removed, true);
	wake_up_interruptible_all(&dev->wait);
	mutex_lock(&dev->tx_lock);
	mutex_lock(&dev->rx_lock);
	WRITE_ONCE(dev->live, false);
	mxguest_reset(dev);
	mutex_unlock(&dev->rx_lock);
	mutex_unlock(&dev->tx_lock);
	mxguest_free_irqs(dev);
	cancel_work_sync(&dev->rx_work);
	pci_clear_master(pdev);
	kref_put(&dev->ref, mxguest_dev_free);
}

static const struct pci_device_id mxguest_pci_ids[] = {
	{ PCI_DEVICE(MX_PCI_VENDOR_ID, MXGA_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, mxguest_pci_ids);

static struct pci_driver mxguest_pci_driver = {
	.name = "mxguest",
	.id_table = mxguest_pci_ids,
	.probe = mxguest_probe,
	.remove = mxguest_remove,
};
module_pci_driver(mxguest_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zak Noble-Clarke");
MODULE_DESCRIPTION("MX guest agent transport");
