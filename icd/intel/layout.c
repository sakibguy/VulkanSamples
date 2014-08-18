/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2014 LunarG, Inc.
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "dev.h"
#include "format.h"
#include "gpu.h"
#include "layout.h"

enum {
   LAYOUT_TILING_NONE = 1 << INTEL_TILING_NONE,
   LAYOUT_TILING_X = 1 << INTEL_TILING_X,
   LAYOUT_TILING_Y = 1 << INTEL_TILING_Y,
   LAYOUT_TILING_W = 1 << (INTEL_TILING_Y + 1),

   LAYOUT_TILING_ALL = (LAYOUT_TILING_NONE |
                        LAYOUT_TILING_X |
                        LAYOUT_TILING_Y |
                        LAYOUT_TILING_W)
};

struct intel_layout_params {
   const struct intel_gpu *gpu;
   const XGL_IMAGE_CREATE_INFO *info;

   bool compressed;

   unsigned h0, h1;
   unsigned max_x, max_y;
};

static void
layout_get_slice_size(const struct intel_layout *layout,
                      const struct intel_layout_params *params,
                      unsigned level, unsigned *width, unsigned *height)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   unsigned w, h;

   w = u_minify(layout->width0, level);
   h = u_minify(layout->height0, level);

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 114:
    *
    *     "The dimensions of the mip maps are first determined by applying the
    *      sizing algorithm presented in Non-Power-of-Two Mipmaps above. Then,
    *      if necessary, they are padded out to compression block boundaries."
    */
   w = u_align(w, layout->block_width);
   h = u_align(h, layout->block_height);

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 111:
    *
    *     "If the surface is multisampled (4x), these values must be adjusted
    *      as follows before proceeding:
    *
    *        W_L = ceiling(W_L / 2) * 4
    *        H_L = ceiling(H_L / 2) * 4"
    *
    * From the Ivy Bridge PRM, volume 1 part 1, page 108:
    *
    *     "If the surface is multisampled and it is a depth or stencil surface
    *      or Multisampled Surface StorageFormat in SURFACE_STATE is
    *      MSFMT_DEPTH_STENCIL, W_L and H_L must be adjusted as follows before
    *      proceeding:
    *
    *        #samples  W_L =                    H_L =
    *        2         ceiling(W_L / 2) * 4     HL [no adjustment]
    *        4         ceiling(W_L / 2) * 4     ceiling(H_L / 2) * 4
    *        8         ceiling(W_L / 2) * 8     ceiling(H_L / 2) * 4
    *        16        ceiling(W_L / 2) * 8     ceiling(H_L / 2) * 8"
    *
    * For interleaved samples (4x), where pixels
    *
    *   (x, y  ) (x+1, y  )
    *   (x, y+1) (x+1, y+1)
    *
    * would be is occupied by
    *
    *   (x, y  , si0) (x+1, y  , si0) (x, y  , si1) (x+1, y  , si1)
    *   (x, y+1, si0) (x+1, y+1, si0) (x, y+1, si1) (x+1, y+1, si1)
    *   (x, y  , si2) (x+1, y  , si2) (x, y  , si3) (x+1, y  , si3)
    *   (x, y+1, si2) (x+1, y+1, si2) (x, y+1, si3) (x+1, y+1, si3)
    *
    * Thus the need to
    *
    *   w = align(w, 2) * 2;
    *   y = align(y, 2) * 2;
    */
   if (layout->interleaved_samples) {
      switch (info->samples) {
      case 0:
      case 1:
         break;
      case 2:
         w = u_align(w, 2) * 2;
         break;
      case 4:
         w = u_align(w, 2) * 2;
         h = u_align(h, 2) * 2;
         break;
      case 8:
         w = u_align(w, 2) * 4;
         h = u_align(h, 2) * 2;
         break;
      case 16:
         w = u_align(w, 2) * 4;
         h = u_align(h, 2) * 4;
         break;
      default:
         assert(!"unsupported sample count");
         break;
      }
   }

   /*
    * From the Ivy Bridge PRM, volume 1 part 1, page 108:
    *
    *     "For separate stencil buffer, the width must be mutiplied by 2 and
    *      height divided by 2..."
    *
    * To make things easier (for transfer), we will just double the stencil
    * stride in 3DSTATE_STENCIL_BUFFER.
    */
   w = u_align(w, layout->align_i);
   h = u_align(h, layout->align_j);

   *width = w;
   *height = h;
}

static unsigned
layout_get_num_layers(const struct intel_layout *layout,
                      const struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   unsigned num_layers = info->arraySize;

   /* samples of the same index are stored in a layer */
   if (info->samples > 1 && !layout->interleaved_samples)
      num_layers *= info->samples;

   return num_layers;
}

static void
layout_init_layer_height(struct intel_layout *layout,
                         struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   unsigned num_layers;

   if (layout->walk != INTEL_LAYOUT_WALK_LAYER)
      return;

   num_layers = layout_get_num_layers(layout, params);
   if (num_layers <= 1)
      return;

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 115:
    *
    *     "The following equation is used for surface formats other than
    *      compressed textures:
    *
    *        QPitch = (h0 + h1 + 11j)"
    *
    *     "The equation for compressed textures (BC* and FXT1 surface formats)
    *      follows:
    *
    *        QPitch = (h0 + h1 + 11j) / 4"
    *
    *     "[DevSNB] Errata: Sampler MSAA Qpitch will be 4 greater than the
    *      value calculated in the equation above, for every other odd Surface
    *      Height starting from 1 i.e. 1,5,9,13"
    *
    * From the Ivy Bridge PRM, volume 1 part 1, page 111-112:
    *
    *     "If Surface Array Spacing is set to ARYSPC_FULL (note that the depth
    *      buffer and stencil buffer have an implied value of ARYSPC_FULL):
    *
    *        QPitch = (h0 + h1 + 12j)
    *        QPitch = (h0 + h1 + 12j) / 4 (compressed)
    *
    *      (There are many typos or missing words here...)"
    *
    * To access the N-th slice, an offset of (Stride * QPitch * N) is added to
    * the base address.  The PRM divides QPitch by 4 for compressed formats
    * because the block height for those formats are 4, and it wants QPitch to
    * mean the number of memory rows, as opposed to texel rows, between
    * slices.  Since we use texel rows everywhere, we do not need to divide
    * QPitch by 4.
    */
   layout->layer_height = params->h0 + params->h1 +
      ((intel_gpu_gen(params->gpu) >= INTEL_GEN(7)) ? 12 : 11) * layout->align_j;

   if (intel_gpu_gen(params->gpu) == INTEL_GEN(6) && info->samples > 1 &&
       layout->height0 % 4 == 1)
      layout->layer_height += 4;

   params->max_y += layout->layer_height * (num_layers - 1);
}

static void
layout_init_lods(struct intel_layout *layout,
                 struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   unsigned cur_x, cur_y;
   unsigned lv;

   cur_x = 0;
   cur_y = 0;
   for (lv = 0; lv < info->mipLevels; lv++) {
      unsigned lod_w, lod_h;

      layout_get_slice_size(layout, params, lv, &lod_w, &lod_h);

      layout->lods[lv].x = cur_x;
      layout->lods[lv].y = cur_y;
      layout->lods[lv].slice_width = lod_w;
      layout->lods[lv].slice_height = lod_h;

      switch (layout->walk) {
      case INTEL_LAYOUT_WALK_LOD:
         lod_h *= layout_get_num_layers(layout, params);
         if (lv == 1)
            cur_x += lod_w;
         else
            cur_y += lod_h;

         /* every LOD begins at tile boundaries */
         if (info->mipLevels > 1) {
            intel_format_is_stencil(params->gpu, layout->format);
            cur_x = u_align(cur_x, 64);
            cur_y = u_align(cur_y, 64);
         }
         break;
      case INTEL_LAYOUT_WALK_LAYER:
         /* MIPLAYOUT_BELOW */
         if (lv == 1)
            cur_x += lod_w;
         else
            cur_y += lod_h;
         break;
      case INTEL_LAYOUT_WALK_3D:
         {
            const unsigned num_slices = u_minify(info->extent.depth, lv);
            const unsigned num_slices_per_row = 1 << lv;
            const unsigned num_rows =
               (num_slices + num_slices_per_row - 1) / num_slices_per_row;

            lod_w *= num_slices_per_row;
            lod_h *= num_rows;

            cur_y += lod_h;
         }
         break;
      }

      if (params->max_x < layout->lods[lv].x + lod_w)
         params->max_x = layout->lods[lv].x + lod_w;
      if (params->max_y < layout->lods[lv].y + lod_h)
         params->max_y = layout->lods[lv].y + lod_h;
   }

   if (layout->walk == INTEL_LAYOUT_WALK_LAYER) {
      params->h0 = layout->lods[0].slice_height;

      if (info->mipLevels > 1)
         params->h1 = layout->lods[1].slice_height;
      else
         layout_get_slice_size(layout, params, 1, &cur_x, &params->h1);
   }
}

static void
layout_init_alignments(struct intel_layout *layout,
                       struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 113:
    *
    *     "surface format           align_i     align_j
    *      YUV 4:2:2 formats        4           *see below
    *      BC1-5                    4           4
    *      FXT1                     8           4
    *      all other formats        4           *see below"
    *
    *     "- align_j = 4 for any depth buffer
    *      - align_j = 2 for separate stencil buffer
    *      - align_j = 4 for any render target surface is multisampled (4x)
    *      - align_j = 4 for any render target surface with Surface Vertical
    *        Alignment = VALIGN_4
    *      - align_j = 2 for any render target surface with Surface Vertical
    *        Alignment = VALIGN_2
    *      - align_j = 2 for all other render target surface
    *      - align_j = 2 for any sampling engine surface with Surface Vertical
    *        Alignment = VALIGN_2
    *      - align_j = 4 for any sampling engine surface with Surface Vertical
    *        Alignment = VALIGN_4"
    *
    * From the Sandy Bridge PRM, volume 4 part 1, page 86:
    *
    *     "This field (Surface Vertical Alignment) must be set to VALIGN_2 if
    *      the Surface Format is 96 bits per element (BPE)."
    *
    * They can be rephrased as
    *
    *                                  align_i        align_j
    *   compressed formats             block width    block height
    *   PIPE_FORMAT_S8_UINT            4              2
    *   other depth/stencil formats    4              4
    *   4x multisampled                4              4
    *   bpp 96                         4              2
    *   others                         4              2 or 4
    */

   /*
    * From the Ivy Bridge PRM, volume 1 part 1, page 110:
    *
    *     "surface defined by      surface format     align_i     align_j
    *      3DSTATE_DEPTH_BUFFER    D16_UNORM          8           4
    *                              not D16_UNORM      4           4
    *      3DSTATE_STENCIL_BUFFER  N/A                8           8
    *      SURFACE_STATE           BC*, ETC*, EAC*    4           4
    *                              FXT1               8           4
    *                              all others         (set by SURFACE_STATE)"
    *
    * From the Ivy Bridge PRM, volume 4 part 1, page 63:
    *
    *     "- This field (Surface Vertical Aligment) is intended to be set to
    *        VALIGN_4 if the surface was rendered as a depth buffer, for a
    *        multisampled (4x) render target, or for a multisampled (8x)
    *        render target, since these surfaces support only alignment of 4.
    *      - Use of VALIGN_4 for other surfaces is supported, but uses more
    *        memory.
    *      - This field must be set to VALIGN_4 for all tiled Y Render Target
    *        surfaces.
    *      - Value of 1 is not supported for format YCRCB_NORMAL (0x182),
    *        YCRCB_SWAPUVY (0x183), YCRCB_SWAPUV (0x18f), YCRCB_SWAPY (0x190)
    *      - If Number of Multisamples is not MULTISAMPLECOUNT_1, this field
    *        must be set to VALIGN_4."
    *      - VALIGN_4 is not supported for surface format R32G32B32_FLOAT."
    *
    *     "- This field (Surface Horizontal Aligment) is intended to be set to
    *        HALIGN_8 only if the surface was rendered as a depth buffer with
    *        Z16 format or a stencil buffer, since these surfaces support only
    *        alignment of 8.
    *      - Use of HALIGN_8 for other surfaces is supported, but uses more
    *        memory.
    *      - This field must be set to HALIGN_4 if the Surface Format is BC*.
    *      - This field must be set to HALIGN_8 if the Surface Format is
    *        FXT1."
    *
    * They can be rephrased as
    *
    *                                  align_i        align_j
    *  compressed formats              block width    block height
    *  PIPE_FORMAT_Z16_UNORM           8              4
    *  PIPE_FORMAT_S8_UINT             8              8
    *  other depth/stencil formats     4              4
    *  2x or 4x multisampled           4 or 8         4
    *  tiled Y                         4 or 8         4 (if rt)
    *  PIPE_FORMAT_R32G32B32_FLOAT     4 or 8         2
    *  others                          4 or 8         2 or 4
    */

   if (params->compressed) {
      /* this happens to be the case */
      layout->align_i = layout->block_width;
      layout->align_j = layout->block_height;
   } else if (info->usage & XGL_IMAGE_USAGE_DEPTH_STENCIL_BIT) {
      if (intel_gpu_gen(params->gpu) >= INTEL_GEN(7)) {
         switch (layout->format.channelFormat) {
         case XGL_CH_FMT_R16:
            layout->align_i = 8;
            layout->align_j = 4;
            break;
         case XGL_CH_FMT_R8:
            layout->align_i = 8;
            layout->align_j = 8;
            break;
         default:
            layout->align_i = 4;
            layout->align_j = 4;
            break;
         }
      } else {
         switch (layout->format.channelFormat) {
         case XGL_CH_FMT_R8:
            layout->align_i = 4;
            layout->align_j = 2;
            break;
         default:
            layout->align_i = 4;
            layout->align_j = 4;
            break;
         }
      }
   } else {
      const bool valign_4 = (info->samples > 1) ||
         (intel_gpu_gen(params->gpu) >= INTEL_GEN(7) &&
          layout->tiling == INTEL_TILING_Y &&
          (info->usage & XGL_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));

      if (valign_4)
         assert(layout->block_size != 12);

      layout->align_i = 4;
      layout->align_j = (valign_4) ? 4 : 2;
   }

   /*
    * the fact that align i and j are multiples of block width and height
    * respectively is what makes the size of the bo a multiple of the block
    * size, slices start at block boundaries, and many of the computations
    * work.
    */
   assert(layout->align_i % layout->block_width == 0);
   assert(layout->align_j % layout->block_height == 0);

   /* make sure u_align() works */
   assert(u_is_pow2(layout->align_i) &&
          u_is_pow2(layout->align_j));
   assert(u_is_pow2(layout->block_width) &&
          u_is_pow2(layout->block_height));
}

static unsigned
layout_get_valid_tilings(const struct intel_layout *layout,
                         const struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   const XGL_FORMAT format = layout->format;
   unsigned valid_tilings = LAYOUT_TILING_ALL;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 318:
    *
    *     "[DevSNB+]: This field (Tiled Surface) must be set to TRUE. Linear
    *      Depth Buffer is not supported."
    *
    *     "The Depth Buffer, if tiled, must use Y-Major tiling."
    *
    * From the Sandy Bridge PRM, volume 1 part 2, page 22:
    *
    *     "W-Major Tile Format is used for separate stencil."
    */
   if (info->usage & XGL_IMAGE_USAGE_DEPTH_STENCIL_BIT) {
      switch (format.channelFormat) {
      case XGL_CH_FMT_R8:
         valid_tilings &= LAYOUT_TILING_W;
         break;
      default:
         valid_tilings &= LAYOUT_TILING_Y;
         break;
      }
   }

   if (info->usage & XGL_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      /*
       * From the Sandy Bridge PRM, volume 1 part 2, page 32:
       *
       *     "NOTE: 128BPE Format Color buffer ( render target ) MUST be
       *      either TileX or Linear."
       */
      if (layout->block_size == 16)
         valid_tilings &= ~LAYOUT_TILING_Y;

      /*
       * From the Ivy Bridge PRM, volume 4 part 1, page 63:
       *
       *     "This field (Surface Vertical Aligment) must be set to VALIGN_4
       *      for all tiled Y Render Target surfaces."
       *
       *     "VALIGN_4 is not supported for surface format R32G32B32_FLOAT."
       */
      if (intel_gpu_gen(params->gpu) >= INTEL_GEN(7) && layout->block_size == 12)
         valid_tilings &= ~LAYOUT_TILING_Y;
   }

   /* no conflicting binding flags */
   assert(valid_tilings);

   return valid_tilings;
}

static void
layout_init_tiling(struct intel_layout *layout,
                   struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   unsigned valid_tilings = layout_get_valid_tilings(layout, params);

   /* no hardware support for W-tile */
   if (valid_tilings & LAYOUT_TILING_W)
      valid_tilings = (valid_tilings & ~LAYOUT_TILING_W) | LAYOUT_TILING_NONE;

   layout->valid_tilings = valid_tilings;

   if (info->usage & (XGL_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      XGL_IMAGE_USAGE_SHADER_ACCESS_READ_BIT)) {
      /*
       * heuristically set a minimum width/height for enabling tiling
       */
      if (layout->width0 < 64 && (valid_tilings & ~LAYOUT_TILING_X))
         valid_tilings &= ~LAYOUT_TILING_X;

      if ((layout->width0 < 32 || layout->height0 < 16) &&
          (layout->width0 < 16 || layout->height0 < 32) &&
          (valid_tilings & ~LAYOUT_TILING_Y))
         valid_tilings &= ~LAYOUT_TILING_Y;
   } else {
      /* force linear if we are not sure where the texture is bound to */
      if (valid_tilings & LAYOUT_TILING_NONE)
         valid_tilings &= LAYOUT_TILING_NONE;
   }

   /* prefer tiled over linear */
   if (valid_tilings & LAYOUT_TILING_Y)
      layout->tiling = INTEL_TILING_Y;
   else if (valid_tilings & LAYOUT_TILING_X)
      layout->tiling = INTEL_TILING_X;
   else
      layout->tiling = INTEL_TILING_NONE;
}

static void
layout_init_walk_gen7(struct intel_layout *layout,
                              struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;

   /*
    * It is not explicitly states, but render targets are expected to be
    * UMS/CMS (samples non-interleaved) and depth/stencil buffers are expected
    * to be IMS (samples interleaved).
    *
    * See "Multisampled Surface Storage Format" field of SURFACE_STATE.
    */
   if (info->usage & XGL_IMAGE_USAGE_DEPTH_STENCIL_BIT) {
      /*
       * From the Ivy Bridge PRM, volume 1 part 1, page 111:
       *
       *     "note that the depth buffer and stencil buffer have an implied
       *      value of ARYSPC_FULL"
       */
      layout->walk = (info->imageType == XGL_IMAGE_3D) ?
         INTEL_LAYOUT_WALK_3D : INTEL_LAYOUT_WALK_LAYER;

      layout->interleaved_samples = true;
   } else {
      /*
       * From the Ivy Bridge PRM, volume 4 part 1, page 66:
       *
       *     "If Multisampled Surface Storage Format is MSFMT_MSS and Number
       *      of Multisamples is not MULTISAMPLECOUNT_1, this field (Surface
       *      Array Spacing) must be set to ARYSPC_LOD0."
       *
       * As multisampled resources are not mipmapped, we never use
       * ARYSPC_FULL for them.
       */
      if (info->samples > 1)
         assert(info->mipLevels == 1);

      layout->walk =
         (info->imageType == XGL_IMAGE_3D) ? INTEL_LAYOUT_WALK_3D :
         (info->mipLevels > 1) ? INTEL_LAYOUT_WALK_LAYER :
         INTEL_LAYOUT_WALK_LOD;

      layout->interleaved_samples = false;
   }
}

static void
layout_init_walk_gen6(struct intel_layout *layout,
                      struct intel_layout_params *params)
{
   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 115:
    *
    *     "The separate stencil buffer does not support mip mapping, thus the
    *      storage for LODs other than LOD 0 is not needed. The following
    *      QPitch equation applies only to the separate stencil buffer:
    *
    *        QPitch = h_0"
    *
    * GEN6 does not support compact spacing otherwise.
    */
   layout->walk =
      (params->info->imageType == XGL_IMAGE_3D) ? INTEL_LAYOUT_WALK_3D :
      intel_format_is_stencil(params->gpu, layout->format) ? INTEL_LAYOUT_WALK_LOD :
      INTEL_LAYOUT_WALK_LAYER;

   /* GEN6 supports only interleaved samples */
   layout->interleaved_samples = true;
}

static void
layout_init_walk(struct intel_layout *layout,
                 struct intel_layout_params *params)
{
   if (intel_gpu_gen(params->gpu) >= INTEL_GEN(7))
      layout_init_walk_gen7(layout, params);
   else
      layout_init_walk_gen6(layout, params);
}

static void
layout_init_size_and_format(struct intel_layout *layout,
                            struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   XGL_FORMAT format = info->format;
   bool require_separate_stencil;

   layout->width0 = info->extent.width;
   layout->height0 = info->extent.height;

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 317:
    *
    *     "This field (Separate Stencil Buffer Enable) must be set to the same
    *      value (enabled or disabled) as Hierarchical Depth Buffer Enable."
    *
    * GEN7+ requires separate stencil buffers.
    */
   if (info->usage & XGL_IMAGE_USAGE_DEPTH_STENCIL_BIT) {
      if (intel_gpu_gen(params->gpu) >= INTEL_GEN(7))
         require_separate_stencil = true;
      else
         require_separate_stencil = (layout->aux == INTEL_LAYOUT_AUX_HIZ);
   }

   if (format.numericFormat == XGL_NUM_FMT_DS) {
      switch (format.channelFormat) {
      case XGL_CH_FMT_R32G8:
         if (require_separate_stencil) {
            format.channelFormat = XGL_CH_FMT_R32;
            layout->separate_stencil = true;
         }
         break;
      default:
         break;
      }
   }

   layout->format = format;
   layout->block_width = icd_format_get_block_width(format);
   layout->block_height = layout->block_width;
   layout->block_size = icd_format_get_size(format);

   params->compressed = icd_format_is_compressed(format);
}

static bool
layout_want_mcs(struct intel_layout *layout,
                struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   bool want_mcs = false;

   /* MCS is for RT on GEN7+ */
   if (intel_gpu_gen(params->gpu) < INTEL_GEN(7))
      return false;

   if (info->imageType != XGL_IMAGE_2D ||
       !(info->usage & XGL_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      return false;

   /*
    * From the Ivy Bridge PRM, volume 4 part 1, page 77:
    *
    *     "For Render Target and Sampling Engine Surfaces:If the surface is
    *      multisampled (Number of Multisamples any value other than
    *      MULTISAMPLECOUNT_1), this field (MCS Enable) must be enabled."
    *
    *     "This field must be set to 0 for all SINT MSRTs when all RT channels
    *      are not written"
    */
   if (info->samples > 1 && !layout->interleaved_samples &&
       !icd_format_is_int(info->format)) {
      want_mcs = true;
   } else if (info->samples <= 1) {
      /*
       * From the Ivy Bridge PRM, volume 2 part 1, page 326:
       *
       *     "When MCS is buffer is used for color clear of non-multisampler
       *      render target, the following restrictions apply.
       *      - Support is limited to tiled render targets.
       *      - Support is for non-mip-mapped and non-array surface types
       *        only.
       *      - Clear is supported only on the full RT; i.e., no partial clear
       *        or overlapping clears.
       *      - MCS buffer for non-MSRT is supported only for RT formats
       *        32bpp, 64bpp and 128bpp.
       *      ..."
       */
      if (layout->tiling != INTEL_TILING_NONE &&
          info->mipLevels == 1 && info->arraySize == 1) {
         switch (layout->block_size) {
         case 4:
         case 8:
         case 16:
            want_mcs = true;
            break;
         default:
            break;
         }
      }
   }

   return want_mcs;
}

static bool
layout_want_hiz(const struct intel_layout *layout,
                const struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;

   if (!(info->usage & XGL_IMAGE_USAGE_DEPTH_STENCIL_BIT))
      return false;

   if (!intel_format_is_depth(params->gpu, info->format))
      return false;

   /*
    * As can be seen in layout_calculate_hiz_size(), HiZ may not be enabled
    * for every level.  This is generally fine except on GEN6, where HiZ and
    * separate stencil are enabled and disabled at the same time.  When the
    * format is PIPE_FORMAT_Z32_FLOAT_S8X24_UINT, enabling and disabling HiZ
    * can result in incompatible formats.
    */
   if (intel_gpu_gen(params->gpu) == INTEL_GEN(6) &&
       info->format.channelFormat == XGL_CH_FMT_R32G8 &&
       info->mipLevels > 1)
      return false;

   return true;
}

static void
layout_init_aux(struct intel_layout *layout,
                struct intel_layout_params *params)
{
   if (layout_want_hiz(layout, params))
      layout->aux = INTEL_LAYOUT_AUX_HIZ;
   else if (layout_want_mcs(layout, params))
      layout->aux = INTEL_LAYOUT_AUX_MCS;
}

static void
layout_align(struct intel_layout *layout, struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   int align_w = 1, align_h = 1, pad_h = 0;

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 118:
    *
    *     "To determine the necessary padding on the bottom and right side of
    *      the surface, refer to the table in Section 7.18.3.4 for the i and j
    *      parameters for the surface format in use. The surface must then be
    *      extended to the next multiple of the alignment unit size in each
    *      dimension, and all texels contained in this extended surface must
    *      have valid GTT entries."
    *
    *     "For cube surfaces, an additional two rows of padding are required
    *      at the bottom of the surface. This must be ensured regardless of
    *      whether the surface is stored tiled or linear.  This is due to the
    *      potential rotation of cache line orientation from memory to cache."
    *
    *     "For compressed textures (BC* and FXT1 surface formats), padding at
    *      the bottom of the surface is to an even compressed row, which is
    *      equal to a multiple of 8 uncompressed texel rows. Thus, for padding
    *      purposes, these surfaces behave as if j = 8 only for surface
    *      padding purposes. The value of 4 for j still applies for mip level
    *      alignment and QPitch calculation."
    */
   if (info->usage & XGL_IMAGE_USAGE_SHADER_ACCESS_READ_BIT) {
      if (align_w < layout->align_i)
          align_w = layout->align_i;
      if (align_h < layout->align_j)
          align_h = layout->align_j;

      /* in case it is used as a cube */
      if (info->imageType == XGL_IMAGE_2D)
         pad_h += 2;

      if (params->compressed && align_h < layout->align_j * 2)
         align_h = layout->align_j * 2;
   }

   /*
    * From the Sandy Bridge PRM, volume 1 part 1, page 118:
    *
    *     "If the surface contains an odd number of rows of data, a final row
    *      below the surface must be allocated."
    */
   if ((info->usage & XGL_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && align_h < 2)
      align_h = 2;

   /*
    * Depth Buffer Clear/Resolve works in 8x4 sample blocks.  In
    * intel_texture_can_enable_hiz(), we always return true for the first slice.
    * To avoid out-of-bound access, we have to pad.
    */
   if (layout->aux == INTEL_LAYOUT_AUX_HIZ &&
       info->mipLevels == 1 &&
       info->arraySize == 1 &&
       info->extent.depth == 1) {
      if (align_w < 8)
          align_w = 8;
      if (align_h < 4)
          align_h = 4;
   }

   params->max_x = u_align(params->max_x, align_w);
   params->max_y = u_align(params->max_y + pad_h, align_h);
}

/* note that this may force the texture to be linear */
static void
layout_calculate_bo_size(struct intel_layout *layout,
                         struct intel_layout_params *params)
{
   assert(params->max_x % layout->block_width == 0);
   assert(params->max_y % layout->block_height == 0);
   assert(layout->layer_height % layout->block_height == 0);

   layout->bo_stride =
      (params->max_x / layout->block_width) * layout->block_size;
   layout->bo_height = params->max_y / layout->block_height;

   while (true) {
      unsigned w = layout->bo_stride, h = layout->bo_height;
      unsigned align_w, align_h;

      /*
       * From the Haswell PRM, volume 5, page 163:
       *
       *     "For linear surfaces, additional padding of 64 bytes is required
       *      at the bottom of the surface. This is in addition to the padding
       *      required above."
       */
      if (intel_gpu_gen(params->gpu) >= INTEL_GEN(7.5) &&
          (params->info->usage & XGL_IMAGE_USAGE_SHADER_ACCESS_READ_BIT) &&
          layout->tiling == INTEL_TILING_NONE) {
         layout->bo_height +=
            (64 + layout->bo_stride - 1) / layout->bo_stride;
      }

      /*
       * From the Sandy Bridge PRM, volume 4 part 1, page 81:
       *
       *     "- For linear render target surfaces, the pitch must be a
       *        multiple of the element size for non-YUV surface formats.
       *        Pitch must be a multiple of 2 * element size for YUV surface
       *        formats.
       *      - For other linear surfaces, the pitch can be any multiple of
       *        bytes.
       *      - For tiled surfaces, the pitch must be a multiple of the tile
       *        width."
       *
       * Different requirements may exist when the bo is used in different
       * places, but our alignments here should be good enough that we do not
       * need to check layout->info->usage.
       */
      switch (layout->tiling) {
      case INTEL_TILING_X:
         align_w = 512;
         align_h = 8;
         break;
      case INTEL_TILING_Y:
         align_w = 128;
         align_h = 32;
         break;
      default:
         if (intel_format_is_stencil(params->gpu, layout->format)) {
            /*
             * From the Sandy Bridge PRM, volume 1 part 2, page 22:
             *
             *     "A 4KB tile is subdivided into 8-high by 8-wide array of
             *      Blocks for W-Major Tiles (W Tiles). Each Block is 8 rows by 8
             *      bytes."
             *
             * Since we asked for INTEL_TILING_NONE instead of the non-existent
             * INTEL_TILING_W, we want to align to W tiles here.
             */
            align_w = 64;
            align_h = 64;
         } else {
            /* some good enough values */
            align_w = 64;
            align_h = 2;
         }
         break;
      }

      w = u_align(w, align_w);
      h = u_align(h, align_h);

      /* make sure the bo is mappable */
      if (layout->tiling != INTEL_TILING_NONE) {
         /*
          * Usually only the first 256MB of the GTT is mappable.
          *
          * See also how intel_context::max_gtt_map_object_size is calculated.
          */
         const size_t mappable_gtt_size = 256 * 1024 * 1024;

         /*
          * Be conservative.  We may be able to switch from VALIGN_4 to
          * VALIGN_2 if the layout was Y-tiled, but let's keep it simple.
          */
         if (mappable_gtt_size / w / 4 < h) {
            if (layout->valid_tilings & LAYOUT_TILING_NONE) {
               layout->tiling = INTEL_TILING_NONE;
               /* MCS support for non-MSRTs is limited to tiled RTs */
               if (layout->aux == INTEL_LAYOUT_AUX_MCS &&
                   params->info->samples <= 1)
                  layout->aux = INTEL_LAYOUT_AUX_NONE;

               continue;
            } else {
               /* mapping will fail */
            }
         }
      }

      layout->bo_stride = w;
      layout->bo_height = h;
      break;
   }
}

static void
layout_calculate_hiz_size(struct intel_layout *layout,
                          struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   const unsigned hz_align_j = 8;
   enum intel_layout_walk_type hz_walk;
   unsigned hz_width, hz_height, lv;
   unsigned hz_clear_w, hz_clear_h;

   assert(layout->aux == INTEL_LAYOUT_AUX_HIZ);

   assert(layout->walk == INTEL_LAYOUT_WALK_LAYER ||
          layout->walk == INTEL_LAYOUT_WALK_3D);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 312:
    *
    *     "The hierarchical depth buffer does not support the LOD field, it is
    *      assumed by hardware to be zero. A separate hierarachical depth
    *      buffer is required for each LOD used, and the corresponding
    *      buffer's state delivered to hardware each time a new depth buffer
    *      state with modified LOD is delivered."
    *
    * We will put all LODs in a single bo with INTEL_LAYOUT_WALK_LOD.
    */
   if (intel_gpu_gen(params->gpu) >= INTEL_GEN(7))
      hz_walk = layout->walk;
   else
      hz_walk = INTEL_LAYOUT_WALK_LOD;

   /*
    * See the Sandy Bridge PRM, volume 2 part 1, page 312, and the Ivy Bridge
    * PRM, volume 2 part 1, page 312-313.
    *
    * It seems HiZ buffer is aligned to 8x8, with every two rows packed into a
    * memory row.
    */
   switch (hz_walk) {
   case INTEL_LAYOUT_WALK_LOD:
      {
         unsigned lod_tx[INTEL_LAYOUT_MAX_LEVELS];
         unsigned lod_ty[INTEL_LAYOUT_MAX_LEVELS];
         unsigned cur_tx, cur_ty;

         /* figure out the tile offsets of LODs */
         hz_width = 0;
         hz_height = 0;
         cur_tx = 0;
         cur_ty = 0;
         for (lv = 0; lv < info->mipLevels; lv++) {
            unsigned tw, th;

            lod_tx[lv] = cur_tx;
            lod_ty[lv] = cur_ty;

            tw = u_align(layout->lods[lv].slice_width, 16);
            th = u_align(layout->lods[lv].slice_height, hz_align_j) *
               info->arraySize / 2;
            /* convert to Y-tiles */
            tw = u_align(tw, 128) / 128;
            th = u_align(th, 32) / 32;

            if (hz_width < cur_tx + tw)
               hz_width = cur_tx + tw;
            if (hz_height < cur_ty + th)
               hz_height = cur_ty + th;

            if (lv == 1)
               cur_tx += tw;
            else
               cur_ty += th;
         }

         /* convert tile offsets to memory offsets */
         for (lv = 0; lv < info->mipLevels; lv++) {
            layout->aux_offsets[lv] =
               (lod_ty[lv] * hz_width + lod_tx[lv]) * 4096;
         }
         hz_width *= 128;
         hz_height *= 32;
      }
      break;
   case INTEL_LAYOUT_WALK_LAYER:
      {
         const unsigned h0 = u_align(params->h0, hz_align_j);
         const unsigned h1 = u_align(params->h1, hz_align_j);
         const unsigned htail =
            ((intel_gpu_gen(params->gpu) >= INTEL_GEN(7)) ? 12 : 11) * hz_align_j;
         const unsigned hz_qpitch = h0 + h1 + htail;

         hz_width = u_align(layout->lods[0].slice_width, 16);

         hz_height = hz_qpitch * info->arraySize / 2;
         if (intel_gpu_gen(params->gpu) >= INTEL_GEN(7))
            hz_height = u_align(hz_height, 8);
      }
      break;
   case INTEL_LAYOUT_WALK_3D:
      hz_width = u_align(layout->lods[0].slice_width, 16);

      hz_height = 0;
      for (lv = 0; lv < info->mipLevels; lv++) {
         const unsigned h = u_align(layout->lods[lv].slice_height, hz_align_j);
         /* according to the formula, slices are packed together vertically */
         hz_height += h * u_minify(info->extent.depth, lv);
      }
      hz_height /= 2;
      break;
   }

   /*
    * In hiz_align_fb(), we will align the LODs to 8x4 sample blocks.
    * Experiments on Haswell show that aligning the RECTLIST primitive and
    * 3DSTATE_DRAWING_RECTANGLE alone are not enough.  The LOD sizes must be
    * aligned.
    */
   hz_clear_w = 8;
   hz_clear_h = 4;
   switch (info->samples) {
   case 0:
   case 1:
   default:
      break;
   case 2:
      hz_clear_w /= 2;
      break;
   case 4:
      hz_clear_w /= 2;
      hz_clear_h /= 2;
      break;
   case 8:
      hz_clear_w /= 4;
      hz_clear_h /= 2;
      break;
   case 16:
      hz_clear_w /= 4;
      hz_clear_h /= 4;
      break;
   }

   for (lv = 0; lv < info->mipLevels; lv++) {
      if (u_minify(layout->width0, lv) % hz_clear_w ||
          u_minify(layout->height0, lv) % hz_clear_h)
         break;
      layout->aux_enables |= 1 << lv;
   }

   /* we padded to allow this in layout_align() */
   if (info->mipLevels == 1 && info->arraySize == 1 && info->extent.depth == 1)
      layout->aux_enables |= 0x1;

   /* align to Y-tile */
   layout->aux_stride = u_align(hz_width, 128);
   layout->aux_height = u_align(hz_height, 32);
}

static void
layout_calculate_mcs_size(struct intel_layout *layout,
                          struct intel_layout_params *params)
{
   const XGL_IMAGE_CREATE_INFO *info = params->info;
   int mcs_width, mcs_height, mcs_cpp;
   int downscale_x, downscale_y;

   assert(layout->aux == INTEL_LAYOUT_AUX_MCS);

   if (info->samples > 1) {
      /*
       * From the Ivy Bridge PRM, volume 2 part 1, page 326, the clear
       * rectangle is scaled down by 8x2 for 4X MSAA and 2x2 for 8X MSAA.  The
       * need of scale down could be that the clear rectangle is used to clear
       * the MCS instead of the RT.
       *
       * For 8X MSAA, we need 32 bits in MCS for every pixel in the RT.  The
       * 2x2 factor could come from that the hardware writes 128 bits (an
       * OWord) at a time, and the OWord in MCS maps to a 2x2 pixel block in
       * the RT.  For 4X MSAA, we need 8 bits in MCS for every pixel in the
       * RT.  Similarly, we could reason that an OWord in 4X MCS maps to a 8x2
       * pixel block in the RT.
       */
      switch (info->samples) {
      case 2:
      case 4:
         downscale_x = 8;
         downscale_y = 2;
         mcs_cpp = 1;
         break;
      case 8:
         downscale_x = 2;
         downscale_y = 2;
         mcs_cpp = 4;
         break;
      case 16:
         downscale_x = 2;
         downscale_y = 1;
         mcs_cpp = 8;
         break;
      default:
         assert(!"unsupported sample count");
         return;
         break;
      }

      /*
       * It also appears that the 2x2 subspans generated by the scaled-down
       * clear rectangle cannot be masked.  The scale-down clear rectangle
       * thus must be aligned to 2x2, and we need to pad.
       */
      mcs_width = u_align(layout->width0, downscale_x * 2);
      mcs_height = u_align(layout->height0, downscale_y * 2);
   } else {
      /*
       * From the Ivy Bridge PRM, volume 2 part 1, page 327:
       *
       *     "              Pixels  Lines
       *      TiledY RT CL
       *          bpp
       *          32          8        4
       *          64          4        4
       *          128         2        4
       *
       *      TiledX RT CL
       *          bpp
       *          32          16       2
       *          64          8        2
       *          128         4        2"
       *
       * This table and the two following tables define the RT alignments, the
       * clear rectangle alignments, and the clear rectangle scale factors.
       * Viewing the RT alignments as the sizes of 128-byte blocks, we can see
       * that the clear rectangle alignments are 16x32 blocks, and the clear
       * rectangle scale factors are 8x16 blocks.
       *
       * For non-MSAA RT, we need 1 bit in MCS for every 128-byte block in the
       * RT.  Similar to the MSAA cases, we can argue that an OWord maps to
       * 8x16 blocks.
       *
       * One problem with this reasoning is that a Y-tile in MCS has 8x32
       * OWords and maps to 64x512 128-byte blocks.  This differs from i965,
       * which says that a Y-tile maps to 128x256 blocks (\see
       * intel_get_non_msrt_mcs_alignment).  It does not really change
       * anything except for the size of the allocated MCS.  Let's see if we
       * hit out-of-bound access.
       */
      switch (layout->tiling) {
      case INTEL_TILING_X:
         downscale_x = 64 / layout->block_size;
         downscale_y = 2;
         break;
      case INTEL_TILING_Y:
         downscale_x = 32 / layout->block_size;
         downscale_y = 4;
         break;
      default:
         assert(!"unsupported tiling mode");
         return;
         break;
      }

      downscale_x *= 8;
      downscale_y *= 16;

      /*
       * From the Haswell PRM, volume 7, page 652:
       *
       *     "Clear rectangle must be aligned to two times the number of
       *      pixels in the table shown below due to 16X16 hashing across the
       *      slice."
       *
       * The scaled-down clear rectangle must be aligned to 4x4 instead of
       * 2x2, and we need to pad.
       */
      mcs_width = u_align(layout->width0, downscale_x * 4) / downscale_x;
      mcs_height = u_align(layout->height0, downscale_y * 4) / downscale_y;
      mcs_cpp = 16; /* an OWord */
   }

   layout->aux_enables = (1 << info->mipLevels) - 1;
   /* align to Y-tile */
   layout->aux_stride = u_align(mcs_width * mcs_cpp, 128);
   layout->aux_height = u_align(mcs_height, 32);
}

/**
 * Initialize the layout.  Callers should zero-initialize \p layout first.
 */
void intel_layout_init(struct intel_layout *layout,
                       const struct intel_dev *dev,
                       const XGL_IMAGE_CREATE_INFO *info)
{
   struct intel_layout_params params;

   memset(&params, 0, sizeof(params));
   params.gpu = dev->gpu;
   params.info = info;

   /* note that there are dependencies between these functions */
   layout_init_aux(layout, &params);
   layout_init_size_and_format(layout, &params);
   layout_init_walk(layout, &params);
   layout_init_tiling(layout, &params);
   layout_init_alignments(layout, &params);
   layout_init_lods(layout, &params);
   layout_init_layer_height(layout, &params);

   layout_align(layout, &params);
   layout_calculate_bo_size(layout, &params);

   switch (layout->aux) {
   case INTEL_LAYOUT_AUX_HIZ:
      layout_calculate_hiz_size(layout, &params);
      break;
   case INTEL_LAYOUT_AUX_MCS:
      layout_calculate_mcs_size(layout, &params);
      break;
   default:
      break;
   }
}

/**
 * Update the tiling mode and bo stride (for imported resources).
 */
bool
intel_layout_update_for_imported_bo(struct intel_layout *layout,
                                  enum intel_tiling_mode tiling,
                                  unsigned bo_stride)
{
   if (!(layout->valid_tilings & (1 << tiling)))
      return false;

   if ((tiling == INTEL_TILING_X && bo_stride % 512) ||
       (tiling == INTEL_TILING_Y && bo_stride % 128))
      return false;

   layout->tiling = tiling;
   layout->bo_stride = bo_stride;

   return true;
}
