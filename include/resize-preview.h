/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_RESIZE_PREVIEW_H
#define LABWC_RESIZE_PREVIEW_H
#include "config.h"
#include <stdbool.h>
#include <wlr/util/box.h>
struct view;
#if HAVE_ANDROID_EMBED
bool resize_preview_begin(struct view *view);
void resize_preview_update(struct view *view, struct wlr_box geo);
void resize_preview_finish(struct view *view);
void resize_preview_cancel(struct view *view);
void resize_preview_committed(struct view *view);
#else
static inline bool resize_preview_begin(struct view *view) { return true; }
static inline void resize_preview_update(struct view *view, struct wlr_box geo) { }
static inline void resize_preview_finish(struct view *view) { }
static inline void resize_preview_cancel(struct view *view) { }
static inline void resize_preview_committed(struct view *view) { }
#endif
#endif
