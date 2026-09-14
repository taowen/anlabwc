/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_ANDROID_BUFFER_LAYOUT_H
#define LABWC_ANDROID_BUFFER_LAYOUT_H
#include <android/hardware_buffer.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Return only a mapper-attested single-plane linear RGBA/BGRA layout. */
bool android_buffer_linear_layout(AHardwareBuffer *buffer, uint32_t *format,
    uint32_t *stride);
#ifdef __cplusplus
}
#endif
#endif
