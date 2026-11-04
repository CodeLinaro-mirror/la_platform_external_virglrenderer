/**************************************************************************
 *
 * Copyright (C) 2026 Red Hat Inc.
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

/* cursor readback tests */
#include "config.h"

#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <virglrenderer.h>

#include "pipe/p_defines.h"
#include "virgl_hw.h"
#include "virgl_iov.h"
#include "testvirgl.h"

static void cursor_readback_test(enum pipe_format format, uint32_t flags,
                                 uint32_t width, uint32_t height,
                                 uint32_t bpp)
{
   struct virgl_resource res;
   struct virgl_box box = {
      .x = 0,
      .y = 0,
      .z = 0,
      .w = width,
      .h = height,
      .d = 1,
   };
   const uint32_t size = width * height * bpp;
   uint32_t out_width = 0;
   uint32_t out_height = 0;
   uint8_t *data;
   uint8_t *input;
   int ret;
   unsigned x, y;

   ret = testvirgl_create_backed_2d_res(&res, 1, PIPE_TEXTURE_2D, format,
                                        width, height, PIPE_BIND_CURSOR,
                                        flags);
   ck_assert_int_eq(ret, 0);
   ck_assert_uint_eq(res.iovs[0].iov_len, size);
   input = res.iovs[0].iov_base;

   /* Make every row different so a missing vertical flip is detected. */
   for (y = 0; y < height; y++) {
      for (x = 0; x < width * bpp; x++)
         input[y * width * bpp + x] = (uint8_t)(y * width * bpp + x + 1);
   }

   virgl_renderer_ctx_attach_resource(1, res.handle);

   ret = virgl_renderer_transfer_write_iov(res.handle, 1, 0, 0, 0, &box, 0,
                                           res.iovs, 1);
   ck_assert_int_eq(ret, 0);

   data = virgl_renderer_get_cursor_data(res.handle, &out_width, &out_height);
   ck_assert_ptr_ne(data, NULL);
   ck_assert_uint_eq(out_width, width);
   ck_assert_uint_eq(out_height, height);
   ck_assert_int_eq(memcmp(data, input, size), 0);

   free(data);
   virgl_renderer_ctx_detach_resource(1, res.handle);
   testvirgl_destroy_backed_res(&res);
}

START_TEST(virgl_cursor_null_dims)
{
   struct virgl_resource res;
   uint32_t width = 0, height = 0;
   int ret;

   ret = testvirgl_init_single_ctx(context_flags);
   ck_assert_int_eq(ret, 0);

   ret = testvirgl_create_backed_2d_res(&res, 1, PIPE_TEXTURE_2D,
                                        PIPE_FORMAT_B8G8R8A8_UNORM, 4, 4,
                                        PIPE_BIND_CURSOR, 0);
   ck_assert_int_eq(ret, 0);

   ck_assert_ptr_eq(virgl_renderer_get_cursor_data(res.handle, NULL, &height),
                    NULL);
   ck_assert_uint_eq(height, 0);
   ck_assert_ptr_eq(virgl_renderer_get_cursor_data(res.handle, &width, NULL),
                    NULL);
   ck_assert_uint_eq(width, 0);

   testvirgl_destroy_backed_res(&res);
   testvirgl_fini_single_ctx();
}
END_TEST

START_TEST(virgl_cursor_too_large)
{
   struct virgl_resource res;
   uint32_t width = 0xdeadbeef;
   uint32_t height = 0xdeadbeef;
   int ret;

   ret = testvirgl_init_single_ctx(context_flags);
   ck_assert_int_eq(ret, 0);

   ret = testvirgl_create_backed_2d_res(&res, 1, PIPE_TEXTURE_2D,
                                        PIPE_FORMAT_B8G8R8A8_UNORM, 129, 1,
                                        PIPE_BIND_CURSOR, 0);
   ck_assert_int_eq(ret, 0);

   ck_assert_ptr_eq(virgl_renderer_get_cursor_data(res.handle, &width, &height),
                    NULL);
   ck_assert_uint_eq(width, 0xdeadbeef);
   ck_assert_uint_eq(height, 0xdeadbeef);

   testvirgl_destroy_backed_res(&res);
   testvirgl_fini_single_ctx();
}
END_TEST

START_TEST(virgl_cursor_wrong_target)
{
   struct virgl_resource res;
   uint32_t width, height;
   int ret;

   ret = testvirgl_init_single_ctx(context_flags);
   ck_assert_int_eq(ret, 0);

   ret = testvirgl_create_backed_2d_res(&res, 1, PIPE_TEXTURE_3D,
                                        PIPE_FORMAT_B8G8R8A8_UNORM, 4, 4,
                                        PIPE_BIND_SAMPLER_VIEW, 0);
   ck_assert_int_eq(ret, 0);

   ck_assert_ptr_eq(virgl_renderer_get_cursor_data(res.handle, &width, &height),
                    NULL);

   testvirgl_destroy_backed_res(&res);
   testvirgl_fini_single_ctx();
}
END_TEST

START_TEST(virgl_cursor_compressed)
{
   struct virgl_resource res;
   uint32_t width, height;
   int ret;

   ret = testvirgl_init_single_ctx(context_flags);
   ck_assert_int_eq(ret, 0);

   ret = testvirgl_create_backed_2d_res(&res, 1, PIPE_TEXTURE_2D,
                                        PIPE_FORMAT_DXT1_RGB, 8, 8,
                                        PIPE_BIND_SAMPLER_VIEW, 0);
   ck_assert_int_eq(ret, 0);

   ck_assert_ptr_eq(virgl_renderer_get_cursor_data(res.handle, &width, &height),
                    NULL);

   testvirgl_destroy_backed_res(&res);
   testvirgl_fini_single_ctx();
}
END_TEST

START_TEST(virgl_cursor_depth_stencil)
{
   struct virgl_resource res;
   uint32_t width, height;
   int ret;

   ret = testvirgl_init_single_ctx(context_flags);
   ck_assert_int_eq(ret, 0);

   ret = testvirgl_create_backed_2d_res(&res, 1, PIPE_TEXTURE_2D,
                                        PIPE_FORMAT_X24S8_UINT, 4, 4,
                                        PIPE_BIND_DEPTH_STENCIL, 0);
   ck_assert_int_eq(ret, 0);

   ck_assert_ptr_eq(virgl_renderer_get_cursor_data(res.handle, &width, &height),
                    NULL);

   testvirgl_destroy_backed_res(&res);
   testvirgl_fini_single_ctx();
}
END_TEST

START_TEST(virgl_cursor_readback)
{
   int ret;

   ret = testvirgl_init_single_ctx(context_flags);
   ck_assert_int_eq(ret, 0);

   cursor_readback_test(PIPE_FORMAT_B8G8R8A8_UNORM, 0, 4, 3, 4);

   testvirgl_fini_single_ctx();
}
END_TEST

START_TEST(virgl_cursor_readback_y_0_top)
{
   int ret;

   ret = testvirgl_init_single_ctx(context_flags);
   ck_assert_int_eq(ret, 0);

   cursor_readback_test(PIPE_FORMAT_B8G8R8A8_UNORM, VIRGL_RESOURCE_Y_0_TOP,
                        4, 3, 4);

   testvirgl_fini_single_ctx();
}
END_TEST

START_TEST(virgl_cursor_readback_unaligned_rows)
{
   int ret;

   /* On GLES only implementation-specific color read formats are eligible,
    * so a 1-byte format cannot be read back there. */
   if (context_flags & VIRGL_RENDERER_USE_GLES)
      return;

   ret = testvirgl_init_single_ctx(context_flags);
   ck_assert_int_eq(ret, 0);

   /* 3 pixels per row at 1 byte per pixel: rows are not 4-byte aligned. */
   cursor_readback_test(PIPE_FORMAT_R8_UNORM, 0, 3, 2, 1);

   testvirgl_fini_single_ctx();
}
END_TEST

static Suite *virgl_cursor_suite(void)
{
   Suite *s;
   TCase *tc_core;

   s = suite_create("virgl_cursor");
   tc_core = tcase_create("cursor");

   tcase_add_test(tc_core, virgl_cursor_null_dims);
   tcase_add_test(tc_core, virgl_cursor_too_large);
   tcase_add_test(tc_core, virgl_cursor_wrong_target);
   tcase_add_test(tc_core, virgl_cursor_compressed);
   tcase_add_test(tc_core, virgl_cursor_depth_stencil);
   tcase_add_test(tc_core, virgl_cursor_readback);
   tcase_add_test(tc_core, virgl_cursor_readback_y_0_top);
   tcase_add_test(tc_core, virgl_cursor_readback_unaligned_rows);
   suite_add_tcase(s, tc_core);

   return s;
}

int main(void)
{
   Suite *s;
   SRunner *sr;
   int number_failed;

   if (getenv("VRENDTEST_USE_EGL_SURFACELESS"))
      context_flags |= VIRGL_RENDERER_USE_SURFACELESS;
   if (getenv("VRENDTEST_USE_EGL_GLES"))
      context_flags |= VIRGL_RENDERER_USE_GLES;

   s = virgl_cursor_suite();
   sr = srunner_create(s);

   srunner_run_all(sr, CK_NORMAL);
   number_failed = srunner_ntests_failed(sr);
   srunner_free(sr);

   return number_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
