/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "config.h"

#if HAVE_ANDROID_EMBED

#include "android_wlegl.h"
#include "anlabwc-embed.h"

#include <android/hardware_buffer.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_compositor.h>
#include "wayland-android-protocol.h"

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241u
#endif

#define AHB_METHOD_REGISTER 2

struct native_handle_hdr {
	int version;
	int numFds;
	int numInts;
	int data[0];
};

typedef int (*create_from_handle_fn)(const AHardwareBuffer_Desc *desc,
	const struct native_handle_hdr *handle, int32_t method,
	AHardwareBuffer **out);

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static create_from_handle_fn g_create_from_handle;
static void (*g_ahb_release)(AHardwareBuffer *);
static void (*g_ahb_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);

static void
load_ahb_syms(void)
{
	void *lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_GLOBAL);

	if (!lib) {
		wlr_log(WLR_ERROR, "android_wlegl: dlopen libnativewindow: %s",
			dlerror());
		return;
	}
	g_create_from_handle = (create_from_handle_fn)dlsym(lib,
		"AHardwareBuffer_createFromHandle");
	g_ahb_release = (void (*)(AHardwareBuffer *))dlsym(lib,
		"AHardwareBuffer_release");
	g_ahb_describe = (void (*)(const AHardwareBuffer *, AHardwareBuffer_Desc *))
		dlsym(lib, "AHardwareBuffer_describe");
}

static AHardwareBuffer *
import_ahb(int32_t width, int32_t height, int32_t stride, int32_t format,
	uint32_t usage, const int *fds, int num_fds, const int32_t *ints,
	int num_ints)
{
	struct native_handle_hdr *h;
	AHardwareBuffer_Desc desc;
	AHardwareBuffer *ahb = NULL;
	size_t n;
	int i, rc;

	pthread_once(&g_once, load_ahb_syms);
	if (!g_create_from_handle || num_fds < 0 || num_ints < 0) {
		return NULL;
	}
	n = sizeof(*h) + (size_t)(num_fds + num_ints) * sizeof(int);
	h = malloc(n);
	if (!h) {
		return NULL;
	}
	h->version = (int)sizeof(*h);
	h->numFds = num_fds;
	h->numInts = num_ints;
	for (i = 0; i < num_fds; i++) {
		h->data[i] = fds[i];
	}
	for (i = 0; i < num_ints; i++) {
		h->data[num_fds + i] = ints[i];
	}
	memset(&desc, 0, sizeof(desc));
	desc.width = (uint32_t)width;
	desc.height = (uint32_t)height;
	desc.layers = 1;
	desc.format = (uint32_t)format;
	desc.usage = usage;
	desc.stride = (uint32_t)stride;
	rc = g_create_from_handle(&desc, h, AHB_METHOD_REGISTER, &ahb);
	if (rc != 0 || !ahb) {
		wlr_log(WLR_ERROR, "android_wlegl: createFromHandle rc=%d", rc);
		free(h);
		return NULL;
	}
	return ahb;
}

struct wlegl_handle {
	struct wl_resource *resource;
	int expected_fds;
	int *fds;
	int nfds;
	int32_t *ints;
	int nints;
};

struct wlegl_buffer {
	struct wlr_buffer base;
	struct wl_resource *resource;
	struct wl_listener release;
	uint8_t *pixels;
	size_t stride;
	AHardwareBuffer *ahb;
};

static const struct wl_buffer_interface wl_buffer_impl;
static const struct wlr_buffer_impl buffer_impl;

static bool
buffer_resource_is_instance(struct wl_resource *resource)
{
	return wl_resource_instance_of(resource, &wl_buffer_interface,
		&wl_buffer_impl);
}

static struct wlegl_buffer *
wlegl_buffer_from_resource(struct wl_resource *resource)
{
	assert(buffer_resource_is_instance(resource));
	return wl_resource_get_user_data(resource);
}

static struct wlr_buffer *
buffer_from_resource(struct wl_resource *resource)
{
	return &wlegl_buffer_from_resource(resource)->base;
}

static const struct wlr_buffer_resource_interface buffer_resource_interface = {
	.name = "android_wlegl",
	.is_instance = buffer_resource_is_instance,
	.from_resource = buffer_from_resource,
};

static void
buffer_destroy(struct wlr_buffer *wlr_buffer)
{
	struct wlegl_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);

	wl_list_remove(&buffer->release.link);
	wlr_buffer_finish(wlr_buffer);
	if (buffer->ahb && g_ahb_release) {
		g_ahb_release(buffer->ahb);
	}
	free(buffer->pixels);
	if (buffer->resource) {
		wl_resource_set_user_data(buffer->resource, NULL);
	}
	free(buffer);
}

static bool
buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
	void **data, uint32_t *format, size_t *stride)
{
	struct wlegl_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);

	if (flags & ~WLR_BUFFER_DATA_PTR_ACCESS_READ) {
		return false;
	}
	if (!buffer->pixels) {
		return false;
	}
	*data = buffer->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = buffer->stride;
	return true;
}

static void
buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
};

static void
destroy_resource(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = {
	.destroy = destroy_resource,
};

static void
buffer_handle_resource_destroy(struct wl_resource *resource)
{
	struct wlegl_buffer *buffer = wlegl_buffer_from_resource(resource);

	buffer->resource = NULL;
	wlr_buffer_drop(&buffer->base);
}

static void
buffer_handle_release(struct wl_listener *listener, void *data)
{
	struct wlegl_buffer *buffer = wl_container_of(listener, buffer, release);

	(void)data;
	if (buffer->resource) {
		wl_buffer_send_release(buffer->resource);
	}
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
handle_add_fd(struct wl_client *client, struct wl_resource *resource, int32_t fd)
{
	struct wlegl_handle *h = wl_resource_get_user_data(resource);

	(void)client;
	if (!h) {
		close(fd);
		return;
	}
	if (h->nfds >= h->expected_fds) {
		wl_resource_post_error(resource, ANDROID_WLEGL_HANDLE_ERROR_TOO_MANY_FDS,
			"too many fds");
		close(fd);
		return;
	}
	h->fds[h->nfds++] = fd;
}

static const struct android_wlegl_handle_interface handle_impl = {
	.add_fd = handle_add_fd,
	.destroy = handle_destroy,
};

static void
handle_resource_destroy(struct wl_resource *resource)
{
	struct wlegl_handle *h = wl_resource_get_user_data(resource);
	int i;

	if (!h) {
		return;
	}
	for (i = 0; i < h->nfds; i++) {
		if (h->fds[i] >= 0) {
			close(h->fds[i]);
		}
	}
	free(h->fds);
	free(h->ints);
	free(h);
}

static void
wlegl_create_handle(struct wl_client *client, struct wl_resource *resource,
	uint32_t id, int32_t num_fds, struct wl_array *ints)
{
	struct wlegl_handle *h;
	struct wl_resource *res;

	if (num_fds < 0 || ints->size % 4 != 0) {
		wl_resource_post_error(resource, ANDROID_WLEGL_ERROR_BAD_VALUE,
			"bad handle");
		return;
	}
	h = calloc(1, sizeof(*h));
	if (!h) {
		wl_client_post_no_memory(client);
		return;
	}
	h->expected_fds = num_fds;
	h->nints = (int)(ints->size / 4);
	if (num_fds > 0) {
		h->fds = calloc((size_t)num_fds, sizeof(int));
		if (!h->fds) {
			free(h);
			wl_client_post_no_memory(client);
			return;
		}
	}
	if (h->nints > 0) {
		h->ints = malloc((size_t)h->nints * sizeof(int32_t));
		if (!h->ints) {
			free(h->fds);
			free(h);
			wl_client_post_no_memory(client);
			return;
		}
		memcpy(h->ints, ints->data, (size_t)h->nints * sizeof(int32_t));
	}
	res = wl_resource_create(client, &android_wlegl_handle_interface, 1, id);
	if (!res) {
		free(h->ints);
		free(h->fds);
		free(h);
		wl_client_post_no_memory(client);
		return;
	}
	h->resource = res;
	wl_resource_set_implementation(res, &handle_impl, h, handle_resource_destroy);
}

static void
wlegl_create_buffer(struct wl_client *client, struct wl_resource *resource,
	uint32_t id, int32_t width, int32_t height, int32_t stride,
	int32_t format, int32_t usage, struct wl_resource *handle_res)
{
	struct wlegl_handle *h = wl_resource_get_user_data(handle_res);
	struct wlegl_buffer *buffer;
	AHardwareBuffer *ahb;
	int i;

	(void)resource;
	if (!h || h->nfds != h->expected_fds || width <= 0 || height <= 0) {
		wl_resource_post_error(handle_res, ANDROID_WLEGL_ERROR_BAD_HANDLE,
			"incomplete handle");
		return;
	}
	ahb = import_ahb(width, height, stride, format, (uint32_t)usage,
		h->fds, h->nfds, h->ints, h->nints);
	/* REGISTER takes the fds; do not close them on the handle. */
	for (i = 0; i < h->nfds; i++) {
		h->fds[i] = -1;
	}
	h->nfds = 0;
	if (!ahb) {
		wl_resource_post_error(handle_res, ANDROID_WLEGL_ERROR_BAD_HANDLE,
			"import failed");
		return;
	}

	buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		if (g_ahb_release) {
			g_ahb_release(ahb);
		}
		wl_client_post_no_memory(client);
		return;
	}
	buffer->stride = (size_t)width * 4;
	buffer->pixels = calloc((size_t)height, buffer->stride);
	if (!buffer->pixels) {
		if (g_ahb_release) {
			g_ahb_release(ahb);
		}
		free(buffer);
		wl_client_post_no_memory(client);
		return;
	}
	buffer->ahb = ahb;
	buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (!buffer->resource) {
		if (g_ahb_release) {
			g_ahb_release(ahb);
		}
		free(buffer->pixels);
		free(buffer);
		wl_client_post_no_memory(client);
		return;
	}
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	wl_resource_set_implementation(buffer->resource, &wl_buffer_impl, buffer,
		buffer_handle_resource_destroy);
	buffer->release.notify = buffer_handle_release;
	wl_signal_add(&buffer->base.events.release, &buffer->release);
}

static void
wlegl_get_server_buffer_handle(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, int32_t width,
	int32_t height, int32_t format, int32_t usage)
{
	(void)client;
	(void)id;
	(void)width;
	(void)height;
	(void)format;
	(void)usage;
	wl_resource_post_error(resource, ANDROID_WLEGL_ERROR_BAD_VALUE,
		"server-side buffers disabled");
}

static const struct android_wlegl_interface wlegl_impl = {
	.create_handle = wlegl_create_handle,
	.create_buffer = wlegl_create_buffer,
	.get_server_buffer_handle = wlegl_get_server_buffer_handle,
};

static void
wlegl_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	(void)data;
	resource = wl_resource_create(client, &android_wlegl_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wlegl_impl, NULL, NULL);
}

struct surface_hook {
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void
surface_handle_destroy(struct wl_listener *listener, void *data)
{
	struct surface_hook *hook = wl_container_of(listener, hook, destroy);

	(void)data;
	gpu_overlay_forget_surface(hook->surface);
	wl_list_remove(&hook->commit.link);
	wl_list_remove(&hook->destroy.link);
	free(hook);
}

static void
surface_handle_commit(struct wl_listener *listener, void *data)
{
	struct surface_hook *hook = wl_container_of(listener, hook, commit);
	struct wlr_surface *surface = hook->surface;
	struct wlr_buffer *buf;
	struct wlegl_buffer *wlegl;
	pid_t pid = 0;
	uid_t uid = 0;
	gid_t gid = 0;

	(void)data;
	buf = surface->pending.buffer;
	if (!buf || buf->impl != &buffer_impl) {
		return;
	}
	wlegl = wl_container_of(buf, wlegl, base);
	if (!wlegl->ahb || !wlegl->resource) {
		return;
	}
	wl_client_get_credentials(wl_resource_get_client(wlegl->resource),
		&pid, &uid, &gid);
	if (pid <= 0) {
		return;
	}
	(void)gpu_overlay_present_surface(surface, wlegl->ahb,
		(uint32_t)pid, buf->width, buf->height);
}

static void
handle_new_surface(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface = data;
	struct surface_hook *hook;

	(void)listener;
	hook = calloc(1, sizeof(*hook));
	if (!hook) {
		return;
	}
	hook->surface = surface;
	hook->commit.notify = surface_handle_commit;
	wl_signal_add(&surface->events.client_commit, &hook->commit);
	hook->destroy.notify = surface_handle_destroy;
	wl_signal_add(&surface->events.destroy, &hook->destroy);
}

static struct wl_listener g_new_surface = { .notify = handle_new_surface };

void
android_wlegl_create(struct wl_display *display,
	struct wlr_compositor *compositor)
{
	if (!wl_global_create(display, &android_wlegl_interface, 2, NULL, wlegl_bind)) {
		wlr_log(WLR_ERROR, "android_wlegl: global failed");
		return;
	}
	wlr_buffer_register_resource_interface(&buffer_resource_interface);
	if (compositor) {
		wl_signal_add(&compositor->events.new_surface, &g_new_surface);
	}
	wlr_log(WLR_INFO, "android_wlegl global advertised (client-side AHB)");
}

#endif /* HAVE_ANDROID_EMBED */
