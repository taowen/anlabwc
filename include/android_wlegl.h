/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_ANDROID_WLEGL_H
#define LABWC_ANDROID_WLEGL_H

struct wl_display;
struct wlr_compositor;
struct wlr_surface;
struct AHardwareBuffer;

void android_wlegl_create(struct wl_display *display,
	struct wlr_compositor *compositor);

/* Compositor-thread-only bindings owned by the actual Wayland surface. */
int gpu_overlay_present_surface(struct wlr_surface *surface,
	struct AHardwareBuffer *ahb, unsigned int pid, int width, int height);
void gpu_overlay_forget_surface(struct wlr_surface *surface);

#endif
