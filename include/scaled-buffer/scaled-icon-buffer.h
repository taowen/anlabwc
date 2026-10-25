/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_SCALED_ICON_BUFFER_H
#define LABWC_SCALED_ICON_BUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_scene_tree;
struct wlr_scene_node;
struct wlr_scene_buffer;

struct scaled_icon_buffer {
	struct wl_list link;
	struct lab_data_buffer *resolved_icon;
	struct scaled_buffer *scaled_buffer;
	struct wlr_scene_buffer *scene_buffer;
	/* for window icon */
	struct view *view;
	bool client_only;
	char *view_app_id;
	char *view_icon_name;
	bool view_icon_prefer_client;
	struct wl_array view_icon_buffers;
	struct {
		struct wl_listener new_app_id;
		struct wl_listener new_title;
		struct wl_listener set_icon;
		struct wl_listener destroy;
	} on_view;
	/* for general icon (e.g. in menus) */
	char *icon_name;

	int width;
	int height;
};

/*
 * Create an auto scaling icon buffer, providing a wlr_scene_buffer node for
 * display. It gets destroyed automatically when the backing scaled_buffer
 * is being destroyed which in turn happens automatically when the backing
 * wlr_scene_buffer (or one of its parents) is being destroyed.
 */
struct scaled_icon_buffer *scaled_icon_buffer_create(
	struct wlr_scene_tree *parent, int width, int height);
void scaled_icon_buffer_configure_resolver(void (*request)(uint64_t, const char *, const char *));
void scaled_icon_buffer_resolve(uint64_t id, const char *app_id, struct lab_data_buffer *buffer);

/* Client-provided pixels, with a built-in application icon when absent.
 * Does not require filesystem icon lookup or libsfdo. */
struct scaled_icon_buffer *scaled_icon_buffer_create_client(
	struct wlr_scene_tree *parent, int width, int height);

void scaled_icon_buffer_set_view(struct scaled_icon_buffer *self,
	struct view *view);

void scaled_icon_buffer_set_icon_name(struct scaled_icon_buffer *self,
	const char *icon_name);

#endif /* LABWC_SCALED_ICON_BUFFER_H */
