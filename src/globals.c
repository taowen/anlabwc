/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"
#include "labwc.h"
#include "config/rcxml.h"

struct rcxml rc = { 0 };
struct server server = {
#if HAVE_ANDROID_EMBED
	.embed = {
		.input_rd = -1,
		.input_wr = -1,
	},
#endif
};
