#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
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

static bool output_test(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	(void)wlr_output;
	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) &&
			!wlr_buffer_get_ahb(state->buffer)) {
		return false;
	}
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
			state->layers[i].accepted = false;
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
	}

	if (backend->window && (state->committed & WLR_OUTPUT_STATE_BUFFER) && state->buffer) {
		if (!backend->renderer ||
				!android_renderer_present(backend->renderer, state->buffer)) {
			return false;
		}
	}

	if (output_pending_enabled(wlr_output, state)) {
		struct wlr_output_event_present present_event = {
			.commit_seq = wlr_output->commit_seq + 1,
			.presented = backend->window != NULL,
		};
		output_defer_present(wlr_output, present_event);
		if (backend->frame_timer && backend->window) {
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
	if (backend->window) wlr_output_send_frame(&backend->output);
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
	wlr_output_set_description(&backend->output, "Android ANativeWindow GLES");
	free(backend->pointer.output_name);
	backend->pointer.output_name = strdup("ANLABWC-1");

	backend->frame_timer = wl_event_loop_add_timer(backend->event_loop,
		signal_frame, backend);
	if (!backend->frame_timer) {
		wlr_log(WLR_ERROR, "Failed to add Android output frame timer");
		wlr_output_finish(&backend->output);
		return false;
	}
	return true;
}
