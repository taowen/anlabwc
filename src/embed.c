/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include "anlabwc-embed.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/backend/android.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/util/log.h>
#include <wlr/version.h>
#include <xkbcommon/xkbcommon.h>
#include "common/font.h"
#include "common/fd-util.h"
#include "common/macros.h"
#include "common/string-helpers.h"
#include "config/rcxml.h"
#include "config/session.h"
#include "input/cursor.h"
#include "labwc.h"
#include "menu/menu.h"
#include "theme.h"
#include "view.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <android/native_window.h>
#endif

enum {
	EMBED_PTR_MOTION = 1,
	EMBED_PTR_BUTTON,
	EMBED_KEY,
	EMBED_AXIS,
	EMBED_UNICODE,
	EMBED_WINDOW,
	EMBED_WINDOW_RESIZE_SHAPE,
	EMBED_WINDOW_TRANSFORM,
};

#define EVDEV_LEFTCTRL 29u
#define EVDEV_LEFTSHIFT 42u
#define EVDEV_SPACE 57u
#define EVDEV_U 22u

struct embed_msg {
	uint32_t type;
	uint32_t pointer_id;
	uint32_t button;
	uint32_t keycode;
	int32_t pressed;
	float x;
	float y;
	float x2;
	float y2;
	uintptr_t request;
};

#define EMBED_RIGHT_HAND 0

static void
embed_cursor_init(void)
{
	/* Android draws both logical hand pointers above the compositor Surface. */
	cursor_set_visible(&server.seat, false);
}

static uint32_t
embed_pointer_id(uint32_t pointer_id)
{
	return pointer_id < 2 ? pointer_id : EMBED_RIGHT_HAND;
}

struct window_request {
	struct ANativeWindow *window;
	int width, height, result;
	sem_t done;
	atomic_int refs;
};

struct transform_request {
	int result;
	sem_t done;
	atomic_int refs;
};

static void window_request_release(struct window_request *request) {
	if (atomic_fetch_sub(&request->refs, 1) != 1) return;
	if (request->window) ANativeWindow_release(request->window);
	sem_destroy(&request->done);
	free(request);
}

static void
transform_request_release(struct transform_request *request)
{
	if (atomic_fetch_sub(&request->refs, 1) != 1) {
		return;
	}
	sem_destroy(&request->done);
	free(request);
}

static char wayland_socket_path[512];
static volatile bool running;
static atomic_uint cursor_shape = 1;
static atomic_uint primary_selection_serial;
static atomic_int window_grab_mode;
static atomic_bool window_transform_ready;

struct embed_window_transform {
	bool active;
	struct view *view;
	struct wlr_box initial_box;
	enum lab_edge edges;
	double initial_anchor_x;
	double initial_anchor_y;
	double initial_focus_x;
	double initial_focus_y;
};

static struct embed_window_transform window_transform;

struct embed_cursor_image {
	pthread_mutex_t lock;
	uint32_t *pixels;
	int width;
	int height;
	int hotspot_x;
	int hotspot_y;
	uint32_t serial;
};

static struct embed_cursor_image cursor_image = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

void
anlabwc_embed_set_cursor_image(const uint32_t *pixels, int width, int height,
		int hotspot_x, int hotspot_y)
{
	size_t count = pixels && width > 0 && height > 0
		? (size_t)width * (size_t)height : 0;
	uint32_t *copy = count ? malloc(count * sizeof(*copy)) : NULL;
	if (count && !copy) {
		return;
	}
	if (copy) {
		memcpy(copy, pixels, count * sizeof(*copy));
	}

	pthread_mutex_lock(&cursor_image.lock);
	free(cursor_image.pixels);
	cursor_image.pixels = copy;
	cursor_image.width = count ? width : 0;
	cursor_image.height = count ? height : 0;
	cursor_image.hotspot_x = hotspot_x;
	cursor_image.hotspot_y = hotspot_y;
	cursor_image.serial++;
	pthread_mutex_unlock(&cursor_image.lock);
}

void
anlabwc_embed_set_cursor_shape(uint32_t shape)
{
	atomic_store_explicit(&cursor_shape, shape, memory_order_relaxed);
	anlabwc_embed_set_cursor_image(NULL, 0, 0, 0, 0);
}

ANLABWC_API int
anlabwc_cursor_shape(void)
{
	return (int)atomic_load_explicit(&cursor_shape, memory_order_relaxed);
}

void
anlabwc_embed_note_primary_selection(void)
{
	atomic_fetch_add_explicit(&primary_selection_serial, 1,
		memory_order_relaxed);
}

ANLABWC_API uint32_t
anlabwc_primary_selection_serial(void)
{
	return atomic_load_explicit(&primary_selection_serial,
		memory_order_relaxed);
}

void
anlabwc_embed_set_window_grab(int mode)
{
	atomic_store_explicit(&window_grab_mode, mode, memory_order_relaxed);
	if (!mode) {
		window_transform.active = false;
		window_transform.view = NULL;
	}
}

void
anlabwc_embed_set_window_transform_ready(bool ready)
{
	atomic_store_explicit(&window_transform_ready, ready,
		memory_order_relaxed);
}

ANLABWC_API int
anlabwc_window_grab_active(void)
{
	return atomic_load_explicit(&window_grab_mode,
		memory_order_relaxed) != 0;
}

ANLABWC_API int
anlabwc_window_grab_mode(void)
{
	int mode = atomic_load_explicit(&window_grab_mode, memory_order_relaxed);
	return mode ? mode
		: atomic_load_explicit(&window_transform_ready,
			memory_order_relaxed) ? 1 : 0;
}

static bool
embed_window_transform_begin(float anchor_x, float anchor_y,
	float focus_x, float focus_y)
{
	struct view *view = server.grabbed_view;
	struct cursor_context focus = get_cursor_context_at(focus_x, focus_y);
	enum lab_edge edges = node_type_to_edges(focus.type);
	if (!view || focus.view != view || !edges) {
		return false;
	}
	if (server.input_mode == LAB_INPUT_STATE_PASSTHROUGH
			&& atomic_load_explicit(&window_transform_ready,
				memory_order_relaxed)
			&& server.grabbed_view == view) {
		interactive_begin(view, LAB_INPUT_STATE_MOVE,
			LAB_EDGE_NONE);
	}
	if (server.input_mode != LAB_INPUT_STATE_MOVE
			|| server.grabbed_view != view) {
		return false;
	}
	struct wlr_box box = view->current;
	if (box.width <= 0 || box.height <= 0) {
		return false;
	}
	window_transform.active = true;
	window_transform.view = view;
	window_transform.initial_box = box;
	window_transform.edges = edges;
	window_transform.initial_anchor_x = anchor_x;
	window_transform.initial_anchor_y = anchor_y;
	window_transform.initial_focus_x = focus_x;
	window_transform.initial_focus_y = focus_y;
	return true;
}

static int
embed_resize_shape_at(float x, float y)
{
	struct view *view = server.grabbed_view;
	if (!view || (server.input_mode != LAB_INPUT_STATE_MOVE
			&& !atomic_load_explicit(&window_transform_ready,
				memory_order_relaxed))) {
		return 1;
	}
	struct cursor_context focus = get_cursor_context_at(x, y);
	if (focus.view != view) {
		return 1;
	}
	switch (cursor_get_from_edge(node_type_to_edges(focus.type))) {
	case LAB_CURSOR_RESIZE_NW:
		return 21;
	case LAB_CURSOR_RESIZE_N:
		return 19;
	case LAB_CURSOR_RESIZE_NE:
		return 20;
	case LAB_CURSOR_RESIZE_E:
		return 18;
	case LAB_CURSOR_RESIZE_SE:
		return 23;
	case LAB_CURSOR_RESIZE_S:
		return 22;
	case LAB_CURSOR_RESIZE_SW:
		return 24;
	case LAB_CURSOR_RESIZE_W:
		return 25;
	default:
		return 1;
	}
}

static void
embed_window_transform_update(float anchor_x, float anchor_y,
	float focus_x, float focus_y)
{
	if (!window_transform.active
			|| server.input_mode != LAB_INPUT_STATE_MOVE
			|| server.grabbed_view != window_transform.view) {
		window_transform.active = false;
		window_transform.view = NULL;
		return;
	}
	double anchor_dx = anchor_x - window_transform.initial_anchor_x;
	double anchor_dy = anchor_y - window_transform.initial_anchor_y;
	double resize_dx = focus_x - window_transform.initial_focus_x - anchor_dx;
	double resize_dy = focus_y - window_transform.initial_focus_y - anchor_dy;
	struct wlr_box initial = window_transform.initial_box;
	struct wlr_box box = initial;
	box.x += lround(anchor_dx);
	box.y += lround(anchor_dy);
	if (window_transform.edges & LAB_EDGE_LEFT) {
		box.width = lround(initial.width - resize_dx);
	} else if (window_transform.edges & LAB_EDGE_RIGHT) {
		box.width = lround(initial.width + resize_dx);
	}
	if (window_transform.edges & LAB_EDGE_TOP) {
		box.height = lround(initial.height - resize_dy);
	} else if (window_transform.edges & LAB_EDGE_BOTTOM) {
		box.height = lround(initial.height + resize_dy);
	}
	view_adjust_size(window_transform.view, &box.width, &box.height);
	if (window_transform.edges & LAB_EDGE_LEFT) {
		box.x = lround(initial.x + anchor_dx + initial.width - box.width);
	}
	if (window_transform.edges & LAB_EDGE_TOP) {
		box.y = lround(initial.y + anchor_dy + initial.height - box.height);
	}
	view_move_resize(window_transform.view, box);

	/* Keep the ordinary move grab continuous. When either hand releases,
	 * its synthetic pointer-up first moves the seat to the anchor. Updating
	 * this context prevents that final motion from jumping the window. */
	server.grab_box = box;
	server.grab_x = anchor_x;
	server.grab_y = anchor_y;
	overlay_update(&server.seat);
}

static void
embed_window_transform_end(void)
{
	window_transform.active = false;
	window_transform.view = NULL;
}

ANLABWC_API int
anlabwc_cursor_image(uint32_t *pixels, int capacity, int *width, int *height,
		int *hotspot_x, int *hotspot_y, uint32_t *serial)
{
	pthread_mutex_lock(&cursor_image.lock);
	int count = cursor_image.width * cursor_image.height;
	if (width) {
		*width = cursor_image.width;
	}
	if (height) {
		*height = cursor_image.height;
	}
	if (hotspot_x) {
		*hotspot_x = cursor_image.hotspot_x;
	}
	if (hotspot_y) {
		*hotspot_y = cursor_image.hotspot_y;
	}
	if (serial) {
		*serial = cursor_image.serial;
	}
	if (pixels && capacity >= count && count > 0) {
		memcpy(pixels, cursor_image.pixels, (size_t)count * sizeof(*pixels));
	}
	pthread_mutex_unlock(&cursor_image.lock);
	return count;
}

#ifdef __ANDROID__
static void
android_wlr_log(enum wlr_log_importance importance, const char *fmt, va_list args)
{
	int prio = ANDROID_LOG_INFO;
	if (importance <= WLR_ERROR) {
		prio = ANDROID_LOG_ERROR;
	} else if (importance >= WLR_DEBUG) {
		prio = ANDROID_LOG_DEBUG;
	}
	__android_log_vprint(prio, "anlabwc", fmt, args);
}
#endif

static int
send_msg(const struct embed_msg *msg)
{
	if (server.embed.input_wr < 0) {
		return -1;
	}
	ssize_t n = write(server.embed.input_wr, msg, sizeof(*msg));
	return n == (ssize_t)sizeof(*msg) ? 0 : -1;
}

static void
embed_send_key(uint32_t evdev, bool pressed)
{
	if (!server.embed.android) {
		return;
	}
	wlr_android_keyboard_key(server.embed.android, evdev, pressed);
}

static void
embed_tap_key(uint32_t evdev)
{
	embed_send_key(evdev, true);
	embed_send_key(evdev, false);
}

static uint32_t
embed_hex_key(unsigned int digit)
{
	static const uint32_t letter_keys[] = { 30, 48, 46, 32, 18, 33 };

	if (digit < 10) {
		return digit == 0 ? 11 : digit + 1;
	}
	return digit < 16 ? letter_keys[digit - 10] : 0;
}

/*
 * Fcitx's Unicode addon accepts Ctrl+Shift+U, hexadecimal digits, Space.
 * This is the text transport for codepoints absent from the physical keymap;
 * the Android IME remains responsible for composition and candidate choice.
 */
static void
embed_send_fcitx_unicode(struct wlr_keyboard *kb, uint32_t codepoint)
{
	char hex[9];
	int length = snprintf(hex, sizeof(hex), "%x", codepoint);
	xkb_mod_index_t ctrl_idx = xkb_keymap_mod_get_index(
		kb->keymap, XKB_MOD_NAME_CTRL);
	xkb_mod_index_t shift_idx = xkb_keymap_mod_get_index(
		kb->keymap, XKB_MOD_NAME_SHIFT);
	bool ctrl_down = ctrl_idx != XKB_MOD_INVALID
		&& xkb_state_mod_index_is_active(kb->xkb_state, ctrl_idx,
			XKB_STATE_MODS_DEPRESSED);
	bool shift_down = shift_idx != XKB_MOD_INVALID
		&& xkb_state_mod_index_is_active(kb->xkb_state, shift_idx,
			XKB_STATE_MODS_DEPRESSED);

	if (!ctrl_down) {
		embed_send_key(EVDEV_LEFTCTRL, true);
	}
	if (!shift_down) {
		embed_send_key(EVDEV_LEFTSHIFT, true);
	}
	embed_tap_key(EVDEV_U);
	embed_send_key(EVDEV_LEFTSHIFT, false);
	embed_send_key(EVDEV_LEFTCTRL, false);
	for (int i = 0; i < length; i++) {
		unsigned int digit = hex[i] <= '9'
			? (unsigned int)(hex[i] - '0')
			: (unsigned int)(hex[i] - 'a' + 10);
		embed_tap_key(embed_hex_key(digit));
	}
	embed_tap_key(EVDEV_SPACE);
	if (ctrl_down) {
		embed_send_key(EVDEV_LEFTCTRL, true);
	}
	if (shift_down) {
		embed_send_key(EVDEV_LEFTSHIFT, true);
	}
}

/*
 * Map a Unicode codepoint onto the current XKB layout (level 0 or Shift).
 * IME ASCII/punctuation lives here; CJK is typically not in the US map.
 */
static void
embed_send_unicode(uint32_t codepoint)
{
	struct wlr_keyboard *kb;
	struct xkb_keymap *keymap;
	xkb_keysym_t want;
	xkb_keycode_t min_kc, max_kc, kc;
	xkb_mod_index_t shift_idx;
	bool shift_down;
	int found_evdev = -1;
	bool need_shift = false;

	if (!server.seat.keyboard_group) {
		return;
	}
	kb = &server.seat.keyboard_group->keyboard;
	keymap = kb->keymap;
	if (!keymap || !kb->xkb_state) {
		return;
	}
	want = xkb_utf32_to_keysym(codepoint);
	if (want == XKB_KEY_NoSymbol) {
		return;
	}
	min_kc = xkb_keymap_min_keycode(keymap);
	max_kc = xkb_keymap_max_keycode(keymap);
	for (kc = min_kc; kc <= max_kc; kc++) {
		int level;
		int nlevels = xkb_keymap_num_levels_for_key(keymap, kc, 0);

		for (level = 0; level < nlevels && level < 2; level++) {
			const xkb_keysym_t *syms;
			int n = xkb_keymap_key_get_syms_by_level(keymap, kc, 0,
				(xkb_level_index_t)level, &syms);
			int i;

			for (i = 0; i < n; i++) {
				if (syms[i] != want) {
					continue;
				}
				found_evdev = (int)kc - 8;
				need_shift = level > 0;
				goto found;
			}
		}
	}
found:
	if (found_evdev <= 0) {
		wlr_log(WLR_DEBUG, "embed unicode U+%04X through Fcitx",
			codepoint);
		embed_send_fcitx_unicode(kb, codepoint);
		return;
	}
	shift_idx = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
	shift_down = shift_idx != XKB_MOD_INVALID
		&& xkb_state_mod_index_is_active(kb->xkb_state, shift_idx,
			XKB_STATE_MODS_DEPRESSED);
	if (need_shift && !shift_down) {
		embed_send_key(EVDEV_LEFTSHIFT, true);
	} else if (!need_shift && shift_down) {
		embed_send_key(EVDEV_LEFTSHIFT, false);
	}
	embed_send_key((uint32_t)found_evdev, true);
	embed_send_key((uint32_t)found_evdev, false);
	if (need_shift && !shift_down) {
		embed_send_key(EVDEV_LEFTSHIFT, false);
	} else if (!need_shift && shift_down) {
		embed_send_key(EVDEV_LEFTSHIFT, true);
	}
}

int
anlabwc_embed_input_dispatch(int fd, uint32_t mask, void *data)
{
	(void)mask;
	(void)data;
	struct embed_msg msg;
	ssize_t n = read(fd, &msg, sizeof(msg));
	if (n != (ssize_t)sizeof(msg) || !server.embed.android) {
		return 0;
	}
	switch (msg.type) {
	case EMBED_PTR_MOTION:
		wlr_android_pointer_motion(server.embed.android, msg.x, msg.y);
		cursor_set_visible(&server.seat, false);
		break;
	case EMBED_PTR_BUTTON:
		/* A synthetic motion on release starts titlebar drag bindings even
		 * when the finger did not move, consuming an ordinary button click.
		 * Compare against the seat so virtual-pointer motion is also honored.
		 */
		if (fabs(server.seat.cursor->x - msg.x) > 0.01
				|| fabs(server.seat.cursor->y - msg.y) > 0.01) {
			wlr_android_pointer_motion(server.embed.android, msg.x, msg.y);
		}
		wlr_android_pointer_button(server.embed.android, msg.button,
			msg.pressed != 0);
		cursor_set_visible(&server.seat, false);
		break;
	case EMBED_KEY:
		wlr_android_keyboard_key(server.embed.android, msg.keycode,
			msg.pressed != 0);
		break;
	case EMBED_AXIS:
		wlr_android_pointer_axis(server.embed.android, msg.x, msg.y);
		break;
	case EMBED_UNICODE:
		embed_send_unicode(msg.button);
		break;
	case EMBED_WINDOW: {
		struct window_request *request = (struct window_request *)msg.request;
		request->result = wlr_android_backend_set_window(server.embed.android,
			request->window, request->width, request->height) ? 0 : -1;
		sem_post(&request->done);
		window_request_release(request);
		break;
	}
	case EMBED_WINDOW_RESIZE_SHAPE: {
		struct transform_request *request =
			(struct transform_request *)msg.request;
		request->result = embed_resize_shape_at(msg.x, msg.y);
		sem_post(&request->done);
		transform_request_release(request);
		break;
	}
	case EMBED_WINDOW_TRANSFORM:
		switch (msg.pressed) {
		case 1: {
			struct transform_request *request =
				(struct transform_request *)msg.request;
			request->result = embed_window_transform_begin(
				msg.x, msg.y, msg.x2, msg.y2) ? 0 : -1;
			sem_post(&request->done);
			transform_request_release(request);
			break;
		}
		case 2:
			embed_window_transform_update(msg.x, msg.y, msg.x2, msg.y2);
			break;
		case 3:
			embed_window_transform_end();
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

ANLABWC_API int
anlabwc_pointer(float x, float y, int button, int pressed)
{
	return anlabwc_pointer_v2(EMBED_RIGHT_HAND, x, y, button, pressed);
}

ANLABWC_API int
anlabwc_pointer_v2(int pointer_id, float x, float y, int button, int pressed)
{
	struct embed_msg msg = {
		.type = pressed < 0 ? EMBED_PTR_MOTION : EMBED_PTR_BUTTON,
		.pointer_id = embed_pointer_id((uint32_t)pointer_id),
		.button = button > 0 ? (uint32_t)button : 0x110u,
		.pressed = pressed,
		.x = x,
		.y = y,
	};
	return send_msg(&msg);
}

ANLABWC_API int
anlabwc_window_resize_shape(float x, float y)
{
	struct transform_request *request = calloc(1, sizeof(*request));
	if (!request || sem_init(&request->done, 0, 0) != 0) {
		free(request);
		return 1;
	}
	atomic_init(&request->refs, 2);
	struct embed_msg msg = {
		.type = EMBED_WINDOW_RESIZE_SHAPE,
		.x = x,
		.y = y,
		.request = (uintptr_t)request,
	};
	int result = 1;
	if (send_msg(&msg) == 0) {
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 100 * 1000 * 1000;
		if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000 * 1000 * 1000;
		}
		int rc;
		do {
			rc = sem_timedwait(&request->done, &deadline);
		} while (rc < 0 && errno == EINTR);
		if (rc == 0) {
			result = request->result;
		}
	} else {
		transform_request_release(request);
	}
	transform_request_release(request);
	return result;
}

ANLABWC_API int
anlabwc_window_transform(int action, float anchor_x, float anchor_y,
	float focus_x, float focus_y)
{
	if (action < 1 || action > 3) {
		return -1;
	}
	struct embed_msg msg = {
		.type = EMBED_WINDOW_TRANSFORM,
		.pressed = action,
		.x = anchor_x,
		.y = anchor_y,
		.x2 = focus_x,
		.y2 = focus_y,
	};
	if (action != 1) {
		return send_msg(&msg);
	}
	struct transform_request *request = calloc(1, sizeof(*request));
	if (!request || sem_init(&request->done, 0, 0) != 0) {
		free(request);
		return -1;
	}
	atomic_init(&request->refs, 2);
	msg.request = (uintptr_t)request;
	int result = -1;
	if (send_msg(&msg) == 0) {
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 1;
		int rc;
		do {
			rc = sem_timedwait(&request->done, &deadline);
		} while (rc < 0 && errno == EINTR);
		if (rc == 0) {
			result = request->result;
		}
	} else {
		transform_request_release(request);
	}
	transform_request_release(request);
	return result;
}

ANLABWC_API int
anlabwc_set_window(struct ANativeWindow *window, int width, int height)
{
	struct window_request *request = calloc(1, sizeof(*request));
	if (!request) return -1;
	if (sem_init(&request->done, 0, 0) != 0) { free(request); return -1; }
	atomic_init(&request->refs, 2);
	request->window = window;
	request->width = width;
	request->height = height;
	if (window) ANativeWindow_acquire(window);
	struct embed_msg msg = {.type=EMBED_WINDOW, .request=(uintptr_t)request};
	int result = -1;
	if (send_msg(&msg) == 0) {
		// Acknowledge EGL detachment before Android releases the old Surface.
		// The event-loop reference remains valid even if a stalled GPU times out.
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 5;
		int rc;
		do { rc = sem_timedwait(&request->done, &deadline); } while (rc < 0 && errno == EINTR);
		if (rc == 0) result = request->result;
	} else {
		window_request_release(request);
	}
	window_request_release(request);
	return result;
}

ANLABWC_API int
anlabwc_axis(float dx, float dy)
{
	struct embed_msg msg = {
		.type = EMBED_AXIS,
		.x = dx,
		.y = dy,
	};
	return send_msg(&msg);
}

ANLABWC_API int
anlabwc_key(int evdev, int pressed)
{
	if (evdev <= 0) {
		return -1;
	}
	struct embed_msg msg = {
		.type = EMBED_KEY,
		.keycode = (uint32_t)evdev,
		.pressed = pressed,
	};
	return send_msg(&msg);
}

ANLABWC_API int
anlabwc_unicode(uint32_t codepoint)
{
	if (codepoint == 0 || codepoint > 0x10FFFFu) {
		return -1;
	}
	struct embed_msg msg = {
		.type = EMBED_UNICODE,
		.button = codepoint,
	};
	return send_msg(&msg);
}

ANLABWC_API const char *
anlabwc_wayland_socket(void)
{
	return wayland_socket_path[0] ? wayland_socket_path : NULL;
}

ANLABWC_API void
anlabwc_request_stop(void)
{
	if (server.wl_display) {
		wl_display_terminate(server.wl_display);
	}
}

static void
write_embed_rc(const char *runtime_dir)
{
	char dir[700];
	char path[720];
	FILE *f;

	snprintf(dir, sizeof(dir), "%s/labwc", runtime_dir);
	if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
		wlr_log(WLR_ERROR, "mkdir %s: %s", dir, strerror(errno));
		return;
	}
	snprintf(path, sizeof(path), "%s/rc.xml", dir);
	f = fopen(path, "w");
	if (!f) {
		wlr_log(WLR_ERROR, "write %s: %s", path, strerror(errno));
		return;
	}
	fputs(
		"<?xml version=\"1.0\"?>\n"
		"<labwc_config>\n"
		"  <core>\n"
		"    <decoration>server</decoration>\n"
		"    <xwaylandPersistence>yes</xwaylandPersistence>\n"
		"  </core>\n"
		"  <theme>\n"
		"    <font place=\"ActiveWindow\" name=\"sans\" size=\"12\"/>\n"
		"    <font place=\"InactiveWindow\" name=\"sans\" size=\"12\"/>\n"
		"    <titlebar>\n"
		"      <layout>menu:iconify,max,close</layout>\n"
		"      <showTitle>yes</showTitle>\n"
		"    </titlebar>\n"
		"  </theme>\n"
		"  <mouse>\n"
		"    <default />\n"
		"    <context name=\"Titlebar\">\n"
		"      <mousebind button=\"Left\" action=\"Drag\">\n"
		"        <action name=\"Move\"/>\n"
		"      </mousebind>\n"
		"    </context>\n"
		"  </mouse>\n"
		"</labwc_config>\n", f);
	fclose(f);
	wlr_log(WLR_INFO, "embed rc.xml %s", path);
}

ANLABWC_API int
anlabwc_run(struct ANativeWindow *window, int width, int height,
	const char *runtime_dir)
{
	if (!window || !runtime_dir || width <= 0 || height <= 0) {
		return -1;
	}

#ifdef __ANDROID__
	wlr_log_init(WLR_INFO, android_wlr_log);
#else
	wlr_log_init(WLR_INFO, NULL);
#endif

	server.wlr_version = _LAB_CALC_WLR_VERSION_NUM(
		wlr_version_get_major(),
		wlr_version_get_minor(),
		wlr_version_get_micro()
	);

	setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	setenv("HOME", runtime_dir, 1);
	setenv("XDG_CONFIG_HOME", runtime_dir, 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("XDG_CURRENT_DESKTOP", "wlroots:labwc", 1);

	char xkb[512];
	snprintf(xkb, sizeof(xkb), "%s", runtime_dir);
	char *slash = strrchr(xkb, '/');
	if (slash) {
		snprintf(slash + 1, sizeof(xkb) - (size_t)(slash + 1 - xkb), "xkb");
	} else {
		snprintf(xkb, sizeof(xkb), "xkb");
	}
	setenv("XKB_CONFIG_ROOT", xkb, 1);

	char cache[600];
	snprintf(cache, sizeof(cache), "%s/cache", runtime_dir);
	if (mkdir(cache, 0700) < 0 && errno != EEXIST) {
		wlr_log(WLR_ERROR, "mkdir %s: %s", cache, strerror(errno));
	}
	setenv("XDG_CACHE_HOME", cache, 1);

	char fonts_dir[600];
	snprintf(fonts_dir, sizeof(fonts_dir), "%s/fonts", runtime_dir);
	if (mkdir(fonts_dir, 0700) < 0 && errno != EEXIST) {
		wlr_log(WLR_ERROR, "mkdir %s: %s", fonts_dir, strerror(errno));
	} else {
		char fc_cache[600];
		snprintf(fc_cache, sizeof(fc_cache), "%s/fontconfig", cache);
		if (mkdir(fc_cache, 0700) < 0 && errno != EEXIST) {
			wlr_log(WLR_ERROR, "mkdir %s: %s", fc_cache, strerror(errno));
		}
		char fonts_conf[700];
		snprintf(fonts_conf, sizeof(fonts_conf), "%s/fonts.conf", fonts_dir);
		FILE *fc = fopen(fonts_conf, "w");
		if (fc) {
			fprintf(fc,
				"<?xml version=\"1.0\"?>\n"
				"<fontconfig>\n"
				"  <dir>/system/fonts</dir>\n"
				"  <cachedir>%s</cachedir>\n"
				"  <alias>\n"
				"    <family>sans</family>\n"
				"    <prefer>\n"
				"      <family>Roboto</family>\n"
				"      <family>Noto Sans</family>\n"
				"      <family>MiSans</family>\n"
				"      <family>sans-serif</family>\n"
				"    </prefer>\n"
				"  </alias>\n"
				"  <alias>\n"
				"    <family>sans-serif</family>\n"
				"    <prefer>\n"
				"      <family>Roboto</family>\n"
				"      <family>Noto Sans</family>\n"
				"    </prefer>\n"
				"  </alias>\n"
				"</fontconfig>\n",
				fc_cache);
			fclose(fc);
			setenv("FONTCONFIG_PATH", fonts_dir, 1);
			setenv("FONTCONFIG_FILE", "fonts.conf", 1);
			wlr_log(WLR_INFO, "fontconfig %s (cache %s)",
				fonts_conf, fc_cache);
		} else {
			wlr_log(WLR_ERROR, "write %s: %s", fonts_conf,
				strerror(errno));
		}
	}

	char stale[600];
	snprintf(stale, sizeof(stale), "%s/wayland-0", runtime_dir);
	unlink(stale);
	snprintf(stale, sizeof(stale), "%s/wayland-0.lock", runtime_dir);
	unlink(stale);
	snprintf(wayland_socket_path, sizeof(wayland_socket_path),
		"%s/wayland-0", runtime_dir);

	int fds[2];
	if (pipe(fds) < 0) {
		wlr_log_errno(WLR_ERROR, "embed input pipe");
		return -1;
	}
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	server.embed.native_window = window;
	server.embed.width = width;
	server.embed.height = height;
	server.embed.input_rd = fds[0];
	server.embed.input_wr = fds[1];
	server.embed.android = NULL;

	write_embed_rc(runtime_dir);
	session_environment_init();
	rcxml_read(rc.config_file);

	char pid[32];
	snprintf(pid, sizeof(pid), "%d", getpid());
	setenv("LABWC_PID", pid, 1);
	setenv("LABWC_VER", LABWC_VERSION, 1);

	if (string_null_or_empty(server.title_fmt)) {
		server.title_fmt = "anlabwc";
	}

	increase_nofile_limit();
	server_init();
	server_start();
	embed_cursor_init();

	struct theme theme = { 0 };
	theme_init(&theme, rc.theme_name);
	rc.theme = &theme;
	menu_init();

	running = true;
	wlr_log(WLR_INFO, "anlabwc running on %s (%dx%d)",
		wayland_socket_path, width, height);
	wl_display_run(server.wl_display);
	running = false;

	menu_finish();
	theme_finish(&theme);
	rcxml_finish();
	font_finish();
	server_finish();

	if (server.embed.input_rd >= 0) {
		close(server.embed.input_rd);
		server.embed.input_rd = -1;
	}
	if (server.embed.input_wr >= 0) {
		close(server.embed.input_wr);
		server.embed.input_wr = -1;
	}
	wayland_socket_path[0] = '\0';
	memset(&server, 0, sizeof(server));
	server.embed.input_rd = -1;
	server.embed.input_wr = -1;
	return 0;
}
