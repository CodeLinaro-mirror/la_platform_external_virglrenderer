/**************************************************************************
 *
 * Copyright (C) 2024 AOSP..
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "vrend_winsys_gbm.h"
#include <stddef.h>

struct virgl_gbm *virgl_gbm_init(int fd) { return NULL; }

void virgl_gbm_fini(struct virgl_gbm *gbm) {
	return;
}

int virgl_gbm_convert_format(uint32_t *virgl_format, uint32_t *gbm_format) {
  return 0;
}

int virgl_gbm_transfer(struct gbm_bo *bo, uint32_t direction,
                       const struct iovec *iovecs, uint32_t num_iovecs,
                       const struct vrend_transfer_info *info) {
  return 0;
}

uint32_t virgl_gbm_convert_flags(uint32_t virgl_bind_flags) { return 0; }

int virgl_gbm_export_fd(struct gbm_device *gbm, uint32_t handle,
                        int32_t *out_fd) {
  return 0;
}

int virgl_gbm_export_query(struct gbm_bo *bo,
                           struct virgl_renderer_export_query *query) {
  return 0;
}

int virgl_gbm_get_plane_width(struct gbm_bo *bo, int plane) { return 0; }

int virgl_gbm_get_plane_height(struct gbm_bo *bo, int plane) { return 0; }

int virgl_gbm_get_plane_bytes_per_pixel(struct gbm_bo *bo, int plane) {
  return 0;
}

bool virgl_gbm_external_allocation_preferred(uint32_t flags) { return false; }

bool virgl_gbm_gpu_import_required(uint32_t flags) { return false; }
