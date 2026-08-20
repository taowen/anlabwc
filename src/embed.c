/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include "anlabwc-embed.h"

#include <pthread.h>
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
#include <sys/types.h>
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
#include "view.h"
#if HAVE_XWAYLAND
#include "xwayland.h"
#include <wlr/xwayland.h>
#endif

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

enum gpu_bind_kind {
	GPU_BIND_SCREEN = 0,
	GPU_BIND_X11 = ANLABWC_GPU_X11,
	GPU_BIND_WAYLAND = ANLABWC_GPU_WAYLAND,
};

#define GPU_BIND_MAX WLR_ANDROID_AHB_MAX

struct gpu_bind {
	int kind;
	uint32_t id;
	uint32_t pid;
	int buf_w;
	int buf_h;
	int x;
	int y;
	struct AHardwareBuffer *ahb;
};

static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gpu_bind gpu_binds[GPU_BIND_MAX];
static int gpu_n;

static void
gpu_bind_release(struct gpu_bind *b)
{
	if (b->ahb) {
		AHardwareBuffer_release(b->ahb);
		b->ahb = NULL;
	}
}

static int
gpu_bind_store(int kind, uint32_t id, uint32_t pid, int x, int y,
	int buf_w, int buf_h, struct AHardwareBuffer *ahb)
{
	int i;
	struct gpu_bind *slot = NULL;

	if (!ahb || buf_w <= 0 || buf_h <= 0) {
		return -1;
	}
	pthread_mutex_lock(&gpu_lock);
	for (i = 0; i < gpu_n; i++) {
		if (gpu_binds[i].kind == kind && gpu_binds[i].id == id
				&& gpu_binds[i].pid == pid) {
			slot = &gpu_binds[i];
			break;
		}
	}
	if (!slot) {
		if (gpu_n >= GPU_BIND_MAX) {
			pthread_mutex_unlock(&gpu_lock);
			return -1;
		}
		slot = &gpu_binds[gpu_n++];
		memset(slot, 0, sizeof(*slot));
	}
	gpu_bind_release(slot);
	AHardwareBuffer_acquire(ahb);
	slot->kind = kind;
	slot->id = id;
	slot->pid = pid;
	slot->x = x;
	slot->y = y;
	slot->buf_w = buf_w;
	slot->buf_h = buf_h;
	slot->ahb = ahb;
	pthread_mutex_unlock(&gpu_lock);
	return 0;
}

#if HAVE_XWAYLAND
static bool
gpu_xsurface_has_xid(struct wlr_xwayland_surface *xs, uint32_t xid)
{
	struct wlr_xwayland_surface *child;

	if (!xs || !xid) {
		return false;
	}
	if (xs->window_id == xid) {
		return true;
	}
	wl_list_for_each(child, &xs->children, parent_link) {
		if (gpu_xsurface_has_xid(child, xid)) {
			return true;
		}
	}
	return false;
}

static bool
gpu_xs_dest(struct wlr_xwayland_surface *xs, const struct gpu_bind *b,
	int *x, int *y, int *w, int *h)
{
	if (!xs) {
		return false;
	}
	*x = xs->x;
	*y = xs->y;
	*w = xs->width > 0 ? (int)xs->width : b->buf_w;
	*h = xs->height > 0 ? (int)xs->height : b->buf_h;
	return *w > 0 && *h > 0;
}
#endif

static bool
gpu_match_x11_view(struct view *view, uint32_t xid, uint32_t pid, bool xid_only)
{
#if HAVE_XWAYLAND
	struct xwayland_view *xv;
	struct wlr_xwayland_surface *xs;

	if (view->type != LAB_XWAYLAND_VIEW) {
		return false;
	}
	xv = (struct xwayland_view *)view;
	xs = xv->xwayland_surface;
	if (!xs) {
		return false;
	}
	if (gpu_xsurface_has_xid(xs, xid)) {
		return true;
	}
	if (!xid_only && pid && (uint32_t)xs->pid == pid) {
		return true;
	}
#else
	(void)view;
	(void)xid;
	(void)pid;
	(void)xid_only;
#endif
	return false;
}

#if HAVE_XWAYLAND
static bool
gpu_match_unmanaged(struct xwayland_unmanaged *u, uint32_t xid, uint32_t pid,
	bool xid_only)
{
	struct wlr_xwayland_surface *xs = u->xwayland_surface;

	if (!xs) {
		return false;
	}
	if (gpu_xsurface_has_xid(xs, xid)) {
		return true;
	}
	if (!xid_only && pid && (uint32_t)xs->pid == pid) {
		return true;
	}
	return false;
}
#endif

static bool
gpu_view_dest(struct view *view, const struct gpu_bind *b,
	int *x, int *y, int *w, int *h)
{
	if (view->mapped && view->current.width > 0 && view->current.height > 0) {
		*x = view->current.x;
		*y = view->current.y;
		*w = view->current.width;
		*h = view->current.height;
		return true;
	}
#if HAVE_XWAYLAND
	if (view->type == LAB_XWAYLAND_VIEW) {
		struct xwayland_view *xv = (struct xwayland_view *)view;

		if (gpu_xs_dest(xv->xwayland_surface, b, x, y, w, h)) {
			return true;
		}
	}
#endif
	if (view->pending.width > 0 && view->pending.height > 0) {
		*x = view->pending.x;
		*y = view->pending.y;
		*w = view->pending.width;
		*h = view->pending.height;
		return true;
	}
	*x = view->current.x;
	*y = view->current.y;
	*w = b->buf_w;
	*h = b->buf_h;
	return *w > 0 && *h > 0;
}

static bool
gpu_resolve_bind(const struct gpu_bind *b, int *x, int *y, int *w, int *h)
{
	struct view *view;

	if (b->kind == GPU_BIND_SCREEN) {
		*x = b->x;
		*y = b->y;
		*w = b->buf_w;
		*h = b->buf_h;
		return *w > 0 && *h > 0;
	}

	if (b->kind == GPU_BIND_WAYLAND) {
		wl_list_for_each(view, &server.views, link) {
			pid_t vpid;

			if (!view->mapped || view->type != LAB_XDG_SHELL_VIEW
					|| !view->impl || !view->impl->get_pid) {
				continue;
			}
			vpid = view->impl->get_pid(view);
			if (vpid >= 0 && (uint32_t)vpid == b->pid
					&& gpu_view_dest(view, b, x, y, w, h)) {
				return true;
			}
		}
		return false;
	}

	wl_list_for_each(view, &server.views, link) {
		if (gpu_match_x11_view(view, b->id, b->pid, true)
				&& gpu_view_dest(view, b, x, y, w, h)) {
			return true;
		}
	}
#if HAVE_XWAYLAND
	{
		struct xwayland_unmanaged *u;

		wl_list_for_each(u, &server.unmanaged_surfaces, link) {
			if (!gpu_match_unmanaged(u, b->id, b->pid, true)) {
				continue;
			}
			*x = u->xwayland_surface->x;
			*y = u->xwayland_surface->y;
			*w = u->xwayland_surface->width > 0
				? u->xwayland_surface->width : b->buf_w;
			*h = u->xwayland_surface->height > 0
				? u->xwayland_surface->height : b->buf_h;
			return *w > 0 && *h > 0;
		}
	}
#endif
	wl_list_for_each(view, &server.views, link) {
		if (gpu_match_x11_view(view, b->id, b->pid, false)
				&& gpu_view_dest(view, b, x, y, w, h)) {
			return true;
		}
	}
#if HAVE_XWAYLAND
	{
		struct xwayland_unmanaged *u;

		wl_list_for_each(u, &server.unmanaged_surfaces, link) {
			if (!gpu_match_unmanaged(u, b->id, b->pid, false)) {
				continue;
			}
			*x = u->xwayland_surface->x;
			*y = u->xwayland_surface->y;
			*w = u->xwayland_surface->width > 0
				? u->xwayland_surface->width : b->buf_w;
			*h = u->xwayland_surface->height > 0
				? u->xwayland_surface->height : b->buf_h;
			return *w > 0 && *h > 0;
		}
	}
#endif
	return false;
}

void
gpu_overlay_sync(void)
{
	struct wlr_android_ahb_blit slots[GPU_BIND_MAX];
	struct gpu_bind local[GPU_BIND_MAX];
	int n = 0;
	int i;
	int count = 0;

	if (!server.embed.android) {
		return;
	}
	pthread_mutex_lock(&gpu_lock);
	n = gpu_n;
	memcpy(local, gpu_binds, sizeof(local));
	for (i = 0; i < n; i++) {
		if (local[i].ahb) {
			AHardwareBuffer_acquire(local[i].ahb);
		}
	}
	pthread_mutex_unlock(&gpu_lock);

	memset(slots, 0, sizeof(slots));
	for (i = 0; i < n; i++) {
		int x = 0, y = 0, w = 0, h = 0;

		if (!local[i].ahb) {
			continue;
		}
		if (gpu_resolve_bind(&local[i], &x, &y, &w, &h)) {
			slots[count].ahb = local[i].ahb;
			slots[count].x = x;
			slots[count].y = y;
			slots[count].w = w;
			slots[count].h = h;
			count++;
		}
	}
	wlr_android_present_ahb_slots(server.embed.android, slots, count);
	if (count > 0) {
		static int logged;

		if (!logged) {
			wlr_log(WLR_INFO, "GPU AHB bound to %d compositor view(s)",
				count);
			logged = 1;
		}
	} else if (n > 0) {
		static int miss_logged;

		if (miss_logged < 4) {
			wlr_log(WLR_ERROR,
				"GPU AHB unresolved kind=%d id=%u pid=%u %dx%d",
				local[0].kind, local[0].id, local[0].pid,
				local[0].buf_w, local[0].buf_h);
			miss_logged++;
		}
	}
	for (i = 0; i < n; i++) {
		if (local[i].ahb) {
			AHardwareBuffer_release(local[i].ahb);
		}
	}
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
	if (gpu_bind_store(GPU_BIND_SCREEN, 0, 0, x, y, w, h, ahb) != 0) {
		return -1;
	}
	struct embed_msg msg = { .type = EMBED_REDRAW };
	return send_msg(&msg);
}

ANLABWC_API int
anlabwc_present_ahb_view(struct AHardwareBuffer *ahb, int kind, uint32_t id,
	uint32_t pid, int w, int h)
{
	if (!ahb || w <= 0 || h <= 0 || !server.embed.android) {
		return -1;
	}
	if (kind != GPU_BIND_X11 && kind != GPU_BIND_WAYLAND) {
		return -1;
	}
	if (gpu_bind_store(kind, id, pid, 0, 0, w, h, ahb) != 0) {
		return -1;
	}
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
	pthread_mutex_lock(&gpu_lock);
	for (int i = 0; i < gpu_n; i++) {
		gpu_bind_release(&gpu_binds[i]);
	}
	gpu_n = 0;
	pthread_mutex_unlock(&gpu_lock);
	wayland_socket_path[0] = '\0';
	memset(&server, 0, sizeof(server));
	server.embed.input_rd = -1;
	server.embed.input_wr = -1;
	return 0;
}
