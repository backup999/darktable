/*
    This file is part of darktable,
    Copyright (C) 2020-2025 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "imageio_common.h"
#include "imageio_gm.h"
#include "develop/develop.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"

#include <memory.h>
#include <stdio.h>
#include <inttypes.h>
#include <strings.h>
#include <assert.h>

#ifdef HAVE_IMAGEMAGICK7
#include <MagickWand/MagickWand.h>
#else
#include <wand/MagickWand.h>
#endif

// We only support images with certain filename extensions via ImageMagick.
// RAWs are excluded as ImageMagick would render them with third party
// libraries in reduced quality - slow and only 8-bit.
static gboolean _supported_image(const gchar *filename)
{
  const char *extensions_whitelist[] =
    {
    "tiff", "tif", // may support more format features than the native loader
    "pam", "pbm", "pgm", "ppm", "pnm", // PNM-style formats
    "jpc", "jp2", "jpf", "jpx", // JPEG 2000 extensions
    "bmp", "miff", "dcm", "jng", "mng", "gif",  // misc exotic formats
    "fits", "fit", "fts",  // FITS format (common in astro imagery)
    "cin", "dpx", // Kodak made formats used in film industry for still frames
    "jxl",  // probably of no practical use
    "webp", // probably of no practical use
    NULL
    };

  gboolean supported = FALSE;
  char *ext = g_strrstr(filename, ".");
  if(!ext) return FALSE;
  ext++;
  for(const char **i = extensions_whitelist; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      supported = TRUE;
      break;
    }
#ifdef HAVE_IMAGEMAGICK7
  supported |= g_ascii_strncasecmp(ext, "qoi", 3) == 0;
#endif
  return supported;
}


dt_imageio_retval_t dt_imageio_open_im(dt_image_t *img,
                                       const char *filename,
                                       dt_mipmap_buffer_t *mbuf)
{
  dt_imageio_retval_t err = DT_IMAGEIO_LOAD_FAILED;

  if(!_supported_image(filename))
    return DT_IMAGEIO_LOAD_FAILED;

  if(!img->exif_inited)
    (void)dt_exif_read(img, filename);

  MagickWand *image = NewMagickWand();
  if(image == NULL)
    goto error;

  MagickBooleanType ret = MagickReadImage(image, filename);
  if(ret != MagickTrue) {
    dt_print(DT_DEBUG_ALWAYS,
             "[ImageMagick_open] cannot open '%s'",
             img->filename);
    err = DT_IMAGEIO_FILE_NOT_FOUND;
    goto error;
  }

  dt_print(DT_DEBUG_IMAGEIO,
           "[ImageMagick_open] image '%s' loading",
           img->filename);

  img->width = MagickGetImageWidth(image);
  img->height = MagickGetImageHeight(image);

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = dt_mipmap_cache_alloc(mbuf, img);
  if(mipbuf == NULL) {
    dt_print(DT_DEBUG_ALWAYS,
        "[ImageMagick_open] could not alloc full buffer for image '%s'",
        img->filename);
    err = DT_IMAGEIO_CACHE_FULL;
    goto error;
  }

  ColorspaceType colorspace = MagickGetImageColorspace(image);

  char *colormap;
  if((colorspace == CMYColorspace) || (colorspace == CMYKColorspace))
    colormap = "CMYK";
  else
    colormap = "RGBP";

  ret = MagickExportImagePixels(image,
                                0,
                                0,
                                img->width,
                                img->height,
                                colormap,
                                FloatPixel,
                                mipbuf);

  if(ret != MagickTrue) {
    dt_print(DT_DEBUG_ALWAYS,
        "[ImageMagick_open] error reading image '%s'", img->filename);
    goto error;
  }

  // If the image in CMYK color space convert it to linear RGB
  if((colorspace == CMYColorspace) || (colorspace == CMYKColorspace))
  {
    for(size_t index = 0; index < img->width * img->height * 4; index += 4)
    {
      const float black = mipbuf[index + 3];
      mipbuf[index]     = (1.f - black) * (1.f - mipbuf[index]);
      mipbuf[index + 1] = (1.f - black) * (1.f - mipbuf[index + 1]);
      mipbuf[index + 2] = (1.f - black) * (1.f - mipbuf[index + 2]);
    }
  }

  size_t profile_length;
  uint8_t *profile_data = (uint8_t *)MagickGetImageProfile(image,
                                                           "icc",
                                                           &profile_length);

  // No alias support for profile names (unlike GraphicsMagick),
  // so we need to try "icm" if we didn't get an "icc" profile
  if(profile_data == NULL)
    profile_data = (uint8_t *)MagickGetImageProfile(image,
                                                    "icm",
                                                    &profile_length);

  if(profile_data)
  {
    img->profile = g_try_malloc0(profile_length);
    if(img->profile)
    {
      memcpy(img->profile, profile_data, profile_length);
      img->profile_size = profile_length;
    }
    MagickRelinquishMemory(profile_data);
  }

  // As a warning to those who will modify the loader in the future:
  // MagickWandTerminus() cannot be called on successful image reading.
  // See https://github.com/darktable-org/darktable/issues/13090
  // regarding the consequences.
  DestroyMagickWand(image);

  img->buf_dsc.cst = IOP_CS_RGB;
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_LDR;

  img->loader = LOADER_IM;
  return DT_IMAGEIO_OK;

error:
  DestroyMagickWand(image);
  return err;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
