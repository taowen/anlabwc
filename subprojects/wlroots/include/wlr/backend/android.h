/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 *
 * ANativeWindow scanout backend for anlabwc. One output, virtual pointer
 * and keyboard. Pixman compose is uploaded; Gladio/Vortek AHBs are
 * GLES-sampled via EGL_NATIVE_BUFFER_ANDROID.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_ANDROID_H
#define WLR_BACKEND_ANDROID_H

#include <stdbool.h>
#include <stdint.h>
#include <wlr/backend.h>

struct ANativeWindow;
struct AHardwareBuffer;

struct wlr_backend *wlr_android_backend_create(struct wl_event_loop *loop,
	struct ANativeWindow *window, int width, int height);

bool wlr_backend_is_android(struct wlr_backend *backend);

void wlr_android_pointer_motion(struct wlr_backend *backend, double x, double y);
void wlr_android_pointer_button(struct wlr_backend *backend, uint32_t button,
	bool pressed);
void wlr_android_keyboard_key(struct wlr_backend *backend, uint32_t keycode,
	bool pressed);

/* GPU overlay: sample AHB onto the next EGL swap. */
void wlr_android_present_ahb(struct wlr_backend *backend,
	struct AHardwareBuffer *ahb, int x, int y, int w, int h);
void wlr_android_schedule_frame(struct wlr_backend *backend);

#endif
