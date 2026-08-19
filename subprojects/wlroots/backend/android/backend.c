#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <android/native_window.h>
#include <wayland-server-protocol.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/util/log.h>
#include "backend/android.h"
#include "util/time.h"

static const struct wlr_pointer_impl pointer_impl = {
	.name = "anlabwc-android-pointer",
};

static const struct wlr_keyboard_impl keyboard_impl = {
	.name = "anlabwc-android-keyboard",
};

struct wlr_android_backend *android_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_android(wlr_backend));
	struct wlr_android_backend *backend =
		wl_container_of(wlr_backend, backend, backend);
	return backend;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_android_backend *backend =
		android_backend_from_backend(wlr_backend);
	wlr_log(WLR_INFO, "Starting Android ANativeWindow backend %dx%d",
		backend->width, backend->height);

	wl_signal_emit_mutable(&backend->backend.events.new_input,
		&backend->pointer.base);
	wl_signal_emit_mutable(&backend->backend.events.new_input,
		&backend->keyboard.base);
	wl_signal_emit_mutable(&backend->backend.events.new_output,
		&backend->output);

	backend->started = true;
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	if (!wlr_backend) {
		return;
	}
	struct wlr_android_backend *backend =
		android_backend_from_backend(wlr_backend);

	wlr_backend_finish(wlr_backend);

	if (backend->frame_timer) {
		wlr_output_destroy(&backend->output);
	}
	wlr_pointer_finish(&backend->pointer);
	wlr_keyboard_finish(&backend->keyboard);

	wl_list_remove(&backend->event_loop_destroy.link);
	pthread_mutex_destroy(&backend->overlay.lock);
	if (backend->window) {
		ANativeWindow_release(backend->window);
		backend->window = NULL;
	}
	free(backend);
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct wlr_android_backend *backend =
		wl_container_of(listener, backend, event_loop_destroy);
	backend_destroy(&backend->backend);
}

struct wlr_backend *wlr_android_backend_create(struct wl_event_loop *loop,
		struct ANativeWindow *window, int width, int height) {
	wlr_log(WLR_INFO, "Creating Android ANativeWindow backend");
	if (!window || width <= 0 || height <= 0) {
		wlr_log(WLR_ERROR, "Android backend needs a window and size");
		return NULL;
	}

	struct wlr_android_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_android_backend");
		return NULL;
	}

	wlr_backend_init(&backend->backend, &backend_impl);
	backend->backend.buffer_caps =
		WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_SHM;
	backend->event_loop = loop;
	backend->window = window;
	ANativeWindow_acquire(window);
	backend->width = width;
	backend->height = height;
	backend->frame_delay = 1000000 / ANDROID_DEFAULT_REFRESH;

	backend->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

	wlr_pointer_init(&backend->pointer, &pointer_impl, "anlabwc-android-pointer");
	wlr_keyboard_init(&backend->keyboard, &keyboard_impl,
		"anlabwc-android-keyboard");
	pthread_mutex_init(&backend->overlay.lock, NULL);
	backend->gles.dpy = EGL_NO_DISPLAY;
	backend->gles.ctx = EGL_NO_CONTEXT;
	backend->gles.surf = EGL_NO_SURFACE;
	backend->gles.image = EGL_NO_IMAGE_KHR;

	if (!android_output_init(backend)) {
		backend_destroy(&backend->backend);
		return NULL;
	}

	return &backend->backend;
}

bool wlr_backend_is_android(struct wlr_backend *backend) {
	return backend && backend->impl == &backend_impl;
}

void wlr_android_pointer_motion(struct wlr_backend *wlr_backend,
		double x, double y) {
	struct wlr_android_backend *backend =
		android_backend_from_backend(wlr_backend);
	if (backend->width <= 0 || backend->height <= 0) {
		return;
	}
	struct wlr_pointer_motion_absolute_event event = {
		.pointer = &backend->pointer,
		.time_msec = (uint32_t)get_current_time_msec(),
		.x = x / (double)backend->width,
		.y = y / (double)backend->height,
	};
	wl_signal_emit_mutable(&backend->pointer.events.motion_absolute, &event);
	wl_signal_emit_mutable(&backend->pointer.events.frame, NULL);
}

void wlr_android_pointer_button(struct wlr_backend *wlr_backend,
		uint32_t button, bool pressed) {
	struct wlr_android_backend *backend =
		android_backend_from_backend(wlr_backend);
	struct wlr_pointer_button_event event = {
		.pointer = &backend->pointer,
		.time_msec = (uint32_t)get_current_time_msec(),
		.button = button,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			: WL_POINTER_BUTTON_STATE_RELEASED,
	};
	wlr_pointer_notify_button(&backend->pointer, &event);
	wl_signal_emit_mutable(&backend->pointer.events.frame, NULL);
}

void wlr_android_keyboard_key(struct wlr_backend *wlr_backend,
		uint32_t keycode, bool pressed) {
	struct wlr_android_backend *backend =
		android_backend_from_backend(wlr_backend);
	struct wlr_keyboard_key_event event = {
		.time_msec = (uint32_t)get_current_time_msec(),
		.keycode = keycode,
		.update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			: WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&backend->keyboard, &event);
}
