/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_ANDROID_WLEGL_H
#define LABWC_ANDROID_WLEGL_H

struct wl_display;
struct wlr_compositor;

void android_wlegl_create(struct wl_display *display,
	struct wlr_compositor *compositor);

#endif
