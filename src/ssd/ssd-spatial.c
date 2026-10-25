// SPDX-License-Identifier: GPL-2.0-only

#include <cairo.h>
#include <limits.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include "buffer.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "node.h"
#include "img/img.h"
#include "scaled-buffer/scaled-font-buffer.h"
#include "scaled-buffer/scaled-icon-buffer.h"
#include "ssd.h"
#include "ssd-internal.h"

/* Heading hit targets and feedback share compositor logical coordinates,
 * independent of the touchpad or Android display density. */
#define CONTROL_HEIGHT 44
#define CONTROL_GAP 24
#define CONTROL_MIN_WIDTH 220
#define CONTROL_MAX_WIDTH 460
#define FRAME_PADDING 24
#define HANDLE_SIZE 88
#define HANDLE_OUTSET 44
#define MOVE_GLOW_WIDTH 640
#define MOVE_TOP_MARGIN 12

static const double pi = 3.141592653589793;
static bool spatial_style;
static struct ssd *hovered_ssd;
static void update_controls(struct ssd *ssd);

void
ssd_set_spatial_style(bool enabled)
{
	spatial_style = enabled;
}

bool
ssd_spatial_style_enabled(void)
{
	return spatial_style;
}

void
ssd_spatial_configure_theme(struct theme *theme)
{
	if (!spatial_style) {
		return;
	}
	theme->titlebar_height = CONTROL_HEIGHT + CONTROL_GAP;
	theme->window_button_width = CONTROL_HEIGHT;
	theme->window_button_height = CONTROL_HEIGHT;
	theme->window_button_hover_bg_corner_radius = CONTROL_HEIGHT / 2;
	/* The wider window surround must not widen menus or switcher outlines. */
	if (theme->menu_border_width == INT_MIN) {
		theme->menu_border_width = theme->border_width;
	}
	if (theme->osd_border_width == INT_MIN) {
		theme->osd_border_width = theme->border_width;
	}
	theme->border_width = FRAME_PADDING;
	enum ssd_active_state active;
	FOR_EACH_ACTIVE_STATE(active) {
		float alpha = active ? 0.18f : 0.10f;
		float color[] = {0.91f * alpha, 0.93f * alpha, 0.95f * alpha, alpha};
		for (int i = 0; i < 4; ++i) {
			theme->window[active].border_color[i] = color[i];
		}
	}
	rc.resize_minimum_area = HANDLE_OUTSET;
	rc.resize_corner_range = HANDLE_SIZE;
}

static int
control_width(const struct ssd *ssd)
{
	int preferred = MAX(CONTROL_MIN_WIDTH,
		MIN(CONTROL_MAX_WIDTH, ssd->spatial_frame.title_width + 2 * CONTROL_HEIGHT + 24));
	return MAX(2 * CONTROL_HEIGHT, MIN(preferred, ssd->view->current.width));
}

bool
ssd_spatial_drag_ready(const struct ssd *ssd, double dx, double dy)
{
	/* A click may jitter slightly; only a real drag should consume Close. */
	return !ssd || !ssd->spatial || dx * dx + dy * dy >= 16;
}

static int
control_y(const struct ssd *ssd)
{
	int frame = ssd->view->maximized == VIEW_AXIS_BOTH ? 0 : FRAME_PADDING;
	return -CONTROL_HEIGHT - CONTROL_GAP - frame;
}

static struct wlr_box
move_box(const struct ssd *ssd)
{
	int y = control_y(ssd) - MOVE_TOP_MARGIN;
	return (struct wlr_box) {
		.x = 0, .y = y, .width = ssd->view->current.width, .height = -y,
	};
}

static int
move_glow_width(int width)
{
	return MIN(MAX(1, width / 2), MOVE_GLOW_WIDTH);
}

static bool
never_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

static struct lab_data_buffer *
control_buffer(int width, bool active)
{
	struct lab_data_buffer *buffer = buffer_create_cairo(width, CONTROL_HEIGHT, 1);
	die_if_null(buffer);
	cairo_t *cr = cairo_create(buffer->surface);
	/* A quiet floating surface, separate from the application's content. */
	cairo_arc(cr, CONTROL_HEIGHT / 2, CONTROL_HEIGHT / 2,
		CONTROL_HEIGHT / 2 - 1, pi / 2, 3 * pi / 2);
	cairo_arc(cr, width - CONTROL_HEIGHT / 2, CONTROL_HEIGHT / 2,
		CONTROL_HEIGHT / 2 - 1, 3 * pi / 2, 5 * pi / 2);
	cairo_close_path(cr);
	cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 0, 0, CONTROL_HEIGHT);
	cairo_pattern_add_color_stop_rgba(gradient, 0, 0.99, 0.99, 1, active ? 1 : 0.92);
	cairo_pattern_add_color_stop_rgba(gradient, 1, 0.88, 0.91, 0.94, active ? 1 : 0.92);
	cairo_set_source(cr, gradient);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(gradient);
	cairo_set_source_rgba(cr, 0.36, 0.40, 0.46, 0.25);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	cairo_surface_flush(buffer->surface);
	cairo_destroy(cr);
	return buffer;
}

static struct wlr_scene_buffer *
corner_buffer(struct wlr_scene_tree *parent, int corner)
{
	struct lab_data_buffer *buffer = buffer_create_cairo(HANDLE_SIZE, HANDLE_SIZE, 1);
	die_if_null(buffer);
	cairo_t *cr = cairo_create(buffer->surface);
	bool right = corner == 1 || corner == 2;
	bool bottom = corner >= 2;
	cairo_translate(cr, right ? HANDLE_SIZE : 0, bottom ? HANDLE_SIZE : 0);
	cairo_scale(cr, right ? -1 : 1, bottom ? -1 : 1);
	cairo_move_to(cr, 8, 56);
	cairo_line_to(cr, 8, 8);
	cairo_line_to(cr, 56, 8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba(cr, 0.08, 0.12, 0.16, 0.5);
	cairo_set_line_width(cr, 9);
	cairo_stroke_preserve(cr);
	cairo_set_source_rgba(cr, 0.96, 0.98, 1, 1);
	cairo_set_line_width(cr, 5);
	cairo_stroke(cr);
	cairo_surface_flush(buffer->surface);
	cairo_destroy(cr);
	struct wlr_scene_buffer *scene = lab_wlr_scene_buffer_create(parent, &buffer->base);
	wlr_buffer_drop(&buffer->base);
	scene->point_accepts_input = never_accepts_input;
	wlr_scene_node_set_enabled(&scene->node, false);
	return scene;
}

static void
draw_close(cairo_t *cr, int width, int height)
{
	double x = width / 2.0, y = height / 2.0;
	cairo_set_source_rgba(cr, 0.18, 0.22, 0.27, 1);
	cairo_set_line_width(cr, 1.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, x - 5, y - 5);
	cairo_line_to(cr, x + 5, y + 5);
	cairo_move_to(cr, x + 5, y - 5);
	cairo_line_to(cr, x - 5, y + 5);
	cairo_stroke(cr);
}

static void
draw_hover(cairo_t *cr, int width, int height)
{
	cairo_set_source_rgba(cr, 0.18, 0.22, 0.27, 0.10);
	cairo_arc(cr, width / 2.0, height / 2.0, 18, 0, 2 * pi);
	cairo_fill(cr);
}

static void draw_close_hover(cairo_t *cr, int w, int h)
{
	draw_hover(cr, w, h);
	draw_close(cr, w, h);
}

static void
attach_close(struct ssd *ssd, struct ssd_titlebar_subtree *subtree)
{
	/* Reuse the normal SSD button lifecycle, hit testing and hover dispatch. */
	const char blank[6] = {0};
	float color[4] = {0};
	struct lab_img *imgs[LAB_BS_ALL + 1] = {0};
	imgs[LAB_BS_DEFAULT] = lab_img_load_from_bitmap(blank, color);
	imgs[LAB_BS_HOVERED] = lab_img_load_from_bitmap(blank, color);
	lab_img_add_modifier(imgs[LAB_BS_DEFAULT], draw_close);
	lab_img_add_modifier(imgs[LAB_BS_HOVERED], draw_close_hover);
	attach_ssd_button(&subtree->buttons_right, LAB_NODE_BUTTON_CLOSE,
		subtree->tree, imgs, 0, 0, ssd->view);
	lab_img_destroy(imgs[LAB_BS_DEFAULT]);
	lab_img_destroy(imgs[LAB_BS_HOVERED]);
}

void
ssd_spatial_border_update(struct ssd *ssd)
{
	int width = ssd->view->current.width;
	int height = view_effective_height(ssd->view, false);
	enum ssd_active_state active;
	FOR_EACH_ACTIVE_STATE(active) {
		struct ssd_border_subtree *subtree = &ssd->border.subtrees[active];
		/* Four disjoint rectangles leave all client pixels and input untouched. */
		wlr_scene_rect_set_size(subtree->top, width + 2 * FRAME_PADDING, FRAME_PADDING);
		wlr_scene_node_set_position(&subtree->top->node, 0, -FRAME_PADDING);
		wlr_scene_rect_set_size(subtree->bottom, width + 2 * FRAME_PADDING, FRAME_PADDING);
		wlr_scene_node_set_position(&subtree->bottom->node, 0, height);
		wlr_scene_rect_set_size(subtree->left, FRAME_PADDING, height);
		wlr_scene_node_set_position(&subtree->left->node, 0, 0);
		wlr_scene_rect_set_size(subtree->right, FRAME_PADDING, height);
		wlr_scene_node_set_position(&subtree->right->node, width + FRAME_PADDING, 0);
	}
}

static struct wlr_scene_buffer *
move_glow_create(struct wlr_scene_tree *parent)
{
	int height = MOVE_TOP_MARGIN + CONTROL_HEIGHT + CONTROL_GAP + FRAME_PADDING;
	struct lab_data_buffer *buffer = buffer_create_cairo(MOVE_GLOW_WIDTH, height, 1);
	die_if_null(buffer);
	cairo_t *cr = cairo_create(buffer->surface);
	/* A cached elliptical highlight, positioned on hover without repainting. */
	cairo_scale(cr, MOVE_GLOW_WIDTH / 2.0, height);
	cairo_pattern_t *glow = cairo_pattern_create_radial(1, 0.4, 0, 1, 0.4, 1);
	cairo_pattern_add_color_stop_rgba(glow, 0, 1, 1, 1, 0.62);
	cairo_pattern_add_color_stop_rgba(glow, 0.45, 0.95, 0.97, 1, 0.28);
	cairo_pattern_add_color_stop_rgba(glow, 1, 1, 1, 1, 0);
	cairo_set_source(cr, glow);
	cairo_paint(cr);
	cairo_pattern_destroy(glow);
	cairo_surface_flush(buffer->surface);
	cairo_destroy(cr);
	struct wlr_scene_buffer *scene = lab_wlr_scene_buffer_create(parent, &buffer->base);
	wlr_buffer_drop(&buffer->base);
	scene->point_accepts_input = never_accepts_input;
	wlr_scene_node_set_enabled(&scene->node, false);
	return scene;
}

void
ssd_spatial_update_title(struct ssd *ssd)
{
	if (!ssd->state.title.text || strcmp(ssd->state.title.text, ssd->view->title)) {
		xstrdup_replace(ssd->state.title.text, ssd->view->title);
		ssd->spatial_frame.title_width = MAX(
			font_width(&rc.font_activewindow, ssd->view->title),
			font_width(&rc.font_inactivewindow, ssd->view->title));
		ssd->spatial_frame.title_dirty = true;
	}
	update_controls(ssd);
}

static void
update_controls(struct ssd *ssd)
{
	int width = control_width(ssd);
	int x = (ssd->view->current.width - width) / 2;
	enum ssd_active_state active;
	FOR_EACH_ACTIVE_STATE(active) {
		struct ssd_titlebar_subtree *subtree = &ssd->titlebar.subtrees[active];
		wlr_scene_node_set_position(&subtree->tree->node,
			x, control_y(ssd));
		if (width != ssd->spatial_frame.width) {
			struct lab_data_buffer *buffer = control_buffer(width, active);
			wlr_scene_buffer_set_buffer(subtree->bar, &buffer->base);
			wlr_buffer_drop(&buffer->base);
		}
		struct ssd_button *button;
		wl_list_for_each(button, &subtree->buttons_right, link) {
			wlr_scene_node_set_position(button->node, width - CONTROL_HEIGHT, 0);
		}
		struct scaled_font_buffer *title = subtree->title;
		int available = MAX(0, width - 2 * CONTROL_HEIGHT - 16);
		wlr_scene_node_set_enabled(&title->scene_buffer->node, rc.show_title && available > 0);
		if (available > 0 && (ssd->spatial_frame.title_dirty || width != ssd->spatial_frame.width)) {
			const float color[] = {0.16, 0.20, 0.25, 1};
			const float transparent[] = {0};
			scaled_font_buffer_update(title, ssd->view->title, available,
				active ? &rc.font_activewindow : &rc.font_inactivewindow, color, transparent);
		}
		wlr_scene_node_set_position(&title->scene_buffer->node,
			(width - title->width) / 2, (CONTROL_HEIGHT - title->height) / 2);
	}
	ssd->spatial_frame.width = width;
	ssd->spatial_frame.title_dirty = false;
}

void
ssd_spatial_update(struct ssd *ssd)
{
	update_controls(ssd);
	struct wlr_box band = move_box(ssd);
	wlr_scene_rect_set_size(ssd->spatial_frame.move_area, band.width, band.height);
	wlr_scene_node_set_position(&ssd->spatial_frame.move_area->node, band.x, band.y);
	wlr_scene_buffer_set_dest_size(ssd->spatial_frame.move_glow,
		move_glow_width(band.width), band.height);
	int height = view_effective_height(ssd->view, false);
	for (int i = 0; i < 4; ++i) {
		bool right = i == 1 || i == 2;
		bool bottom = i >= 2;
		wlr_scene_node_set_position(&ssd->spatial_frame.corners[i]->node,
			right ? ssd->view->current.width - HANDLE_SIZE + HANDLE_OUTSET : -HANDLE_OUTSET,
			bottom ? height - HANDLE_SIZE + HANDLE_OUTSET : -HANDLE_OUTSET);
	}
	ssd->state.was_maximized = ssd->view->maximized == VIEW_AXIS_BOTH;
	ssd->state.was_shaded = ssd->view->shaded;
	ssd->state.was_omnipresent = ssd->view->visible_on_all_workspaces;
	ssd->state.was_squared = ssd_should_be_squared(ssd);
}

void
ssd_spatial_create(struct ssd *ssd)
{
	ssd->titlebar.tree = lab_wlr_scene_tree_create(ssd->tree);
	node_descriptor_create(&ssd->titlebar.tree->node,
		LAB_NODE_TITLEBAR, ssd->view, NULL);
	float invisible[4] = {0};
	ssd->spatial_frame.move_area = lab_wlr_scene_rect_create(ssd->titlebar.tree, 0, 0, invisible);
	node_descriptor_create(&ssd->spatial_frame.move_area->node,
		LAB_NODE_TITLE, ssd->view, NULL);
	ssd->spatial_frame.move_glow = move_glow_create(ssd->titlebar.tree);
	enum ssd_active_state active;
	FOR_EACH_ACTIVE_STATE(active) {
		struct ssd_titlebar_subtree *subtree = &ssd->titlebar.subtrees[active];
		subtree->tree = lab_wlr_scene_tree_create(ssd->titlebar.tree);
		subtree->bar = lab_wlr_scene_buffer_create(subtree->tree, NULL);
		subtree->title = scaled_font_buffer_create(subtree->tree);
		die_if_null(subtree->title);
		node_descriptor_create(&subtree->title->scene_buffer->node,
			LAB_NODE_TITLE, ssd->view, NULL);
		struct scaled_icon_buffer *icon = scaled_icon_buffer_create_client(subtree->tree, 24, 24);
		scaled_icon_buffer_set_view(icon, ssd->view);
		wlr_scene_node_set_position(&icon->scene_buffer->node, 10, 10);
		node_descriptor_create(&icon->scene_buffer->node, LAB_NODE_TITLE, ssd->view, NULL);
		wl_list_init(&subtree->buttons_left);
		wl_list_init(&subtree->buttons_right);
		attach_close(ssd, subtree);
	}
	ssd->spatial_frame.feedback = lab_wlr_scene_tree_create(ssd->tree);
	for (int i = 0; i < 4; ++i) {
		ssd->spatial_frame.corners[i] = corner_buffer(ssd->spatial_frame.feedback, i);
	}
	ssd_spatial_update_title(ssd);
	ssd_spatial_update(ssd);
}

enum lab_node_type
ssd_spatial_context(const struct ssd *ssd, struct wlr_cursor *cursor)
{
	struct wlr_box box = ssd->view->current;
	box.height = view_effective_height(ssd->view, false);
	struct wlr_box controls = {
		.x = box.x + (box.width - control_width(ssd)) / 2,
		.y = box.y + control_y(ssd),
		.width = control_width(ssd), .height = CONTROL_HEIGHT,
	};
	struct wlr_box move_area = move_box(ssd);
	move_area.x += box.x;
	move_area.y += box.y;
	/* Buttons keep their own click/drag bindings inside the larger move target. */
	if (wlr_box_contains_point(&box, cursor->x, cursor->y)
			|| (view_titlebar_visible(ssd->view)
			&& wlr_box_contains_point(&controls, cursor->x, cursor->y))) {
		return LAB_NODE_NONE;
	}
	if (view_titlebar_visible(ssd->view)
			&& wlr_box_contains_point(&move_area, cursor->x, cursor->y)) {
		return LAB_NODE_TITLE;
	}
	int corner = MIN(rc.resize_corner_range, MIN(box.width, box.height) / 2);
	bool left = cursor->x < box.x + corner;
	bool right = cursor->x >= box.x + box.width - corner;
	bool top = cursor->y < box.y + corner;
	bool bottom = cursor->y >= box.y + box.height - corner;
	if (left && top) {
		return LAB_NODE_CORNER_TOP_LEFT;
	} else if (right && top) {
		return LAB_NODE_CORNER_TOP_RIGHT;
	} else if (right && bottom) {
		return LAB_NODE_CORNER_BOTTOM_RIGHT;
	} else if (left && bottom) {
		return LAB_NODE_CORNER_BOTTOM_LEFT;
	}
	/* The outer edges move; only corners resize. Client content is untouched. */
	return LAB_NODE_TITLEBAR;
}

void
ssd_spatial_update_hover(struct view *view, enum lab_node_type type, struct wlr_cursor *cursor)
{
	struct ssd *ssd = view ? view->ssd : NULL;
	if (hovered_ssd && hovered_ssd != ssd) {
		wlr_scene_node_set_enabled(&hovered_ssd->spatial_frame.move_glow->node, false);
		for (int i = 0; i < 4; ++i) {
			wlr_scene_node_set_enabled(&hovered_ssd->spatial_frame.corners[i]->node, false);
		}
	}
	hovered_ssd = ssd && ssd->spatial ? ssd : NULL;
	if (!hovered_ssd) {
		return;
	}
	struct wlr_box band = move_box(ssd);
	int glow_width = move_glow_width(band.width);
	bool move_hover = view_titlebar_visible(view)
		&& (type == LAB_NODE_TITLE || type == LAB_NODE_TITLEBAR)
		&& wlr_box_contains_point(&band, cursor->x - view->current.x,
			cursor->y - view->current.y);
	wlr_scene_node_set_enabled(&ssd->spatial_frame.move_glow->node, move_hover);
	if (move_hover) {
		int x = MAX(0, MIN(band.width - glow_width,
			(int)(cursor->x - view->current.x) - glow_width / 2));
		wlr_scene_node_set_position(&ssd->spatial_frame.move_glow->node, x, band.y);
	}
	const enum lab_node_type types[] = {LAB_NODE_CORNER_TOP_LEFT,
		LAB_NODE_CORNER_TOP_RIGHT, LAB_NODE_CORNER_BOTTOM_RIGHT, LAB_NODE_CORNER_BOTTOM_LEFT};
	for (int i = 0; i < 4; ++i) {
		wlr_scene_node_set_enabled(&ssd->spatial_frame.corners[i]->node, types[i] == type);
	}
}

void
ssd_spatial_finish(struct ssd *ssd)
{
	if (hovered_ssd == ssd) {
		hovered_ssd = NULL;
	}
	/* All buffers and scene nodes are owned by ssd->tree. */
}
