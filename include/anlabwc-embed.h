#ifndef ANLABWC_EMBED_H
#define ANLABWC_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct ANativeWindow;

#ifndef ANLABWC_API
#if defined(__GNUC__)
#define ANLABWC_API __attribute__((visibility("default")))
#else
#define ANLABWC_API
#endif
#endif

/*
 * Run labwc on the Activity ANativeWindow. Blocks until
 * anlabwc_request_stop() or the display loop ends.
 * Wayland socket is ${runtime_dir}/wayland-0.
 */
ANLABWC_API int anlabwc_run(struct ANativeWindow *window, int width, int height,
	const char *runtime_dir);
ANLABWC_API void anlabwc_request_stop(void);
/* NULL suspends scanout without destroying clients; called outside the event loop. */
ANLABWC_API int anlabwc_set_window(struct ANativeWindow *window, int width, int height);
ANLABWC_API int anlabwc_pointer(float x, float y, int button, int pressed);
/* Logical hand id: 0 is right, 1 is left; Android owns cursor rendering. */
ANLABWC_API int anlabwc_pointer_v2(int pointer_id, float x, float y,
	int button, int pressed);
ANLABWC_API int anlabwc_cursor_shape(void);
/*
 * Copies the current client-provided cursor image as Android ARGB pixels.
 * Returns the required pixel count, or zero when no custom image is active.
 * Passing NULL/zero capacity only queries metadata and the required size.
 */
ANLABWC_API int anlabwc_cursor_image(uint32_t *pixels, int capacity,
	int *width, int *height, int *hotspot_x, int *hotspot_y,
	uint32_t *serial);
/* Increments whenever the seat's primary text selection changes. */
ANLABWC_API uint32_t anlabwc_primary_selection_serial(void);
/* True while the compositor owns the pointer for window move or resize. */
ANLABWC_API int anlabwc_window_grab_active(void);
/* 0=none, 1=move, 2=resize. Mirrors the compositor input mode. */
ANLABWC_API int anlabwc_window_grab_mode(void);
/* Cursor shape for resizing the currently grabbed window at (x, y), or 1. */
ANLABWC_API int anlabwc_window_resize_shape(float x, float y);
/* action: 1=begin, 2=update, 3=end. Coordinates are compositor pixels. */
ANLABWC_API int anlabwc_window_transform(int action, float anchor_x,
	float anchor_y, float focus_x, float focus_y);
ANLABWC_API int anlabwc_axis(float dx, float dy);
ANLABWC_API int anlabwc_key(int evdev, int pressed);
/* UTF-32 codepoint. Looks up the current XKB map (US + Shift). */
ANLABWC_API int anlabwc_unicode(uint32_t codepoint);
ANLABWC_API const char *anlabwc_wayland_socket(void);

#ifdef __cplusplus
}
#endif

#endif
