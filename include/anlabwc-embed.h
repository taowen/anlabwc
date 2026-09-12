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
ANLABWC_API int anlabwc_axis(float dx, float dy);
ANLABWC_API int anlabwc_key(int evdev, int pressed);
/* UTF-32 codepoint. Looks up the current XKB map (US + Shift). */
ANLABWC_API int anlabwc_unicode(uint32_t codepoint);
ANLABWC_API const char *anlabwc_wayland_socket(void);

#ifdef __cplusplus
}
#endif

#endif
