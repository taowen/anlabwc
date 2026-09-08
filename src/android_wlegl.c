/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "config.h"

#if HAVE_ANDROID_EMBED

#include "android_wlegl.h"

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

#define AHB_METHOD_CLONE 3

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
static const struct native_handle_hdr *(*g_get_native_handle)(const AHardwareBuffer *);

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
	g_get_native_handle = (const struct native_handle_hdr *(*)(const AHardwareBuffer *))
		dlsym(lib, "AHardwareBuffer_getNativeHandle");

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
	rc = g_create_from_handle(&desc, h, AHB_METHOD_CLONE, &ahb);
	free(h);
	if (rc != 0 || !ahb) {
		wlr_log(WLR_ERROR, "android_wlegl: createFromHandle rc=%d", rc);
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
	if (buffer->ahb) {
		AHardwareBuffer_release(buffer->ahb);
	}
	if (buffer->resource) {
		wl_resource_set_user_data(buffer->resource, NULL);
	}
	free(buffer);
}

static struct AHardwareBuffer *
buffer_get_ahb(struct wlr_buffer *wlr_buffer)
{
	struct wlegl_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	return buffer->ahb;
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_ahb = buffer_get_ahb,
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

	if (num_fds < 0 || num_fds > 16 || ints->size > 1024 ||
		ints->size % 4 != 0) {
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

/* The wl_resource owns the initial buffer reference. Scene locks delay
 * wl_buffer.release until wlroots no longer uses this commit. */
static struct wlegl_buffer *
create_buffer(struct wl_client *client, uint32_t id, AHardwareBuffer *ahb)
{
	AHardwareBuffer_Desc desc;
	AHardwareBuffer_describe(ahb, &desc);
	struct wlegl_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		goto fail;
	}
	buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (!buffer->resource) {
		free(buffer);
		goto fail;
	}
	buffer->ahb = ahb;
	wlr_buffer_init(&buffer->base, &buffer_impl, desc.width, desc.height);
	wl_resource_set_implementation(buffer->resource, &wl_buffer_impl, buffer,
		buffer_handle_resource_destroy);
	buffer->release.notify = buffer_handle_release;
	wl_signal_add(&buffer->base.events.release, &buffer->release);
	return buffer;
fail:
	AHardwareBuffer_release(ahb);
	wl_client_post_no_memory(client);
	return NULL;
}

static void
wlegl_create_buffer(struct wl_client *client, struct wl_resource *resource,
	uint32_t id, int32_t width, int32_t height, int32_t stride,
	int32_t format, int32_t usage, struct wl_resource *handle_res)
{
	struct wlegl_handle *h = wl_resource_get_user_data(handle_res);
	if (!h || h->nfds != h->expected_fds || width <= 0 || height <= 0 ||
		width > 16384 || height > 16384 || stride < width) {
		wl_resource_post_error(resource, ANDROID_WLEGL_ERROR_BAD_HANDLE,
			"invalid buffer handle or dimensions");
		return;
	}
	/* CLONE leaves the received handle and its FDs owned by handle_res. */
	AHardwareBuffer *ahb = import_ahb(width, height, stride, format,
		(uint32_t)usage, h->fds, h->nfds, h->ints, h->nints);
	if (!ahb) {
		wl_resource_post_error(resource, ANDROID_WLEGL_ERROR_BAD_HANDLE,
			"import failed");
		return;
	}
	create_buffer(client, id, ahb);
}

static void
wlegl_get_server_buffer_handle(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, int32_t width,
	int32_t height, int32_t format, int32_t usage)
{
	if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
		(format != 1 && format != 5)) {
		wl_resource_post_error(resource, ANDROID_WLEGL_ERROR_BAD_VALUE,
			"unsupported buffer dimensions or format");
		return;
	}
	pthread_once(&g_once, load_ahb_syms);
	if (!g_get_native_handle) {
		wl_resource_post_error(resource, ANDROID_WLEGL_ERROR_BAD_VALUE,
			"native handle export unavailable");
		return;
	}
	AHardwareBuffer_Desc desc = {
		.width = width, .height = height, .layers = 1,
		.format = format, .usage = (uint32_t)usage,
	};
	AHardwareBuffer *ahb = NULL;
	if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || !ahb) {
		wl_resource_post_error(resource, ANDROID_WLEGL_ERROR_BAD_VALUE,
			"AHardwareBuffer allocation failed");
		return;
	}
	struct wl_resource *reply = wl_resource_create(client,
		&android_wlegl_server_buffer_handle_interface, 1, id);
	if (!reply) {
		AHardwareBuffer_release(ahb);
		wl_client_post_no_memory(client);
		return;
	}
	struct wlegl_buffer *buffer = create_buffer(client, 0, ahb);
	if (buffer) {
		const struct native_handle_hdr *handle = (const void *)
			g_get_native_handle(ahb);
		AHardwareBuffer_describe(ahb, &desc);
		struct wl_array ints = {
			.size = (size_t)handle->numInts * sizeof(int),
			.data = (void *)(handle->data + handle->numFds),
		};
		for (int i = 0; i < handle->numFds; i++) {
			android_wlegl_server_buffer_handle_send_buffer_fd(reply,
				handle->data[i]);
		}
		android_wlegl_server_buffer_handle_send_buffer_ints(reply, &ints);
		android_wlegl_server_buffer_handle_send_buffer(reply,
			buffer->resource, desc.format, desc.stride);
	}
	/* This protocol object has events only, no client destructor request. */
	wl_resource_destroy(reply);
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

void
android_wlegl_create(struct wl_display *display,
	struct wlr_compositor *compositor)
{
	(void)compositor;
	if (!wl_global_create(display, &android_wlegl_interface, 2, NULL, wlegl_bind)) {
		wlr_log(WLR_ERROR, "android_wlegl: global failed");
		return;
	}
	wlr_buffer_register_resource_interface(&buffer_resource_interface);
	wlr_log(WLR_INFO, "android_wlegl global advertised");
}

#endif /* HAVE_ANDROID_EMBED */
