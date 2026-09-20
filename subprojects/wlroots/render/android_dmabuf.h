/* SPDX-License-Identifier: MIT */
#ifndef RENDER_ANDROID_DMABUF_H
#define RENDER_ANDROID_DMABUF_H
#include <android/hardware_buffer.h>
#include <wlr/render/dmabuf.h>

struct android_dmabuf_bridge;
/* Optional vendor-Vulkan bridge. NULL means that the capability is unavailable. */
struct android_dmabuf_bridge *android_dmabuf_bridge_create(void);
void android_dmabuf_bridge_destroy(struct android_dmabuf_bridge *bridge);
/* Single-plane explicit LINEAR 32-bit RGB only. Copies bytes, not channels.
 * Waits for implicit producer fences and GPU completion. Caller owns the AHB.
 * No CPU pixel mapping or readback. Call only on the renderer thread. */
AHardwareBuffer *android_dmabuf_bridge_copy(struct android_dmabuf_bridge *bridge,
    const struct wlr_dmabuf_attributes *attrs);
#endif
