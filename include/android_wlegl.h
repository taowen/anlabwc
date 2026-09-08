/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_ANDROID_WLEGL_H
#define LABWC_ANDROID_WLEGL_H

struct wl_display;
struct wlr_compositor;
struct wlr_buffer;
struct AHardwareBuffer;

void android_wlegl_create(struct wl_display *display,
	struct wlr_compositor *compositor);
struct AHardwareBuffer *android_wlegl_ahb_from_buffer(struct wlr_buffer *buffer);

#endif
