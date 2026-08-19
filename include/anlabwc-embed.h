#ifndef ANLABWC_EMBED_H
#define ANLABWC_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;
struct AHardwareBuffer;

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
ANLABWC_API int anlabwc_pointer(float x, float y, int button, int pressed);
ANLABWC_API int anlabwc_key(int evdev, int pressed);
ANLABWC_API int anlabwc_present_ahb(struct AHardwareBuffer *ahb,
	int x, int y, int w, int h);
ANLABWC_API const char *anlabwc_wayland_socket(void);

#ifdef __cplusplus
}
#endif

#endif
