/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include "anlabwc-embed.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/backend/android.h>
#include <wlr/util/log.h>
#include <wlr/version.h>
#include "common/font.h"
#include "common/fd-util.h"
#include "common/macros.h"
#include "common/string-helpers.h"
#include "config/rcxml.h"
#include "config/session.h"
#include "labwc.h"
#include "menu/menu.h"
#include "theme.h"

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#endif

enum {
	EMBED_PTR_MOTION = 1,
	EMBED_PTR_BUTTON,
	EMBED_KEY,
	EMBED_REDRAW,
};

struct embed_msg {
	uint32_t type;
	uint32_t button;
	uint32_t keycode;
	int32_t pressed;
	float x;
	float y;
};

static char wayland_socket_path[512];
static volatile bool running;

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
		break;
	case EMBED_PTR_BUTTON:
		wlr_android_pointer_motion(server.embed.android, msg.x, msg.y);
		wlr_android_pointer_button(server.embed.android, msg.button,
			msg.pressed != 0);
		break;
	case EMBED_KEY:
		wlr_android_keyboard_key(server.embed.android, msg.keycode,
			msg.pressed != 0);
		break;
	case EMBED_REDRAW:
		wlr_android_schedule_frame(server.embed.android);
		break;
	default:
		break;
	}
	return 0;
}

ANLABWC_API int
anlabwc_pointer(float x, float y, int button, int pressed)
{
	struct embed_msg msg = {
		.type = pressed < 0 ? EMBED_PTR_MOTION : EMBED_PTR_BUTTON,
		.button = button > 0 ? (uint32_t)button : 0x110u,
		.pressed = pressed,
		.x = x,
		.y = y,
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
anlabwc_present_ahb(struct AHardwareBuffer *ahb, int x, int y, int w, int h)
{
	if (!ahb || w <= 0 || h <= 0 || !server.embed.android) {
		return -1;
	}
	wlr_android_present_ahb(server.embed.android, ahb, x, y, w, h);
	struct embed_msg msg = { .type = EMBED_REDRAW };
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
	setenv("WLR_RENDERER", "pixman", 1);
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
