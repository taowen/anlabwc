// SPDX-License-Identifier: GPL-2.0-only
#include "resize-preview.h"
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include "labwc.h"
#include "ssd.h"
#include "view.h"

void
resize_preview_cancel(struct view *view)
{
	if (!view->resize_preview.buffer) {
		return;
	}
	wlr_scene_node_destroy(&view->resize_preview.buffer->node);
	view->resize_preview.buffer = NULL;
	view->resize_preview.submitted = false;
	if (view->content_tree) {
		wlr_scene_node_set_enabled(&view->content_tree->node, !view->shaded);
	}
	wlr_scene_node_set_position(&view->scene_tree->node, view->current.x, view->current.y);
	ssd_update_geometry(view->ssd);
}

bool
resize_preview_begin(struct view *view)
{
	resize_preview_cancel(view);
	struct wlr_box extent = { .width = view->current.width, .height = view->current.height };
	struct wlr_buffer *buffer = wlr_scene_tree_snapshot(view->render_tree,
		server.renderer, server.allocator, &extent);
	if (!buffer) {
		wlr_log(WLR_ERROR, "Cannot capture resize preview for %s", view->app_id);
		return false;
	}
	struct wlr_scene_buffer *preview = wlr_scene_buffer_create(
		view->scene_tree, buffer);
	wlr_buffer_drop(buffer);
	if (!preview) {
		return false;
	}
	view->resize_preview.buffer = preview;
	view->resize_preview.geometry = view->current;
	wlr_scene_node_set_enabled(&view->content_tree->node, false);
	resize_preview_update(view, view->current);
	return true;
}

void
resize_preview_update(struct view *view, struct wlr_box geo)
{
	struct wlr_scene_buffer *preview = view->resize_preview.buffer;
	if (!preview) {
		return;
	}
	view->resize_preview.geometry = geo;
	/* Decorations use render geometry; clients retain their committed allocation. */
	wlr_scene_node_set_position(&view->scene_tree->node, geo.x, geo.y);
	wlr_scene_buffer_set_dest_size(preview, geo.width, geo.height);
	ssd_update_geometry(view->ssd);
}

void
resize_preview_finish(struct view *view)
{
	if (!view->resize_preview.buffer) {
		return;
	}
	struct wlr_box geo = view->resize_preview.geometry;
	/* Apply client constraints once, then retain the ordinary opposite edge. */
	view_constrain_host_orientation(view, &geo);
	geo.x = server.grab_box.x;
	geo.y = server.grab_box.y;
	if (server.resize_edges & LAB_EDGE_LEFT) {
		geo.x += server.grab_box.width - geo.width;
	}
	if (server.resize_edges & LAB_EDGE_TOP) {
		geo.y += server.grab_box.height - geo.height;
	}
	view->resize_preview.submitted = true;
	view_move_resize(view, geo);
}

void
resize_preview_committed(struct view *view)
{
	if (view->resize_preview.submitted && !view->pending_configure_serial) {
		resize_preview_cancel(view);
	}
}
