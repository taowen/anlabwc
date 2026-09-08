#ifndef BACKEND_ANDROID_H
#define BACKEND_ANDROID_H

#include <wlr/backend/android.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_output.h>

#define ANDROID_DEFAULT_REFRESH (60 * 1000)

struct wlr_android_backend {
	struct wlr_backend backend;
	struct wl_event_loop *event_loop;
	struct wl_listener event_loop_destroy;
	struct ANativeWindow *window;
	int width;
	int height;
	bool started;

	struct wlr_output output;
	struct wl_event_source *frame_timer;
	int frame_delay;

	struct wlr_pointer pointer;
	struct wlr_keyboard keyboard;
	struct wlr_renderer *renderer;
};

struct wlr_android_backend *android_backend_from_backend(
	struct wlr_backend *wlr_backend);
bool android_renderer_present(struct wlr_renderer *renderer, struct wlr_buffer *buffer);
bool android_output_init(struct wlr_android_backend *backend);

#endif
