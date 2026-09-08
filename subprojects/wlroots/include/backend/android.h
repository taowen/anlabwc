#ifndef BACKEND_ANDROID_H
#define BACKEND_ANDROID_H

#include <pthread.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <android/hardware_buffer.h>
#include <wlr/backend/android.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_output.h>

#define ANDROID_DEFAULT_REFRESH (60 * 1000)

#define WLR_ANDROID_AHB_MAX 8
#define GLES_AHB_CACHE 4

struct wlr_android_ahb_slot {
	AHardwareBuffer *ahb;
	int x;
	int y;
	int w;
	int h;
};

struct wlr_android_overlay {
	pthread_mutex_t lock;
	struct wlr_android_ahb_slot slots[WLR_ANDROID_AHB_MAX];
	int n;
};

struct wlr_android_gles_ahb {
	AHardwareBuffer *ahb;
	EGLImageKHR image;
	GLuint tex;
};

struct wlr_android_gles {
	bool ok;
	EGLDisplay dpy;
	EGLConfig config;
	EGLContext ctx;
	EGLSurface surf;
	GLuint prog_comp;
	GLuint prog_ahb;
	GLuint tex_comp;
	GLuint tex_ahb;
	GLint a_comp_pos;
	GLint a_comp_uv;
	GLint a_ahb_pos;
	GLint a_ahb_uv;
	EGLImageKHR image;
	AHardwareBuffer *image_ahb;
	struct wlr_android_gles_ahb ahb_cache[GLES_AHB_CACHE];
	int ahb_cache_used;
	int ahb_cache_clock;
	int tex_w;
	int tex_h;
};

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
	struct wlr_android_overlay overlay;
	struct wlr_android_gles gles;
};

struct wlr_android_backend *android_backend_from_backend(
	struct wlr_backend *wlr_backend);
bool android_output_init(struct wlr_android_backend *backend);

#endif
