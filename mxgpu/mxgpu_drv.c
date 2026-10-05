// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

#include <linux/aperture.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dma-fence.h>
#include <linux/iosys-map.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/screen_info.h>
#include <linux/sysfb.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_edid.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "mxgpu_wire.h"
#include "mxgpu_drm_uapi.h"

#define DRM_IOCTL_MXGPU_GET_INFO DRM_IOWR(DRM_COMMAND_BASE + 0, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_CTX_CREATE DRM_IOWR(DRM_COMMAND_BASE + 1, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_CTX_DESTROY DRM_IOWR(DRM_COMMAND_BASE + 2, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_GEM_CREATE DRM_IOWR(DRM_COMMAND_BASE + 3, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_GEM_CLOSE DRM_IOWR(DRM_COMMAND_BASE + 4, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_SUBMIT DRM_IOWR(DRM_COMMAND_BASE + 5, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_WAIT DRM_IOWR(DRM_COMMAND_BASE + 6, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_BO_WRITE DRM_IOWR(DRM_COMMAND_BASE + 7, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_BO_READ DRM_IOWR(DRM_COMMAND_BASE + 8, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_GET_CAPS DRM_IOWR(DRM_COMMAND_BASE + 9, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_GET_TRANSFER_LIMITS DRM_IOWR(DRM_COMMAND_BASE + 10, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_GET_BATCH_LIMITS                                                           \
	DRM_IOWR(DRM_COMMAND_BASE + MXGPU_DRM_IOCTL_GET_BATCH_LIMITS, struct mxgpu_drm_user)
#define DRM_IOCTL_MXGPU_SUBMIT_BATCH                                                               \
	DRM_IOWR(DRM_COMMAND_BASE + MXGPU_DRM_IOCTL_SUBMIT_BATCH, struct mxgpu_drm_user)

struct mxgpu_object {
	struct list_head link;
	u32 id;
	u16 destroy_opcode;
};

struct mxgpu_context {
	struct list_head objects;
	bool cleanup_blocked;
	u64 sequence, transport_generation, cleanup_revision;
	bool host_live, batch_fault;
	struct list_head link;
	u32 id;
};

struct mxgpu_pending_ownership {
	struct mxgpu_context *owner;
	u64 sequence, transport_generation;
	u32 context_id, object_id, device_generation;
	u16 opcode;
	bool valid, speculative_create;
};

struct mxgpu_file {
	struct mutex lock;
	struct list_head contexts;
};

struct mxgpu_scanout_job {
	struct list_head link;
	struct drm_plane_state primary;
	struct drm_plane_state cursor;
	struct drm_pending_vblank_event *event;
	struct drm_crtc_commit *commit;
	u32 row;
	bool started, clearing;
	u32 clear_row;
	int error;
};

struct mxgpu_atomic_state {
	struct drm_atomic_state base;
	struct mxgpu_scanout_job *scanout;
	bool planes_snapshotted;
	bool primary_updated;
	bool cursor_updated;
};

struct mxgpu_batch_cell {
	u64 wire_sequence, wire_fence;
	u32 queue, command_offset, command_bytes;
	bool posted;
};

struct mxgpu_batch_pending {
	struct mxgpu_context *owner;
	struct mxgpu_drm_batch_response response;
	struct mxgpu_batch_cell cells[MXGPU_DRM_BATCH_MAX_COMMANDS];
	ktime_t deadline;
	u64 transport_generation;
	u32 consumed[MXGPU_QUEUE_COUNT];
	bool active;
};

struct mxgpu_cursor_slot {
	u64 generation;
	u32 device_generation;
	int x, y, width, height;
	u8 *background;
};

struct mxgpu_device {
	u64 transport_generation;
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_plane cursor;
	spinlock_t fence_lock;
	struct mutex submit_lock;
	bool submit_stopped;
	struct mutex scanout_lock;
	struct mutex cleanup_lock;
	struct delayed_work cleanup_work;
	struct list_head cleanup_contexts;
	bool cleanup_stopped;
	struct delayed_work scanout_work;
	struct list_head scanout_jobs;
	bool scanout_stopped;
	u64 fence_context;
	u64 next_context;
	void __iomem *regs;
	void __iomem *fb;
	void __iomem *scanout;
	bool fb_ram;
	bool present_active;
	bool present_pending;
	resource_size_t scanout_size;
	resource_size_t scanout_off;
	u64 scanout_phys;
	resource_size_t scanout_slot_bytes;
	resource_size_t scanout_mapping_off;
	u32 scanout_front;
	struct mxgpu_cursor_slot *cursor_slots;
	u64 cursor_generation;
	struct drm_framebuffer *cursor_primary_fb;
	u32 cursor_primary_x, cursor_primary_y;
	bool cursor_cache_inhibited, cursor_cache_resetting;
	u32 width;
	u32 height;
	u32 pitch;
	void *dma;
	dma_addr_t dma_addr;
	u32 queue_tail[6];
	u32 queue_size[6];
	struct mxgpu_batch_pending batch;
	u64 wire_sequence[MXGPU_QUEUE_COUNT];
	u64 wire_fence[MXGPU_QUEUE_COUNT];
	ktime_t pending_deadline[MXGPU_QUEUE_COUNT];
	struct mxgpu_pending_ownership pending_ownership[MXGPU_QUEUE_COUNT];
	u16 pending_opcode[MXGPU_QUEUE_COUNT];
	bool queue_live;
	wait_queue_head_t completion_wait;
	bool irq_registered;
	int completion_irq;
	bool transport_validated;
	u32 max_command_bytes;
	struct mxgpu_negotiated negotiated_caps;
	int cursor_x;
	int cursor_y;
	int cursor_w;
	int cursor_h;
};

#define MXGPU_WAIT_SPINS 20u
/* Sleep between polls. A cold shader compile can take longer than one pump park. */
#define MXGPU_COMPLETE_SPINS 250u
#define MXGPU_FAST_COMPLETE_SPINS 4u
#define MXGPU_COMPLETE_TIMEOUT_MS 375u
#define MXGPU_EXTENDED_TIMEOUT_MS 5000u
#define MXGPU_EXTENDED_SPINS 5000u

#define MXGPU_DMA_BYTES (4u * 1024u * 1024u)
#define MXGPU_OFF_REQUEST 0u
#define MXGPU_OFF_RESPONSE 256u
#define MXGPU_CMD_BYTES (512u * 1024u)
#define MXGPU_OFF_COMPLETION 0x1000u
#define MXGPU_RESPONSE_AREA (2u * 1024u * 1024u)
#define MXGPU_QSIZE 2u
#define MXGPU_RENDER_QUEUE 1u

struct mxgpu_qring {
	u32 desc;
	u32 avail;
	u32 used;
};

static const struct mxgpu_qring mxgpu_rings[6] = {
	[0] = {0x200u, 0x220u, 0x230u},
	[1] = {0x250u, 0x270u, 0x280u},
	[3] = {0x2a0u, 0x2c0u, 0x2d0u},
};

#define MXGPU_BATCH_QSIZE 64u
#define MXGPU_BATCH_RING_BYTES                                                                     \
	(MXGPU_BATCH_QSIZE * (MXGPU_QUEUE_DESCRIPTOR_SIZE + MXGPU_AVAILABLE_ENTRY_SIZE +           \
				     MXGPU_USED_ENTRY_SIZE))
#define MXGPU_BATCH_RING_BASE 0x2000u
#define MXGPU_BATCH_COMPLETIONS (MXGPU_BATCH_RING_BASE + 2u * MXGPU_BATCH_RING_BYTES)
#define MXGPU_BATCH_ARENA 0x2a0000u

static const struct mxgpu_qring mxgpu_batch_rings[6] = {
	[1] = {MXGPU_BATCH_RING_BASE,
		MXGPU_BATCH_RING_BASE + MXGPU_BATCH_QSIZE * MXGPU_QUEUE_DESCRIPTOR_SIZE,
		MXGPU_BATCH_RING_BASE + MXGPU_BATCH_QSIZE * (MXGPU_QUEUE_DESCRIPTOR_SIZE +
							    MXGPU_AVAILABLE_ENTRY_SIZE)},
	[3] = {MXGPU_BATCH_RING_BASE + MXGPU_BATCH_RING_BYTES,
		MXGPU_BATCH_RING_BASE + MXGPU_BATCH_RING_BYTES +
			MXGPU_BATCH_QSIZE * MXGPU_QUEUE_DESCRIPTOR_SIZE,
		MXGPU_BATCH_RING_BASE + MXGPU_BATCH_RING_BYTES +
			MXGPU_BATCH_QSIZE * (
				MXGPU_QUEUE_DESCRIPTOR_SIZE + MXGPU_AVAILABLE_ENTRY_SIZE)},
};
static_assert(
	MXGPU_BATCH_COMPLETIONS + MXGPU_DRM_BATCH_MAX_COMMANDS * MXGPU_COMPLETION_SIZE <= 0x10000u);
static_assert(MXGPU_BATCH_ARENA + MXGPU_DRM_BATCH_MAX_COMMAND_BYTES <= MXGPU_DMA_BYTES);

static const struct mxgpu_qring *mxgpu_ring(struct mxgpu_device *mxdev, u32 queue)
{
	return mxdev->queue_size[queue] == MXGPU_BATCH_QSIZE ? &mxgpu_batch_rings[queue]
							     : &mxgpu_rings[queue];
}

static int mxgpu_batch_drain(struct mxgpu_device *mxdev, ktime_t deadline);

/* Register file in BAR0. Layout is the host transport ABI. */
#define MXGPU_REG_BAR 0
#define MXGPU_FB_BAR 2
#define MXGPU_APERTURE_BASE 0x380u
#define MXGPU_AREG_CONTROL 0x20u
#define MXGPU_AREG_STATUS 0x24u
#define MXGPU_AREG_POWER_ON_BASE_LOW 0x28u
#define MXGPU_AREG_POWER_ON_BASE_HIGH 0x2cu
#define MXGPU_AREG_POWER_ON_STRIDE 0x30u
#define MXGPU_AREG_POWER_ON_WIDTH 0x34u
#define MXGPU_AREG_POWER_ON_HEIGHT 0x38u
#define MXGPU_APERTURE_CONTROL_ARM (1u << 0)
#define MXGPU_APERTURE_BASE_ALIGNMENT 4096u
#define MXGPU_APERTURE_CONTROL_REFRESH (1u << 2)
#define MXGPU_APERTURE_STATUS_SUPPORTED (1u << 0)
#define MXGPU_APERTURE_STATUS_ARMED (1u << 1)
#define MXGPU_APERTURE_STATUS_FAULT_MASK (0xffu << 8)

static const u32 mxgpu_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static int mxgpu_connector_get_modes(struct drm_connector *connector)
{
	struct mxgpu_device *mxdev = container_of(connector, struct mxgpu_device, connector);
	struct drm_display_mode *mode;

	mode = drm_cvt_mode(connector->dev, mxdev->width, mxdev->height, 60, false, false, false);
	if (!mode)
		return 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_connector_helper_funcs mxgpu_connector_helper = {
	.get_modes = mxgpu_connector_get_modes,
};

static const struct drm_connector_funcs mxgpu_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_plane_funcs mxgpu_cursor_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static int mxgpu_cursor_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!plane_state->crtc || !plane_state->fb)
		return 0;
	if (plane_state->fb->width > 64 || plane_state->fb->height > 64)
		return -EINVAL;
	crtc_state = drm_atomic_get_crtc_state(state, plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	return drm_atomic_helper_check_plane_state(
		plane_state, crtc_state, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, true, true);
}

static void mxgpu_cursor_update(struct drm_plane *plane, struct drm_atomic_state *state);
static void mxgpu_cursor_disable(struct drm_plane *plane, struct drm_atomic_state *state);

static const struct drm_plane_helper_funcs mxgpu_cursor_helper = {
	.atomic_check = mxgpu_cursor_check,
	.atomic_update = mxgpu_cursor_update,
	.atomic_disable = mxgpu_cursor_disable,
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
};

static int mxgpu_pipe_prepare(struct drm_simple_display_pipe *pipe, struct drm_plane_state *state)
{
	return drm_gem_plane_helper_prepare_fb(&pipe->plane, state);
}

static enum drm_mode_status mxgpu_mode_valid(
	struct drm_simple_display_pipe *pipe, const struct drm_display_mode *mode)
{
	struct mxgpu_device *mxdev = container_of(pipe, struct mxgpu_device, pipe);

	if (mode->hdisplay != mxdev->width || mode->vdisplay != mxdev->height)
		return MODE_BAD;
	return MODE_OK;
}

static bool mxgpu_clip_rect(int *dst_x, int *dst_y, int *src_x, int *src_y, int *width, int *height,
	int dst_w, int dst_h, int src_w, int src_h)
{
	s64 x0 = 0, y0 = 0;
	s64 x1 = *width, y1 = *height;

	if (*width <= 0 || *height <= 0 || dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0)
		return false;
	x0 = max_t(s64, x0, -(s64)*dst_x);
	x0 = max_t(s64, x0, -(s64)*src_x);
	y0 = max_t(s64, y0, -(s64)*dst_y);
	y0 = max_t(s64, y0, -(s64)*src_y);
	x1 = min_t(s64, x1, (s64)dst_w - *dst_x);
	x1 = min_t(s64, x1, (s64)src_w - *src_x);
	y1 = min_t(s64, y1, (s64)dst_h - *dst_y);
	y1 = min_t(s64, y1, (s64)src_h - *src_y);
	if (x0 >= x1 || y0 >= y1)
		return false;
	*dst_x = (s64)*dst_x + x0;
	*src_x = (s64)*src_x + x0;
	*dst_y = (s64)*dst_y + y0;
	*src_y = (s64)*src_y + y0;
	*width = x1 - x0;
	*height = y1 - y0;
	return true;
}

static u32 mxgpu_premul(u32 dst, u32 src)
{
	u32 alpha = src >> 24;
	u32 inv;
	u32 chan;
	u32 out;
	unsigned int shift;

	if (!alpha)
		return dst;
	if (alpha == 255)
		return src | 0xff000000u;
	inv = 255 - alpha;
	out = 0xff000000u;
	for (shift = 0; shift < 24; shift += 8) {
		chan = ((src >> shift) & 0xffu) + ((((dst >> shift) & 0xffu) * inv + 127u) / 255u);
		if (chan > 255)
			chan = 255;
		out |= chan << shift;
	}
	return out;
}

static int mxgpu_scanout_layout(
	u64 base, u64 bar, u64 bar_bytes, u32 width, u32 height, u32 pitch, u64 *slot_bytes)
{
	u64 view, slot;
	if (!base || base % MXGPU_APERTURE_BASE_ALIGNMENT || !width || !height || width > 4096 ||
		height > 4096 || pitch < (u64)width * 4 || pitch % 4)
		return -EINVAL;
	view = (u64)pitch * height;
	if (view > U32_MAX || view > U64_MAX - (MXGPU_APERTURE_BASE_ALIGNMENT - 1))
		return -EOVERFLOW;
	slot = ALIGN(view, MXGPU_APERTURE_BASE_ALIGNMENT);
	if (base < bar || base - bar > bar_bytes || slot > (bar_bytes - (base - bar)) / 2 ||
		base > U64_MAX - slot * 2)
		return -ENOSPC;
	*slot_bytes = slot;
	return 0;
}

static void mxgpu_cursor_cache_invalidate(struct mxgpu_device *mxdev)
{
	u32 slot;

	mxdev->cursor_generation++;
	if (!mxdev->cursor_generation)
		mxdev->cursor_generation = 1;
	mxdev->cursor_primary_fb = NULL;
	if (!mxdev->cursor_slots)
		return;
	for (slot = 0; slot < 2; slot++) {
		mxdev->cursor_slots[slot].generation = 0;
		mxdev->cursor_slots[slot].width = 0;
		mxdev->cursor_slots[slot].height = 0;
	}
}

static bool mxgpu_cursor_cache_geometry(struct mxgpu_device *mxdev, struct drm_plane_state *primary)
{
	u32 width, height, x, y;

	if (!mxdev->cursor_slots || !mxdev->fb_ram || mxdev->scanout_stopped ||
		mxdev->cursor_cache_inhibited || mxdev->cursor_cache_resetting || !primary ||
		!primary->fb || !primary->crtc || primary->crtc_x || primary->crtc_y ||
		(primary->src_x & 0xffffu) || (primary->src_y & 0xffffu) ||
		(primary->src_w & 0xffffu) || (primary->src_h & 0xffffu))
		return false;
	width = primary->src_w >> 16;
	height = primary->src_h >> 16;
	if (!width || !height) {
		width = primary->fb->width;
		height = primary->fb->height;
	}
	x = primary->src_x >> 16;
	y = primary->src_y >> 16;
	return width == mxdev->width && height == mxdev->height && x <= primary->fb->width &&
	       width <= primary->fb->width - x && y <= primary->fb->height &&
	       height <= primary->fb->height - y;
}

static void mxgpu_scanout_begin(struct mxgpu_device *mxdev)
{
	resource_size_t offset = mxdev->scanout_mapping_off +
				 (mxdev->scanout_front ^ 1u) * mxdev->scanout_slot_bytes;
	mxdev->scanout = mxdev->fb + offset;
	mxdev->scanout_off = offset;
}

static int mxgpu_scanout_publish(struct mxgpu_device *mxdev)
{
	u64 base;
	u32 status;
	if (mxdev->scanout_stopped || !mxdev->regs || !mxdev->scanout_slot_bytes)
		return -ENODEV;
	base = mxdev->scanout_phys + (mxdev->scanout_front ^ 1u) * mxdev->scanout_slot_bytes;
	wmb();
	writel(lower_32_bits(base), mxdev->regs + MXGPU_APERTURE_BASE + 0x00);
	writel(upper_32_bits(base), mxdev->regs + MXGPU_APERTURE_BASE + 0x04);
	writel(MXGPU_APERTURE_CONTROL_ARM, mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_CONTROL);
	status = readl(mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_STATUS);
	if (!(status & MXGPU_APERTURE_STATUS_ARMED) || (status & MXGPU_APERTURE_STATUS_FAULT_MASK))
		return -EIO;
	mxdev->scanout_front ^= 1u;
	writel(MXGPU_APERTURE_CONTROL_REFRESH,
		mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_CONTROL);
	return 0;
}

static void mxgpu_copy_rows(struct mxgpu_device *mxdev, const u8 *src, u32 src_pitch, int dst_x,
	int dst_y, int width, int height)
{
	u32 y;
	u32 row = mxdev->pitch;
	u32 copy = (u32)width * 4;

	for (y = 0; y < (u32)height; y++) {
		if (mxdev->fb_ram)
			memcpy((__force void *)(mxdev->scanout + ((u32)dst_y + y) * row +
						(u32)dst_x * 4),
				src + y * src_pitch, copy);
		else
			memcpy_toio(mxdev->scanout + ((u32)dst_y + y) * row + (u32)dst_x * 4,
				src + y * src_pitch, copy);
		if ((y & 15u) == 15u)
			cond_resched();
	}
}

static void mxgpu_clear_scanout_rows(struct mxgpu_device *mxdev, u32 first, u32 count)
{
	u32 y;
	for (y = first; y < first + count; y++) {
		if (mxdev->fb_ram)
			memset((__force void *)(mxdev->scanout + (size_t)y * mxdev->pitch), 0,
				mxdev->width * 4);
		else
			memset_io(mxdev->scanout + (size_t)y * mxdev->pitch, 0, mxdev->width * 4);
		if ((y & 15u) == 15u)
			cond_resched();
	}
}

struct mxgpu_fbmap {
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	struct iosys_map data[DRM_FORMAT_MAX_PLANES];
	const u8 *ptr;
	struct drm_framebuffer *fb;
};

static int mxgpu_fb_map(struct drm_framebuffer *fb, struct mxgpu_fbmap *out)
{
	struct drm_gem_object *object;
	u64 end;

	memset(out, 0, sizeof(*out));
	if (!fb || !fb->width || !fb->height || (u64)fb->width * 4 > fb->pitches[0])
		return -EINVAL;
	object = drm_gem_fb_get_obj(fb, 0);
	if (!object)
		return -EINVAL;
	end = fb->offsets[0] + (u64)(fb->height - 1) * fb->pitches[0] + (u64)fb->width * 4;
	if (end > object->size)
		return -EINVAL;
	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		return -EIO;
	if (drm_gem_fb_vmap(fb, out->map, out->data)) {
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
		return -EIO;
	}
	out->ptr = out->data[0].is_iomem ? (__force const u8 *)out->data[0].vaddr_iomem
					 : out->data[0].vaddr;
	out->fb = fb;
	if (!out->ptr) {
		drm_gem_fb_vunmap(fb, out->map);
		drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
		return -EIO;
	}
	return 0;
}

static void mxgpu_fb_unmap(struct mxgpu_fbmap *out)
{
	if (!out->fb)
		return;
	drm_gem_fb_vunmap(out->fb, out->map);
	drm_gem_fb_end_cpu_access(out->fb, DMA_FROM_DEVICE);
	out->fb = NULL;
}

static int mxgpu_blit(struct mxgpu_device *mxdev, struct drm_framebuffer *fb, int dst_x, int dst_y,
	int src_x, int src_y, int width, int height)
{
	struct mxgpu_fbmap mapped;
	struct drm_gem_object *object;
	u64 source_end;

	if (!fb || !mxdev->scanout)
		return -EINVAL;
	if (!mxgpu_clip_rect(&dst_x, &dst_y, &src_x, &src_y, &width, &height, mxdev->width,
		    mxdev->height, fb->width, fb->height))
		return -EINVAL;
	if (!fb->pitches[0] || ((u64)src_x + (u32)width) * 4 > fb->pitches[0])
		return -EINVAL;
	object = drm_gem_fb_get_obj(fb, 0);
	if (!object)
		return -EINVAL;
	source_end = fb->offsets[0] + ((u64)src_y + (u32)height - 1) * fb->pitches[0] +
		     ((u64)src_x + (u32)width) * 4;
	if (source_end > object->size)
		return -EINVAL;
	if ((!mxdev->fb_ram ? mxdev->scanout_off : 0) + ((u64)dst_y + (u32)height) * mxdev->pitch >
		mxdev->scanout_size)
		return -EINVAL;
	if (mxgpu_fb_map(fb, &mapped))
		return -EIO;
	mxgpu_copy_rows(mxdev, mapped.ptr + (size_t)src_y * fb->pitches[0] + (size_t)src_x * 4,
		fb->pitches[0], dst_x, dst_y, width, height);
	mxgpu_fb_unmap(&mapped);
	return 0;
}

static void mxgpu_screen_to_primary(
	struct drm_plane_state *primary, int sx, int sy, int *fx, int *fy)
{
	*fx = sx - primary->crtc_x + (primary->src_x >> 16);
	*fy = sy - primary->crtc_y + (primary->src_y >> 16);
}

static void mxgpu_restore_cursor(struct mxgpu_device *mxdev, struct mxgpu_fbmap *primary,
	struct drm_plane_state *primary_state)
{
	int fx;
	int fy;
	u32 pitch;

	if (!mxdev->cursor_w || !mxdev->cursor_h || !primary->ptr || !primary->fb)
		goto clear;
	mxgpu_screen_to_primary(primary_state, mxdev->cursor_x, mxdev->cursor_y, &fx, &fy);
	pitch = primary->fb->pitches[0];
	if (fx < 0 || fy < 0 || fx + mxdev->cursor_w > primary->fb->width ||
		fy + mxdev->cursor_h > primary->fb->height)
		goto clear;
	mxgpu_copy_rows(mxdev, primary->ptr + (size_t)fy * pitch + (size_t)fx * 4, pitch,
		mxdev->cursor_x, mxdev->cursor_y, mxdev->cursor_w, mxdev->cursor_h);
clear:
	mxdev->cursor_w = 0;
	mxdev->cursor_h = 0;
}

static int mxgpu_paint_cursor_state(struct mxgpu_device *mxdev, struct drm_plane_state *primary,
	struct drm_plane_state *cursor, bool frame_is_clean)
{
	struct mxgpu_fbmap under;
	struct mxgpu_fbmap cmap;
	int dst_x;
	int dst_y;
	int src_x;
	int src_y;
	int width;
	int height;
	int y;
	int ret = 0;

	if (frame_is_clean && (!cursor || !cursor->fb || !cursor->crtc)) {
		mxdev->cursor_w = 0;
		mxdev->cursor_h = 0;
		return 0;
	}
	memset(&under, 0, sizeof(under));
	if (primary && primary->fb) {
		ret = mxgpu_fb_map(primary->fb, &under);
		if (ret)
			goto out;
	}
	if (!frame_is_clean)
		mxgpu_restore_cursor(mxdev, &under, primary);
	else {
		mxdev->cursor_w = 0;
		mxdev->cursor_h = 0;
	}
	if (!cursor || !cursor->fb || !cursor->crtc || !under.ptr || !primary)
		goto out;
	dst_x = cursor->crtc_x;
	dst_y = cursor->crtc_y;
	src_x = cursor->src_x >> 16;
	src_y = cursor->src_y >> 16;
	width = cursor->src_w >> 16;
	height = cursor->src_h >> 16;
	if (!width || !height) {
		width = cursor->fb->width;
		height = cursor->fb->height;
	}
	if (!mxgpu_clip_rect(&dst_x, &dst_y, &src_x, &src_y, &width, &height, mxdev->width,
		    mxdev->height, cursor->fb->width, cursor->fb->height))
		goto out;
	if (width > 64 || height > 64)
		goto out;
	ret = mxgpu_fb_map(cursor->fb, &cmap);
	if (ret)
		goto out;
	for (y = 0; y < height; y++) {
		u8 rowbuf[64 * 4];
		int x;
		int pfx;
		int pfy;
		const u8 *crow =
			cmap.ptr + ((size_t)src_y + y) * cursor->fb->pitches[0] + (size_t)src_x * 4;

		mxgpu_screen_to_primary(primary, dst_x, dst_y + y, &pfx, &pfy);
		for (x = 0; x < width; x++) {
			const u8 *pp = NULL;
			u32 dst = 0;
			u32 src;
			u32 out;
			const u8 *spx = crow + x * 4;

			if (pfx + x >= 0 && pfy >= 0 && pfx + x < primary->fb->width &&
				pfy < primary->fb->height)
				pp = under.ptr + (size_t)pfy * primary->fb->pitches[0] +
				     (size_t)(pfx + x) * 4;
			if (pp)
				dst = pp[0] | ((u32)pp[1] << 8) | ((u32)pp[2] << 16) |
				      ((u32)pp[3] << 24);
			src = spx[0] | ((u32)spx[1] << 8) | ((u32)spx[2] << 16) |
			      ((u32)spx[3] << 24);
			out = mxgpu_premul(dst, src);
			rowbuf[x * 4 + 0] = (u8)out;
			rowbuf[x * 4 + 1] = (u8)(out >> 8);
			rowbuf[x * 4 + 2] = (u8)(out >> 16);
			rowbuf[x * 4 + 3] = (u8)(out >> 24);
		}
		mxgpu_copy_rows(mxdev, rowbuf, (u32)width * 4, dst_x, dst_y + y, width, 1);
	}
	mxgpu_fb_unmap(&cmap);
	mxdev->cursor_x = dst_x;
	mxdev->cursor_y = dst_y;
	mxdev->cursor_w = width;
	mxdev->cursor_h = height;
out:
	mxgpu_fb_unmap(&under);
	return ret;
}

static void mxgpu_cursor_slot_restore(struct mxgpu_device *mxdev, struct mxgpu_cursor_slot *slot)
{
	if (slot->width && slot->height)
		mxgpu_copy_rows(mxdev, slot->background, 64 * 4, slot->x, slot->y, slot->width,
			slot->height);
	slot->width = 0;
	slot->height = 0;
}

static int mxgpu_cursor_slot_paint(
	struct mxgpu_device *mxdev, struct mxgpu_cursor_slot *slot, struct drm_plane_state *cursor)
{
	struct mxgpu_fbmap cmap;
	int dst_x, dst_y, src_x, src_y, width, height, y, ret;

	slot->width = 0;
	slot->height = 0;
	mxdev->cursor_w = 0;
	mxdev->cursor_h = 0;
	if (!cursor || !cursor->fb || !cursor->crtc)
		return 0;
	dst_x = cursor->crtc_x;
	dst_y = cursor->crtc_y;
	src_x = cursor->src_x >> 16;
	src_y = cursor->src_y >> 16;
	width = cursor->src_w >> 16;
	height = cursor->src_h >> 16;
	if (!width || !height) {
		width = cursor->fb->width;
		height = cursor->fb->height;
	}
	if (!mxgpu_clip_rect(&dst_x, &dst_y, &src_x, &src_y, &width, &height, mxdev->width,
		    mxdev->height, cursor->fb->width, cursor->fb->height) ||
		width > 64 || height > 64)
		return 0;
	ret = mxgpu_fb_map(cursor->fb, &cmap);
	if (ret)
		return ret;
	for (y = 0; y < height; y++) {
		u8 rowbuf[64 * 4];
		u8 *background = slot->background + y * 64 * 4;
		const u8 *source =
			cmap.ptr + ((size_t)src_y + y) * cursor->fb->pitches[0] + (size_t)src_x * 4;
		const void *destination =
			(__force const void *)(mxdev->scanout + ((size_t)dst_y + y) * mxdev->pitch +
					       (size_t)dst_x * 4);
		int x;

		memcpy(background, destination, width * 4);
		for (x = 0; x < width; x++) {
			const u8 *dst = background + x * 4, *src = source + x * 4;
			u32 output = mxgpu_premul(dst[0] | ((u32)dst[1] << 8) |
							  ((u32)dst[2] << 16) | ((u32)dst[3] << 24),
				src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16) |
					((u32)src[3] << 24));
			rowbuf[x * 4] = output;
			rowbuf[x * 4 + 1] = output >> 8;
			rowbuf[x * 4 + 2] = output >> 16;
			rowbuf[x * 4 + 3] = output >> 24;
		}
		mxgpu_copy_rows(mxdev, rowbuf, width * 4, dst_x, dst_y + y, width, 1);
	}
	mxgpu_fb_unmap(&cmap);
	slot->x = mxdev->cursor_x = dst_x;
	slot->y = mxdev->cursor_y = dst_y;
	slot->width = mxdev->cursor_w = width;
	slot->height = mxdev->cursor_h = height;
	return 0;
}

static int mxgpu_scanout_frame(
	struct mxgpu_device *mxdev, struct drm_plane_state *primary, struct drm_plane_state *cursor)
{
	struct mxgpu_cursor_slot *slot = NULL;
	u32 target = mxdev->scanout_front ^ 1u, device_generation = 0;
	int width, height, ret;

	if (mxdev->scanout_stopped)
		return -ENODEV;
	if (!primary || !primary->fb)
		return -EINVAL;
	if (mxgpu_cursor_cache_geometry(mxdev, primary) && mxdev->regs &&
		!(readl(mxdev->regs + MXGPU_REG_STATUS) &
			(MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT))) {
		slot = &mxdev->cursor_slots[target];
		device_generation = readl(mxdev->regs + MXGPU_REG_DEVICE_GENERATION);
	}
	width = primary->src_w >> 16;
	height = primary->src_h >> 16;
	if (!width || !height) {
		width = primary->fb->width;
		height = primary->fb->height;
	}
	mxgpu_scanout_begin(mxdev);
	if (primary->crtc_x || primary->crtc_y || width != mxdev->width || height != mxdev->height)
		mxgpu_clear_scanout_rows(mxdev, 0, mxdev->height);
	ret = mxgpu_blit(mxdev, primary->fb, primary->crtc_x, primary->crtc_y, primary->src_x >> 16,
		primary->src_y >> 16, width, height);
	if (!ret)
		ret = slot ? mxgpu_cursor_slot_paint(mxdev, slot, cursor)
			   : mxgpu_paint_cursor_state(mxdev, primary, cursor, true);
	if (!ret)
		ret = mxgpu_scanout_publish(mxdev);
	if (ret) {
		mxgpu_cursor_cache_invalidate(mxdev);
	} else if (slot && readl(mxdev->regs + MXGPU_REG_DEVICE_GENERATION) == device_generation) {
		slot->generation = mxdev->cursor_generation;
		slot->device_generation = device_generation;
		mxdev->cursor_primary_fb = primary->fb;
		mxdev->cursor_primary_x = primary->src_x;
		mxdev->cursor_primary_y = primary->src_y;
	} else {
		mxgpu_cursor_cache_invalidate(mxdev);
	}
	return ret;
}

static int mxgpu_cursor_scanout_frame(
	struct mxgpu_device *mxdev, struct drm_plane_state *primary, struct drm_plane_state *cursor)
{
	struct mxgpu_cursor_slot *slot;
	int ret;

	if (!mxgpu_cursor_cache_geometry(mxdev, primary) ||
		mxdev->cursor_primary_fb != primary->fb ||
		mxdev->cursor_primary_x != primary->src_x ||
		mxdev->cursor_primary_y != primary->src_y) {
		mxgpu_cursor_cache_invalidate(mxdev);
		return mxgpu_scanout_frame(mxdev, primary, cursor);
	}
	slot = &mxdev->cursor_slots[mxdev->scanout_front ^ 1u];
	if (!slot->generation || slot->generation != mxdev->cursor_generation)
		return mxgpu_scanout_frame(mxdev, primary, cursor);
	if (!mxdev->regs ||
		slot->device_generation != readl(mxdev->regs + MXGPU_REG_DEVICE_GENERATION) ||
		(readl(mxdev->regs + MXGPU_REG_STATUS) &
			(MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT))) {
		mxgpu_cursor_cache_invalidate(mxdev);
		return mxgpu_scanout_frame(mxdev, primary, cursor);
	}
	mxgpu_scanout_begin(mxdev);
	mxgpu_cursor_slot_restore(mxdev, slot);
	ret = mxgpu_cursor_slot_paint(mxdev, slot, cursor);
	if (!ret)
		ret = mxgpu_scanout_publish(mxdev);
	if (ret || slot->device_generation != readl(mxdev->regs + MXGPU_REG_DEVICE_GENERATION))
		mxgpu_cursor_cache_invalidate(mxdev);
	return ret;
}

static void mxgpu_scanout_job_free(struct mxgpu_scanout_job *job)
{
	if (!job)
		return;
	if (job->primary.fb)
		drm_framebuffer_put(job->primary.fb);
	if (job->cursor.fb)
		drm_framebuffer_put(job->cursor.fb);
	if (job->commit)
		drm_crtc_commit_put(job->commit);
	kfree(job);
}

static void mxgpu_scanout_job_complete(struct mxgpu_device *mxdev, struct mxgpu_scanout_job *job)
{
	unsigned long flags;

	if (job->event) {
		if (job->error && job->event->base.fence)
			dma_fence_set_error(job->event->base.fence, job->error);
		spin_lock_irqsave(&mxdev->drm.event_lock, flags);
		drm_crtc_send_vblank_event(&mxdev->pipe.crtc, job->event);
		spin_unlock_irqrestore(&mxdev->drm.event_lock, flags);
		job->event = NULL;
	} else if (job->commit) {
		complete_all(&job->commit->flip_done);
	}
}

static void mxgpu_scanout_work(struct work_struct *work)
{
	struct mxgpu_device *mxdev =
		container_of(to_delayed_work(work), struct mxgpu_device, scanout_work);
	struct mxgpu_scanout_job *job;
	struct drm_plane_state *state;
	int width, height, rows;
	int idx;
	bool finished;

	mutex_lock(&mxdev->scanout_lock);
	if (mxdev->scanout_stopped || list_empty(&mxdev->scanout_jobs))
		goto out;
	if (mxdev->present_pending) {
		schedule_delayed_work(&mxdev->scanout_work, msecs_to_jiffies(1));
		goto out;
	}
	job = list_first_entry(&mxdev->scanout_jobs, struct mxgpu_scanout_job, link);
	state = &job->primary;
	mxdev->present_active = false;
	if (!drm_dev_enter(&mxdev->drm, &idx)) {
		list_del(&job->link);
		job->error = -ENODEV;
		mxgpu_scanout_job_complete(mxdev, job);
		mxgpu_scanout_job_free(job);
		if (!list_empty(&mxdev->scanout_jobs))
			schedule_delayed_work(&mxdev->scanout_work, 0);
		goto out;
	}
	if (!job->started) {
		mxgpu_scanout_begin(mxdev);
		job->started = true;
		job->clearing = state->fb &&
				(state->crtc_x || state->crtc_y ||
					(state->src_w && (state->src_w >> 16) != mxdev->width) ||
					(state->src_h && (state->src_h >> 16) != mxdev->height) ||
					(!state->src_w && state->fb->width != mxdev->width) ||
					(!state->src_h && state->fb->height != mxdev->height));
	}
	mutex_unlock(&mxdev->scanout_lock);
	width = state->src_w >> 16;
	height = state->src_h >> 16;
	if (state->fb && (!width || !height)) {
		width = state->fb->width;
		height = state->fb->height;
	}
	if (job->clearing) {
		u32 count = min_t(u32, 8, mxdev->height - job->clear_row);
		mxgpu_clear_scanout_rows(mxdev, job->clear_row, count);
		job->clear_row += count;
		job->clearing = job->clear_row < mxdev->height;
		finished = false;
		goto copy_done;
	}
	rows = min_t(int, 8, height - job->row);
	if (state->fb && rows > 0) {
		job->error = mxgpu_blit(mxdev, state->fb, state->crtc_x, state->crtc_y + job->row,
			state->src_x >> 16, (state->src_y >> 16) + job->row, width, rows);
		if (!job->error)
			job->row += rows;
	}
	finished = job->error || !state->fb || job->row >= height;
	if (finished && !job->error)
		job->error = mxgpu_paint_cursor_state(mxdev, &job->primary, &job->cursor, true);
	if (job->error)
		drm_err_ratelimited(&mxdev->drm, "scanout copy failed: %d\n", job->error);
copy_done:
	mutex_lock(&mxdev->scanout_lock);
	if (mxdev->scanout_stopped) {
		job->error = -ENODEV;
		finished = true;
	}
	if (finished && state->fb && !job->error)
		job->error = mxgpu_scanout_publish(mxdev);
	if (finished)
		list_del(&job->link);
	if (!mxdev->scanout_stopped && !list_empty(&mxdev->scanout_jobs))
		schedule_delayed_work(&mxdev->scanout_work, msecs_to_jiffies(1));
	mutex_unlock(&mxdev->scanout_lock);
	if (finished) {
		mxgpu_scanout_job_complete(mxdev, job);
		mxgpu_scanout_job_free(job);
	}
	drm_dev_exit(idx);
	return;
out:
	mutex_unlock(&mxdev->scanout_lock);
}

static void mxgpu_scanout_enqueue(struct mxgpu_device *mxdev, struct drm_atomic_state *state)
{
	struct mxgpu_atomic_state *mxstate = container_of(state, struct mxgpu_atomic_state, base);
	struct mxgpu_scanout_job *job;
	struct drm_crtc_state *crtc = drm_atomic_get_new_crtc_state(state, &mxdev->pipe.crtc);
	unsigned long flags;

	if (!mxstate->scanout || !crtc)
		return;
	job = mxstate->scanout;
	mxstate->scanout = NULL;
	if (crtc->commit)
		job->commit = drm_crtc_commit_get(crtc->commit);
	spin_lock_irqsave(&mxdev->drm.event_lock, flags);
	job->event = crtc->event;
	crtc->event = NULL;
	spin_unlock_irqrestore(&mxdev->drm.event_lock, flags);
	mutex_lock(&mxdev->scanout_lock);
	if (mxdev->scanout_stopped) {
		job->error = -ENODEV;
		mxgpu_scanout_job_complete(mxdev, job);
		mxgpu_scanout_job_free(job);
	} else {
		list_add_tail(&job->link, &mxdev->scanout_jobs);
		schedule_delayed_work(&mxdev->scanout_work, 0);
	}
	mutex_unlock(&mxdev->scanout_lock);
}

static void mxgpu_scanout_stop(struct mxgpu_device *mxdev)
{
	struct mxgpu_scanout_job *job, *next;

	mutex_lock(&mxdev->scanout_lock);
	mxdev->scanout_stopped = true;
	mxgpu_cursor_cache_invalidate(mxdev);
	mutex_unlock(&mxdev->scanout_lock);
	cancel_delayed_work_sync(&mxdev->scanout_work);
	mutex_lock(&mxdev->scanout_lock);
	list_for_each_entry_safe (job, next, &mxdev->scanout_jobs, link) {
		list_del(&job->link);
		job->error = -ENODEV;
		mxgpu_scanout_job_complete(mxdev, job);
		mxgpu_scanout_job_free(job);
	}
	mutex_unlock(&mxdev->scanout_lock);
}

static void mxgpu_pipe_update_active(
	struct drm_simple_display_pipe *pipe, struct drm_plane_state *old_plane_state)
{
	struct mxgpu_device *mxdev = container_of(pipe, struct mxgpu_device, pipe);
	struct drm_atomic_state *commit = old_plane_state ? old_plane_state->state : NULL;
	struct drm_plane_state *state =
		commit ? drm_atomic_get_new_plane_state(commit, &pipe->plane) : NULL;
	struct mxgpu_atomic_state *mxstate =
		commit ? container_of(commit, struct mxgpu_atomic_state, base) : NULL;

	if (!state)
		return;
	if (!mxdev->fb_ram) {
		if (state->fb)
			mxdev->present_active = false;
		mxgpu_scanout_enqueue(mxdev, commit);
		return;
	}
	if (!mxstate->primary_updated)
		return;
	mutex_lock(&mxdev->scanout_lock);
	mxgpu_cursor_cache_invalidate(mxdev);
	if (!state->fb || !mxstate->scanout) {
		mutex_unlock(&mxdev->scanout_lock);
		return;
	}
	state = &mxstate->scanout->primary;
	mxdev->present_active = false;
	if (mxgpu_scanout_frame(mxdev, state, &mxstate->scanout->cursor))
		drm_err_ratelimited(&mxdev->drm, "scanout publication failed\n");
	mutex_unlock(&mxdev->scanout_lock);
}

static void mxgpu_pipe_update(
	struct drm_simple_display_pipe *pipe, struct drm_plane_state *old_plane_state)
{
	int idx;
	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;
	mxgpu_pipe_update_active(pipe, old_plane_state);
	drm_dev_exit(idx);
}

static void mxgpu_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct mxgpu_device *mxdev = container_of(pipe, struct mxgpu_device, pipe);

	mutex_lock(&mxdev->scanout_lock);
	mxgpu_cursor_cache_invalidate(mxdev);
	mutex_unlock(&mxdev->scanout_lock);
}

static const struct drm_simple_display_pipe_funcs mxgpu_pipe_funcs = {
	.mode_valid = mxgpu_mode_valid,
	.prepare_fb = mxgpu_pipe_prepare,
	.disable = mxgpu_pipe_disable,
	.update = mxgpu_pipe_update,
};

static void mxgpu_cursor_update_active(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct mxgpu_device *mxdev = container_of(plane, struct mxgpu_device, cursor);

	struct mxgpu_atomic_state *mxstate = container_of(state, struct mxgpu_atomic_state, base);

	if (!mxdev->fb_ram) {
		if (READ_ONCE(mxdev->present_active))
			return;
		mxgpu_scanout_enqueue(mxdev, state);
		return;
	}
	if (!mxstate->scanout || !mxstate->cursor_updated || mxstate->primary_updated)
		return;
	mutex_lock(&mxdev->scanout_lock);
	if (!mxdev->present_active && mxgpu_cursor_scanout_frame(mxdev, &mxstate->scanout->primary,
					      &mxstate->scanout->cursor))
		drm_err_ratelimited(&mxdev->drm, "cursor scanout publication failed\n");
	mutex_unlock(&mxdev->scanout_lock);
}

static void mxgpu_cursor_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	int idx;
	if (!drm_dev_enter(plane->dev, &idx))
		return;
	mxgpu_cursor_update_active(plane, state);
	drm_dev_exit(idx);
}

static void mxgpu_cursor_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
	mxgpu_cursor_update(plane, state);
}

static struct drm_atomic_state *mxgpu_atomic_state_alloc(struct drm_device *dev)
{
	struct mxgpu_atomic_state *state = kzalloc(sizeof(*state), GFP_KERNEL);

	if (!state)
		return NULL;
	if (drm_atomic_state_init(dev, &state->base)) {
		kfree(state);
		return NULL;
	}
	return &state->base;
}

static void mxgpu_atomic_state_clear(struct drm_atomic_state *state)
{
	struct mxgpu_atomic_state *mxstate = container_of(state, struct mxgpu_atomic_state, base);

	mxgpu_scanout_job_free(mxstate->scanout);
	mxstate->scanout = NULL;
	mxstate->planes_snapshotted = false;
	mxstate->primary_updated = false;
	mxstate->cursor_updated = false;
	drm_atomic_state_default_clear(state);
}

static void mxgpu_atomic_state_free(struct drm_atomic_state *state)
{
	struct mxgpu_atomic_state *mxstate = container_of(state, struct mxgpu_atomic_state, base);

	mxgpu_scanout_job_free(mxstate->scanout);
	mxstate->scanout = NULL;
	drm_atomic_state_default_release(state);
	kfree(mxstate);
}

static int mxgpu_atomic_check(struct drm_device *dev, struct drm_atomic_state *state)
{
	struct mxgpu_device *mxdev = container_of(dev, struct mxgpu_device, drm);
	struct mxgpu_atomic_state *mxstate = container_of(state, struct mxgpu_atomic_state, base);
	struct drm_plane_state *primary, *cursor;
	struct drm_crtc_state *crtc;
	struct mxgpu_scanout_job *job;
	int ret;

	if (!mxstate->planes_snapshotted) {
		mxstate->primary_updated =
			drm_atomic_get_new_plane_state(state, &mxdev->pipe.plane) != NULL;
		mxstate->cursor_updated =
			drm_atomic_get_new_plane_state(state, &mxdev->cursor) != NULL;
		mxstate->planes_snapshotted = true;
	}
	primary = drm_atomic_get_plane_state(state, &mxdev->pipe.plane);
	if (IS_ERR(primary))
		return PTR_ERR(primary);
	cursor = drm_atomic_get_plane_state(state, &mxdev->cursor);
	if (IS_ERR(cursor))
		return PTR_ERR(cursor);
	crtc = drm_atomic_get_crtc_state(state, &mxdev->pipe.crtc);
	if (IS_ERR(crtc))
		return PTR_ERR(crtc);
	ret = drm_atomic_helper_check(dev, state);
	if (ret)
		return ret;
	if (crtc->mode_changed || crtc->active_changed)
		mxstate->primary_updated = true;
	if (!mxdev->fb_ram)
		crtc->no_vblank = false;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	job->primary = *primary;
	job->cursor = *cursor;
	if (job->primary.fb)
		drm_framebuffer_get(job->primary.fb);
	if (job->cursor.fb)
		drm_framebuffer_get(job->cursor.fb);
	mxgpu_scanout_job_free(mxstate->scanout);
	mxstate->scanout = job;
	return 0;
}

static const struct drm_mode_config_funcs mxgpu_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = mxgpu_atomic_check,
	.atomic_state_alloc = mxgpu_atomic_state_alloc,
	.atomic_state_clear = mxgpu_atomic_state_clear,
	.atomic_state_free = mxgpu_atomic_state_free,
	.atomic_commit = drm_atomic_helper_commit,
};

static const char *mxgpu_fence_driver(struct dma_fence *fence)
{
	return "mxgpu";
}

static const char *mxgpu_fence_timeline(struct dma_fence *fence)
{
	return "submit";
}

static const struct dma_fence_ops mxgpu_fence_ops = {
	.get_driver_name = mxgpu_fence_driver,
	.get_timeline_name = mxgpu_fence_timeline,
};

static int mxgpu_signal(struct mxgpu_device *mxdev)
{
	struct dma_fence *fence;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return -ENOMEM;
	dma_fence_init(fence, &mxgpu_fence_ops, &mxdev->fence_lock, mxdev->fence_context, 1);
	dma_fence_signal(fence);
	dma_fence_put(fence);
	return 0;
}

static int mxgpu_copy_record(struct mxgpu_drm_user *user, u8 **out, u32 *out_len)
{
	if (!user->pointer || !user->size || user->size > MXGPU_CMD_BYTES)
		return -EINVAL;
	*out = memdup_user(u64_to_user_ptr(user->pointer), user->size);
	if (IS_ERR(*out))
		return PTR_ERR(*out);
	*out_len = user->size;
	return 0;
}

static int mxgpu_ioctl_get_caps(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_device *mxdev = container_of(dev, struct mxgpu_device, drm);
	struct mxgpu_drm_user *user = data;
	struct mxgpu_drm_caps caps;
	u8 *record = NULL;
	u8 request[MXGPU_DRM_HEADER_BYTES], response[MXGPU_DRM_HEADER_BYTES + 28];
	u32 record_len = 0, request_len = 0, response_len = 0;
	int ret, idx;

	(void)file;
	ret = mxgpu_copy_record(user, &record, &record_len);
	if (ret)
		return ret;
	ret = mxgpu_drm_get_caps_encode(request, sizeof(request), &request_len);
	if (ret || record_len != request_len || memcmp(record, request, request_len))
		ret = -EINVAL;
	kfree(record);
	user->size = 0;
	if (ret)
		return ret;
	if (user->capacity < sizeof(response))
		return -ENOSPC;
	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;
	ret = mutex_lock_interruptible(&mxdev->submit_lock);
	if (ret)
		goto exit;
	if (!mxdev->queue_live) {
		ret = -ENODEV;
		goto unlock;
	}
	caps.major = mxdev->negotiated_caps.major;
	caps.minor = mxdev->negotiated_caps.minor;
	caps.features = mxdev->negotiated_caps.features;
	caps.max_command_bytes = mxdev->max_command_bytes;
	caps.max_queues = mxdev->negotiated_caps.limits.max_queues;
	caps.max_descriptors_per_queue = mxdev->negotiated_caps.limits.max_descriptors_per_queue;
	ret = mxgpu_drm_get_caps_response_encode(&caps, response, sizeof(response), &response_len);
	if (ret)
		ret = -EPROTO;
unlock:
	mutex_unlock(&mxdev->submit_lock);
	if (!ret) {
		if (copy_to_user(u64_to_user_ptr(user->pointer), response, response_len))
			ret = -EFAULT;
		else
			user->size = response_len;
	}
exit:
	drm_dev_exit(idx);
	return ret;
}

static int mxgpu_ioctl_get_transfer_limits(
	struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_device *mxdev = container_of(dev, struct mxgpu_device, drm);
	struct mxgpu_drm_user *user = data;
	struct mxgpu_drm_transfer_limits limits;
	u32 command_limit, data_limit;
	u8 *record = NULL;
	u8 request[MXGPU_DRM_HEADER_BYTES], response[MXGPU_DRM_HEADER_BYTES + 8];
	u32 record_len = 0, request_len = 0, response_len = 0;
	int ret, idx;

	(void)file;
	ret = mxgpu_copy_record(user, &record, &record_len);
	if (ret)
		return ret;
	ret = mxgpu_drm_get_transfer_limits_encode(request, sizeof(request), &request_len);
	if (ret || record_len != request_len || memcmp(record, request, request_len))
		ret = -EINVAL;
	kfree(record);
	user->size = 0;
	if (ret)
		return ret;
	if (user->capacity < sizeof(response))
		return -ENOSPC;
	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;
	ret = mutex_lock_interruptible(&mxdev->submit_lock);
	if (ret)
		goto exit;
	if (!mxdev->queue_live) {
		ret = -ENODEV;
		goto unlock;
	}
	command_limit = min_t(
		u32, mxdev->max_command_bytes, MXGPU_CMD_BYTES - (MXGPU_DRM_HEADER_BYTES + 24u));
	if (command_limit <= MXGPU_COMMAND_HEADER_SIZE + MXGPU_TRANSFER_REQUEST_SIZE) {
		ret = -EPROTO;
		goto unlock;
	}
	data_limit = command_limit - MXGPU_COMMAND_HEADER_SIZE - MXGPU_TRANSFER_REQUEST_SIZE;
	limits.max_transfer_to_host_bytes =
		min_t(u32, mxdev->negotiated_caps.limits.max_transfer_to_host_bytes, data_limit);
	limits.max_transfer_from_host_bytes =
		min_t(u32, mxdev->negotiated_caps.limits.max_transfer_from_host_bytes,
			MXGPU_RESPONSE_AREA - MXGPU_COMPLETION_SIZE);
	ret = mxgpu_drm_get_transfer_limits_response_encode(
		&limits, response, sizeof(response), &response_len);
	if (ret)
		ret = -EPROTO;
unlock:
	mutex_unlock(&mxdev->submit_lock);
	if (!ret) {
		if (copy_to_user(u64_to_user_ptr(user->pointer), response, response_len))
			ret = -EFAULT;
		else
			user->size = response_len;
	}
exit:
	drm_dev_exit(idx);
	return ret;
}

static int mxgpu_ioctl_get_info(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_drm_user *user = data;
	u8 *record = NULL;
	u8 request[MXGPU_DRM_HEADER_BYTES];
	u8 response[MXGPU_DRM_HEADER_BYTES + 12];
	u32 record_len = 0, request_len = 0;
	u32 response_len = 0;
	int status;

	(void)dev;
	(void)file;
	status = mxgpu_copy_record(user, &record, &record_len);
	if (status)
		return status;
	status = mxgpu_drm_get_info_encode(request, sizeof(request), &request_len);
	if (status || record_len != request_len || memcmp(record, request, request_len))
		status = -EINVAL;
	kfree(record);
	user->size = 0;
	if (status)
		return status;
	status = mxgpu_drm_get_info_response_encode(MXGPU_DRIVER_VERSION_MAJOR,
		MXGPU_DRIVER_VERSION_MINOR, MXGPU_DRIVER_VERSION_PATCH, response, sizeof(response),
		&response_len);
	if (status)
		return -EINVAL;
	if (user->capacity < response_len)
		return -ENOSPC;
	if (copy_to_user(u64_to_user_ptr(user->pointer), response, response_len))
		return -EFAULT;
	user->size = response_len;
	return 0;
}

#define MXGPU_COMPLETION_IRQ_MASK                                                                  \
	(MXGPU_IRQ_COMPLETION | MXGPU_IRQ_QUEUE_FAULT | MXGPU_IRQ_TRANSPORT_FAULT)

static irqreturn_t mxgpu_completion_interrupt(int irq, void *data)
{
	struct mxgpu_device *mxdev = data;
	u32 active;
	(void)irq;
	if (!READ_ONCE(mxdev->irq_registered))
		return IRQ_NONE;
	active = readl(mxdev->regs + MXGPU_REG_IRQ_STATUS) &
		 readl(mxdev->regs + MXGPU_REG_IRQ_MASK) & MXGPU_COMPLETION_IRQ_MASK;
	if (!active)
		return IRQ_NONE;
	writel(active, mxdev->regs + MXGPU_REG_IRQ_STATUS);
	wake_up_all(&mxdev->completion_wait);
	return IRQ_HANDLED;
}

static void mxgpu_completion_irq_enable(struct mxgpu_device *mxdev)
{
	if (READ_ONCE(mxdev->irq_registered))
		writel(MXGPU_COMPLETION_IRQ_MASK, mxdev->regs + MXGPU_REG_IRQ_MASK);
}

static void mxgpu_completion_irq_stop(struct mxgpu_device *mxdev)
{
	if (!READ_ONCE(mxdev->irq_registered))
		return;
	WRITE_ONCE(mxdev->irq_registered, false);
	writel(0, mxdev->regs + MXGPU_REG_IRQ_MASK);
	readl(mxdev->regs + MXGPU_REG_IRQ_MASK);
	synchronize_irq(mxdev->completion_irq);
	free_irq(mxdev->completion_irq, mxdev);
	wake_up_all(&mxdev->completion_wait);
}

static void mxgpu_writel_addr(struct mxgpu_device *mxdev, u32 low, u32 high, dma_addr_t addr)
{
	writel(lower_32_bits(addr), mxdev->regs + low);
	writel(upper_32_bits(addr), mxdev->regs + high);
}

static u32 mxgpu_qreg(u32 queue, u32 reg)
{
	return 0x100u + queue * 0x40u + reg;
}

static u32 mxgpu_completion_off(u32 queue)
{
	if (queue == 3u)
		return 0x10000u;
	if (queue == MXGPU_RENDER_QUEUE)
		return 0x1040u;
	return MXGPU_OFF_COMPLETION;
}

static u32 mxgpu_completion_len(u32 queue)
{
	if (queue == 3u)
		return MXGPU_RESPONSE_AREA;
	if (queue == MXGPU_QUEUE_CONTROL)
		return MXGPU_COMPLETION_SIZE + MXGPU_FORMAT_CAPABILITIES_SIZE;
	return 32u;
}

/* Command bytes sit past the transfer response so queues cannot overwrite each other. */
static u32 mxgpu_command_off(u32 queue)
{
	if (queue == MXGPU_RENDER_QUEUE)
		return 0x2a0000u;
	if (queue == 3u)
		return 0x320000u;
	return 0x220000u;
}

static void mxgpu_program_queues(struct mxgpu_device *mxdev)
{
	static const u32 queues[] = {0u, MXGPU_RENDER_QUEUE, 3u};
	unsigned int i;

	memset(mxdev->queue_tail, 0, sizeof mxdev->queue_tail);
	memset(mxdev->wire_sequence, 0, sizeof mxdev->wire_sequence);
	memset(mxdev->wire_fence, 0, sizeof mxdev->wire_fence);
	memset(mxdev->pending_deadline, 0, sizeof mxdev->pending_deadline);
	memset(mxdev->pending_opcode, 0, sizeof mxdev->pending_opcode);
	memset(mxdev->pending_ownership, 0, sizeof mxdev->pending_ownership);
	memset(&mxdev->batch, 0, sizeof mxdev->batch);
	for (i = 0; i < ARRAY_SIZE(queues); i++) {
		u32 queue = queues[i];
		const struct mxgpu_qring *ring;
		mxdev->queue_size[queue] =
			queue != MXGPU_QUEUE_CONTROL &&
					mxdev->negotiated_caps.limits.max_descriptors_per_queue >=
						MXGPU_BATCH_QSIZE
				? MXGPU_BATCH_QSIZE
				: MXGPU_QSIZE;
		ring = mxgpu_ring(mxdev, queue);

		mxgpu_writel_addr(mxdev, mxgpu_qreg(queue, 0x00), mxgpu_qreg(queue, 0x04),
			mxdev->dma_addr + ring->desc);
		mxgpu_writel_addr(mxdev, mxgpu_qreg(queue, 0x08), mxgpu_qreg(queue, 0x0c),
			mxdev->dma_addr + ring->avail);
		mxgpu_writel_addr(mxdev, mxgpu_qreg(queue, 0x10), mxgpu_qreg(queue, 0x14),
			mxdev->dma_addr + ring->used);
		writel(mxdev->queue_size[queue], mxdev->regs + mxgpu_qreg(queue, 0x18));
		writel(1, mxdev->regs + mxgpu_qreg(queue, 0x1c));
	}
}

static int mxgpu_validate_negotiation(const u8 *response, u32 response_bytes,
	const struct mxgpu_negotiation_request *request, struct mxgpu_negotiated *negotiated)
{
	if (mxgpu_negotiation_response_decode(response, response_bytes, negotiated))
		return -EPROTO;
	if (negotiated->status)
		return -EIO;
	if (negotiated->major != request->maximum_major ||
		negotiated->minor < request->minimum_minor ||
		negotiated->minor > request->maximum_minor ||
		(negotiated->features & request->required_features) != request->required_features ||
		(negotiated->features & ~request->requested_features))
		return -EPROTO;
	if (!(negotiated->features & MXGPU_FEAT_MULTI_QUEUE) ||
		negotiated->limits.max_queues <= MXGPU_QUEUE_TRANSFER ||
		negotiated->limits.max_descriptors_per_queue < MXGPU_QSIZE ||
		!negotiated->limits.max_command_bytes)
		return -EPROTO;
	return 0;
}

static int mxgpu_negotiate(struct pci_dev *pdev, struct mxgpu_device *mxdev)
{
	struct mxgpu_negotiation_request request;
	struct mxgpu_negotiated negotiated;
	int ret;
	u8 *request_bytes;
	u8 *response;
	u32 len = 0;
	unsigned int spins;

	mxdev->queue_live = false;
	if (!mxdev->regs)
		return -ENODEV;
	if (!mxdev->dma) {
		if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64)))
			return -EIO;
		mxdev->dma = dma_alloc_coherent(
			&pdev->dev, MXGPU_DMA_BYTES, &mxdev->dma_addr, GFP_KERNEL);
		if (!mxdev->dma)
			return -ENOMEM;
	}
	memset(&request, 0, sizeof request);
	request.minimum_major = 1;
	request.minimum_minor = 0;
	request.maximum_major = 1;
	request.maximum_minor = MXGPU_PROTOCOL_VERSION_MINOR;
	request.requested_features =
		MXGPU_FEAT_RENDER | MXGPU_FEAT_SCANOUT | MXGPU_FEAT_CURSOR |
		MXGPU_FEAT_FENCE_SIGNAL | MXGPU_FEAT_RESOURCE_PLACEMENT | MXGPU_FEAT_MULTI_QUEUE |
		MXGPU_FEAT_COLOR_CLEAR | MXGPU_FEAT_SCANOUT_APERTURE | MXGPU_FEAT_LARGE_READBACK |
		MXGPU_FEAT_VIEWPORT_SCISSOR | MXGPU_FEAT_BLEND_STATE | MXGPU_FEAT_RASTERIZER_STATE |
		MXGPU_FEAT_VIEWPORT_Y_FLIP | MXGPU_FEAT_SAMPLER_OBJECTS |
		MXGPU_FEAT_EXTENDED_PIXEL_FORMATS | MXGPU_FEAT_DEPTH_STENCIL_TARGET |
		MXGPU_FEAT_DEPTH24_STENCIL8 | MXGPU_FEAT_FLOAT_BUFFER_FORMATS |
		MXGPU_FEAT_TEXTURE_ARRAY | MXGPU_FEAT_TEXTURE_COMPARE;
	request.required_features = MXGPU_FEAT_RENDER | MXGPU_FEAT_MULTI_QUEUE;
	request_bytes = (u8 *)mxdev->dma + MXGPU_OFF_REQUEST;
	response = (u8 *)mxdev->dma + MXGPU_OFF_RESPONSE;
	memset(response, 0, 96);
	if (mxgpu_negotiation_request_encode(&request, request_bytes, 256, &len))
		return -EINVAL;
	pci_set_master(pdev);
	mxgpu_writel_addr(mxdev, 0x020, 0x024, mxdev->dma_addr + MXGPU_OFF_REQUEST);
	mxgpu_writel_addr(mxdev, 0x028, 0x02c, mxdev->dma_addr + MXGPU_OFF_RESPONSE);
	dma_wmb();
	writel(MXGPU_CONTROL_NEGOTIATE, mxdev->regs + MXGPU_REG_CONTROL);
	for (spins = 0; spins < MXGPU_WAIT_SPINS; spins++) {
		u32 magic = le32_to_cpu(READ_ONCE(*(const __le32 *)response));

		if (magic == MXGPU_PROTOCOL_MAGIC)
			break;
		usleep_range(1000, 1500);
	}
	if (le32_to_cpu(READ_ONCE(*(const __le32 *)response)) != MXGPU_PROTOCOL_MAGIC)
		return -ETIMEDOUT;
	dma_rmb();
	ret = mxgpu_validate_negotiation(
		response, MXGPU_NEGOTIATION_RESPONSE_COMPUTE_LIMIT_SIZE, &request, &negotiated);
	if (ret)
		return ret;
	if (readl(mxdev->regs + MXGPU_REG_STATUS) &
		(MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT))
		return -EPIPE;
	mxdev->max_command_bytes = min_t(u32, MXGPU_CMD_BYTES, negotiated.limits.max_command_bytes);
	mxdev->negotiated_caps = negotiated;
	mxgpu_program_queues(mxdev);
	mxdev->queue_live = true;
	mxgpu_completion_irq_enable(mxdev);
	return 0;
}

static void mxgpu_arm_scanout(struct pci_dev *pdev, struct mxgpu_device *mxdev)
{
	resource_size_t phys;
	u32 bytes;

	(void)pdev;
	if (!mxdev->regs || !mxdev->pitch || !mxdev->width || !mxdev->height)
		return;
	mutex_lock(&mxdev->scanout_lock);
	if (mxdev->scanout_stopped)
		goto out;
	mxgpu_cursor_cache_invalidate(mxdev);
	mxdev->cursor_cache_resetting = false;
	phys = mxdev->scanout_phys + mxdev->scanout_front * mxdev->scanout_slot_bytes;
	if (!phys)
		goto out;
	bytes = mxdev->pitch * mxdev->height;
	writel(lower_32_bits(phys), mxdev->regs + MXGPU_APERTURE_BASE + 0x00);
	writel(upper_32_bits(phys), mxdev->regs + MXGPU_APERTURE_BASE + 0x04);
	writel(bytes, mxdev->regs + MXGPU_APERTURE_BASE + 0x08);
	writel(mxdev->pitch, mxdev->regs + MXGPU_APERTURE_BASE + 0x0c);
	writel(mxdev->width, mxdev->regs + MXGPU_APERTURE_BASE + 0x10);
	writel(mxdev->height, mxdev->regs + MXGPU_APERTURE_BASE + 0x14);
	writel(3, mxdev->regs + MXGPU_APERTURE_BASE + 0x18);
	writel(0, mxdev->regs + MXGPU_APERTURE_BASE + 0x1c);
	writel(MXGPU_APERTURE_CONTROL_ARM, mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_CONTROL);
out:
	mutex_unlock(&mxdev->scanout_lock);
}

static int mxgpu_recover(struct mxgpu_device *mxdev)
{
	u32 status;
	unsigned int spins;
	int ret;

	if (!mxdev->regs || !mxdev->dma)
		return -ENODEV;
	status = readl(mxdev->regs + MXGPU_REG_STATUS);
	if (!(status & (MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT)))
		return 0;
	mxdev->queue_live = false;
	mutex_lock(&mxdev->scanout_lock);
	mxgpu_cursor_cache_invalidate(mxdev);
	mxdev->cursor_cache_resetting = true;
	mxdev->present_active = false;
	mutex_unlock(&mxdev->scanout_lock);
	writel(MXGPU_CONTROL_RESET, mxdev->regs + MXGPU_REG_CONTROL);
	WRITE_ONCE(mxdev->transport_generation, mxdev->transport_generation + 1);
	mutex_lock(&mxdev->cleanup_lock);
	if (!mxdev->cleanup_stopped)
		schedule_delayed_work(&mxdev->cleanup_work, 0);
	mutex_unlock(&mxdev->cleanup_lock);
	for (spins = 0; spins < MXGPU_WAIT_SPINS; spins++) {
		status = readl(mxdev->regs + MXGPU_REG_STATUS);
		if (!(status & MXGPU_STATUS_RESET_REQUIRED))
			break;
		usleep_range(500, 1000);
	}
	if (status & MXGPU_STATUS_RESET_REQUIRED)
		return -ETIMEDOUT;
	writel(readl(mxdev->regs + MXGPU_REG_IRQ_STATUS) & MXGPU_IRQ_KNOWN_MASK,
		mxdev->regs + MXGPU_REG_IRQ_STATUS);
	ret = mxgpu_negotiate(to_pci_dev(mxdev->drm.dev), mxdev);
	if (ret)
		return ret;
	mxgpu_arm_scanout(to_pci_dev(mxdev->drm.dev), mxdev);
	return 0;
}

static bool mxgpu_queue_ready(struct mxgpu_device *mxdev, u32 queue, u32 expected)
{
	return readl(mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_USED_TAIL)) == expected ||
	       (readl(mxdev->regs + MXGPU_REG_STATUS) &
		       (MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT)) ||
	       readl(mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_FAULT));
}

static int mxgpu_wait_queue_until(struct mxgpu_device *mxdev, u32 queue, u32 expected,
	ktime_t deadline, unsigned int attempts)
{
	unsigned int attempt;
	for (attempt = 0;
		READ_ONCE(mxdev->irq_registered) || attempt < attempts + MXGPU_FAST_COMPLETE_SPINS;
		attempt++) {
		u32 used = readl(mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_USED_TAIL));
		u32 status;
		if (used == expected)
			return 0;
		status = readl(mxdev->regs + MXGPU_REG_STATUS);
		if (status & (MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT))
			return -EPIPE;
		if (readl(mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_FAULT)))
			return -EPIPE;
		if (ktime_compare(ktime_get(), deadline) >= 0)
			return -ETIMEDOUT;
		if (attempt < MXGPU_FAST_COMPLETE_SPINS)
			usleep_range(50, 100);
		else if (READ_ONCE(mxdev->irq_registered)) {
			ktime_t remaining = ktime_sub(deadline, ktime_get());
			if (ktime_to_ns(remaining) <= 0)
				return -ETIMEDOUT;
			wait_event_hrtimeout(mxdev->completion_wait,
				mxgpu_queue_ready(mxdev, queue, expected),
				ktime_to_ns(remaining) < NSEC_PER_MSEC
					? remaining
					: ns_to_ktime(NSEC_PER_MSEC));
		} else
			usleep_range(1000, 1500);
	}
	return -ETIMEDOUT;
}

static bool mxgpu_extended_wait_opcode(u16 opcode)
{
	return opcode == MXGPU_OP_SHADER_CREATE || opcode == MXGPU_OP_PIPELINE_CREATE ||
	       opcode == MXGPU_OP_TRANSFER_TO_HOST || opcode == MXGPU_OP_TRANSFER_FROM_HOST;
}

static u16 mxgpu_object_destroy_opcode(u16 opcode);

static int mxgpu_pending_complete(struct mxgpu_device *mxdev, u32 queue)
{
	struct mxgpu_pending_ownership *pending = &mxdev->pending_ownership[queue];
	struct mxgpu_completion completion;
	struct mxgpu_context *owner;
	struct mxgpu_object *object, *next;
	u16 destroy;
	u32 encoded_bytes;
	if (!pending->valid)
		return 0;
	dma_rmb();
	if (mxgpu_completion_decode((u8 *)mxdev->dma + mxgpu_completion_off(queue),
		    MXGPU_COMPLETION_SIZE, &completion) ||
		mxgpu_completion_encode(&completion, NULL, 0, &encoded_bytes) ||
		completion.sequence != pending->sequence ||
		completion.device_generation != pending->device_generation ||
		readl(mxdev->regs + MXGPU_REG_DEVICE_GENERATION) != pending->device_generation ||
		pending->transport_generation != mxdev->transport_generation ||
		completion.response_bytes > mxgpu_completion_len(queue) - MXGPU_COMPLETION_SIZE)
		return -EPROTO;
	owner = pending->owner;
	if (owner && (owner->id != pending->context_id ||
			     owner->transport_generation != pending->transport_generation))
		return -EPROTO;
	if (owner) {
		destroy = mxgpu_object_destroy_opcode(pending->opcode);
		list_for_each_entry_safe (object, next, &owner->objects, link) {
			if (object->id != pending->object_id ||
				object->destroy_opcode != (destroy ? destroy : pending->opcode))
				continue;
			if ((destroy && pending->speculative_create && completion.status) ||
				(!destroy && !completion.status)) {
				list_del(&object->link);
				kfree(object);
			}
			break;
		}
		if (pending->opcode == MXGPU_OP_CONTEXT_CREATE && pending->speculative_create)
			owner->host_live = !completion.status;
		else if (pending->opcode == MXGPU_OP_CONTEXT_DESTROY && !completion.status)
			owner->host_live = false;
		if (!completion.status || pending->speculative_create) {
			mutex_lock(&mxdev->cleanup_lock);
			owner->cleanup_blocked = false;
			owner->cleanup_revision++;
			if (!mxdev->cleanup_stopped)
				schedule_delayed_work(&mxdev->cleanup_work, 0);
			mutex_unlock(&mxdev->cleanup_lock);
		}
	}
	pending->owner = NULL;
	pending->valid = false;
	return 0;
}

static int mxgpu_wait_pending(struct mxgpu_device *mxdev, u32 queue, bool sliced)
{
	ktime_t deadline = mxdev->pending_deadline[queue];
	ktime_t slice = ktime_add_ms(ktime_get(), MXGPU_COMPLETE_TIMEOUT_MS);
	bool extended = mxgpu_extended_wait_opcode(mxdev->pending_opcode[queue]);
	int ret;
	if (sliced && ktime_compare(slice, deadline) < 0)
		deadline = slice;
	ret = mxgpu_wait_queue_until(mxdev, queue, mxdev->queue_tail[queue], deadline,
		extended && !sliced ? MXGPU_EXTENDED_SPINS : MXGPU_COMPLETE_SPINS);
	if (ret == -ETIMEDOUT && sliced &&
		ktime_compare(ktime_get(), mxdev->pending_deadline[queue]) < 0)
		return -EAGAIN;
	if (!ret)
		ret = mxgpu_pending_complete(mxdev, queue);
	return ret;
}

static int mxgpu_queue_post_internal(
	struct mxgpu_device *mxdev, u32 queue, const u8 *command, u32 command_bytes, bool cleanup)
{
	struct mxgpu_command_header header;
	const u8 *payload;
	u32 payload_bytes;
	struct mxgpu_descriptor read_desc;
	struct mxgpu_descriptor write_desc;
	struct mxgpu_available_entry available;
	const struct mxgpu_qring *ring;
	u8 *desc;
	u8 *avail;
	u8 *cmd;
	u32 slot;
	u32 len = 0;
	u32 posted;
	int ret;

	if (mxdev->submit_stopped || !mxdev->queue_live || !command || !command_bytes ||
		command_bytes > mxdev->max_command_bytes || queue >= MXGPU_QUEUE_COUNT)
		return -ENODEV;
	if (mxgpu_command_decode(command, command_bytes, mxdev->max_command_bytes, &header,
		    &payload, &payload_bytes) ||
		header.queue != queue)
		return -EINVAL;
	if (mxdev->batch.active) {
		ktime_t deadline = mxdev->batch.deadline;
		if (cleanup) {
			ktime_t slice = ktime_add_ms(ktime_get(), MXGPU_COMPLETE_TIMEOUT_MS);
			if (ktime_compare(slice, deadline) < 0)
				deadline = slice;
		}
		ret = mxgpu_batch_drain(mxdev, deadline);
		if (ret == -ETIMEDOUT && cleanup &&
			ktime_compare(ktime_get(), mxdev->batch.deadline) < 0)
			return -EAGAIN;
		if (ret)
			return ret;
	}
	if (mxdev->wire_sequence[queue] == U64_MAX ||
		(!cleanup && mxdev->wire_sequence[queue] == U64_MAX - 1) ||
		((header.flags & MXGPU_CMD_SIGNAL_FENCE) && mxdev->wire_fence[queue] == U64_MAX))
		return -EOVERFLOW;
	header.sequence = mxdev->wire_sequence[queue] + 1;
	if (header.flags & MXGPU_CMD_SIGNAL_FENCE)
		header.fence_value = mxdev->wire_fence[queue] + 1;
	ring = mxgpu_ring(mxdev, queue);
	if (!ring->desc)
		return -ENODEV;
	/*
	 * The host copies the command after the doorbell. Leave that buffer
	 * unchanged until the used tail reports the copy is done.
	 */
	ret = mxgpu_wait_pending(mxdev, queue, cleanup);
	if (ret)
		return ret;
	if ((readl(mxdev->regs + MXGPU_REG_STATUS) &
		    (MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT)) ||
		readl(mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_FAULT)))
		return -EPIPE;

	cmd = (u8 *)mxdev->dma + mxgpu_command_off(queue);
	if (mxgpu_command_encode(&header, payload, payload_bytes, mxdev->max_command_bytes, cmd,
		    MXGPU_CMD_BYTES, &len))
		return -EINVAL;
	memset((u8 *)mxdev->dma + mxgpu_completion_off(queue), 0, 32);
	memset(&read_desc, 0, sizeof read_desc);
	read_desc.address = mxdev->dma_addr + mxgpu_command_off(queue);
	read_desc.byte_len = command_bytes;
	read_desc.flags = 1;
	read_desc.next = 1;
	memset(&write_desc, 0, sizeof write_desc);
	write_desc.address = mxdev->dma_addr + mxgpu_completion_off(queue);
	write_desc.byte_len = mxgpu_completion_len(queue);
	write_desc.flags = 2;
	write_desc.next = 0;
	desc = (u8 *)mxdev->dma + ring->desc;
	if (mxgpu_descriptor_encode(&read_desc, desc, 16, &len))
		return -EINVAL;
	if (mxgpu_descriptor_encode(&write_desc, desc + 16, 16, &len))
		return -EINVAL;
	slot = mxdev->queue_tail[queue] % mxdev->queue_size[queue];
	memset(&available, 0, sizeof available);
	available.descriptor_head = 0;
	avail = (u8 *)mxdev->dma + ring->avail + slot * MXGPU_AVAILABLE_ENTRY_SIZE;
	if (mxgpu_available_encode(&available, avail, MXGPU_AVAILABLE_ENTRY_SIZE, &len))
		return -EINVAL;
	dma_wmb();
	posted = mxdev->queue_tail[queue] + 1;
	mxdev->queue_tail[queue] = posted;
	mxdev->wire_sequence[queue] = header.sequence;
	if (header.flags & MXGPU_CMD_SIGNAL_FENCE)
		mxdev->wire_fence[queue] = header.fence_value;
	mxdev->pending_opcode[queue] = header.opcode;
	mxdev->pending_ownership[queue] = (struct mxgpu_pending_ownership){
		.sequence = header.sequence,
		.transport_generation = mxdev->transport_generation,
		.context_id = header.context_id,
		.device_generation = readl(mxdev->regs + MXGPU_REG_DEVICE_GENERATION),
		.opcode = header.opcode,
		.valid = true,
	};
	mxdev->pending_deadline[queue] = ktime_add_ms(
		ktime_get(), mxgpu_extended_wait_opcode(header.opcode) ? MXGPU_EXTENDED_TIMEOUT_MS
								       : MXGPU_COMPLETE_TIMEOUT_MS);
	writel(posted, mxdev->regs + mxgpu_qreg(queue, 0x20));
	ret = mxgpu_wait_pending(mxdev, queue, cleanup);
	if (ret)
		return ret;
	dma_rmb();
	ret = get_unaligned_le32((u8 *)mxdev->dma + mxgpu_completion_off(queue) + 8) ? -EIO : 0;
	writel(posted, mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_USED_CONSUMER));
	return ret;
}

static int mxgpu_queue_post(
	struct mxgpu_device *mxdev, u32 queue, const u8 *command, u32 command_bytes)
{
	return mxgpu_queue_post_internal(mxdev, queue, command, command_bytes, false);
}

static int mxgpu_copy_response(
	struct mxgpu_drm_user *user, const u8 *completion, u32 completion_bytes, u32 response_cap)
{
	u32 response_bytes;

	user->size = 0;
	if (completion_bytes < 32)
		return -EPROTO;
	response_bytes = get_unaligned_le32(completion + 12);
	if (response_bytes > completion_bytes - 32)
		return -EPROTO;
	if (!response_bytes)
		return 0;
	if (response_bytes > user->capacity || (response_cap && response_bytes > response_cap))
		return -ENOSPC;
	if (copy_to_user(u64_to_user_ptr(user->pointer), completion + 32, response_bytes))
		return -EFAULT;
	user->size = response_bytes;
	return 0;
}

static bool mxgpu_context_owned(struct mxgpu_file *priv, u32 id)
{
	struct mxgpu_context *context;

	list_for_each_entry (context, &priv->contexts, link)
		if (context->id == id)
			return true;
	return false;
}

static void mxgpu_context_forget_objects(struct mxgpu_context *context)
{
	struct mxgpu_object *object, *next;
	list_for_each_entry_safe (object, next, &context->objects, link) {
		list_del(&object->link);
		kfree(object);
	}
}

static void mxgpu_context_record(struct mxgpu_device *mxdev, struct mxgpu_file *priv,
	const struct mxgpu_command_header *header, bool success)
{
	struct mxgpu_context *context;
	list_for_each_entry (context, &priv->contexts, link) {
		if (context->id != header->context_id)
			continue;
		if (header->sequence > context->sequence)
			context->sequence = header->sequence;
		if (success && header->opcode == MXGPU_OP_CONTEXT_CREATE) {
			if (context->transport_generation != mxdev->transport_generation)
				mxgpu_context_forget_objects(context);
			context->host_live = true;
			context->transport_generation = mxdev->transport_generation;
		} else if (success && header->opcode == MXGPU_OP_CONTEXT_DESTROY) {
			context->host_live = false;
		}
		break;
	}
}

static void mxgpu_context_free(struct mxgpu_context *context)
{
	mxgpu_context_forget_objects(context);
	kfree(context);
}

static u16 mxgpu_object_destroy_opcode(u16 opcode)
{
	switch (opcode) {
	case MXGPU_OP_RESOURCE_CREATE:
		return MXGPU_OP_RESOURCE_DESTROY;
	case MXGPU_OP_SHADER_CREATE:
		return MXGPU_OP_SHADER_DESTROY;
	case MXGPU_OP_PIPELINE_CREATE:
		return MXGPU_OP_PIPELINE_DESTROY;
	case MXGPU_OP_DEPTH_STENCIL_STATE_CREATE:
		return MXGPU_OP_DEPTH_STENCIL_STATE_DESTROY;
	case MXGPU_OP_BLEND_STATE_CREATE:
		return MXGPU_OP_BLEND_STATE_DESTROY;
	case MXGPU_OP_RASTERIZER_STATE_CREATE:
		return MXGPU_OP_RASTERIZER_STATE_DESTROY;
	case MXGPU_OP_SAMPLER_CREATE:
		return MXGPU_OP_SAMPLER_DESTROY;
	default:
		return 0;
	}
}

static int mxgpu_object_prepare(struct mxgpu_file *priv, const struct mxgpu_command_header *header,
	const u8 *payload, u32 payload_bytes, struct mxgpu_context **owner,
	struct mxgpu_object **candidate, struct mxgpu_object **existing)
{
	struct mxgpu_context *context;
	struct mxgpu_object *object;
	u16 destroy = mxgpu_object_destroy_opcode(header->opcode);
	u32 id;
	*owner = NULL;
	*candidate = NULL;
	*existing = NULL;
	if (!destroy && header->opcode != MXGPU_OP_RESOURCE_DESTROY &&
		header->opcode != MXGPU_OP_SHADER_DESTROY &&
		header->opcode != MXGPU_OP_PIPELINE_DESTROY &&
		header->opcode != MXGPU_OP_DEPTH_STENCIL_STATE_DESTROY &&
		header->opcode != MXGPU_OP_BLEND_STATE_DESTROY &&
		header->opcode != MXGPU_OP_RASTERIZER_STATE_DESTROY &&
		header->opcode != MXGPU_OP_SAMPLER_DESTROY)
		return 0;
	if (payload_bytes < sizeof(u32))
		return -EINVAL;
	id = get_unaligned_le32(payload);
	if (!id)
		return -EINVAL;
	list_for_each_entry (context, &priv->contexts, link) {
		if (context->id != header->context_id)
			continue;
		*owner = context;
		list_for_each_entry (object, &context->objects, link)
			if (object->id == id &&
				object->destroy_opcode == (destroy ? destroy : header->opcode)) {
				*existing = object;
				break;
			}
		if (destroy && !*existing) {
			object = kzalloc(sizeof(*object), GFP_KERNEL);
			if (!object)
				return -ENOMEM;
			object->id = id;
			object->destroy_opcode = destroy;
			*candidate = object;
		}
		return 0;
	}
	return -EACCES;
}

static void mxgpu_object_record(struct mxgpu_device *mxdev, struct mxgpu_context *owner,
	struct mxgpu_object *candidate, struct mxgpu_object *existing,
	const struct mxgpu_command_header *header, int posted, bool advanced)
{
	if (candidate) {
		if (!posted || (advanced && posted != -EIO)) {
			list_add_tail(&candidate->link, &owner->objects);
			owner->host_live = true;
			owner->transport_generation = mxdev->transport_generation;
		} else
			kfree(candidate);
	} else if (existing && !posted && !mxgpu_object_destroy_opcode(header->opcode)) {
		list_del(&existing->link);
		kfree(existing);
	}
}

static void mxgpu_cleanup_enqueue(struct drm_device *dev, struct mxgpu_context *context)
{
	struct mxgpu_device *mxdev = container_of(dev, struct mxgpu_device, drm);
	int idx;
	if (!drm_dev_enter(dev, &idx)) {
		mxgpu_context_free(context);
		return;
	}
	mutex_lock(&mxdev->cleanup_lock);
	if (mxdev->cleanup_stopped)
		mxgpu_context_free(context);
	else {
		list_add_tail(&context->link, &mxdev->cleanup_contexts);
		schedule_delayed_work(&mxdev->cleanup_work, 0);
	}
	mutex_unlock(&mxdev->cleanup_lock);
	drm_dev_exit(idx);
}

static void mxgpu_cleanup_work(struct work_struct *work)
{
	struct mxgpu_device *mxdev =
		container_of(to_delayed_work(work), struct mxgpu_device, cleanup_work);
	struct mxgpu_context *context = NULL, *entry;
	struct mxgpu_object *object = NULL, *item;
	struct mxgpu_command_header header = {0};
	u8 payload[MXGPU_RESOURCE_ID_SIZE],
		command[MXGPU_COMMAND_HEADER_SIZE + MXGPU_RESOURCE_ID_SIZE];
	u32 len, payload_len = 0, previous_tail;
	int ret = 0, idx;
	bool finished = false, pending = false;
	u64 cleanup_revision;
	mutex_lock(&mxdev->cleanup_lock);
	if (mxdev->cleanup_stopped)
		goto unlock;
	list_for_each_entry (entry, &mxdev->cleanup_contexts, link)
		if (!entry->cleanup_blocked ||
			entry->transport_generation != READ_ONCE(mxdev->transport_generation)) {
			context = entry;
			break;
		}
	if (!context)
		goto unlock;
	cleanup_revision = context->cleanup_revision;
	mutex_unlock(&mxdev->cleanup_lock);
	if (!drm_dev_enter(&mxdev->drm, &idx)) {
		finished = true;
		goto complete;
	}
	mutex_lock(&mxdev->submit_lock);
	if (mxdev->submit_stopped) {
		ret = -ENODEV;
		goto submit_unlock;
	}
	if (mxdev->batch.active) {
		ktime_t slice = ktime_add_ms(ktime_get(), MXGPU_COMPLETE_TIMEOUT_MS);
		if (ktime_compare(slice, mxdev->batch.deadline) > 0)
			slice = mxdev->batch.deadline;
		ret = mxgpu_batch_drain(mxdev, slice);
		if (ret == -ETIMEDOUT && ktime_compare(ktime_get(), mxdev->batch.deadline) < 0)
			ret = -EAGAIN;
		if (ret)
			goto submit_unlock;
	}
	if (!context->host_live || context->transport_generation != mxdev->transport_generation) {
		finished = true;
		goto submit_unlock;
	}
	if (!mxdev->queue_live || !mxdev->regs || !mxdev->dma) {
		ret = -ENODEV;
		goto submit_unlock;
	}
	if (mxdev->pending_ownership[MXGPU_QUEUE_CONTROL].valid ||
		readl(mxdev->regs + mxgpu_qreg(MXGPU_QUEUE_CONTROL, MXGPU_QREG_USED_TAIL)) !=
			mxdev->queue_tail[MXGPU_QUEUE_CONTROL]) {
		ret = mxgpu_wait_pending(mxdev, MXGPU_QUEUE_CONTROL, true);
		goto submit_unlock;
	}
	list_for_each_entry (item, &context->objects, link) {
		if (!object || item->destroy_opcode == MXGPU_OP_PIPELINE_DESTROY ||
			(object->destroy_opcode == MXGPU_OP_SHADER_DESTROY &&
				item->destroy_opcode != MXGPU_OP_SHADER_DESTROY))
			object = item;
		if (item->destroy_opcode == MXGPU_OP_PIPELINE_DESTROY)
			break;
	}
	header.opcode = object ? object->destroy_opcode : MXGPU_OP_CONTEXT_DESTROY;
	header.context_id = context->id;
	header.queue = MXGPU_QUEUE_CONTROL;
	header.sequence = 1;
	if (object &&
		mxgpu_resource_id_encode(object->id, payload, sizeof(payload), &payload_len)) {
		ret = -EPROTO;
		goto submit_unlock;
	}
	ret = mxgpu_command_encode(&header, object ? payload : NULL, payload_len,
		mxdev->max_command_bytes, command, sizeof(command), &len);
	previous_tail = mxdev->queue_tail[MXGPU_QUEUE_CONTROL];
	if (!ret)
		ret = mxgpu_queue_post_internal(mxdev, MXGPU_QUEUE_CONTROL, command, len, true);
	if (ret && previous_tail != mxdev->queue_tail[MXGPU_QUEUE_CONTROL] &&
		mxdev->pending_ownership[MXGPU_QUEUE_CONTROL].valid) {
		mxdev->pending_ownership[MXGPU_QUEUE_CONTROL].owner = context;
		mxdev->pending_ownership[MXGPU_QUEUE_CONTROL].object_id = object ? object->id : 0;
	}
	if (!ret) {
		if (object) {
			list_del(&object->link);
			kfree(object);
		} else
			finished = true;
	}
submit_unlock:
	mutex_unlock(&mxdev->submit_lock);
	drm_dev_exit(idx);
complete:
	mutex_lock(&mxdev->cleanup_lock);
	if (finished) {
		list_del(&context->link);
		mxgpu_context_free(context);
	} else if (ret && ret != -EAGAIN && context->cleanup_revision == cleanup_revision) {
		context->cleanup_blocked = true;
		drm_err_ratelimited(&mxdev->drm, "context cleanup deferred after error: %d\n", ret);
	} else
		list_move_tail(&context->link, &mxdev->cleanup_contexts);
	list_for_each_entry (entry, &mxdev->cleanup_contexts, link)
		if (!entry->cleanup_blocked ||
			entry->transport_generation != READ_ONCE(mxdev->transport_generation)) {
			pending = true;
			break;
		}
	if (pending && !mxdev->cleanup_stopped)
		schedule_delayed_work(&mxdev->cleanup_work, msecs_to_jiffies(1));
unlock:
	mutex_unlock(&mxdev->cleanup_lock);
}

static void mxgpu_cleanup_stop(struct mxgpu_device *mxdev)
{
	struct mxgpu_context *context, *next;
	mutex_lock(&mxdev->submit_lock);
	mxdev->submit_stopped = true;
	mutex_lock(&mxdev->cleanup_lock);
	mxdev->cleanup_stopped = true;
	mutex_unlock(&mxdev->cleanup_lock);
	mutex_unlock(&mxdev->submit_lock);
	cancel_delayed_work_sync(&mxdev->cleanup_work);
	mutex_lock(&mxdev->submit_lock);
	memset(mxdev->pending_ownership, 0, sizeof mxdev->pending_ownership);
	memset(&mxdev->batch, 0, sizeof mxdev->batch);
	mutex_lock(&mxdev->cleanup_lock);
	list_for_each_entry_safe (context, next, &mxdev->cleanup_contexts, link) {
		list_del(&context->link);
		mxgpu_context_free(context);
	}
	mutex_unlock(&mxdev->cleanup_lock);
	mutex_unlock(&mxdev->submit_lock);
}

static int mxgpu_batch_collect(struct mxgpu_device *mxdev)
{
	struct mxgpu_batch_pending *batch = &mxdev->batch;
	static const u32 queues[] = {MXGPU_QUEUE_RENDER, MXGPU_QUEUE_TRANSFER};
	u32 q, i, encoded;
	bool pending = false;
	if (!batch->active)
		return 0;
	if (batch->transport_generation != mxdev->transport_generation ||
		readl(mxdev->regs + MXGPU_REG_DEVICE_GENERATION) !=
			batch->response.device_generation)
		return -EPIPE;
	for (q = 0; q < ARRAY_SIZE(queues); q++) {
		u32 queue = queues[q],
		    used = readl(mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_USED_TAIL));
		const struct mxgpu_qring *ring = mxgpu_ring(mxdev, queue);
		if (used - batch->consumed[queue] >
			mxdev->queue_tail[queue] - batch->consumed[queue])
			return -EPROTO;
		while (batch->consumed[queue] != used) {
			struct mxgpu_used_entry entry;
			struct mxgpu_completion completion;
			struct mxgpu_batch_cell *cell;
			struct mxgpu_drm_batch_outcome *outcome;
			u32 slot = batch->consumed[queue] % mxdev->queue_size[queue];
			dma_rmb();
			if (mxgpu_used_decode(
				    (u8 *)mxdev->dma + ring->used + slot * MXGPU_USED_ENTRY_SIZE,
				    MXGPU_USED_ENTRY_SIZE, &entry) ||
				entry.descriptor_head % 2 ||
				entry.descriptor_head / 2 >= batch->response.count ||
				entry.queue != queue ||
				entry.device_generation != batch->response.device_generation ||
				entry.written_bytes != MXGPU_COMPLETION_SIZE)
				return -EPROTO;
			i = entry.descriptor_head / 2;
			cell = &batch->cells[i];
			outcome = &batch->response.outcomes[i];
			if (!cell->posted || cell->queue != queue ||
				outcome->state != MXGPU_DRM_BATCH_PENDING ||
				mxgpu_completion_decode((u8 *)mxdev->dma + MXGPU_BATCH_COMPLETIONS +
								i * MXGPU_COMPLETION_SIZE,
					MXGPU_COMPLETION_SIZE, &completion) ||
				mxgpu_completion_encode(&completion, NULL, 0, &encoded) ||
				completion.sequence != cell->wire_sequence ||
				completion.response_bytes ||
				(!completion.status &&
					completion.completed_fence != cell->wire_fence) ||
				completion.device_generation != batch->response.device_generation)
				return -EPROTO;
			outcome->status = completion.status;
			outcome->state = MXGPU_DRM_BATCH_COMPLETED;
			if (completion.status && batch->owner)
				batch->owner->batch_fault = true;
			batch->consumed[queue]++;
			writel(batch->consumed[queue],
				mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_USED_CONSUMER));
		}
	}
	for (i = 0; i < batch->response.count; i++)
		pending |= batch->response.outcomes[i].state == MXGPU_DRM_BATCH_PENDING;
	if (!pending) {
		if (batch->owner) {
			mutex_lock(&mxdev->cleanup_lock);
			batch->owner->cleanup_blocked = false;
			batch->owner->cleanup_revision++;
			if (!mxdev->cleanup_stopped)
				schedule_delayed_work(&mxdev->cleanup_work, 0);
			mutex_unlock(&mxdev->cleanup_lock);
		}
		batch->owner = NULL;
		batch->active = false;
	}
	return 0;
}

static int mxgpu_batch_drain(struct mxgpu_device *mxdev, ktime_t deadline)
{
	struct mxgpu_batch_pending *batch = &mxdev->batch;
	u32 i;
	int ret;
	if (!batch->active)
		return 0;
	ret = mxgpu_batch_collect(mxdev);
	if (ret || !batch->active)
		return ret;
	for (i = 0; i < batch->response.count; i++) {
		u32 queue = batch->cells[i].queue;
		if (batch->response.outcomes[i].state != MXGPU_DRM_BATCH_PENDING)
			continue;
		ret = mxgpu_wait_queue_until(
			mxdev, queue, mxdev->queue_tail[queue], deadline, MXGPU_EXTENDED_SPINS);
		if (ret)
			return ret;
		ret = mxgpu_batch_collect(mxdev);
		if (ret)
			return ret;
	}
	return batch->active ? -EPROTO : 0;
}

static int mxgpu_batch_accept(struct mxgpu_device *mxdev, u32 queue, ktime_t deadline)
{
	unsigned int attempt = 0;
	while (readl(mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_SUBMISSION_HEAD)) !=
		mxdev->queue_tail[queue]) {
		if ((readl(mxdev->regs + MXGPU_REG_STATUS) &
			    (MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT)) ||
			readl(mxdev->regs + mxgpu_qreg(queue, MXGPU_QREG_FAULT)))
			return -EPIPE;
		if (ktime_compare(ktime_get(), deadline) >= 0)
			return -ETIMEDOUT;
		if (attempt++ < MXGPU_FAST_COMPLETE_SPINS)
			usleep_range(50, 100);
		else
			usleep_range(1000, 1500);
	}
	return 0;
}

static bool mxgpu_batch_object_owned(struct mxgpu_context *owner, u32 id, u16 destroy_opcode)
{
	struct mxgpu_object *object;
	list_for_each_entry (object, &owner->objects, link)
		if (object->id == id && object->destroy_opcode == destroy_opcode)
			return true;
	return false;
}

static int mxgpu_batch_validate_command(struct mxgpu_device *mxdev,
	const struct mxgpu_drm_batch_command *command, struct mxgpu_context *owner,
	struct mxgpu_command_header *header, struct mxgpu_render_extended *render,
	struct mxgpu_execution_binding *bindings)
{
	const u8 *payload;
	u32 bytes;
	struct mxgpu_transfer transfer;
	u32 i;
	if (mxgpu_command_decode(command->command, command->command_bytes, mxdev->max_command_bytes,
		    header, &payload, &bytes) ||
		!header->sequence || header->context_id != owner->id ||
		header->queue != command->queue || header->fence_value != command->fence_value ||
		(header->flags & MXGPU_CMD_RESPONSE_REQUIRED))
		return -EINVAL;
	if (header->sequence == U64_MAX)
		return -EOVERFLOW;
	if (header->opcode == MXGPU_OP_TRANSFER_TO_HOST && command->queue == MXGPU_QUEUE_TRANSFER) {
		if (mxgpu_transfer_decode(payload, bytes, &transfer, NULL))
			return -EINVAL;
		return mxgpu_batch_object_owned(
			       owner, transfer.resource_id, MXGPU_OP_RESOURCE_DESTROY)
			       ? 0
			       : -EACCES;
	}
	if (header->opcode != MXGPU_OP_RENDER_SUBMIT_EXTENDED ||
		command->queue != MXGPU_QUEUE_RENDER ||
		mxgpu_render_extended_decode(payload, bytes, render, bindings, 64) ||
		render->depth_stencil_target_id || render->depth_stencil_state_id ||
		render->vertex_layout_id ||
		mxgpu_render_extended_features(render, mxdev->negotiated_caps.features))
		return -EINVAL;
	if (!mxgpu_batch_object_owned(owner, render->pipeline_id, MXGPU_OP_PIPELINE_DESTROY) ||
		(render->rasterizer_state_id &&
			!mxgpu_batch_object_owned(owner, render->rasterizer_state_id,
				MXGPU_OP_RASTERIZER_STATE_DESTROY)) ||
		(render->blend_state_id && !mxgpu_batch_object_owned(owner, render->blend_state_id,
						   MXGPU_OP_BLEND_STATE_DESTROY)) ||
		(render->index_resource_id &&
			!mxgpu_batch_object_owned(
				owner, render->index_resource_id, MXGPU_OP_RESOURCE_DESTROY)))
		return -EACCES;
	for (i = 0; i < render->color_target_count; i++)
		if (!mxgpu_batch_object_owned(
			    owner, render->color_targets[i].resource_id, MXGPU_OP_RESOURCE_DESTROY))
			return -EACCES;
	for (i = 0; i < render->vertex_buffer_count; i++)
		if (!mxgpu_batch_object_owned(owner, render->vertex_buffers[i].resource_id,
			    MXGPU_OP_RESOURCE_DESTROY))
			return -EACCES;
	for (i = 0; i < render->binding_count; i++)
		if (!mxgpu_batch_object_owned(owner, bindings[i].resource_id,
			    bindings[i].kind == MXGPU_BIND_KIND_SAMPLER
				    ? MXGPU_OP_SAMPLER_DESTROY
				    : MXGPU_OP_RESOURCE_DESTROY))
			return -EACCES;
	return 0;
}

static int mxgpu_batch_post_cell(struct mxgpu_device *mxdev, u32 i,
	const struct mxgpu_drm_batch_command *command, struct mxgpu_command_header *header,
	u32 offset)
{
	struct mxgpu_batch_pending *batch = &mxdev->batch;
	const struct mxgpu_qring *ring = mxgpu_ring(mxdev, command->queue);
	struct mxgpu_descriptor descriptor = {0};
	struct mxgpu_available_entry available = {.descriptor_head = i * 2};
	struct mxgpu_drm_batch_outcome *outcome = &batch->response.outcomes[i];
	struct mxgpu_batch_cell *cell = &batch->cells[i];
	u32 bytes, slot = mxdev->queue_tail[command->queue] % mxdev->queue_size[command->queue];
	u64 logical_sequence = header->sequence, logical_fence = header->fence_value;
	header->sequence = mxdev->wire_sequence[command->queue] + 1;
	if (header->flags & MXGPU_CMD_SIGNAL_FENCE)
		header->fence_value = mxdev->wire_fence[command->queue] + 1;
	if (mxgpu_command_encode(header, command->command + MXGPU_COMMAND_HEADER_SIZE,
		    command->command_bytes - MXGPU_COMMAND_HEADER_SIZE, mxdev->max_command_bytes,
		    (u8 *)mxdev->dma + MXGPU_BATCH_ARENA + offset, command->command_bytes, &bytes))
		return -EINVAL;
	memset((u8 *)mxdev->dma + MXGPU_BATCH_COMPLETIONS + i * MXGPU_COMPLETION_SIZE, 0,
		MXGPU_COMPLETION_SIZE);
	descriptor.address = mxdev->dma_addr + MXGPU_BATCH_ARENA + offset;
	descriptor.byte_len = command->command_bytes;
	descriptor.flags = 1;
	descriptor.next = i * 2 + 1;
	if (mxgpu_descriptor_encode(&descriptor,
		    (u8 *)mxdev->dma + ring->desc + i * 2 * MXGPU_QUEUE_DESCRIPTOR_SIZE,
		    MXGPU_QUEUE_DESCRIPTOR_SIZE, &bytes))
		return -EINVAL;
	descriptor.address = mxdev->dma_addr + MXGPU_BATCH_COMPLETIONS + i * MXGPU_COMPLETION_SIZE;
	descriptor.byte_len = MXGPU_COMPLETION_SIZE;
	descriptor.flags = 2;
	descriptor.next = 0;
	if (mxgpu_descriptor_encode(&descriptor,
		    (u8 *)mxdev->dma + ring->desc + (i * 2 + 1) * MXGPU_QUEUE_DESCRIPTOR_SIZE,
		    MXGPU_QUEUE_DESCRIPTOR_SIZE, &bytes) ||
		mxgpu_available_encode(&available,
			(u8 *)mxdev->dma + ring->avail + slot * MXGPU_AVAILABLE_ENTRY_SIZE,
			MXGPU_AVAILABLE_ENTRY_SIZE, &bytes))
		return -EINVAL;
	*cell = (struct mxgpu_batch_cell){.queue = command->queue,
		.wire_sequence = header->sequence,
		.wire_fence = header->fence_value,
		.command_offset = offset,
		.command_bytes = command->command_bytes,
		.posted = true};
	*outcome = (struct mxgpu_drm_batch_outcome){.state = MXGPU_DRM_BATCH_PENDING,
		.sequence = logical_sequence,
		.fence_value = logical_fence,
		.device_generation = batch->response.device_generation};
	batch->active = true;
	mxdev->wire_sequence[command->queue] = header->sequence;
	if (header->flags & MXGPU_CMD_SIGNAL_FENCE)
		mxdev->wire_fence[command->queue] = header->fence_value;
	if (logical_sequence > batch->owner->sequence)
		batch->owner->sequence = logical_sequence;
	dma_wmb();
	mxdev->queue_tail[command->queue]++;
	writel(mxdev->queue_tail[command->queue], mxdev->regs + mxgpu_qreg(command->queue, 0x20));
	return 0;
}

static int mxgpu_ioctl_get_batch_limits(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_device *mxdev = container_of(dev, struct mxgpu_device, drm);
	struct mxgpu_drm_user *user = data;
	struct mxgpu_drm_batch_limits limits = {.abi_major = MXGPU_DRM_ABI_MAJOR,
		.abi_minor = MXGPU_DRM_ABI_BATCH_MINOR,
		.outcome_bytes = MXGPU_DRM_BATCH_OUTCOME_BYTES};
	u8 *record, expected[MXGPU_DRM_HEADER_BYTES], response[MXGPU_DRM_HEADER_BYTES + 24];
	u32 bytes, expected_bytes, response_bytes;
	int ret, idx;
	(void)file;
	ret = mxgpu_copy_record(user, &record, &bytes);
	if (ret)
		return ret;
	if (mxgpu_drm_get_batch_limits_encode(expected, sizeof expected, &expected_bytes) ||
		bytes != expected_bytes || memcmp(record, expected, bytes)) {
		kfree(record);
		return -EINVAL;
	}
	kfree(record);
	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;
	ret = mutex_lock_interruptible(&mxdev->submit_lock);
	if (ret)
		goto exit;
	if (mxdev->submit_stopped || !mxdev->queue_live) {
		ret = -ENODEV;
		goto unlock;
	}
	if (mxdev->queue_size[MXGPU_QUEUE_RENDER] == MXGPU_BATCH_QSIZE &&
		mxdev->queue_size[MXGPU_QUEUE_TRANSFER] == MXGPU_BATCH_QSIZE) {
		limits.max_commands = MXGPU_DRM_BATCH_MAX_COMMANDS;
		limits.max_command_bytes = MXGPU_DRM_BATCH_MAX_COMMAND_BYTES;
		limits.max_input_bytes = MXGPU_DRM_BATCH_MAX_INPUT_BYTES;
	}
	ret = mxgpu_drm_get_batch_limits_response_encode(
		      &limits, response, sizeof response, &response_bytes)
		      ? -EPROTO
		      : 0;
	if (!ret) {
		if (user->capacity < response_bytes)
			ret = -ENOSPC;
		else if (copy_to_user(u64_to_user_ptr(user->pointer), response, response_bytes))
			ret = -EFAULT;
		else
			user->size = response_bytes;
	}
unlock:
	mutex_unlock(&mxdev->submit_lock);
exit:
	drm_dev_exit(idx);
	return ret;
}

static int mxgpu_ioctl_submit_batch(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_device *mxdev = container_of(dev, struct mxgpu_device, drm);
	struct mxgpu_file *priv = file->driver_priv;
	struct mxgpu_drm_user *user = data;
	struct mxgpu_drm_batch *request;
	struct mxgpu_command_header *headers;
	struct mxgpu_render_extended *render;
	struct mxgpu_execution_binding *bindings;
	struct mxgpu_context *owner = NULL, *entry;
	struct mxgpu_drm_batch_response *response;
	u8 *record, *encoded;
	u32 count[MXGPU_QUEUE_COUNT] = {0}, fences[MXGPU_QUEUE_COUNT] = {0};
	u32 bytes, i, offset = 0, previous_queue = MXGPU_QUEUE_COUNT;
	int ret = -EINVAL, idx = -1;
	bool locked = false, file_locked = false, posted = false;
	ktime_t deadline;
	if (!priv || !user->pointer || !user->size || user->size > MXGPU_DRM_BATCH_MAX_INPUT_BYTES)
		return -EINVAL;
	record = memdup_user(u64_to_user_ptr(user->pointer), user->size);
	if (IS_ERR(record))
		return PTR_ERR(record);
	request = kzalloc(sizeof(*request), GFP_KERNEL);
	headers = kcalloc(MXGPU_DRM_BATCH_MAX_COMMANDS, sizeof(*headers), GFP_KERNEL);
	render = kzalloc(sizeof(*render), GFP_KERNEL);
	bindings = kcalloc(64, sizeof(*bindings), GFP_KERNEL);
	response = kzalloc(sizeof(*response), GFP_KERNEL);
	encoded = kmalloc(
		32 + MXGPU_DRM_BATCH_MAX_COMMANDS * MXGPU_DRM_BATCH_OUTCOME_BYTES, GFP_KERNEL);
	if (!request || !headers || !render || !bindings || !response || !encoded) {
		ret = -ENOMEM;
		goto free;
	}
	if (mxgpu_drm_batch_decode(record, user->size, request))
		goto free;
	response->context_id = request->context_id;
	response->count = request->count;
	response->aggregate_result = MXGPU_DRM_BATCH_REJECTED;
	if (user->capacity < 32 + request->count * MXGPU_DRM_BATCH_OUTCOME_BYTES) {
		ret = -ENOSPC;
		goto free;
	}
	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		goto free;
	file_locked = true;
	list_for_each_entry (entry, &priv->contexts, link)
		if (entry->id == request->context_id) {
			owner = entry;
			break;
		}
	if (!owner) {
		ret = -EACCES;
		goto copy;
	}
	if (!drm_dev_enter(dev, &idx)) {
		idx = -1;
		ret = -ENODEV;
		goto copy;
	}
	ret = mutex_lock_interruptible(&mxdev->submit_lock);
	if (ret)
		goto copy;
	locked = true;
	deadline = ktime_add_ms(ktime_get(), MXGPU_EXTENDED_TIMEOUT_MS);
	if (mxdev->submit_stopped || !mxdev->queue_live || owner->batch_fault ||
		!owner->host_live || owner->transport_generation != mxdev->transport_generation) {
		ret = -EPIPE;
		goto copy;
	}
	if (mxdev->queue_size[1] != MXGPU_BATCH_QSIZE ||
		mxdev->queue_size[3] != MXGPU_BATCH_QSIZE) {
		ret = -EOPNOTSUPP;
		goto copy;
	}
	for (i = 0; i < request->count; i++) {
		ret = mxgpu_batch_validate_command(
			mxdev, &request->commands[i], owner, &headers[i], render, bindings);
		if (ret)
			goto copy;
		count[headers[i].queue]++;
		fences[headers[i].queue] += !!(headers[i].flags & MXGPU_CMD_SIGNAL_FENCE);
	}
	for (i = 0; i < MXGPU_QUEUE_COUNT; i++)
		if (count[i] && (mxdev->wire_sequence[i] >= U64_MAX - count[i] ||
					mxdev->wire_fence[i] > U64_MAX - fences[i])) {
			ret = -EOVERFLOW;
			goto copy;
		}
	ret = mxdev->batch.active ? mxgpu_batch_drain(mxdev, deadline) : 0;
	if (ret)
		goto copy;
	for (i = 0; i < MXGPU_QUEUE_COUNT; i++) {
		ktime_t saved;
		if (i != MXGPU_QUEUE_CONTROL && i != MXGPU_QUEUE_RENDER &&
			i != MXGPU_QUEUE_TRANSFER)
			continue;
		saved = mxdev->pending_deadline[i];
		if (ktime_compare(saved, deadline) > 0)
			mxdev->pending_deadline[i] = deadline;
		ret = mxgpu_wait_pending(mxdev, i, false);
		mxdev->pending_deadline[i] = saved;
		if (ret)
			goto copy;
	}
	for (i = 0; i < request->count; i++) {
		ret = mxgpu_batch_validate_command(
			mxdev, &request->commands[i], owner, &headers[i], render, bindings);
		if (ret)
			goto copy;
	}
	if (!owner->host_live || owner->transport_generation != mxdev->transport_generation) {
		ret = -EPIPE;
		goto copy;
	}
	memset(&mxdev->batch, 0, sizeof mxdev->batch);
	mxdev->batch.owner = owner;
	mxdev->batch.deadline = deadline;
	mxdev->batch.transport_generation = mxdev->transport_generation;
	mxdev->batch.response = *response;
	mxdev->batch.response.device_generation = readl(mxdev->regs + MXGPU_REG_DEVICE_GENERATION);
	memcpy(mxdev->batch.consumed, mxdev->queue_tail, sizeof mxdev->batch.consumed);
	for (i = 0; i < request->count; i++) {
		if (previous_queue != MXGPU_QUEUE_COUNT && previous_queue != headers[i].queue) {
			ret = mxgpu_batch_accept(mxdev, previous_queue, deadline);
			if (ret)
				break;
		}
		ret = mxgpu_batch_collect(mxdev);
		if (ret)
			break;
		if (owner->batch_fault) {
			ret = -EIO;
			break;
		}
		if (ktime_compare(ktime_get(), deadline) >= 0 ||
			(readl(mxdev->regs + MXGPU_REG_STATUS) &
				(MXGPU_STATUS_RESET_REQUIRED | MXGPU_STATUS_TRANSPORT_FAULT)) ||
			readl(mxdev->regs + mxgpu_qreg(headers[i].queue, MXGPU_QREG_FAULT))) {
			ret = -ETIMEDOUT;
			break;
		}
		mxdev->batch.owner = owner;
		ret = mxgpu_batch_post_cell(mxdev, i, &request->commands[i], &headers[i], offset);
		if (ret)
			break;
		posted = true;
		offset += request->commands[i].command_bytes;
		previous_queue = headers[i].queue;
	}
	if (!ret)
		ret = mxgpu_batch_drain(mxdev, deadline);
	if (posted) {
		if (ret)
			owner->batch_fault = true;
		for (i = 0; i < request->count; i++)
			if (mxdev->batch.response.outcomes[i].state == MXGPU_DRM_BATCH_COMPLETED &&
				mxdev->batch.response.outcomes[i].status) {
				owner->batch_fault = true;
				if (!ret)
					ret = -EIO;
			}
		mxdev->batch.response.aggregate_result =
			ret ? MXGPU_DRM_BATCH_TERMINAL : MXGPU_DRM_BATCH_COMPLETE;
		*response = mxdev->batch.response;
	} else {
		mxdev->batch.owner = NULL;
		mxdev->batch.active = false;
	}
copy:
	user->size = 0;
	if (mxgpu_drm_batch_response_encode(response, encoded,
		    32 + MXGPU_DRM_BATCH_MAX_COMMANDS * MXGPU_DRM_BATCH_OUTCOME_BYTES, &bytes))
		ret = -EPROTO;
	else if (copy_to_user(u64_to_user_ptr(user->pointer), encoded, bytes)) {
		if (posted)
			owner->batch_fault = true;
		ret = -EFAULT;
	} else
		user->size = bytes;
	if (locked)
		mutex_unlock(&mxdev->submit_lock);
	if (idx >= 0)
		drm_dev_exit(idx);
	if (file_locked)
		mutex_unlock(&priv->lock);
free:
	kfree(encoded);
	kfree(response);
	kfree(bindings);
	kfree(render);
	kfree(headers);
	kfree(request);
	kfree(record);
	return ret;
}

static int mxgpu_ioctl_submit_active(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_device *mxdev = container_of(dev, struct mxgpu_device, drm);
	struct mxgpu_drm_user *user = data;
	struct mxgpu_command_header header;
	u8 *record = NULL;
	u32 record_len = 0;
	u32 context_id, queue, response_cap, command_bytes;
	u64 fence_value;
	const u8 *command = NULL;
	int copied;
	int decoded;

	copied = mxgpu_copy_record(user, &record, &record_len);
	if (copied)
		return copied;
	decoded = mxgpu_drm_submit_decode(record, record_len, &context_id, &queue, &fence_value,
		&response_cap, &command, &command_bytes);
	if (decoded) {
		kfree(record);
		return -EINVAL;
	}
	if (mxgpu_command_decode(command, command_bytes, MXGPU_CMD_BYTES, &header, NULL, NULL)) {
		kfree(record);
		return -EINVAL;
	}
	if (context_id && header.sequence == U64_MAX) {
		kfree(record);
		return -EOVERFLOW;
	}
	if (!context_id) {
		if ((header.opcode != MXGPU_OP_QUERY_ADAPTER &&
			    header.opcode != MXGPU_OP_QUERY_FORMAT_CAPABILITIES) ||
			command_bytes != MXGPU_COMMAND_HEADER_SIZE) {
			kfree(record);
			return -EACCES;
		}
	} else if (!mxgpu_context_owned(file->driver_priv, context_id)) {
		kfree(record);
		return -EACCES;
	}
	if (header.context_id != context_id || header.queue != queue ||
		header.fence_value != fence_value) {
		kfree(record);
		return -EINVAL;
	}
	user->size = 0;
	copied = mutex_lock_interruptible(&mxdev->submit_lock);
	if (copied) {
		kfree(record);
		return copied;
	}
	if (context_id) {
		struct mxgpu_file *priv = file->driver_priv;
		struct mxgpu_context *owned;
		list_for_each_entry (owned, &priv->contexts, link)
			if (owned->id == context_id && owned->batch_fault) {
				mutex_unlock(&mxdev->submit_lock);
				kfree(record);
				return -EPIPE;
			}
	}
	copied = mxdev->submit_stopped ? -ENODEV : mxgpu_recover(mxdev);
	if (copied) {
		mutex_unlock(&mxdev->submit_lock);
		kfree(record);
		return copied;
	}
	if (!context_id && header.opcode == MXGPU_OP_QUERY_FORMAT_CAPABILITIES &&
		mxgpu_format_capabilities_features(mxdev->negotiated_caps.features)) {
		mutex_unlock(&mxdev->submit_lock);
		kfree(record);
		return -EOPNOTSUPP;
	}
	if (mxdev->queue_live) {
		int posted;
		struct mxgpu_context *object_owner = NULL, *context_owner;
		struct mxgpu_object *candidate = NULL, *existing = NULL;
		bool speculative_context = false;
		u32 previous_tail = mxdev->queue_tail[queue];
		bool reserve_present = !mxdev->fb_ram && header.opcode == MXGPU_OP_PRESENT;
		u8 *completion = (u8 *)mxdev->dma + mxgpu_completion_off(queue);

		if (header.opcode == MXGPU_OP_PRESENT) {
			mutex_lock(&mxdev->scanout_lock);
			mxgpu_cursor_cache_invalidate(mxdev);
			mxdev->cursor_cache_inhibited = true;
			mutex_unlock(&mxdev->scanout_lock);
		}
		if (reserve_present) {
			mutex_lock(&mxdev->scanout_lock);
			if (mxdev->scanout_stopped || !list_empty(&mxdev->scanout_jobs)) {
				mutex_unlock(&mxdev->scanout_lock);
				mutex_unlock(&mxdev->submit_lock);
				kfree(record);
				return -EBUSY;
			}
			mxdev->present_pending = true;
			mutex_unlock(&mxdev->scanout_lock);
		}
		posted = mxdev->batch.active ? mxgpu_batch_drain(mxdev, mxdev->batch.deadline) : 0;
		if (!posted)
			posted = mxgpu_wait_pending(mxdev, queue, false);
		{
			struct mxgpu_file *priv = file->driver_priv;
			list_for_each_entry (context_owner, &priv->contexts, link)
				if (context_owner->id == header.context_id) {
					if (context_owner->batch_fault)
						posted = -EPIPE;
					speculative_context = !context_owner->host_live ||
							      context_owner->transport_generation !=
								      mxdev->transport_generation;
					break;
				}
		}
		if (!posted)
			posted = mxgpu_object_prepare(file->driver_priv, &header,
				command + MXGPU_COMMAND_HEADER_SIZE,
				command_bytes - MXGPU_COMMAND_HEADER_SIZE, &object_owner,
				&candidate, &existing);
		if (!posted) {
			posted = mxgpu_queue_post(mxdev, queue, command, command_bytes);
			mxgpu_object_record(mxdev, object_owner, candidate, existing, &header,
				posted, mxdev->queue_tail[queue] != previous_tail);
		}
		mxgpu_context_record(mxdev, file->driver_priv, &header,
			!posted || (header.opcode == MXGPU_OP_CONTEXT_CREATE && posted != -EIO &&
					   mxdev->queue_tail[queue] != previous_tail));
		if (posted && mxdev->queue_tail[queue] != previous_tail &&
			mxdev->pending_ownership[queue].valid) {
			struct mxgpu_pending_ownership *pending = &mxdev->pending_ownership[queue];
			struct mxgpu_context *owned;
			if (object_owner) {
				pending->owner = object_owner;
				pending->object_id =
					get_unaligned_le32(command + MXGPU_COMMAND_HEADER_SIZE);
				pending->speculative_create = candidate != NULL;
			} else if (header.opcode == MXGPU_OP_CONTEXT_CREATE ||
				   header.opcode == MXGPU_OP_CONTEXT_DESTROY) {
				struct mxgpu_file *priv = file->driver_priv;
				list_for_each_entry (owned, &priv->contexts, link)
					if (owned->id == header.context_id) {
						pending->owner = owned;
						pending->speculative_create =
							header.opcode == MXGPU_OP_CONTEXT_CREATE &&
							speculative_context;
						break;
					}
			}
		}
		if (reserve_present) {
			mutex_lock(&mxdev->scanout_lock);
			mxdev->present_pending = false;
			mxdev->present_active = !posted && list_empty(&mxdev->scanout_jobs);
			if (!list_empty(&mxdev->scanout_jobs))
				schedule_delayed_work(&mxdev->scanout_work, 0);
			mutex_unlock(&mxdev->scanout_lock);
		}
		if (posted == -EIO)
			user->size = get_unaligned_le32(completion + 8);
		if (!posted) {
			if (header.opcode == MXGPU_OP_PRESENT && !reserve_present) {
				mutex_lock(&mxdev->scanout_lock);
				mxdev->present_active = true;
				mutex_unlock(&mxdev->scanout_lock);
			}
			posted = mxgpu_copy_response(
				user, completion, mxgpu_completion_len(queue), response_cap);
		}
		mutex_unlock(&mxdev->submit_lock);
		kfree(record);
		if (posted)
			return posted;
	} else {
		mutex_unlock(&mxdev->submit_lock);
		kfree(record);
		return -ENODEV;
	}
	return mxgpu_signal(mxdev);
}

static int mxgpu_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_file *priv = file->driver_priv;
	int idx, ret;

	if (!priv)
		return -EACCES;
	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;
	if (!drm_dev_enter(dev, &idx)) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = mxgpu_ioctl_submit_active(dev, data, file);
	drm_dev_exit(idx);
unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static int mxgpu_file_open(struct drm_device *dev, struct drm_file *file)
{
	struct mxgpu_file *priv = kzalloc(sizeof(*priv), GFP_KERNEL);

	(void)dev;
	if (!priv)
		return -ENOMEM;
	mutex_init(&priv->lock);
	INIT_LIST_HEAD(&priv->contexts);
	file->driver_priv = priv;
	return 0;
}

static void mxgpu_file_close(struct drm_device *dev, struct drm_file *file)
{
	struct mxgpu_file *priv = file->driver_priv;
	struct mxgpu_context *context, *next;
	if (!priv)
		return;
	list_for_each_entry_safe (context, next, &priv->contexts, link) {
		list_del(&context->link);
		mxgpu_cleanup_enqueue(dev, context);
	}
	mutex_destroy(&priv->lock);
	kfree(priv);
	file->driver_priv = NULL;
}

static int mxgpu_ioctl_ctx_create(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_device *mxdev = container_of(dev, struct mxgpu_device, drm);
	struct mxgpu_file *priv = file->driver_priv;
	struct mxgpu_drm_user *user = data;
	struct mxgpu_context *context;
	u8 *record = NULL;
	u8 request[MXGPU_DRM_HEADER_BYTES], response[MXGPU_DRM_HEADER_BYTES + 8];
	u32 record_len = 0, request_len = 0, response_len = 0;
	int ret, idx;

	ret = mxgpu_copy_record(user, &record, &record_len);
	if (ret)
		return ret;
	ret = mxgpu_drm_ctx_create_encode(request, sizeof(request), &request_len);
	if (ret || record_len != request_len || memcmp(record, request, request_len))
		ret = -EINVAL;
	kfree(record);
	user->size = 0;
	if (ret)
		return ret;
	if (!priv)
		return -EACCES;
	if (user->capacity < sizeof(response))
		return -ENOSPC;
	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	INIT_LIST_HEAD(&context->objects);
	if (!drm_dev_enter(dev, &idx)) {
		ret = -ENODEV;
		goto free_context;
	}
	ret = mutex_lock_interruptible(&mxdev->submit_lock);
	if (ret)
		goto exit;
	if (mxdev->next_context == U32_MAX)
		ret = -ENOSPC;
	else
		context->id = ++mxdev->next_context;
	mutex_unlock(&mxdev->submit_lock);
	if (ret)
		goto exit;
	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		goto exit;
	ret = mxgpu_drm_ctx_create_response_encode(
		context->id, response, sizeof(response), &response_len);
	if (ret)
		ret = -EINVAL;
	else if (copy_to_user(u64_to_user_ptr(user->pointer), response, response_len))
		ret = -EFAULT;
	else {
		list_add_tail(&context->link, &priv->contexts);
		user->size = response_len;
		context = NULL;
	}
	mutex_unlock(&priv->lock);
exit:
	drm_dev_exit(idx);
free_context:
	kfree(context);
	return ret;
}

static int mxgpu_ioctl_ctx_destroy(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_file *priv = file->driver_priv;
	struct mxgpu_drm_user *user = data;
	struct mxgpu_context *context, *next;
	u8 *record = NULL;
	u32 record_len = 0, id;
	int ret;

	ret = mxgpu_copy_record(user, &record, &record_len);
	if (ret)
		return ret;
	ret = mxgpu_drm_ctx_destroy_decode(record, record_len, &id);
	kfree(record);
	user->size = 0;
	if (ret)
		return -EINVAL;
	if (!priv)
		return -EACCES;
	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;
	ret = -EACCES;
	list_for_each_entry_safe (context, next, &priv->contexts, link) {
		if (context->id != id)
			continue;
		list_del(&context->link);
		mxgpu_cleanup_enqueue(dev, context);
		ret = 0;
		break;
	}
	mutex_unlock(&priv->lock);
	return ret;
}

static int mxgpu_ioctl_gem_create(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_drm_user *user = data;
	struct drm_gem_shmem_object *shmem;
	u8 *record = NULL;
	u8 response[MXGPU_DRM_HEADER_BYTES + 8];
	u32 record_len = 0, response_len = 0, handle = 0;
	u64 size;
	int ret, idx;

	ret = mxgpu_copy_record(user, &record, &record_len);
	if (ret)
		return ret;
	ret = mxgpu_drm_gem_create_decode(record, record_len, &size);
	kfree(record);
	user->size = 0;
	if (ret)
		return -EINVAL;
	if (size > MAX_LFS_FILESIZE || size > SIZE_MAX - (PAGE_SIZE - 1))
		return -E2BIG;
	if (user->capacity < sizeof(response))
		return -ENOSPC;
	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;
	shmem = drm_gem_shmem_create(dev, (size_t)size);
	if (IS_ERR(shmem)) {
		ret = PTR_ERR(shmem);
		goto out;
	}
	ret = drm_gem_handle_create(file, &shmem->base, &handle);
	drm_gem_object_put(&shmem->base);
	if (ret)
		goto out;
	ret = mxgpu_drm_gem_create_response_encode(
		handle, response, sizeof(response), &response_len);
	if (ret) {
		ret = -EINVAL;
		goto delete_handle;
	}
	if (copy_to_user(u64_to_user_ptr(user->pointer), response, response_len)) {
		ret = -EFAULT;
		goto delete_handle;
	}
	user->size = response_len;
	goto out;
delete_handle:
	drm_gem_handle_delete(file, handle);
out:
	drm_dev_exit(idx);
	return ret;
}

static int mxgpu_ioctl_gem_close(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_drm_user *user = data;
	u8 *record = NULL;
	u32 record_len = 0, handle;
	int ret;

	(void)dev;
	ret = mxgpu_copy_record(user, &record, &record_len);
	if (ret)
		return ret;
	ret = mxgpu_drm_gem_close_decode(record, record_len, &handle);
	kfree(record);
	user->size = 0;
	if (ret)
		return -EINVAL;
	return drm_gem_handle_delete(file, handle);
}

static int mxgpu_ioctl_passthrough(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct mxgpu_drm_user *user = data;
	u8 *record = NULL;
	u32 record_len = 0;
	int copied = mxgpu_copy_record(user, &record, &record_len);

	if (copied)
		return copied;
	kfree(record);
	return 0;
}

static const struct drm_ioctl_desc mxgpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(MXGPU_GET_BATCH_LIMITS, mxgpu_ioctl_get_batch_limits, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_SUBMIT_BATCH, mxgpu_ioctl_submit_batch, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_GET_INFO, mxgpu_ioctl_get_info, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_CTX_CREATE, mxgpu_ioctl_ctx_create, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_CTX_DESTROY, mxgpu_ioctl_ctx_destroy, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_GEM_CREATE, mxgpu_ioctl_gem_create, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_GEM_CLOSE, mxgpu_ioctl_gem_close, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_SUBMIT, mxgpu_ioctl_submit, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_WAIT, mxgpu_ioctl_passthrough, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_BO_WRITE, mxgpu_ioctl_passthrough, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_BO_READ, mxgpu_ioctl_passthrough, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MXGPU_GET_CAPS, mxgpu_ioctl_get_caps, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(
		MXGPU_GET_TRANSFER_LIMITS, mxgpu_ioctl_get_transfer_limits, DRM_RENDER_ALLOW),
};

static const struct file_operations mxgpu_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = drm_gem_mmap,
	.fop_flags = FOP_UNSIGNED_OFFSET,
};

static const struct drm_driver mxgpu_driver = {
	.open = mxgpu_file_open,
	.postclose = mxgpu_file_close,
	.driver_features =
		DRIVER_MODESET | DRIVER_GEM | DRIVER_RENDER | DRIVER_ATOMIC | DRIVER_SYNCOBJ,
	.major = MXGPU_DRIVER_VERSION_MAJOR,
	.minor = MXGPU_DRIVER_VERSION_MINOR,
	.patchlevel = MXGPU_DRIVER_VERSION_PATCH,
	.name = "mxgpu",
	.desc = "MX GPU",
	.ioctls = mxgpu_ioctls,
	.num_ioctls = ARRAY_SIZE(mxgpu_ioctls),
	.fops = &mxgpu_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static int mxgpu_kms_init(struct mxgpu_device *mxdev)
{
	struct drm_device *drm = &mxdev->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.funcs = &mxgpu_mode_funcs;
	ret = drm_connector_init(
		drm, &mxdev->connector, &mxgpu_connector_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;
	drm_connector_helper_add(&mxdev->connector, &mxgpu_connector_helper);
	ret = drm_simple_display_pipe_init(drm, &mxdev->pipe, &mxgpu_pipe_funcs, mxgpu_formats,
		ARRAY_SIZE(mxgpu_formats), NULL, &mxdev->connector);
	if (ret)
		return ret;
	ret = drm_universal_plane_init(drm, &mxdev->cursor, 1, &mxgpu_cursor_funcs, mxgpu_formats,
		ARRAY_SIZE(mxgpu_formats), NULL, DRM_PLANE_TYPE_CURSOR, "cursor");
	if (ret)
		return ret;
	drm_plane_helper_add(&mxdev->cursor, &mxgpu_cursor_helper);
	mxdev->pipe.crtc.cursor = &mxdev->cursor;
	drm_mode_config_reset(drm);
	return 0;
}

static void mxgpu_read_firmware_mode(struct pci_dev *pdev, struct mxgpu_device *mxdev)
{
	const struct screen_info *si = &sysfb_primary_display.screen;
	u64 base = 0;
	u32 stride = 0;
	u64 slot_bytes;
	resource_size_t bar = pci_resource_start(pdev, MXGPU_FB_BAR);
	resource_size_t bar_len = pci_resource_len(pdev, MXGPU_FB_BAR);

	if (mxdev->regs) {
		base = (u64)readl(mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_POWER_ON_BASE_LOW);
		base |= (u64)readl(
				mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_POWER_ON_BASE_HIGH)
			<< 32;
		stride = readl(mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_POWER_ON_STRIDE);
		mxdev->width = readl(mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_POWER_ON_WIDTH);
		mxdev->height =
			readl(mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_POWER_ON_HEIGHT);
		mxdev->pitch = stride;
	}
	if (!mxdev->width || !mxdev->height) {
		base = __screen_info_lfb_base(si);
		mxdev->width = si->lfb_width;
		mxdev->height = si->lfb_height;
		mxdev->pitch = si->lfb_linelength;
	}
	if (!mxdev->width || !mxdev->height) {
		mxdev->width = 1280;
		mxdev->height = 720;
	}
	if (!mxdev->pitch)
		mxdev->pitch = mxdev->width * 4;
	if (!base)
		base = bar;
	if (mxgpu_scanout_layout(
		    base, bar, bar_len, mxdev->width, mxdev->height, mxdev->pitch, &slot_bytes))
		return;
	mxdev->scanout_phys = base;
	mxdev->scanout_slot_bytes = slot_bytes;
	mxdev->fb = (void __force __iomem *)memremap(base, slot_bytes * 2, MEMREMAP_WB);
	if (!mxdev->fb)
		mxdev->fb = (void __force __iomem *)memremap(base, slot_bytes * 2, MEMREMAP_WC);
	if (mxdev->fb) {
		mxdev->fb_ram = true;
		mxdev->scanout_mapping_off = 0;
		mxdev->scanout_size = (resource_size_t)mxdev->pitch * mxdev->height;
	} else {
		mxdev->fb = pci_ioremap_wc_bar(pdev, MXGPU_FB_BAR);
		if (!mxdev->fb)
			mxdev->fb = pci_iomap(pdev, MXGPU_FB_BAR, 0);
		mxdev->fb_ram = false;
		mxdev->scanout_mapping_off = base - bar;
		mxdev->scanout_size = bar_len;
	}
	mxdev->scanout_off = mxdev->scanout_mapping_off;
	if (mxdev->fb)
		mxdev->scanout = mxdev->fb + mxdev->scanout_mapping_off;
	if (!mxdev->scanout) {
		mxdev->fb = NULL;
		return;
	}
	if (mxdev->regs && base && mxdev->pitch && mxdev->width && mxdev->height) {
		u32 bytes = mxdev->pitch * mxdev->height;

		writel(lower_32_bits(base), mxdev->regs + MXGPU_APERTURE_BASE + 0x00);
		writel(upper_32_bits(base), mxdev->regs + MXGPU_APERTURE_BASE + 0x04);
		writel(bytes, mxdev->regs + MXGPU_APERTURE_BASE + 0x08);
		writel(mxdev->pitch, mxdev->regs + MXGPU_APERTURE_BASE + 0x0c);
		writel(mxdev->width, mxdev->regs + MXGPU_APERTURE_BASE + 0x10);
		writel(mxdev->height, mxdev->regs + MXGPU_APERTURE_BASE + 0x14);
		writel(3, mxdev->regs + MXGPU_APERTURE_BASE + 0x18);
		writel(0, mxdev->regs + MXGPU_APERTURE_BASE + 0x1c);
		writel(1, mxdev->regs + MXGPU_APERTURE_BASE + 0x20);
	}
}

static int mxgpu_quiesce_transport(struct pci_dev *pdev, struct mxgpu_device *mxdev)
{
	unsigned int attempt;

	mxdev->queue_live = false;
	pci_clear_master(pdev);
	if (!mxdev->regs || !mxdev->transport_validated)
		return 0;
	writel(MXGPU_CONTROL_RESET, mxdev->regs + MXGPU_REG_CONTROL);
	for (attempt = 0; attempt < MXGPU_WAIT_SPINS; attempt++) {
		u32 status = readl(mxdev->regs + MXGPU_REG_STATUS);

		if (!(status & (MXGPU_STATUS_NEGOTIATED | MXGPU_STATUS_RESET_REQUIRED |
				       MXGPU_STATUS_TRANSPORT_FAULT)))
			return 0;
		usleep_range(1000, 1500);
	}
	return -ETIMEDOUT;
}

static int mxgpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mxgpu_device *mxdev;
	int ret;
	u32 slot;

	(void)id;
	mxdev = devm_drm_dev_alloc(&pdev->dev, &mxgpu_driver, struct mxgpu_device, drm);
	if (IS_ERR(mxdev))
		return PTR_ERR(mxdev);
	init_waitqueue_head(&mxdev->completion_wait);
	mutex_init(&mxdev->submit_lock);
	mutex_init(&mxdev->scanout_lock);
	mutex_init(&mxdev->cleanup_lock);
	INIT_DELAYED_WORK(&mxdev->cleanup_work, mxgpu_cleanup_work);
	INIT_LIST_HEAD(&mxdev->cleanup_contexts);
	INIT_DELAYED_WORK(&mxdev->scanout_work, mxgpu_scanout_work);
	INIT_LIST_HEAD(&mxdev->scanout_jobs);
	spin_lock_init(&mxdev->fence_lock);
	mxdev->fence_context = dma_fence_context_alloc(1);
	pci_set_drvdata(pdev, mxdev);
	ret = pci_enable_device(pdev);
	if (ret)
		return ret;
	if (!(pci_resource_flags(pdev, MXGPU_REG_BAR) & IORESOURCE_MEM) ||
		pci_resource_len(pdev, MXGPU_REG_BAR) < MXGPU_REGISTER_BAR_SIZE ||
		!(pci_resource_flags(pdev, MXGPU_FB_BAR) & IORESOURCE_MEM) ||
		!pci_resource_len(pdev, MXGPU_FB_BAR)) {
		ret = -ENODEV;
		goto disable;
	}
	ret = pci_request_region(pdev, MXGPU_REG_BAR, "mxgpu-regs");
	if (ret)
		goto disable;
	mxdev->regs = pci_iomap(pdev, MXGPU_REG_BAR, 0);
	if (!mxdev->regs) {
		ret = -ENOMEM;
		goto release_regs_only;
	}
	if (readl(mxdev->regs + MXGPU_REG_MAGIC) != MXGPU_PROTOCOL_MAGIC) {
		ret = -EPROTO;
		goto release_regs_only;
	}
	mxdev->transport_validated = true;
	ret = mxgpu_quiesce_transport(pdev, mxdev);
	if (ret)
		goto release_regs_only;
	writel(0, mxdev->regs + MXGPU_REG_IRQ_MASK);
	mxdev->completion_irq = pdev->irq;
	if (pdev->irq > 0 &&
		!request_irq(pdev->irq, mxgpu_completion_interrupt, IRQF_SHARED, "mxgpu", mxdev))
		WRITE_ONCE(mxdev->irq_registered, true);
	mutex_lock(&mxdev->submit_lock);
	ret = mxgpu_negotiate(pdev, mxdev);
	mutex_unlock(&mxdev->submit_lock);
	if (ret)
		goto release_regs_only;
	if (!(mxdev->negotiated_caps.features & MXGPU_FEAT_SCANOUT_APERTURE) ||
		!(readl(mxdev->regs + MXGPU_APERTURE_BASE + MXGPU_AREG_STATUS) &
			MXGPU_APERTURE_STATUS_SUPPORTED)) {
		ret = -EOPNOTSUPP;
		goto release_regs_only;
	}
	ret = aperture_remove_conflicting_pci_devices(pdev, "mxgpu");
	if (ret)
		goto release_regs_only;
	ret = pci_request_region(pdev, MXGPU_FB_BAR, "mxgpu-fb");
	if (ret)
		goto release_regs_only;
	mxgpu_read_firmware_mode(pdev, mxdev);
	if (!mxdev->scanout) {
		ret = -ENOMEM;
		goto release;
	}
	mxdev->cursor_slots = devm_kcalloc(
		&pdev->dev, 2, sizeof(*mxdev->cursor_slots), GFP_NOWAIT | __GFP_NOWARN);
	if (mxdev->cursor_slots) {
		for (slot = 0; slot < 2; slot++) {
			mxdev->cursor_slots[slot].background =
				devm_kmalloc(&pdev->dev, 64 * 64 * 4, GFP_NOWAIT | __GFP_NOWARN);
			if (!mxdev->cursor_slots[slot].background) {
				mxdev->cursor_slots = NULL;
				break;
			}
		}
	}
	ret = mxgpu_kms_init(mxdev);
	if (ret)
		goto unmap;
	mxgpu_arm_scanout(pdev, mxdev);
	ret = drm_dev_register(&mxdev->drm, 0);
	if (ret)
		goto unmap;
	mxdev->connector.status = connector_status_connected;
	drm_client_setup(&mxdev->drm, NULL);
	return 0;
unmap:
	mxgpu_scanout_stop(mxdev);
	mxgpu_completion_irq_stop(mxdev);
	mxgpu_quiesce_transport(pdev, mxdev);
	if (mxdev->fb_ram)
		memunmap((void __force *)mxdev->fb);
	else
		pci_iounmap(pdev, mxdev->fb);
	mxdev->fb = NULL;
	mxdev->scanout = NULL;
release:
	pci_release_region(pdev, MXGPU_FB_BAR);
release_regs_only:
	mxgpu_completion_irq_stop(mxdev);
	if (mxdev->regs) {
		mxgpu_quiesce_transport(pdev, mxdev);
		pci_iounmap(pdev, mxdev->regs);
		mxdev->regs = NULL;
	}
	pci_release_region(pdev, MXGPU_REG_BAR);
disable:
	mxgpu_cleanup_stop(mxdev);
	pci_clear_master(pdev);
	if (mxdev->dma) {
		dma_free_coherent(&pdev->dev, MXGPU_DMA_BYTES, mxdev->dma, mxdev->dma_addr);
		mxdev->dma = NULL;
	}
	mutex_destroy(&mxdev->cleanup_lock);
	mutex_destroy(&mxdev->scanout_lock);
	mutex_destroy(&mxdev->submit_lock);
	pci_disable_device(pdev);
	return ret;
}

static void mxgpu_remove(struct pci_dev *pdev)
{
	struct mxgpu_device *mxdev = pci_get_drvdata(pdev);

	if (!mxdev)
		return;
	mxgpu_cleanup_stop(mxdev);
	mxgpu_completion_irq_stop(mxdev);
	mxgpu_scanout_stop(mxdev);
	drm_dev_unplug(&mxdev->drm);
	drm_atomic_helper_shutdown(&mxdev->drm);
	mxgpu_quiesce_transport(pdev, mxdev);
	mutex_destroy(&mxdev->cleanup_lock);
	mutex_destroy(&mxdev->scanout_lock);
	mutex_destroy(&mxdev->submit_lock);
	if (mxdev->dma)
		dma_free_coherent(&pdev->dev, MXGPU_DMA_BYTES, mxdev->dma, mxdev->dma_addr);
	if (mxdev->fb) {
		if (mxdev->fb_ram)
			memunmap((void __force *)mxdev->fb);
		else
			pci_iounmap(pdev, mxdev->fb);
	}
	if (mxdev->regs)
		pci_iounmap(pdev, mxdev->regs);
	pci_release_region(pdev, MXGPU_FB_BAR);
	pci_release_region(pdev, MXGPU_REG_BAR);
	pci_disable_device(pdev);
}

static const struct pci_device_id mxgpu_pci_ids[] = {{PCI_DEVICE(0x4d58, 0x4750)}, {}};
MODULE_DEVICE_TABLE(pci, mxgpu_pci_ids);

static struct pci_driver mxgpu_pci = {
	.name = "mxgpu",
	.id_table = mxgpu_pci_ids,
	.probe = mxgpu_probe,
	.remove = mxgpu_remove,
};

static int __init mxgpu_init(void)
{
	return pci_register_driver(&mxgpu_pci);
}

static void __exit mxgpu_exit(void)
{
	pci_unregister_driver(&mxgpu_pci);
}

module_init(mxgpu_init);
module_exit(mxgpu_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zak Noble-Clarke");
MODULE_DESCRIPTION("MX GPU");
MODULE_VERSION(MXGPU_DRIVER_VERSION);
