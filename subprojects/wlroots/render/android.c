/* SPDX-License-Identifier: MIT
 * Native Android EGL renderer. AHB and shm textures use the same scene pass.
 * Protocol clients finish producing before commit; we finish sampling before
 * wlroots can release a buffer because android_wlegl has no release fence.
 */
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <assert.h>
#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/interface.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>
#include "backend/android.h"
#include "util/matrix.h"
#include "android_dmabuf.h"

struct android_renderer {
	struct wlr_renderer base;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	EGLSurface idle_surface;
	EGLConfig config;
	struct ANativeWindow *window;
	GLuint rgba, external, solid;
	bool unpack_row_length;
	struct wlr_drm_format_set shm_formats, ahb_formats, dmabuf_formats;
	struct android_dmabuf_bridge *dmabuf_bridge;
	struct wl_list textures;
	uint64_t frames, ahb_draws, copy_draws, shm_draws, uploads, readbacks;
	uint64_t upload_pixels;
};

struct android_texture {
	struct wlr_texture base;
	struct wl_list link;
	struct android_renderer *renderer;
	struct wlr_buffer *buffer;
	GLuint name;
	GLenum target;
	EGLImageKHR image;
	AHardwareBuffer *copied_ahb;
	bool swizzle, opaque;
};

struct android_pass {
	struct wlr_render_pass base;
	struct android_renderer *renderer;
	struct android_texture *target;
	GLuint framebuffer;
	int width, height;
	bool window;
};

static bool make_current(struct android_renderer *r) {
	if (eglMakeCurrent(r->display, r->surface, r->surface, r->context)) {
		return true;
	}
	wlr_log(WLR_ERROR, "Android renderer: eglMakeCurrent failed 0x%x", eglGetError());
	return false;
}

static GLuint shader(GLenum type, const char *source) {
	GLuint name = glCreateShader(type);
	glShaderSource(name, 1, &source, NULL);
	glCompileShader(name);
	GLint ok;
	glGetShaderiv(name, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(name, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "Android renderer shader: %s", log);
		glDeleteShader(name);
		return 0;
	}
	return name;
}

static GLuint program(const char *fragment) {
	const char *vertex =
		"attribute vec2 pos; attribute vec2 uv; varying vec2 texcoord;"
		"void main() { gl_Position=vec4(pos,0.,1.); texcoord=uv; }";
	GLuint vs = shader(GL_VERTEX_SHADER, vertex);
	GLuint fs = shader(GL_FRAGMENT_SHADER, fragment);
	GLuint name = 0;
	if (vs && fs) {
		name = glCreateProgram();
		glAttachShader(name, vs);
		glAttachShader(name, fs);
		glBindAttribLocation(name, 0, "pos");
		glBindAttribLocation(name, 1, "uv");
		glLinkProgram(name);
		GLint ok;
		glGetProgramiv(name, GL_LINK_STATUS, &ok);
		if (!ok) {
			glDeleteProgram(name);
			name = 0;
		}
	}
	glDeleteShader(vs);
	glDeleteShader(fs);
	return name;
}

static void texture_destroy(struct wlr_texture *base) {
	struct android_texture *t = wl_container_of(base, t, base);
	if (make_current(t->renderer)) {
		glDeleteTextures(1, &t->name);
	}
	if (t->image != EGL_NO_IMAGE_KHR) {
		eglDestroyImageKHR(t->renderer->display, t->image);
	}
	if (t->copied_ahb) AHardwareBuffer_release(t->copied_ahb);
	wl_list_remove(&t->link);
	if (t->buffer) {
		wlr_buffer_unlock(t->buffer);
	}
	free(t);
}

static bool upload_shm(struct android_texture *t, struct wlr_buffer *buffer) {
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
			&data, &format, &stride)) {
		return false;
	}
	bool supported = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888 ||
		format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888;
	bool ok = false;
	if (supported && stride >= (size_t)buffer->width * 4) {
		t->renderer->uploads++;
		t->renderer->upload_pixels += (uint64_t)buffer->width * buffer->height;
		t->swizzle = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888;
		t->opaque = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888;
		glBindTexture(GL_TEXTURE_2D, t->name);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		bool tight = stride == (size_t)buffer->width * 4;
		bool row_length = t->renderer->unpack_row_length &&
			stride % 4 == 0 && stride / 4 <= INT_MAX;
		if (tight || row_length) {
			/* One upload for the whole buffer, including padded scanlines.
			 * Restore pixel-store state before importing another client. */
			if (row_length) {
				glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / 4);
			}
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, buffer->width, buffer->height,
				0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			if (row_length) {
				glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
			}
		} else {
			/* GLES2 without EXT_unpack_subimage still accepts arbitrary strides. */
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, buffer->width, buffer->height,
				0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			for (int y = 0; y < buffer->height; y++) {
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, buffer->width, 1,
					GL_RGBA, GL_UNSIGNED_BYTE, (char *)data + y * stride);
			}
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		ok = glGetError() == GL_NO_ERROR;
	}
	wlr_buffer_end_data_ptr_access(buffer);
	return ok;
}

static bool texture_update(struct wlr_texture *base, struct wlr_buffer *buffer,
		const pixman_region32_t *damage) {
	(void)damage;
	struct android_texture *t = wl_container_of(base, t, base);
	if (t->image != EGL_NO_IMAGE_KHR || wlr_buffer_get_ahb(buffer) ||
			base->width != (uint32_t)buffer->width || base->height != (uint32_t)buffer->height) {
		return false;
	}
	return make_current(t->renderer) && upload_shm(t, buffer);
}

static bool texture_read_pixels(struct wlr_texture *base,
		const struct wlr_texture_read_pixels_options *options) {
	struct android_texture *t = wl_container_of(base, t, base);
	if (options->format != DRM_FORMAT_ARGB8888
			|| !make_current(t->renderer)) {
		return false;
	}
	struct wlr_box src;
	wlr_texture_read_pixels_options_get_src_box(options, base, &src);
	if (src.x < 0 || src.y < 0 || src.width <= 0 || src.height <= 0
			|| src.x + src.width > (int)base->width
			|| src.y + src.height > (int)base->height
			|| options->stride < (options->dst_x + (uint32_t)src.width) * 4) {
		return false;
	}
	uint32_t *scratch = malloc((size_t)src.width * sizeof(*scratch));
	if (!scratch) {
		return false;
	}
	GLint previous_fbo = 0;
	GLuint framebuffer = 0;
	GLuint readable = 0;
	GLuint texture = t->name;
	if (t->target != GL_TEXTURE_2D) {
		if (t->image == EGL_NO_IMAGE_KHR) { free(scratch); return false; }
		glGenTextures(1, &readable);
		glBindTexture(GL_TEXTURE_2D, readable);
		glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, t->image);
		texture = readable;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, texture, 0);
	bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER)
		== GL_FRAMEBUFFER_COMPLETE;
	uint32_t *dst = wlr_texture_read_pixel_options_get_data(options);
	if (ok) t->renderer->readbacks++;
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	for (int y = 0; ok && y < src.height; y++) {
		glReadPixels(src.x, src.y + y, src.width, 1,
			GL_RGBA, GL_UNSIGNED_BYTE, scratch);
		ok = glGetError() == GL_NO_ERROR;
		uint32_t *row = (uint32_t *)((char *)dst + (size_t)y * options->stride);
		for (int x = 0; ok && x < src.width; x++) {
			uint32_t pixel = scratch[x];
			if (!t->swizzle) {
				pixel = (pixel & 0xFF00FF00u)
					| ((pixel & 0x00FF0000u) >> 16)
					| ((pixel & 0x000000FFu) << 16);
			}
			if (t->opaque) {
				pixel |= 0xFF000000u;
			}
			row[x] = pixel;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous_fbo);
	glDeleteFramebuffers(1, &framebuffer);
	if (readable) glDeleteTextures(1, &readable);
	free(scratch);
	return ok;
}

static uint32_t texture_preferred_read_format(struct wlr_texture *base) {
	(void)base;
	return DRM_FORMAT_ARGB8888;
}

static const struct wlr_texture_impl texture_impl = {
	.destroy = texture_destroy,
	.update_from_buffer = texture_update,
	.read_pixels = texture_read_pixels,
	.preferred_read_format = texture_preferred_read_format,
};

static struct android_texture *import_texture(struct android_renderer *r,
		struct wlr_buffer *buffer, bool render_target) {
	if (!make_current(r)) {
		return NULL;
	}
	struct android_texture *t = calloc(1, sizeof(*t));
	if (!t) {
		return NULL;
	}
	t->renderer = r;
	t->image = EGL_NO_IMAGE_KHR;
	wlr_texture_init(&t->base, &r->base, &texture_impl, buffer->width, buffer->height);
	wl_list_insert(&r->textures, &t->link);
	AHardwareBuffer *ahb = wlr_buffer_get_ahb(buffer);
	struct wlr_dmabuf_attributes dmabuf = {0};
	if (!ahb && wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		if (render_target) goto fail;
		ahb = t->copied_ahb = android_dmabuf_bridge_copy(r->dmabuf_bridge, &dmabuf);
		if (!ahb) goto fail;
	}
	t->target = ahb && !render_target ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
	glGenTextures(1, &t->name);
	glBindTexture(t->target, t->name);
	glTexParameteri(t->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(t->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(t->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(t->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	if (ahb) {
		EGLClientBuffer client = eglGetNativeClientBufferANDROID(ahb);
		const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
		if (!client) {
			goto fail;
		}
		t->image = eglCreateImageKHR(r->display, EGL_NO_CONTEXT,
			EGL_NATIVE_BUFFER_ANDROID, client, attrs);
		if (t->image == EGL_NO_IMAGE_KHR) {
			goto fail;
		}
		glEGLImageTargetTexture2DOES(t->target, t->image);
		if (glGetError() != GL_NO_ERROR) {
			goto fail;
		}
		AHardwareBuffer_Desc desc;
		AHardwareBuffer_describe(ahb, &desc);
		t->opaque = desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM ||
			desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
		if (t->copied_ahb) {
			t->swizzle = dmabuf.format == DRM_FORMAT_ARGB8888 || dmabuf.format == DRM_FORMAT_XRGB8888;
			t->opaque = dmabuf.format == DRM_FORMAT_XRGB8888 || dmabuf.format == DRM_FORMAT_XBGR8888;
		}
		t->buffer = wlr_buffer_lock(buffer);
	} else if (render_target || !upload_shm(t, buffer)) {
		goto fail;
	}
	return t;
fail:
	wlr_log(WLR_ERROR, "Android renderer: buffer import failed (AHB=%d, EGL=0x%x)",
		ahb != NULL, eglGetError());
	texture_destroy(&t->base);
	return NULL;
}

static struct wlr_texture *texture_from_buffer(struct wlr_renderer *base,
		struct wlr_buffer *buffer) {
	struct android_renderer *r = wl_container_of(base, r, base);
	struct android_texture *t = import_texture(r, buffer, false);
	return t ? &t->base : NULL;
}

/* The FBO stores row zero at GL y=0, matching Android buffer texture imports.
 * Only the final window pass changes to the window framebuffer's origin. */
static void draw(struct android_pass *pass, const struct wlr_box *box,
		const pixman_region32_t *clip, const GLfloat uv[8]) {
	float x0 = 2.f * box->x / pass->width - 1.f;
	float x1 = 2.f * (box->x + box->width) / pass->width - 1.f;
	float y0 = 2.f * box->y / pass->height - 1.f;
	float y1 = 2.f * (box->y + box->height) / pass->height - 1.f;
	if (pass->window) {
		y0 = -y0;
		y1 = -y1;
	}
	const GLfloat pos[] = {x0,y0, x1,y0, x0,y1, x1,y1};
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	if (uv) {
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
	} else {
		glDisableVertexAttribArray(1);
	}
	pixman_region32_t region;
	pixman_region32_init_rect(&region, box->x, box->y, box->width, box->height);
	pixman_region32_intersect_rect(&region, &region, 0, 0, pass->width, pass->height);
	if (clip) {
		pixman_region32_intersect(&region, &region, clip);
	}
	int n;
	const pixman_box32_t *rects = pixman_region32_rectangles(&region, &n);
	glEnable(GL_SCISSOR_TEST);
	for (int i = 0; i < n; i++) {
		const pixman_box32_t *b = &rects[i];
		glScissor(b->x1, pass->window ? pass->height - b->y2 : b->y1,
			b->x2 - b->x1, b->y2 - b->y1);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glDisable(GL_SCISSOR_TEST);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	pixman_region32_fini(&region);
}

static void blend(enum wlr_render_blend_mode mode) {
	if (mode == WLR_RENDER_BLEND_MODE_NONE) {
		glDisable(GL_BLEND);
	} else {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}
}

static void add_texture(struct wlr_render_pass *base,
		const struct wlr_render_texture_options *options) {
	struct android_pass *pass = wl_container_of(base, pass, base);
	struct android_texture *t = wl_container_of(options->texture, t, base);
	assert(t->renderer == pass->renderer);
	struct wlr_fbox src;
	struct wlr_box dst;
	wlr_render_texture_options_get_src_box(options, &src);
	wlr_render_texture_options_get_dst_box(options, &dst);
	float matrix[9];
	wlr_matrix_identity(matrix);
	wlr_matrix_translate(matrix, src.x / t->base.width, src.y / t->base.height);
	wlr_matrix_scale(matrix, src.width / t->base.width, src.height / t->base.height);
	wlr_matrix_translate(matrix, .5, .5);
	enum wl_output_transform transform = options->transform;
	wlr_matrix_transform(matrix, transform & WL_OUTPUT_TRANSFORM_90 ?
		wlr_output_transform_invert(transform) : transform);
	wlr_matrix_translate(matrix, -.5, -.5);
	GLfloat uv[8];
	for (int i = 0; i < 4; i++) {
		float x = i & 1, y = i >> 1;
		uv[2*i] = matrix[0]*x + matrix[1]*y + matrix[2];
		uv[2*i+1] = matrix[3]*x + matrix[4]*y + matrix[5];
	}
	GLuint p = t->target == GL_TEXTURE_EXTERNAL_OES ? pass->renderer->external : pass->renderer->rgba;
	glUseProgram(p);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(t->target, t->name);
	GLint filter = options->filter_mode == WLR_SCALE_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
	glTexParameteri(t->target, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(t->target, GL_TEXTURE_MAG_FILTER, filter);
	glUniform1i(glGetUniformLocation(p, "tex"), 0);
	glUniform1i(glGetUniformLocation(p, "swizzle"), t->swizzle);
	glUniform1i(glGetUniformLocation(p, "opaque"), t->opaque);
	glUniform1f(glGetUniformLocation(p, "alpha"), wlr_render_texture_options_get_alpha(options));
	blend(options->blend_mode);
	if (!pass->window) {
		if (t->copied_ahb) pass->renderer->copy_draws++;
		else if (t->image != EGL_NO_IMAGE_KHR) pass->renderer->ahb_draws++;
		else pass->renderer->shm_draws++;
	}
	draw(pass, &dst, options->clip, uv);
}

static void add_rect(struct wlr_render_pass *base,
		const struct wlr_render_rect_options *options) {
	struct android_pass *pass = wl_container_of(base, pass, base);
	struct wlr_box box;
	wlr_render_rect_options_get_box(options, pass->target->buffer, &box);
	GLuint p = pass->renderer->solid;
	glUseProgram(p);
	glUniform4f(glGetUniformLocation(p, "color"), options->color.r,
		options->color.g, options->color.b, options->color.a);
	blend(options->blend_mode);
	draw(pass, &box, options->clip, NULL);
}

static bool submit(struct wlr_render_pass *base) {
	struct android_pass *pass = wl_container_of(base, pass, base);
	glFinish();
	GLenum error = glGetError();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &pass->framebuffer);
	texture_destroy(&pass->target->base);
	free(pass);
	if (error != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "Android scene pass failed: GL 0x%x", error);
	}
	return error == GL_NO_ERROR;
}

static const struct wlr_render_pass_impl pass_impl = {
	.submit = submit,
	.add_texture = add_texture,
	.add_rect = add_rect,
};

static struct wlr_render_pass *begin_pass(struct wlr_renderer *base,
		struct wlr_buffer *buffer, const struct wlr_buffer_pass_options *options) {
	struct android_renderer *r = wl_container_of(base, r, base);
	if (options && (options->color_transform || options->signal_timeline || options->timer)) {
		return NULL;
	}
	struct android_pass *pass = calloc(1, sizeof(*pass));
	if (!pass) {
		return NULL;
	}
	pass->target = import_texture(r, buffer, true);
	if (!pass->target) {
		free(pass);
		return NULL;
	}
	pass->renderer = r;
	pass->width = buffer->width;
	pass->height = buffer->height;
	glGenFramebuffers(1, &pass->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, pass->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		pass->target->name, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &pass->framebuffer);
		texture_destroy(&pass->target->base);
		free(pass);
		return NULL;
	}
	glViewport(0, 0, pass->width, pass->height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	wlr_render_pass_init(&pass->base, &pass_impl);
	return &pass->base;
}

bool android_renderer_present(struct wlr_renderer *base, struct wlr_buffer *buffer) {
	struct android_renderer *r = wl_container_of(base, r, base);
	struct android_texture *t = import_texture(r, buffer, false);
	if (!t) {
		return false;
	}
	EGLint width, height;
	bool ok = eglQuerySurface(r->display, r->surface, EGL_WIDTH, &width) &&
		eglQuerySurface(r->display, r->surface, EGL_HEIGHT, &height);
	if (ok) {
		struct android_pass pass = {.renderer=r, .width=width, .height=height, .window=true};
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, width, height);
		add_texture(&pass.base, &(struct wlr_render_texture_options){
			.texture=&t->base, .dst_box={0,0,width,height},
			/* Copy the composed output without blending neighboring pixels. */
			.filter_mode=WLR_SCALE_FILTER_NEAREST,
			.blend_mode=WLR_RENDER_BLEND_MODE_NONE,
		});
		glFinish();
		ok = glGetError() == GL_NO_ERROR && eglSwapBuffers(r->display, r->surface);
	}
	texture_destroy(&t->base);
	if (ok && (++r->frames == 1 || r->frames % 120 == 0)) {
		char totals[384];
		snprintf(totals, sizeof(totals), "frames=%" PRIu64
			" ahb_draws=%" PRIu64 " gpu_copy_draws=%" PRIu64
			" shm_draws=%" PRIu64 " cpu_uploads=%" PRIu64
			" cpu_upload_pixels=%" PRIu64 " readbacks=%" PRIu64,
			r->frames, r->ahb_draws, r->copy_draws, r->shm_draws,
			r->uploads, r->upload_pixels, r->readbacks);
		wlr_log(WLR_INFO, "Android render totals: %s", totals);
		/* Some Android builds suppress application logcat output. */
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		char path[PATH_MAX];
		if (runtime && snprintf(path, sizeof(path), "%s/render-stats.txt", runtime) < (int)sizeof(path)) {
			FILE *file = fopen(path, "w");
			if (file) { fprintf(file, "%s\n", totals); fclose(file); }
		}
	}
	if (!ok) {
		wlr_log(WLR_ERROR, "Android Surface present failed: EGL 0x%x", eglGetError());
	}
	return ok;
}

bool android_renderer_set_window(struct wlr_renderer *base, struct ANativeWindow *window) {
	struct android_renderer *r = wl_container_of(base, r, base);
	if (!eglMakeCurrent(r->display, r->idle_surface, r->idle_surface, r->context)) return false;
	if (r->surface != EGL_NO_SURFACE && r->surface != r->idle_surface)
		eglDestroySurface(r->display, r->surface);
	r->surface = r->idle_surface;
	if (r->window) ANativeWindow_release(r->window);
	r->window = NULL;
	if (!window) return true;
	EGLint visual;
	if (!eglGetConfigAttrib(r->display, r->config, EGL_NATIVE_VISUAL_ID, &visual) ||
			ANativeWindow_setBuffersGeometry(window, 0, 0, visual) != 0) return false;
	EGLSurface surface = eglCreateWindowSurface(r->display, r->config,
		(EGLNativeWindowType)window, NULL);
	if (surface == EGL_NO_SURFACE) return false;
	r->surface = surface;
	r->window = window;
	ANativeWindow_acquire(window);
	return make_current(r);
}

static const struct wlr_drm_format_set *texture_formats(struct wlr_renderer *base, uint32_t caps) {
	struct android_renderer *r = wl_container_of(base, r, base);
	if (caps & WLR_BUFFER_CAP_AHB) {
		return &r->ahb_formats;
	}
	if ((caps & WLR_BUFFER_CAP_DMABUF) && r->dmabuf_bridge) return &r->dmabuf_formats;
	return caps & (WLR_BUFFER_CAP_SHM | WLR_BUFFER_CAP_DATA_PTR) ? &r->shm_formats : NULL;
}

static const struct wlr_drm_format_set *render_formats(struct wlr_renderer *base) {
	struct android_renderer *r = wl_container_of(base, r, base);
	return &r->ahb_formats;
}

static void renderer_destroy(struct wlr_renderer *base) {
	struct android_renderer *r = wl_container_of(base, r, base);
	if (r->context != EGL_NO_CONTEXT && make_current(r)) {
		glFinish();
		glDeleteProgram(r->rgba);
		glDeleteProgram(r->external);
		glDeleteProgram(r->solid);
	}
	struct android_texture *t, *tmp;
	wl_list_for_each_safe(t, tmp, &r->textures, link) {
		texture_destroy(&t->base);
	}
	if (r->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(r->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (r->surface != EGL_NO_SURFACE) eglDestroySurface(r->display, r->surface);
		if (r->idle_surface != EGL_NO_SURFACE && r->idle_surface != r->surface)
			eglDestroySurface(r->display, r->idle_surface);
		if (r->context != EGL_NO_CONTEXT) eglDestroyContext(r->display, r->context);
	}
	if (r->window) ANativeWindow_release(r->window);
	wlr_drm_format_set_finish(&r->shm_formats);
	wlr_drm_format_set_finish(&r->ahb_formats);
	wlr_drm_format_set_finish(&r->dmabuf_formats);
	android_dmabuf_bridge_destroy(r->dmabuf_bridge);
	free(r);
}

static const struct wlr_renderer_impl renderer_impl = {
	.destroy=renderer_destroy,
	.get_texture_formats=texture_formats,
	.get_render_formats=render_formats,
	.texture_from_buffer=texture_from_buffer,
	.begin_buffer_pass=begin_pass,
};

struct wlr_renderer *wlr_android_renderer_create(struct wlr_backend *base) {
	struct wlr_android_backend *backend = android_backend_from_backend(base);
	struct android_renderer *r = calloc(1, sizeof(*r));
	if (!r) return NULL;
	wl_list_init(&r->textures);
	r->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	r->context = EGL_NO_CONTEXT;
	r->surface = EGL_NO_SURFACE;
	r->window = backend->window;
	ANativeWindow_acquire(r->window);
	if (r->display == EGL_NO_DISPLAY || !eglInitialize(r->display, NULL, NULL) ||
			!eglBindAPI(EGL_OPENGL_ES_API)) goto fail;
	const EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
		EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
		EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
	EGLConfig config;
	EGLint n, visual;
	if (!eglChooseConfig(r->display, attrs, &config, 1, &n) || n != 1 ||
			!eglGetConfigAttrib(r->display, config, EGL_NATIVE_VISUAL_ID, &visual)) goto fail;
	r->config = config;
	const EGLint idle_attrs[] = {EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
	r->idle_surface = eglCreatePbufferSurface(r->display, config, idle_attrs);
	if (r->idle_surface == EGL_NO_SURFACE) goto fail;
	if (ANativeWindow_setBuffersGeometry(r->window, 0, 0, visual) != 0) goto fail;
	const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
	r->context = eglCreateContext(r->display, config, EGL_NO_CONTEXT, ctx);
	if (r->context == EGL_NO_CONTEXT) goto fail;
	r->surface = eglCreateWindowSurface(r->display, config, (EGLNativeWindowType)r->window, NULL);
	if (r->surface == EGL_NO_SURFACE || !make_current(r)) goto fail;
	int gl_major = 0;
	const char *gl_version = (const char *)glGetString(GL_VERSION);
	const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
	if (gl_version) sscanf(gl_version, "OpenGL ES %d", &gl_major);
	const char *unpack = gl_extensions ? strstr(gl_extensions, "GL_EXT_unpack_subimage") : NULL;
	r->unpack_row_length = gl_major >= 3 || (unpack &&
		(unpack == gl_extensions || unpack[-1] == ' ') &&
		(unpack[strlen("GL_EXT_unpack_subimage")] == ' ' ||
		 unpack[strlen("GL_EXT_unpack_subimage")] == '\0'));
	eglSwapInterval(r->display, 0);
#define SAMPLE_BODY "precision mediump float; varying vec2 texcoord;" \
	"uniform bool swizzle; uniform bool opaque; uniform float alpha;" \
	"void main(){ vec4 c=texture2D(tex,texcoord); if(swizzle)c=c.bgra;" \
	"if(opaque)c.a=1.; gl_FragColor=c*alpha; }"
	r->rgba = program("uniform sampler2D tex;" SAMPLE_BODY);
	r->external = program("#extension GL_OES_EGL_image_external : require\n"
		"uniform samplerExternalOES tex;" SAMPLE_BODY);
#undef SAMPLE_BODY
	r->solid = program("precision mediump float; uniform vec4 color; void main(){gl_FragColor=color;}");
	if (!r->rgba || !r->external || !r->solid) goto fail;
	r->dmabuf_bridge = android_dmabuf_bridge_create();
	uint32_t formats[] = {DRM_FORMAT_ARGB8888,DRM_FORMAT_XRGB8888,DRM_FORMAT_ABGR8888,DRM_FORMAT_XBGR8888};
	for (size_t i = 0; i < sizeof(formats)/sizeof(formats[0]); i++) {
		if (!wlr_drm_format_set_add(&r->shm_formats, formats[i], DRM_FORMAT_MOD_LINEAR)) goto fail;
		if (!wlr_drm_format_set_add(&r->ahb_formats, formats[i], DRM_FORMAT_MOD_INVALID)) goto fail;
		if (r->dmabuf_bridge && !wlr_drm_format_set_add(&r->dmabuf_formats, formats[i], DRM_FORMAT_MOD_LINEAR)) goto fail;
	}
	wlr_renderer_init(&r->base, &renderer_impl, WLR_BUFFER_CAP_AHB);
	backend->renderer = &r->base;
	wlr_log(WLR_INFO, "Android scene renderer: %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
	return &r->base;
fail:
	wlr_log(WLR_ERROR, "Android scene renderer creation failed: EGL 0x%x", eglGetError());
	renderer_destroy(&r->base);
	return NULL;
}

struct android_buffer {
	struct wlr_buffer base;
	AHardwareBuffer *ahb;
};

static struct AHardwareBuffer *buffer_ahb(struct wlr_buffer *base) {
	struct android_buffer *b = wl_container_of(base, b, base);
	return b->ahb;
}

static void buffer_destroy(struct wlr_buffer *base) {
	struct android_buffer *b = wl_container_of(base, b, base);
	wlr_buffer_finish(base);
	AHardwareBuffer_release(b->ahb);
	free(b);
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy=buffer_destroy,
	.get_ahb=buffer_ahb,
};

static struct wlr_buffer *allocate(struct wlr_allocator *alloc, int width, int height,
		const struct wlr_drm_format *format) {
	(void)alloc;
	bool implicit = false;
	for (size_t i = 0; i < format->len; i++) {
		implicit |= format->modifiers[i] == DRM_FORMAT_MOD_INVALID;
	}
	if (!implicit || (format->format != DRM_FORMAT_ARGB8888 && format->format != DRM_FORMAT_XRGB8888 &&
			format->format != DRM_FORMAT_ABGR8888 && format->format != DRM_FORMAT_XBGR8888)) return NULL;
	struct android_buffer *b = calloc(1, sizeof(*b));
	if (!b) return NULL;
	const AHardwareBuffer_Desc desc = {
		.width=width, .height=height, .layers=1,
		.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
		.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
	};
	if (AHardwareBuffer_allocate(&desc, &b->ahb) != 0) {
		free(b);
		return NULL;
	}
	wlr_buffer_init(&b->base, &buffer_impl, width, height);
	return &b->base;
}

static void allocator_destroy(struct wlr_allocator *alloc) {
	free(alloc);
}

struct wlr_allocator *wlr_android_allocator_create(void) {
	static const struct wlr_allocator_interface impl = {
		.create_buffer=allocate, .destroy=allocator_destroy,
	};
	struct wlr_allocator *alloc = calloc(1, sizeof(*alloc));
	if (alloc) wlr_allocator_init(alloc, &impl, WLR_BUFFER_CAP_AHB);
	return alloc;
}
