/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Xiang Haihao <haihao.xiang@intel.com>
 *
 */

#include "sysdeps.h"

#include <va/va.h>
#include <va/va_dec_hevc.h>

#include "intel_batchbuffer.h"
#include "intel_driver.h"
#include "i965_defines.h"
#include "i965_drv_video.h"
#include "i965_decoder_utils.h"

#include "gen9_mfd.h"
#include "intel_media.h"

#define OUT_BUFFER(buf_bo, is_target, ma)  do {                         \
        if (buf_bo) {                                                   \
            OUT_BCS_RELOC(batch,                                        \
                          buf_bo,                                       \
                          I915_GEM_DOMAIN_RENDER,                       \
                          is_target ? I915_GEM_DOMAIN_RENDER : 0,       \
                          0);                                           \
        } else {                                                        \
            OUT_BCS_BATCH(batch, 0);                                    \
        }                                                               \
        OUT_BCS_BATCH(batch, 0);                                        \
        if (ma)                                                         \
            OUT_BCS_BATCH(batch, 0);                                    \
    } while (0)

#define OUT_BUFFER_MA_TARGET(buf_bo)       OUT_BUFFER(buf_bo, 1, 1)
#define OUT_BUFFER_MA_REFERENCE(buf_bo)    OUT_BUFFER(buf_bo, 0, 1)
#define OUT_BUFFER_NMA_TARGET(buf_bo)      OUT_BUFFER(buf_bo, 1, 0)
#define OUT_BUFFER_NMA_REFERENCE(buf_bo)   OUT_BUFFER(buf_bo, 0, 0)

static void
gen9_hcpd_init_hevc_surface(VADriverContextP ctx,
                            VAPictureParameterBufferHEVC *pic_param,
                            struct object_surface *obj_surface,
                            struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    GenHevcSurface *gen9_hevc_surface;

    if (!obj_surface)
        return;

    obj_surface->free_private_data = gen_free_hevc_surface;
    gen9_hevc_surface = obj_surface->private_data;

    if (!gen9_hevc_surface) {
        gen9_hevc_surface = calloc(sizeof(GenHevcSurface), 1);
        obj_surface->private_data = gen9_hevc_surface;
    }

    if (gen9_hevc_surface->motion_vector_temporal_bo == NULL) {
        uint32_t size;

        if (gen9_hcpd_context->ctb_size == 16)
            size = ((gen9_hcpd_context->picture_width_in_pixels + 63) >> 6) *
                ((gen9_hcpd_context->picture_height_in_pixels + 15) >> 4);
        else
            size = ((gen9_hcpd_context->picture_width_in_pixels + 31) >> 5) *
                ((gen9_hcpd_context->picture_height_in_pixels + 31) >> 5);

        size <<= 6; /* in unit of 64bytes */
        gen9_hevc_surface->motion_vector_temporal_bo = dri_bo_alloc(i965->intel.bufmgr,
                                                                    "motion vector temporal buffer",
                                                                    size,
                                                                    0x1000);
    }
}

static VAStatus
gen9_hcpd_hevc_decode_init(VADriverContextP ctx,
                           struct decode_state *decode_state,
                           struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    VAPictureParameterBufferHEVC *pic_param;
    VASliceParameterBufferHEVC *slice_param;
    struct object_surface *obj_surface;
    uint32_t size;
    int i, j, has_inter = 0;

    for (j = 0; j < decode_state->num_slice_params && !has_inter; j++) {
        assert(decode_state->slice_params && decode_state->slice_params[j]->buffer);
        slice_param = (VASliceParameterBufferHEVC *)decode_state->slice_params[j]->buffer;

        for (i = 0; i < decode_state->slice_params[j]->num_elements; i++) {
            if (slice_param->LongSliceFlags.fields.slice_type == HEVC_SLICE_B ||
                slice_param->LongSliceFlags.fields.slice_type == HEVC_SLICE_P) {
                has_inter = 1;
                break;
            }

            slice_param++;
        }
    }

    assert(decode_state->pic_param && decode_state->pic_param->buffer);
    pic_param = (VAPictureParameterBufferHEVC *)decode_state->pic_param->buffer;

    gen9_hcpd_context->picture_width_in_pixels = pic_param->pic_width_in_luma_samples;
    gen9_hcpd_context->picture_height_in_pixels = pic_param->pic_height_in_luma_samples;
    gen9_hcpd_context->ctb_size = (1 << (pic_param->log2_min_luma_coding_block_size_minus3 +
                                         3 +
                                         pic_param->log2_diff_max_min_luma_coding_block_size));
    gen9_hcpd_context->picture_width_in_ctbs = ALIGN(gen9_hcpd_context->picture_width_in_pixels, gen9_hcpd_context->ctb_size) / gen9_hcpd_context->ctb_size;
    gen9_hcpd_context->picture_height_in_ctbs = ALIGN(gen9_hcpd_context->picture_height_in_pixels, gen9_hcpd_context->ctb_size) / gen9_hcpd_context->ctb_size;
    gen9_hcpd_context->min_cb_size = (1 << (pic_param->log2_min_luma_coding_block_size_minus3 + 3));
    gen9_hcpd_context->picture_width_in_min_cb_minus1 = gen9_hcpd_context->picture_width_in_pixels / gen9_hcpd_context->min_cb_size - 1;
    gen9_hcpd_context->picture_height_in_min_cb_minus1 = gen9_hcpd_context->picture_height_in_pixels / gen9_hcpd_context->min_cb_size - 1;

    /* Current decoded picture */
    obj_surface = decode_state->render_object;
    gen9_hcpd_init_hevc_surface(ctx, pic_param, obj_surface, gen9_hcpd_context);

    size = ALIGN(gen9_hcpd_context->picture_width_in_pixels, 32) >> 3;
    size <<= 6;
    ALLOC_GEN_BUFFER((&gen9_hcpd_context->deblocking_filter_line_buffer), "line buffer", size);
    ALLOC_GEN_BUFFER((&gen9_hcpd_context->deblocking_filter_tile_line_buffer), "tile line buffer", size);

    size = ALIGN(gen9_hcpd_context->picture_height_in_pixels + 6 * gen9_hcpd_context->picture_height_in_ctbs, 32) >> 3;
    size <<= 6;
    ALLOC_GEN_BUFFER((&gen9_hcpd_context->deblocking_filter_tile_column_buffer), "tile column buffer", size);

    if (has_inter) {
        size = (((gen9_hcpd_context->picture_width_in_pixels + 15) >> 4) * 188 + 9 * gen9_hcpd_context->picture_width_in_ctbs + 1023) >> 9;
        size <<= 6;
        ALLOC_GEN_BUFFER((&gen9_hcpd_context->metadata_line_buffer), "metadata line buffer", size);

        size = (((gen9_hcpd_context->picture_width_in_pixels + 15) >> 4) * 172 + 9 * gen9_hcpd_context->picture_width_in_ctbs + 1023) >> 9;
        size <<= 6;
        ALLOC_GEN_BUFFER((&gen9_hcpd_context->metadata_tile_line_buffer), "metadata tile line buffer", size);

        size = (((gen9_hcpd_context->picture_height_in_pixels + 15) >> 4) * 176 + 89 * gen9_hcpd_context->picture_width_in_ctbs + 1023) >> 9;
        size <<= 6;
        ALLOC_GEN_BUFFER((&gen9_hcpd_context->metadata_tile_column_buffer), "metadata tile column buffer", size);
    } else {
        size = (gen9_hcpd_context->picture_width_in_pixels + 8 * gen9_hcpd_context->picture_width_in_ctbs + 1023) >> 9;
        size <<= 6;
        ALLOC_GEN_BUFFER((&gen9_hcpd_context->metadata_line_buffer), "metadata line buffer", size);

        size = (gen9_hcpd_context->picture_width_in_pixels + 16 * gen9_hcpd_context->picture_width_in_ctbs + 1023) >> 9;
        size <<= 6;
        ALLOC_GEN_BUFFER((&gen9_hcpd_context->metadata_tile_line_buffer), "metadata tile line buffer", size);

        size = (gen9_hcpd_context->picture_height_in_pixels + 8 * gen9_hcpd_context->picture_height_in_ctbs + 1023) >> 9;
        size <<= 6;
        ALLOC_GEN_BUFFER((&gen9_hcpd_context->metadata_tile_column_buffer), "metadata tile column buffer", size);
    }

    size = ALIGN(((gen9_hcpd_context->picture_width_in_pixels >> 1) + 3 * gen9_hcpd_context->picture_width_in_ctbs), 16) >> 3;
    size <<= 6;
    ALLOC_GEN_BUFFER((&gen9_hcpd_context->sao_line_buffer), "sao line buffer", size);

    size = ALIGN(((gen9_hcpd_context->picture_width_in_pixels >> 1) + 6 * gen9_hcpd_context->picture_width_in_ctbs), 16) >> 3;
    size <<= 6;
    ALLOC_GEN_BUFFER((&gen9_hcpd_context->sao_tile_line_buffer), "sao tile line buffer", size);

    size = ALIGN(((gen9_hcpd_context->picture_height_in_pixels >> 1) + 6 * gen9_hcpd_context->picture_height_in_ctbs), 16) >> 3;
    size <<= 6;
    ALLOC_GEN_BUFFER((&gen9_hcpd_context->sao_tile_column_buffer), "sao tile column buffer", size);

    return VA_STATUS_SUCCESS;
}

static void
gen9_hcpd_pipe_mode_select(VADriverContextP ctx,
                           struct decode_state *decode_state,
                           int codec,
                           struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct intel_batchbuffer *batch = gen9_hcpd_context->base.batch;

    assert(codec == HCP_CODEC_HEVC);

    BEGIN_BCS_BATCH(batch, 4);

    OUT_BCS_BATCH(batch, HCP_PIPE_MODE_SELECT | (4 - 2));
    OUT_BCS_BATCH(batch,
                  (codec << 5) |
                  (0 << 3) | /* disable Pic Status / Error Report */
                  HCP_CODEC_SELECT_DECODE);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen9_hcpd_surface_state(VADriverContextP ctx,
                        struct decode_state *decode_state,
                        struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct intel_batchbuffer *batch = gen9_hcpd_context->base.batch;
    struct object_surface *obj_surface = decode_state->render_object;
    unsigned int y_cb_offset;

    assert(obj_surface);

    y_cb_offset = obj_surface->y_cb_offset;

    BEGIN_BCS_BATCH(batch, 3);

    OUT_BCS_BATCH(batch, HCP_SURFACE_STATE | (3 - 2));
    OUT_BCS_BATCH(batch,
                  (0 << 28) |                   /* surface id */
                  (obj_surface->width - 1));    /* pitch - 1 */
    OUT_BCS_BATCH(batch,
                  (SURFACE_FORMAT_PLANAR_420_8 << 28) |
                  y_cb_offset);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen9_hcpd_pipe_buf_addr_state(VADriverContextP ctx,
                              struct decode_state *decode_state,
                              struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct intel_batchbuffer *batch = gen9_hcpd_context->base.batch;
    struct object_surface *obj_surface;
    GenHevcSurface *gen9_hevc_surface;
    int i;

    BEGIN_BCS_BATCH(batch, 95);

    OUT_BCS_BATCH(batch, HCP_PIPE_BUF_ADDR_STATE | (95 - 2));

    obj_surface = decode_state->render_object;
    assert(obj_surface && obj_surface->bo);
    gen9_hevc_surface = obj_surface->private_data;
    assert(gen9_hevc_surface && gen9_hevc_surface->motion_vector_temporal_bo);

    OUT_BUFFER_MA_TARGET(obj_surface->bo); /* DW 1..3 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->deblocking_filter_line_buffer.bo);/* DW 4..6 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->deblocking_filter_tile_line_buffer.bo); /* DW 7..9 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->deblocking_filter_tile_column_buffer.bo); /* DW 10..12 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->metadata_line_buffer.bo);         /* DW 13..15 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->metadata_tile_line_buffer.bo);    /* DW 16..18 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->metadata_tile_column_buffer.bo);  /* DW 19..21 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->sao_line_buffer.bo);              /* DW 22..24 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->sao_tile_line_buffer.bo);         /* DW 25..27 */
    OUT_BUFFER_MA_TARGET(gen9_hcpd_context->sao_tile_column_buffer.bo);       /* DW 28..30 */
    OUT_BUFFER_MA_TARGET(gen9_hevc_surface->motion_vector_temporal_bo); /* DW 31..33 */
    OUT_BUFFER_MA_TARGET(NULL); /* DW 34..36, reserved */

    for (i = 0; i < ARRAY_ELEMS(gen9_hcpd_context->reference_surfaces); i++) {
        obj_surface = gen9_hcpd_context->reference_surfaces[i].obj_surface;

        if (obj_surface)
            OUT_BUFFER_NMA_REFERENCE(obj_surface->bo);
        else
            OUT_BUFFER_NMA_REFERENCE(NULL);
    }
    OUT_BCS_BATCH(batch, 0);    /* DW 53, memory address attributes */

    OUT_BUFFER_MA_REFERENCE(NULL); /* DW 54..56, ignore for decoding mode */
    OUT_BUFFER_MA_TARGET(NULL);
    OUT_BUFFER_MA_TARGET(NULL);
    OUT_BUFFER_MA_TARGET(NULL);

    for (i = 0; i < ARRAY_ELEMS(gen9_hcpd_context->reference_surfaces); i++) {
        obj_surface = gen9_hcpd_context->reference_surfaces[i].obj_surface;
        gen9_hevc_surface = NULL;

        if (obj_surface && obj_surface->private_data)
            gen9_hevc_surface = obj_surface->private_data;

        if (gen9_hevc_surface)
            OUT_BUFFER_NMA_REFERENCE(gen9_hevc_surface->motion_vector_temporal_bo);
        else
            OUT_BUFFER_NMA_REFERENCE(NULL);
    }
    OUT_BCS_BATCH(batch, 0);    /* DW 82, memory address attributes */

    OUT_BUFFER_MA_TARGET(NULL);    /* DW 83..85, ignore for HEVC */
    OUT_BUFFER_MA_TARGET(NULL);    /* DW 86..88, ignore for HEVC */
    OUT_BUFFER_MA_TARGET(NULL);    /* DW 89..91, ignore for HEVC */
    OUT_BUFFER_MA_TARGET(NULL);    /* DW 92..94, ignore for HEVC */

    ADVANCE_BCS_BATCH(batch);
}

static void
gen9_hcpd_ind_obj_base_addr_state(VADriverContextP ctx,
                                  dri_bo *slice_data_bo,
                                  struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct intel_batchbuffer *batch = gen9_hcpd_context->base.batch;

    BEGIN_BCS_BATCH(batch, 14);

    OUT_BCS_BATCH(batch, HCP_IND_OBJ_BASE_ADDR_STATE | (14 - 2));
    OUT_BUFFER_MA_REFERENCE(slice_data_bo);        /* DW 1..3 */
    OUT_BUFFER_NMA_REFERENCE(NULL);                /* DW 4..5, Upper Bound */
    OUT_BUFFER_MA_REFERENCE(NULL);                 /* DW 6..8, CU, ignored */
    OUT_BUFFER_MA_TARGET(NULL);                    /* DW 9..11, PAK-BSE, ignored */
    OUT_BUFFER_NMA_TARGET(NULL);                   /* DW 12..13, Upper Bound  */

    ADVANCE_BCS_BATCH(batch);
}

static void
gen9_hcpd_qm_state(VADriverContextP ctx,
                   int size_id,
                   int color_component,
                   int pred_type,
                   int dc,
                   unsigned char *qm,
                   int qm_length,
                   struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct intel_batchbuffer *batch = gen9_hcpd_context->base.batch;
    unsigned char qm_buffer[64];

    assert(qm_length <= 64);
    memset(qm_buffer, 0, sizeof(qm_buffer));
    memcpy(qm_buffer, qm, qm_length);

    BEGIN_BCS_BATCH(batch, 18);

    OUT_BCS_BATCH(batch, HCP_QM_STATE | (18 - 2));
    OUT_BCS_BATCH(batch,
                  dc << 5 |
                  color_component << 3 |
                  size_id << 1 |
                  pred_type);
    intel_batchbuffer_data(batch, qm_buffer, 64);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen9_hcpd_hevc_qm_state(VADriverContextP ctx,
                        struct decode_state *decode_state,
                        struct gen9_hcpd_context *gen9_hcpd_context)
{
    VAIQMatrixBufferHEVC *iq_matrix;
    VAPictureParameterBufferHEVC *pic_param;
    int i;

    if (decode_state->iq_matrix && decode_state->iq_matrix->buffer)
        iq_matrix = (VAIQMatrixBufferHEVC *)decode_state->iq_matrix->buffer;
    else
        iq_matrix = &gen9_hcpd_context->iq_matrix_hevc;

    assert(decode_state->pic_param && decode_state->pic_param->buffer);
    pic_param = (VAPictureParameterBufferHEVC *)decode_state->pic_param->buffer;

    if (!pic_param->pic_fields.bits.scaling_list_enabled_flag)
        iq_matrix = &gen9_hcpd_context->iq_matrix_hevc;

    for (i = 0; i < 6; i++) {
        gen9_hcpd_qm_state(ctx,
                           0, i % 3, i / 3, 0,
                           iq_matrix->ScalingList4x4[i], 16,
                           gen9_hcpd_context);
    }

    for (i = 0; i < 6; i++) {
        gen9_hcpd_qm_state(ctx,
                           1, i % 3, i / 3, 0,
                           iq_matrix->ScalingList8x8[i], 64,
                           gen9_hcpd_context);
    }

    for (i = 0; i < 6; i++) {
        gen9_hcpd_qm_state(ctx,
                           2, i % 3, i / 3, iq_matrix->ScalingListDC16x16[i],
                           iq_matrix->ScalingList16x16[i], 64,
                           gen9_hcpd_context);
    }

    for (i = 0; i < 2; i++) {
        gen9_hcpd_qm_state(ctx,
                           3, 0, i % 2, iq_matrix->ScalingListDC32x32[i],
                           iq_matrix->ScalingList32x32[i], 64,
                           gen9_hcpd_context);
    }
}

static void
gen9_hcpd_pic_state(VADriverContextP ctx,
                    struct decode_state *decode_state,
                    struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct intel_batchbuffer *batch = gen9_hcpd_context->base.batch;
    VAPictureParameterBufferHEVC *pic_param;
    int max_pcm_size_minus3 = 0, min_pcm_size_minus3 = 0;
    int pcm_sample_bit_depth_luma_minus1 = 7, pcm_sample_bit_depth_chroma_minus1 = 7;
    /*
     * 7.4.3.1
     *
     * When not present, the value of loop_filter_across_tiles_enabled_flag
     * is inferred to be equal to 1.
     */
    int loop_filter_across_tiles_enabled_flag = 1;

    assert(decode_state->pic_param && decode_state->pic_param->buffer);
    pic_param = (VAPictureParameterBufferHEVC *)decode_state->pic_param->buffer;

    if (pic_param->pic_fields.bits.pcm_enabled_flag) {
        max_pcm_size_minus3 = pic_param->log2_min_pcm_luma_coding_block_size_minus3 +
            pic_param->log2_diff_max_min_pcm_luma_coding_block_size;
        min_pcm_size_minus3 = pic_param->log2_min_pcm_luma_coding_block_size_minus3;
        pcm_sample_bit_depth_luma_minus1 = (pic_param->pcm_sample_bit_depth_luma_minus1 & 0x0f);
        pcm_sample_bit_depth_chroma_minus1 = (pic_param->pcm_sample_bit_depth_chroma_minus1 & 0x0f);
    } else {
        max_pcm_size_minus3 = MIN(pic_param->log2_min_luma_coding_block_size_minus3 + pic_param->log2_diff_max_min_luma_coding_block_size, 2);
    }

    if (pic_param->pic_fields.bits.tiles_enabled_flag)
        loop_filter_across_tiles_enabled_flag = pic_param->pic_fields.bits.loop_filter_across_tiles_enabled_flag;

    BEGIN_BCS_BATCH(batch, 19);

    OUT_BCS_BATCH(batch, HCP_PIC_STATE | (19 - 2));

    OUT_BCS_BATCH(batch,
                  gen9_hcpd_context->picture_height_in_min_cb_minus1 << 16 |
                  gen9_hcpd_context->picture_width_in_min_cb_minus1);
    OUT_BCS_BATCH(batch,
                  max_pcm_size_minus3 << 10 |
                  min_pcm_size_minus3 << 8 |
                  (pic_param->log2_min_transform_block_size_minus2 +
                   pic_param->log2_diff_max_min_transform_block_size) << 6 |
                  pic_param->log2_min_transform_block_size_minus2 << 4 |
                  (pic_param->log2_min_luma_coding_block_size_minus3 +
                   pic_param->log2_diff_max_min_luma_coding_block_size) << 2 |
                  pic_param->log2_min_luma_coding_block_size_minus3);
    OUT_BCS_BATCH(batch, 0); /* DW 3, ignored */
    OUT_BCS_BATCH(batch,
                  0 << 27 |
                  pic_param->pic_fields.bits.strong_intra_smoothing_enabled_flag << 26 |
                  pic_param->pic_fields.bits.transquant_bypass_enabled_flag << 25 |
                  pic_param->pic_fields.bits.amp_enabled_flag << 23 |
                  pic_param->pic_fields.bits.transform_skip_enabled_flag << 22 |
                  !(pic_param->CurrPic.flags & VA_PICTURE_HEVC_BOTTOM_FIELD) << 21 |
                  !!(pic_param->CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC) << 20 |
                  pic_param->pic_fields.bits.weighted_pred_flag << 19 |
                  pic_param->pic_fields.bits.weighted_bipred_flag << 18 |
                  pic_param->pic_fields.bits.tiles_enabled_flag << 17 |
                  pic_param->pic_fields.bits.entropy_coding_sync_enabled_flag << 16 |
                  loop_filter_across_tiles_enabled_flag << 15 |
                  pic_param->pic_fields.bits.sign_data_hiding_enabled_flag << 13 |
                  pic_param->log2_parallel_merge_level_minus2 << 10 |
                  pic_param->pic_fields.bits.constrained_intra_pred_flag << 9 |
                  pic_param->pic_fields.bits.pcm_loop_filter_disabled_flag << 8 |
                  (pic_param->diff_cu_qp_delta_depth & 0x03) << 6 |
                  pic_param->pic_fields.bits.cu_qp_delta_enabled_flag << 5 |
                  pic_param->pic_fields.bits.pcm_enabled_flag << 4 |
                  pic_param->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag << 3 |
                  0);
    OUT_BCS_BATCH(batch,
                  pcm_sample_bit_depth_luma_minus1 << 20 |
                  pcm_sample_bit_depth_chroma_minus1 << 16 |
                  pic_param->max_transform_hierarchy_depth_inter << 13 |
                  pic_param->max_transform_hierarchy_depth_intra << 10 |
                  (pic_param->pps_cr_qp_offset & 0x1f) << 5 |
                  (pic_param->pps_cb_qp_offset & 0x1f));
    OUT_BCS_BATCH(batch,
                  0 << 29 |
                  0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0); /* DW 10 */
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0); /* DW 15 */
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);
    OUT_BCS_BATCH(batch, 0);

    ADVANCE_BCS_BATCH(batch);
}

static void
gen9_hcpd_tile_state(VADriverContextP ctx,
                     struct decode_state *decode_state,
                     struct gen9_hcpd_context *gen9_hcpd_context)
{
    struct intel_batchbuffer *batch = gen9_hcpd_context->base.batch;
    VAPictureParameterBufferHEVC *pic_param;
    uint8_t pos_col[20], pos_row[24];
    int i;

    assert(decode_state->pic_param && decode_state->pic_param->buffer);
    pic_param = (VAPictureParameterBufferHEVC *)decode_state->pic_param->buffer;

    memset(pos_col, 0, sizeof(pos_col));
    memset(pos_row, 0, sizeof(pos_row));

    for (i = 0; i <= MIN(pic_param->num_tile_columns_minus1, 18); i++)
        pos_col[i + 1] = pos_col[i] + pic_param->column_width_minus1[i] + 1;

    for (i = 0; i <= MIN(pic_param->num_tile_rows_minus1, 20); i++)
        pos_row[i + 1] = pos_row[i] + pic_param->row_height_minus1[i] + 1;

    BEGIN_BCS_BATCH(batch, 13);

    OUT_BCS_BATCH(batch, HCP_TILE_STATE | (13 - 2));

    OUT_BCS_BATCH(batch,
                  pic_param->num_tile_columns_minus1 << 5 |
                  pic_param->num_tile_rows_minus1);
    intel_batchbuffer_data(batch, pos_col, 20);
    intel_batchbuffer_data(batch, pos_row, 24);

    ADVANCE_BCS_BATCH(batch);
}

static VAStatus
gen9_hcpd_hevc_decode_picture(VADriverContextP ctx,
                              struct decode_state *decode_state,
                              struct gen9_hcpd_context *gen9_hcpd_context)
{
    VAStatus vaStatus;
    struct intel_batchbuffer *batch = gen9_hcpd_context->base.batch;
    dri_bo *slice_data_bo;
    int j;

    vaStatus = gen9_hcpd_hevc_decode_init(ctx, decode_state, gen9_hcpd_context);

    if (vaStatus != VA_STATUS_SUCCESS)
        goto out;

    intel_batchbuffer_start_atomic_bcs(batch, 0x1000);
    intel_batchbuffer_emit_mi_flush(batch);

    gen9_hcpd_pipe_mode_select(ctx, decode_state, HCP_CODEC_HEVC, gen9_hcpd_context);
    gen9_hcpd_surface_state(ctx, decode_state, gen9_hcpd_context);
    gen9_hcpd_pipe_buf_addr_state(ctx, decode_state, gen9_hcpd_context);
    gen9_hcpd_hevc_qm_state(ctx, decode_state, gen9_hcpd_context);
    gen9_hcpd_pic_state(ctx, decode_state, gen9_hcpd_context);
    gen9_hcpd_tile_state(ctx, decode_state, gen9_hcpd_context);

    /* Need to double it works or not if the two slice groups have differenct slice data buffers */
    for (j = 0; j < decode_state->num_slice_params; j++) {
        slice_data_bo = decode_state->slice_datas[j]->bo;

        gen9_hcpd_ind_obj_base_addr_state(ctx, slice_data_bo, gen9_hcpd_context);
    }

    intel_batchbuffer_end_atomic(batch);
    intel_batchbuffer_flush(batch);

out:
    return vaStatus;
}

static VAStatus
gen9_hcpd_decode_picture(VADriverContextP ctx,
                         VAProfile profile,
                         union codec_state *codec_state,
                         struct hw_context *hw_context)
{
    struct gen9_hcpd_context *gen9_hcpd_context = (struct gen9_hcpd_context *)hw_context;
    struct decode_state *decode_state = &codec_state->decode;
    VAStatus vaStatus;

    assert(gen9_hcpd_context);

    vaStatus = intel_decoder_sanity_check_input(ctx, profile, decode_state);

    if (vaStatus != VA_STATUS_SUCCESS)
        goto out;

    switch (profile) {
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:
        vaStatus = gen9_hcpd_hevc_decode_picture(ctx, decode_state, gen9_hcpd_context);
        break;

    default:
        /* should never get here 1!! */
        assert(0);
        break;
    }

out:
    return vaStatus;
}

static void
gen9_hcpd_context_destroy(void *hw_context)
{
    struct gen9_hcpd_context *gen9_hcpd_context = (struct gen9_hcpd_context *)hw_context;

    FREE_GEN_BUFFER((&gen9_hcpd_context->deblocking_filter_line_buffer));
    FREE_GEN_BUFFER((&gen9_hcpd_context->deblocking_filter_tile_line_buffer));
    FREE_GEN_BUFFER((&gen9_hcpd_context->deblocking_filter_tile_column_buffer));
    FREE_GEN_BUFFER((&gen9_hcpd_context->metadata_line_buffer));
    FREE_GEN_BUFFER((&gen9_hcpd_context->metadata_tile_line_buffer));
    FREE_GEN_BUFFER((&gen9_hcpd_context->metadata_tile_column_buffer));
    FREE_GEN_BUFFER((&gen9_hcpd_context->sao_line_buffer));
    FREE_GEN_BUFFER((&gen9_hcpd_context->sao_tile_line_buffer));
    FREE_GEN_BUFFER((&gen9_hcpd_context->sao_tile_column_buffer));

    intel_batchbuffer_free(gen9_hcpd_context->base.batch);
    free(gen9_hcpd_context);
}

static void
gen9_hcpd_hevc_context_init(VADriverContextP ctx,
                            struct gen9_hcpd_context *gen9_hcpd_context)
{
    hevc_gen_default_iq_matrix(&gen9_hcpd_context->iq_matrix_hevc);
}

static struct hw_context *
gen9_hcpd_context_init(VADriverContextP ctx, struct object_config *object_config)
{
    struct intel_driver_data *intel = intel_driver_data(ctx);
    struct gen9_hcpd_context *gen9_hcpd_context = calloc(1, sizeof(struct gen9_hcpd_context));
    int i;

    if (!gen9_hcpd_context)
        return NULL;

    gen9_hcpd_context->base.destroy = gen9_hcpd_context_destroy;
    gen9_hcpd_context->base.run = gen9_hcpd_decode_picture;
    gen9_hcpd_context->base.batch = intel_batchbuffer_new(intel, I915_EXEC_VEBOX, 0);

    for (i = 0; i < ARRAY_ELEMS(gen9_hcpd_context->reference_surfaces); i++) {
        gen9_hcpd_context->reference_surfaces[i].surface_id = VA_INVALID_ID;
        gen9_hcpd_context->reference_surfaces[i].frame_store_id = -1;
        gen9_hcpd_context->reference_surfaces[i].obj_surface = NULL;
    }

    switch (object_config->profile) {
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:
        gen9_hcpd_hevc_context_init(ctx, gen9_hcpd_context);
        break;

    default:
        break;
    }

    return (struct hw_context *)gen9_hcpd_context;
}

struct hw_context *
gen9_dec_hw_context_init(VADriverContextP ctx, struct object_config *obj_config)
{
    if (obj_config->profile == VAProfileHEVCMain ||
        obj_config->profile == VAProfileHEVCMain10) {
        return gen9_hcpd_context_init(ctx, obj_config);
    } else {
        return gen8_dec_hw_context_init(ctx, obj_config);
    }
}
