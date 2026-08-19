#ifndef ANLABWC_EMBED_H
#define ANLABWC_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

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
/*
 * Bind a GPU AHB to a compositor view. Dest comes from the view layout
 * (SSD content box), not from caller screen coordinates.
 * kind: ANLABWC_GPU_X11 (id = XID) or ANLABWC_GPU_WAYLAND (id unused,
 * pid identifies the xdg client). pid is SO_PEERCRED of the GL process.
 */
#define ANLABWC_GPU_X11 1
#define ANLABWC_GPU_WAYLAND 2
ANLABWC_API int anlabwc_present_ahb_view(struct AHardwareBuffer *ahb,
	int kind, uint32_t id, uint32_t pid, int w, int h);
ANLABWC_API const char *anlabwc_wayland_socket(void);

#ifdef __cplusplus
}
#endif

#endif
