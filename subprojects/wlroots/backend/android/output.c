#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <assert.h>
#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
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

static const char *COMP_VS =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"  v_uv = a_uv;\n"
	"}\n";

static const char *COMP_FS =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec4 c = texture2D(u_tex, v_uv);\n"
	"  gl_FragColor = vec4(c.b, c.g, c.r, c.a);\n"
	"}\n";

static const char *AHB_VS =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"  v_uv = a_uv;\n"
	"}\n";

static const char *AHB_FS =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"uniform samplerExternalOES u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  gl_FragColor = texture2D(u_tex, v_uv);\n"
	"}\n";

static struct wlr_android_backend *backend_from_output(
		struct wlr_output *wlr_output) {
	struct wlr_android_backend *backend =
		wl_container_of(wlr_output, backend, output);
	return backend;
}

static GLuint compile_shader(GLenum type, const char *src) {
	GLuint sh = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[256];
		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "shader compile: %s", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint link_program(const char *vs, const char *fs) {
	GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
	GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
	GLuint p;
	GLint ok = 0;
	if (!v || !f) {
		if (v) {
			glDeleteShader(v);
		}
		if (f) {
			glDeleteShader(f);
		}
		return 0;
	}
	p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glLinkProgram(p);
	glDeleteShader(v);
	glDeleteShader(f);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[256];
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "program link: %s", log);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

static void gles_destroy_image(struct wlr_android_gles *g) {
	int i;

	/* image and tex_ahb are borrowed aliases of cache entries. */
	g->image = EGL_NO_IMAGE_KHR;
	g->image_ahb = NULL;
	for (i = 0; i < g->ahb_cache_used; i++) {
		if (g->ahb_cache[i].image != EGL_NO_IMAGE_KHR && g->dpy != EGL_NO_DISPLAY)
			eglDestroyImageKHR(g->dpy, g->ahb_cache[i].image);
		if (g->ahb_cache[i].tex)
			glDeleteTextures(1, &g->ahb_cache[i].tex);
		g->ahb_cache[i].image = EGL_NO_IMAGE_KHR;
		g->ahb_cache[i].tex = 0;
		if (g->ahb_cache[i].ahb)
			AHardwareBuffer_release(g->ahb_cache[i].ahb);
		g->ahb_cache[i].ahb = NULL;
	}
	g->ahb_cache_used = 0;
	g->ahb_cache_clock = 0;
	g->tex_ahb = 0;
}

static void gles_fini(struct wlr_android_backend *backend) {
	struct wlr_android_gles *g = &backend->gles;
	if (g->dpy == EGL_NO_DISPLAY) {
		return;
	}
	eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx);
	glFinish();
	gles_destroy_image(g);
	if (g->tex_comp) {
		glDeleteTextures(1, &g->tex_comp);
		g->tex_comp = 0;
	}
	if (g->tex_ahb) {
		glDeleteTextures(1, &g->tex_ahb);
		g->tex_ahb = 0;
	}
	if (g->prog_comp) {
		glDeleteProgram(g->prog_comp);
		g->prog_comp = 0;
	}
	if (g->prog_ahb) {
		glDeleteProgram(g->prog_ahb);
		g->prog_ahb = 0;
	}
	eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (g->ctx != EGL_NO_CONTEXT) {
		eglDestroyContext(g->dpy, g->ctx);
		g->ctx = EGL_NO_CONTEXT;
	}
	if (g->surf != EGL_NO_SURFACE) {
		eglDestroySurface(g->dpy, g->surf);
		g->surf = EGL_NO_SURFACE;
	}
	g->ok = false;
}

static bool gles_create_window_surface(struct wlr_android_backend *backend) {
	struct wlr_android_gles *g = &backend->gles;
	EGLint vis = 0;
	EGLint sw = 0, sh = 0;

	if (g->dpy == EGL_NO_DISPLAY || !g->config || g->ctx == EGL_NO_CONTEXT) {
		return false;
	}
	if (g->surf != EGL_NO_SURFACE) {
		eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(g->dpy, g->surf);
		g->surf = EGL_NO_SURFACE;
	}
	if (!eglGetConfigAttrib(g->dpy, g->config, EGL_NATIVE_VISUAL_ID, &vis)) {
		wlr_log(WLR_ERROR, "EGL_NATIVE_VISUAL_ID failed");
		return false;
	}
	/* Keep the Surface size; only the format must match the EGL config.
	 * WINDOW_FORMAT_RGBA_8888 here would force a CPU producer on some GPUs. */
	if (ANativeWindow_setBuffersGeometry(backend->window, 0, 0, vis) != 0) {
		wlr_log(WLR_ERROR, "setBuffersGeometry vis=0x%x failed", vis);
	}
	g->surf = eglCreateWindowSurface(g->dpy, g->config,
		(EGLNativeWindowType)backend->window, NULL);
	if (g->surf == EGL_NO_SURFACE) {
		wlr_log(WLR_ERROR, "eglCreateWindowSurface failed 0x%x", eglGetError());
		return false;
	}
	if (!eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx)) {
		wlr_log(WLR_ERROR, "eglMakeCurrent window failed 0x%x", eglGetError());
		return false;
	}
	eglSwapInterval(g->dpy, 0);
	eglQuerySurface(g->dpy, g->surf, EGL_WIDTH, &sw);
	eglQuerySurface(g->dpy, g->surf, EGL_HEIGHT, &sh);
	wlr_log(WLR_INFO, "EGL window surface %dx%d vis=0x%x", sw, sh, vis);
	return true;
}

static bool gles_init(struct wlr_android_backend *backend) {
	struct wlr_android_gles *g = &backend->gles;
	const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE,
	};
	const EGLint ctx_attr[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE,
	};
	EGLint n = 0;

	if (g->ok) {
		return true;
	}
	g->dpy = EGL_NO_DISPLAY;
	g->ctx = EGL_NO_CONTEXT;
	g->surf = EGL_NO_SURFACE;
	g->image = EGL_NO_IMAGE_KHR;

	g->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, NULL, NULL)) {
		wlr_log(WLR_ERROR, "eglInitialize failed");
		return false;
	}
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		wlr_log(WLR_ERROR, "eglBindAPI failed");
		return false;
	}
	if (!eglChooseConfig(g->dpy, cfg_attr, &g->config, 1, &n) || n < 1) {
		wlr_log(WLR_ERROR, "eglChooseConfig failed");
		return false;
	}
	g->ctx = eglCreateContext(g->dpy, g->config, EGL_NO_CONTEXT, ctx_attr);
	if (g->ctx == EGL_NO_CONTEXT) {
		wlr_log(WLR_ERROR, "eglCreateContext failed 0x%x", eglGetError());
		return false;
	}
	if (!gles_create_window_surface(backend)) {
		return false;
	}
	g->prog_comp = link_program(COMP_VS, COMP_FS);
	g->prog_ahb = link_program(AHB_VS, AHB_FS);
	if (!g->prog_comp || !g->prog_ahb) {
		gles_fini(backend);
		return false;
	}
	g->a_comp_pos = glGetAttribLocation(g->prog_comp, "a_pos");
	g->a_comp_uv = glGetAttribLocation(g->prog_comp, "a_uv");
	g->a_ahb_pos = glGetAttribLocation(g->prog_ahb, "a_pos");
	g->a_ahb_uv = glGetAttribLocation(g->prog_ahb, "a_uv");
	glGenTextures(1, &g->tex_comp);
	glBindTexture(GL_TEXTURE_2D, g->tex_comp);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	g->ok = true;
	wlr_log(WLR_INFO, "Android GLES scanout ready");
	return true;
}

static void upload_comp(struct wlr_android_gles *g, const uint8_t *src,
		size_t src_stride, int buf_w, int buf_h) {
	int y;
	glBindTexture(GL_TEXTURE_2D, g->tex_comp);
	if (g->tex_w != buf_w || g->tex_h != buf_h) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, buf_w, buf_h, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		g->tex_w = buf_w;
		g->tex_h = buf_h;
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for (y = 0; y < buf_h; y++) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, buf_w, 1,
			GL_RGBA, GL_UNSIGNED_BYTE, src + (size_t)y * src_stride);
	}
}

static void draw_fullscreen(struct wlr_android_gles *g) {
	static const GLfloat pos[] = {
		-1.f, -1.f,  1.f, -1.f,  -1.f, 1.f,  1.f, 1.f,
	};
	static const GLfloat uv[] = {
		0.f, 1.f,  1.f, 1.f,  0.f, 0.f,  1.f, 0.f,
	};
	glUseProgram(g->prog_comp);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g->tex_comp);
	glUniform1i(glGetUniformLocation(g->prog_comp, "u_tex"), 0);
	glEnableVertexAttribArray((GLuint)g->a_comp_pos);
	glEnableVertexAttribArray((GLuint)g->a_comp_uv);
	glVertexAttribPointer((GLuint)g->a_comp_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer((GLuint)g->a_comp_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static bool bind_ahb(struct wlr_android_backend *backend, AHardwareBuffer *ahb) {
	struct wlr_android_gles *g = &backend->gles;
	EGLClientBuffer client;
	const EGLint attribs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
	struct wlr_android_gles_ahb *slot = NULL;
	int i;

	for (i = 0; i < g->ahb_cache_used; i++) {
		if (g->ahb_cache[i].ahb == ahb && g->ahb_cache[i].image != EGL_NO_IMAGE_KHR) {
			g->tex_ahb = g->ahb_cache[i].tex;
			g->image = g->ahb_cache[i].image;
			g->image_ahb = ahb;
			return true;
		}
	}
	if (g->ahb_cache_used < GLES_AHB_CACHE) {
		slot = &g->ahb_cache[g->ahb_cache_used++];
		memset(slot, 0, sizeof(*slot));
		slot->image = EGL_NO_IMAGE_KHR;
	} else {
		slot = &g->ahb_cache[g->ahb_cache_clock % GLES_AHB_CACHE];
		g->ahb_cache_clock++;
		glFinish();
		if (slot->image != EGL_NO_IMAGE_KHR)
			eglDestroyImageKHR(g->dpy, slot->image);
		slot->image = EGL_NO_IMAGE_KHR;
		if (slot->ahb) AHardwareBuffer_release(slot->ahb);
		slot->ahb = NULL;
	}
	client = eglGetNativeClientBufferANDROID(ahb);
	if (!client) {
		wlr_log(WLR_ERROR, "eglGetNativeClientBufferANDROID failed");
		return false;
	}
	slot->image = eglCreateImageKHR(g->dpy, EGL_NO_CONTEXT,
		EGL_NATIVE_BUFFER_ANDROID, client, attribs);
	if (slot->image == EGL_NO_IMAGE_KHR) {
		wlr_log(WLR_ERROR, "eglCreateImageKHR AHB failed 0x%x", eglGetError());
		return false;
	}
	if (!slot->tex) {
		glGenTextures(1, &slot->tex);
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, slot->tex);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, slot->tex);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, slot->image);
	if (glGetError() != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "glEGLImageTargetTexture2DOES failed");
		eglDestroyImageKHR(g->dpy, slot->image);
		slot->image = EGL_NO_IMAGE_KHR;
		return false;
	}
	AHardwareBuffer_acquire(ahb);
	slot->ahb = ahb;
	g->tex_ahb = slot->tex;
	g->image = slot->image;
	g->image_ahb = ahb;
	return true;
}

static void draw_ahb(struct wlr_android_backend *backend, int x, int y, int w, int h) {
	struct wlr_android_gles *g = &backend->gles;
	float W = (float)backend->width;
	float H = (float)backend->height;
	float x0, x1, y0, y1;
	GLfloat pos[8];
	static const GLfloat uv[] = {
		0.f, 0.f,  1.f, 0.f,  0.f, 1.f,  1.f, 1.f,
	};

	if (W <= 0.f || H <= 0.f || w <= 0 || h <= 0) {
		return;
	}
	x0 = 2.f * (float)x / W - 1.f;
	x1 = 2.f * (float)(x + w) / W - 1.f;
	y0 = 1.f - 2.f * (float)y / H;
	y1 = 1.f - 2.f * (float)(y + h) / H;
	pos[0] = x0; pos[1] = y0;
	pos[2] = x1; pos[3] = y0;
	pos[4] = x0; pos[5] = y1;
	pos[6] = x1; pos[7] = y1;
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(g->prog_ahb);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, g->tex_ahb);
	glUniform1i(glGetUniformLocation(g->prog_ahb, "u_tex"), 0);
	glEnableVertexAttribArray((GLuint)g->a_ahb_pos);
	glEnableVertexAttribArray((GLuint)g->a_ahb_uv);
	glVertexAttribPointer((GLuint)g->a_ahb_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer((GLuint)g->a_ahb_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_BLEND);
}

static void overlay_release_locked(struct wlr_android_overlay *overlay) {
	int i;

	for (i = 0; i < overlay->n; i++) {
		if (overlay->slots[i].ahb) {
			AHardwareBuffer_release(overlay->slots[i].ahb);
			overlay->slots[i].ahb = NULL;
		}
	}
	overlay->n = 0;
}

static void blit_to_window(struct wlr_android_backend *backend,
		const uint8_t *src, uint32_t src_fmt, size_t src_stride,
		int buf_w, int buf_h) {
	struct wlr_android_ahb_slot local[WLR_ANDROID_AHB_MAX];
	int n = 0;
	int i;
	(void)src_fmt;

	if (!gles_init(backend)) {
		return;
	}
	if (!eglMakeCurrent(backend->gles.dpy, backend->gles.surf,
			backend->gles.surf, backend->gles.ctx)) {
		wlr_log(WLR_ERROR, "eglMakeCurrent scanout failed");
		return;
	}
	glViewport(0, 0, backend->width, backend->height);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_BLEND);
	upload_comp(&backend->gles, src, src_stride, buf_w, buf_h);
	draw_fullscreen(&backend->gles);

	pthread_mutex_lock(&backend->overlay.lock);
	n = backend->overlay.n;
	if (n > WLR_ANDROID_AHB_MAX) {
		n = WLR_ANDROID_AHB_MAX;
	}
	for (i = 0; i < n; i++) {
		local[i] = backend->overlay.slots[i];
		if (local[i].ahb) {
			AHardwareBuffer_acquire(local[i].ahb);
		}
	}
	pthread_mutex_unlock(&backend->overlay.lock);

	for (i = 0; i < n; i++) {
		if (!local[i].ahb) {
			continue;
		}
		if (bind_ahb(backend, local[i].ahb)) {
			draw_ahb(backend, local[i].x, local[i].y,
				local[i].w, local[i].h);
		}
		AHardwareBuffer_release(local[i].ahb);
	}
	/* The protocol has no release fence. Finish sampling before scene
	 * commit may unlock a client buffer and send wl_buffer.release. */
	glFinish();
	if (!eglSwapBuffers(backend->gles.dpy, backend->gles.surf)) {
		wlr_log(WLR_ERROR, "eglSwapBuffers failed 0x%x", eglGetError());
	}
}

void wlr_android_present_ahb_slots(struct wlr_backend *wlr_backend,
		const struct wlr_android_ahb_blit *slots, int n) {
	struct wlr_android_backend *backend;
	struct wlr_android_ahb_slot next[WLR_ANDROID_AHB_MAX];
	int count = 0;
	int i;

	if (!wlr_backend_is_android(wlr_backend)) {
		return;
	}
	backend = android_backend_from_backend(wlr_backend);
	if (n < 0) {
		n = 0;
	}
	if (n > WLR_ANDROID_AHB_MAX) {
		n = WLR_ANDROID_AHB_MAX;
	}
	memset(next, 0, sizeof(next));
	for (i = 0; i < n; i++) {
		if (!slots || !slots[i].ahb || slots[i].w <= 0 || slots[i].h <= 0) {
			continue;
		}
		AHardwareBuffer_acquire(slots[i].ahb);
		next[count].ahb = slots[i].ahb;
		next[count].x = slots[i].x;
		next[count].y = slots[i].y;
		next[count].w = slots[i].w;
		next[count].h = slots[i].h;
		count++;
	}
	pthread_mutex_lock(&backend->overlay.lock);
	overlay_release_locked(&backend->overlay);
	memcpy(backend->overlay.slots, next, sizeof(next));
	backend->overlay.n = count;
	pthread_mutex_unlock(&backend->overlay.lock);
}

void wlr_android_present_ahb(struct wlr_backend *wlr_backend,
		struct AHardwareBuffer *ahb, int x, int y, int w, int h) {
	struct wlr_android_ahb_blit slot = {
		.ahb = ahb,
		.x = x,
		.y = y,
		.w = w,
		.h = h,
	};
	wlr_android_present_ahb_slots(wlr_backend, &slot, 1);
}

void wlr_android_schedule_frame(struct wlr_backend *wlr_backend) {
	struct wlr_android_backend *backend;

	if (!wlr_backend_is_android(wlr_backend)) {
		return;
	}
	backend = android_backend_from_backend(wlr_backend);
	wlr_output_schedule_frame(&backend->output);
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
		if (backend->gles.ok) {
			gles_create_window_surface(backend);
		}
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

	gles_fini(backend);
	pthread_mutex_lock(&backend->overlay.lock);
	overlay_release_locked(&backend->overlay);
	pthread_mutex_unlock(&backend->overlay.lock);
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
