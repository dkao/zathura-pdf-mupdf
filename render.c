/* See LICENSE file for license and copyright information */

#define _POSIX_C_SOURCE 1

#include <glib.h>

#include "plugin.h"

static void
buffer_blit_row(unsigned char* dst, int dst_ncmpt,
		unsigned char* src, int src_ncmpt,
		int width)
{
  for (; width; width--) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst += dst_ncmpt;
    src += src_ncmpt;
  }
}

static void
buffer_blit(unsigned char* dst, int dst_ncmpt, int dst_stride,
	    unsigned char* src, int src_ncmpt, int src_stride,
	    int width, int height)
{
  for (unsigned int y = 0; y < height; y++) {
    buffer_blit_row(dst, dst_ncmpt, src, src_ncmpt, width);
    dst += dst_stride;
    src += src_stride;
  }
}

enum {SUBPIXEL_HRGB, SUBPIXEL_HBGR, SUBPIXEL_VRGB, SUBPIXEL_VBGR};

/* 5-tap filter from Freetype, see ftlcdfil.h.  */
static inline int
filter(int x0, int x1, int x2, int x3, int x4)
{
  int FIR_WA = 0x30, FIR_WC = 0x20;
  int w0 = FIR_WA - FIR_WC, w1 = FIR_WA + FIR_WC, w2 = FIR_WA * 2;
  int W = w0 + w1 + w2 + w1 + w0;

  return (x0 * w0 + x1 * w1 + x2 * w2 + x3 * w1 + x4 * w0 + W / 2) / W;
}

/* Shrink a bitmap by 3x horizontally with subpixel precision.  For pixel
   data, BGR layout is assumed.  SUBPIX_ORDER is physical layout.  */
static void
buffer_subpix_x(unsigned char* dst, int dst_ncmpt, int dst_stride,
		unsigned char* src, int src_ncmpt, int src_stride,
		int width, int height, int subpix_order)
{
  int n = src_ncmpt;
  int blue_offset = (subpix_order == SUBPIXEL_HBGR) ? -n : n;
  src += n;

  for (unsigned int y = 0; y < height; y++) {
    unsigned char* d = dst;
    unsigned char* s = src;
    /* Do not bother filtering the first and the last columns.  */
    {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      s += src_ncmpt * 3;
      d += dst_ncmpt;
    }
    for (unsigned int x = 1; x < width - 1; x++) {
      s += blue_offset;
      d[0] = filter(s[-n * 2], s[-n], s[0], s[n], s[n * 2]);
      s += -blue_offset + 1;
      d[1] = filter(s[-n * 2], s[-n], s[0], s[n], s[n * 2]);
      s += -blue_offset + 1;
      d[2] = filter(s[-n * 2], s[-n], s[0], s[n], s[n * 2]);
      s += blue_offset - 2 + src_ncmpt * 3;
      d += dst_ncmpt;
    }
    {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
    }
    dst += dst_stride;
    src += src_stride;
  }
}

static void
buffer_subpix_y(unsigned char* dst, int dst_ncmpt, int dst_stride,
		unsigned char* src, int src_ncmpt, int src_stride,
		int width, int height, int subpix_order)
{
  int n = src_stride;
  int blue_offset = (subpix_order == SUBPIXEL_VBGR) ? -n : n;
  src += n;

  buffer_blit_row(dst, dst_ncmpt, src, src_ncmpt, width);
  dst += dst_stride;
  src += src_stride * 3;
  for (unsigned int y = 1; y < height - 1; y++) {
    unsigned char* d = dst;
    unsigned char* s = src;
    for (unsigned int x = 0; x < width; x++) {
      s += blue_offset;
      d[0] = filter(s[-n * 2], s[-n], s[0], s[n], s[n * 2]);
      s += -blue_offset + 1;
      d[1] = filter(s[-n * 2], s[-n], s[0], s[n], s[n * 2]);
      s += -blue_offset + 1;
      d[2] = filter(s[-n * 2], s[-n], s[0], s[n], s[n * 2]);
      s += blue_offset - 2 + src_ncmpt;
      d += dst_ncmpt;
    }
    dst += dst_stride;
    src += src_stride * 3;
  }
  buffer_blit_row(dst, dst_ncmpt, src, src_ncmpt, width);
}


static zathura_error_t
pdf_page_render_to_buffer(mupdf_document_t* mupdf_document, mupdf_page_t* mupdf_page,
			  unsigned char* image, int rowstride, int components,
			  unsigned int page_width, unsigned int page_height,
			  double scalex, double scaley)
{
  if (mupdf_document == NULL ||
      mupdf_document->ctx == NULL ||
      mupdf_page == NULL ||
      mupdf_page->page == NULL ||
      image == NULL) {
    return ZATHURA_ERROR_UNKNOWN;
  }

  fz_display_list* display_list = fz_new_display_list(mupdf_page->ctx);
  fz_device* device             = fz_new_list_device(mupdf_page->ctx, display_list);

  int subpixx = 1, subpixy = 1;
  int subpix_order = SUBPIXEL_HRGB;
  if (subpix_order == SUBPIXEL_HRGB || subpix_order == SUBPIXEL_HBGR) {
    subpixx = 3;
  } else if (subpix_order == SUBPIXEL_VRGB || subpix_order == SUBPIXEL_VBGR) {
    subpixy = 3;
  }

  fz_try (mupdf_document->ctx) {
    fz_matrix m;
    fz_scale(&m, scalex * subpixx, scaley * subpixy);
    fz_run_page(mupdf_document->ctx, mupdf_page->page, device, &m, NULL);
  } fz_catch (mupdf_document->ctx) {
    return ZATHURA_ERROR_UNKNOWN;
  }

  fz_drop_device(mupdf_page->ctx, device);

  fz_irect irect = { .x1 = page_width * subpixx, .y1 = page_height * subpixy };
  fz_rect rect = { .x1 = page_width * subpixx, .y1 = page_height * subpixy };

  fz_colorspace* colorspace = fz_device_bgr(mupdf_document->ctx);
  fz_pixmap* pixmap = fz_new_pixmap_with_bbox(mupdf_page->ctx, colorspace, &irect);
  fz_clear_pixmap_with_value(mupdf_page->ctx, pixmap, 0xFF);

  device = fz_new_draw_device(mupdf_page->ctx, pixmap);
  fz_run_display_list(mupdf_page->ctx, display_list, device, &fz_identity, &rect, NULL);
  fz_drop_device(mupdf_page->ctx, device);

  unsigned char* s = fz_pixmap_samples(mupdf_page->ctx, pixmap);
  unsigned int n   = fz_pixmap_components(mupdf_page->ctx, pixmap);
  if (subpixx == 3) {
    buffer_subpix_x(image, components, rowstride, s, n, n * page_width * 3, page_width, page_height, subpix_order);
  } else if (subpixy == 3) {
    buffer_subpix_y(image, components, rowstride, s, n, n * page_width, page_width, page_height, subpix_order);
  } else {
    buffer_blit(image, components, rowstride, s, n, n * page_width, page_width, page_height);
  }

  fz_drop_pixmap(mupdf_page->ctx, pixmap);
  fz_drop_display_list(mupdf_page->ctx, display_list);

  return ZATHURA_ERROR_OK;
}

zathura_image_buffer_t*
pdf_page_render(zathura_page_t* page, mupdf_page_t* mupdf_page, zathura_error_t* error)
{
  if (page == NULL || mupdf_page == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return NULL;
  }

  /* calculate sizes */
  double scalex            = zathura_document_get_scale(document);
  double scaley            = scalex;
  unsigned int page_width  = scalex * zathura_page_get_width(page);
  unsigned int page_height = scaley * zathura_page_get_height(page);

  /* create image buffer */
  zathura_image_buffer_t* image_buffer = zathura_image_buffer_create(page_width, page_height);

  if (image_buffer == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }

  int rowstride        = image_buffer->rowstride;
  unsigned char* image = image_buffer->data;

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  zathura_error_t error_render = pdf_page_render_to_buffer(mupdf_document, mupdf_page, image, rowstride, 3,
						page_width, page_height, scalex, scaley);

  if (error_render != ZATHURA_ERROR_OK) {
    zathura_image_buffer_free(image_buffer);
    if (error != NULL) {
      *error = error_render;
    }
    return NULL;
  }

  return image_buffer;
}

#if HAVE_CAIRO
zathura_error_t
pdf_page_render_cairo(zathura_page_t* page, mupdf_page_t* mupdf_page, cairo_t* cairo, bool GIRARA_UNUSED(printing))
{
  if (page == NULL || mupdf_page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  cairo_surface_t* surface = cairo_get_target(cairo);
  if (surface == NULL ||
      cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
      cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) {
    return ZATHURA_ERROR_UNKNOWN;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return ZATHURA_ERROR_UNKNOWN;
  }

  unsigned int page_width  = cairo_image_surface_get_width(surface);
  unsigned int page_height = cairo_image_surface_get_height(surface);

  double scalex = ((double) page_width) / zathura_page_get_width(page);
  double scaley = ((double) page_height) /zathura_page_get_height(page);

  int rowstride        = cairo_image_surface_get_stride(surface);
  unsigned char* image = cairo_image_surface_get_data(surface);

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  return pdf_page_render_to_buffer(mupdf_document, mupdf_page, image, rowstride, 4,
				   page_width, page_height, scalex, scaley);
}
#endif

