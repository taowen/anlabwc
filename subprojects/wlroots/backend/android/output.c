#include <assert.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <android/native_window.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/util/log.h>
#include "backend/android.h"
#include "types/wlr_output.h"

static const uint32_t SUPPORTED_OUTPUT_STATE =
	WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER |
	WLR_OUTPUT_STATE_ENABLED |
	WLR_OUTPUT_STATE_MODE;

static struct wlr_android_backend *backend_from_output(
		struct wlr_output *wlr_output) {
	struct wlr_android_backend *backend =
		wl_container_of(wlr_output, backend, output);
	return backend;
}

static void blit_to_window(struct wlr_android_backend *backend,
		const uint8_t *src, uint32_t src_fmt, size_t src_stride,
		int buf_w, int buf_h) {
	ANativeWindow_Buffer dst;
	if (ANativeWindow_lock(backend->window, &dst, NULL) != 0) {
		wlr_log(WLR_ERROR, "ANativeWindow_lock failed");
		return;
	}

	int copy_w = buf_w < dst.width ? buf_w : dst.width;
	int copy_h = buf_h < dst.height ? buf_h : dst.height;
	size_t dst_stride = (size_t)dst.stride * 4;
	uint8_t *d = dst.bits;
	/* pixman ARGB8888 LE is BGRA bytes; Android RGBA_8888 is RGBA bytes. */
	bool swizzle = src_fmt == DRM_FORMAT_ARGB8888
		|| src_fmt == DRM_FORMAT_XRGB8888;

	for (int y = 0; y < copy_h; y++) {
		const uint8_t *srow = src + (size_t)y * src_stride;
		uint8_t *drow = d + (size_t)y * dst_stride;
		if (!swizzle) {
			memcpy(drow, srow, (size_t)copy_w * 4);
			continue;
		}
		for (int x = 0; x < copy_w; x++) {
			drow[x * 4 + 0] = srow[x * 4 + 2];
			drow[x * 4 + 1] = srow[x * 4 + 1];
			drow[x * 4 + 2] = srow[x * 4 + 0];
			drow[x * 4 + 3] = srow[x * 4 + 3];
		}
	}

	ANativeWindow_unlockAndPost(backend->window);
}

static bool output_test(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	(void)wlr_output;
	uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		wlr_log(WLR_DEBUG, "Unsupported output state fields: 0x%" PRIx32,
			unsupported);
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		assert(state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
	}

	if (state->committed & WLR_OUTPUT_STATE_LAYERS) {
		for (size_t i = 0; i < state->layers_len; i++) {
			state->layers[i].accepted = true;
		}
	}

	return true;
}

static bool output_commit(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	struct wlr_android_backend *backend = backend_from_output(wlr_output);

	if (!output_test(wlr_output, state)) {
		return false;
	}

	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		int32_t refresh = state->custom_mode.refresh;
		if (refresh <= 0) {
			refresh = ANDROID_DEFAULT_REFRESH;
		}
		backend->frame_delay = 1000000 / refresh;
		backend->width = state->custom_mode.width;
		backend->height = state->custom_mode.height;
		ANativeWindow_setBuffersGeometry(backend->window,
			backend->width, backend->height, WINDOW_FORMAT_RGBA_8888);
	}

	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) && state->buffer) {
		void *data = NULL;
		uint32_t fmt = 0;
		size_t stride = 0;
		if (wlr_buffer_begin_data_ptr_access(state->buffer,
				WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
			blit_to_window(backend, data, fmt, stride,
				state->buffer->width, state->buffer->height);
			wlr_buffer_end_data_ptr_access(state->buffer);
		} else {
			wlr_log(WLR_ERROR, "Android backend could not map scanout buffer");
		}
	}

	if (output_pending_enabled(wlr_output, state)) {
		struct wlr_output_event_present present_event = {
			.commit_seq = wlr_output->commit_seq + 1,
			.presented = true,
		};
		output_defer_present(wlr_output, present_event);
		if (backend->frame_timer) {
			wl_event_source_timer_update(backend->frame_timer,
				backend->frame_delay);
		}
	}

	return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_android_backend *backend = backend_from_output(wlr_output);

	wlr_output_finish(wlr_output);
	if (backend->frame_timer) {
		wl_event_source_remove(backend->frame_timer);
		backend->frame_timer = NULL;
	}
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
};

static int signal_frame(void *data) {
	struct wlr_android_backend *backend = data;
	wlr_output_send_frame(&backend->output);
	return 0;
}

bool android_output_init(struct wlr_android_backend *backend) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, backend->width, backend->height,
		ANDROID_DEFAULT_REFRESH);

	wlr_output_init(&backend->output, &backend->backend, &output_impl,
		backend->event_loop, &state);
	wlr_output_state_finish(&state);

	wlr_output_set_name(&backend->output, "ANLABWC-1");
	wlr_output_set_description(&backend->output, "Android ANativeWindow");

	backend->frame_timer = wl_event_loop_add_timer(backend->event_loop,
		signal_frame, backend);
	if (!backend->frame_timer) {
		wlr_log(WLR_ERROR, "Failed to add Android output frame timer");
		wlr_output_finish(&backend->output);
		return false;
	}
	return true;
}
