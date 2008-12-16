/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

/*
 * Part of this file is:
 *
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * This file is part of the Independent JPEG Group's software.
 * For conditions of distribution and use, see the accompanying README file.
 */

#include "config.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <jpeglib.h>
#include <jerror.h>
#include <inttypes.h>

#include <sys/types.h>   // for off_t ?

#include "openslide-private.h"
#include "openslide-cache.h"

struct one_jpeg {
  FILE *f;

  int64_t mcu_starts_count;
  int64_t *mcu_starts;

  int32_t tile_width;
  int32_t tile_height;

  int32_t width;
  int32_t height;

  char *comment;
};

struct layer {
  struct one_jpeg **layer_jpegs; // count given by jpeg_w * jpeg_h

  // total size (not premultiplied by scale_denom)
  int64_t pixel_w;
  int64_t pixel_h;

  int32_t jpegs_across;       // how many distinct jpeg files across?
  int32_t jpegs_down;         // how many distinct jpeg files down?

  // the size of image (0,0), which is used to find the jpeg we want
  // from a given (x,y) (not premultiplied)
  int32_t image00_w;
  int32_t image00_h;

  int32_t scale_denom;
  double no_scale_denom_downsample;  // layer0_w div non_premult_pixel_w
};

struct jpegops_data {
  int32_t jpeg_count;
  struct one_jpeg *all_jpegs;

  // layer_count is in the osr struct
  struct layer *layers;

  // cache
  struct _openslide_cache *cache;
};


static bool is_zxy_successor(int64_t pz, int64_t px, int64_t py,
			     int64_t z, int64_t x, int64_t y) {
  //  g_debug("p_zxy: (%" PRId64 ",%" PRId64 ",%" PRId64 "), zxy: (%"
  //	  PRId64 ",%" PRId64 ",%" PRId64 ")",
  //	  pz, px, py, z, x, y);
  if (z == pz + 1) {
    return x == 0 && y == 0;
  }
  if (z != pz) {
    return false;
  }

  // z == pz

  if (y == py + 1) {
    return x == 0;
  }
  if (y != py) {
    return false;
  }

  // y == py

  return x == px + 1;
}

static guint int64_hash(gconstpointer v) {
  int64_t i = *((const int64_t *) v);
  return i ^ (i >> 32);
}

static gboolean int64_equal(gconstpointer v1, gconstpointer v2) {
  return *((int64_t *) v1) == *((int64_t *) v2);
}

static void int64_free(gpointer data) {
  g_slice_free(int64_t, data);
}

static void layer_free(gpointer data) {
  //  g_debug("layer_free: %p", data);

  struct layer *l = data;

  //  g_debug("g_free(%p)", (void *) l->layer_jpegs);
  g_free(l->layer_jpegs);
  g_slice_free(struct layer, l);
}

static void print_wlmap_entry(gpointer key, gpointer value,
			      gpointer user_data) {
  int64_t k = *((int64_t *) key);
  struct layer *v = (struct layer *) value;

  g_debug("%" PRId64 " -> ( pw: %" PRId64 ", ph: %" PRId64
	  ", jw: %" PRId32 ", jh: %" PRId32 ", scale_denom: %" PRId32
	  ", img00_w: %" PRId32 ", img00_h: %" PRId32 ", no_scale_denom_downsample: %g )",
	  k, v->pixel_w, v->pixel_h, v->jpegs_across, v->jpegs_down, v->scale_denom, v->image00_w, v->image00_h, v->no_scale_denom_downsample);
}

static void generate_layers_into_map(GSList *jpegs,
				     int32_t jpegs_across, int32_t jpegs_down,
				     int64_t pixel_w, int64_t pixel_h,
				     int32_t image00_w, int32_t image00_h,
				     int64_t layer0_w,
				     GHashTable *width_to_layer_map) {
  // JPEG files can give us 1/1, 1/2, 1/4, 1/8 downsamples, so we
  // need to create 4 layers per set of JPEGs

  int32_t num_jpegs = jpegs_across * jpegs_down;

  int scale_denom = 1;
  while (scale_denom <= 8) {
    // create layer
    struct layer *l = g_slice_new0(struct layer);
    l->jpegs_across = jpegs_across;
    l->jpegs_down = jpegs_down;
    l->pixel_w = pixel_w;
    l->pixel_h = pixel_h;
    l->scale_denom = scale_denom;
    l->image00_w = image00_w;
    l->image00_h = image00_h;
    l->no_scale_denom_downsample = (double) layer0_w / (double) pixel_w;

    // create array and copy
    l->layer_jpegs = g_new(struct one_jpeg *, num_jpegs);
    //    g_debug("g_new(struct one_jpeg *) -> %p", (void *) l->layer_jpegs);
    GSList *jj = jpegs;
    for (int32_t i = 0; i < num_jpegs; i++) {
      g_assert(jj);
      l->layer_jpegs[i] = (struct one_jpeg *) jj->data;
      jj = jj->next;
    }

    // put into map
    int64_t *key = g_slice_new(int64_t);
    *key = l->pixel_w / l->scale_denom;

    //    g_debug("insert %" PRId64 ", scale_denom: %d", *key, scale_denom);
    g_hash_table_insert(width_to_layer_map, key, l);

    scale_denom <<= 1;
  }
}

static GHashTable *create_width_to_layer_map(int32_t count,
					     struct _openslide_jpeg_fragment **fragments,
					     struct one_jpeg *jpegs) {
  int64_t prev_z = -1;
  int64_t prev_x = -1;
  int64_t prev_y = -1;

  GSList *layer_jpegs_tmp = NULL;
  int64_t l_pw = 0;
  int64_t l_ph = 0;

  int32_t img00_w = 0;
  int32_t img00_h = 0;

  int64_t layer0_w = 0;

  // int* -> struct layer*
  GHashTable *width_to_layer_map = g_hash_table_new_full(int64_hash,
							 int64_equal,
							 int64_free,
							 layer_free);

  // go through the fragments, accumulating to layers
  for (int32_t i = 0; i < count; i++) {
    struct _openslide_jpeg_fragment *fr = fragments[i];
    struct one_jpeg *oj = jpegs + i;

    // the fragments MUST be in sorted order by z,x,y
    g_assert(is_zxy_successor(prev_z, prev_x, prev_y,
			      fr->z, fr->x, fr->y));

    // special case for first layer
    if (prev_z == -1) {
      prev_z = 0;
      prev_x = 0;
      prev_y = 0;
    }

    // save first image dimensions
    if (fr->x == 0 && fr->y == 0) {
      img00_w = oj->width;
      img00_h = oj->height;
    }

    // accumulate size
    if (fr->y == 0) {
      l_pw += oj->width;
    }
    if (fr->x == 0) {
      l_ph += oj->height;
    }

    //    g_debug(" pw: %" PRId64 ", ph: %" PRId64, l_pw, l_ph);

    // accumulate to layer
    layer_jpegs_tmp = g_slist_prepend(layer_jpegs_tmp, oj);

    // is this the end of this layer? then flush
    if (i == count - 1 || fragments[i + 1]->z != fr->z) {
      layer_jpegs_tmp = g_slist_reverse(layer_jpegs_tmp);

      // save layer0 width
      if (fr->z == 0) {
	layer0_w = l_pw;
      }

      generate_layers_into_map(layer_jpegs_tmp, fr->x + 1, fr->y + 1,
			       l_pw, l_ph,
			       img00_w, img00_h,
			       layer0_w,
			       width_to_layer_map);

      // clear for next round
      l_pw = 0;
      l_ph = 0;
      img00_w = 0;
      img00_h = 0;

      while (layer_jpegs_tmp != NULL) {
	layer_jpegs_tmp = g_slist_delete_link(layer_jpegs_tmp, layer_jpegs_tmp);
      }
    }

    // update prevs
    prev_z = fr->z;
    prev_x = fr->x;
    prev_y = fr->y;
  }

  return width_to_layer_map;
}


static void read_from_one_jpeg (struct one_jpeg *jpeg,
				uint32_t *dest,
				int32_t x, int32_t y,
				int32_t scale_denom,
				int32_t w, int32_t h,
				int32_t stride) {
  //  g_debug("read_from_one_jpeg: %p, dest: %p, x: %d, y: %d, scale_denom: %d, w: %d, h: %d", (void *) jpeg, (void *) dest, x, y, scale_denom, w, h);

  // init JPEG
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);

  // figure out where to start the data stream
  int32_t tile_y = y / jpeg->tile_height;
  int32_t tile_x = x / jpeg->tile_width;

  int32_t stride_in_tiles = jpeg->width / jpeg->tile_width;
  int32_t img_height_in_tiles = jpeg->height / jpeg->tile_height;

  imaxdiv_t divtmp;
  divtmp = imaxdiv((w * scale_denom) + (x % jpeg->tile_width), jpeg->tile_width);
  int32_t width_in_tiles = divtmp.quot + !!divtmp.rem;  // integer ceil
  divtmp = imaxdiv((h * scale_denom) + (y % jpeg->tile_height), jpeg->tile_height);
  int32_t height_in_tiles = divtmp.quot + !!divtmp.rem;

  // clamp width and height
  width_in_tiles = MIN(width_in_tiles, stride_in_tiles - tile_x);
  height_in_tiles = MIN(height_in_tiles, img_height_in_tiles - tile_y);

  //  g_debug("width_in_tiles: %d, stride_in_tiles: %d", width_in_tiles, stride_in_tiles);
  //  g_debug("tile_x: %d, tile_y: %d\n", tile_x, tile_y);

  rewind(jpeg->f);
  _openslide_jpeg_fancy_src(&cinfo, jpeg->f,
			    jpeg->mcu_starts,
			    jpeg->mcu_starts_count,
			    tile_y * stride_in_tiles + tile_x,
			    width_in_tiles,
			    stride_in_tiles);

  // begin decompress
  int32_t rows_left = h;
  jpeg_read_header(&cinfo, FALSE);
  cinfo.scale_denom = scale_denom;
  cinfo.image_width = width_in_tiles * jpeg->tile_width;  // cunning
  cinfo.image_height = height_in_tiles * jpeg->tile_height;

  jpeg_start_decompress(&cinfo);

  //  g_debug("output_width: %d", cinfo.output_width);
  //  g_debug("output_height: %d", cinfo.output_height);

  // allocate scanline buffers
  JSAMPARRAY buffer =
    g_slice_alloc(sizeof(JSAMPROW) * cinfo.rec_outbuf_height);
  gsize row_size =
    sizeof(JSAMPLE)
    * cinfo.output_width
    * 3;  // output components
  for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
    buffer[i] = g_slice_alloc(row_size);
    //g_debug("buffer[%d]: %p", i, buffer[i]);
  }

  // decompress
  int32_t d_x = (x % jpeg->tile_width) / scale_denom;
  int32_t d_y = (y % jpeg->tile_height) / scale_denom;
  int32_t rows_to_skip = d_y;

  //  g_debug("d_x: %d, d_y: %d", d_x, d_y);

  int64_t pixels_wasted = rows_to_skip * cinfo.output_width;

  //  abort();

  while (cinfo.output_scanline < cinfo.output_height
	 && rows_left > 0) {
    JDIMENSION rows_read = jpeg_read_scanlines(&cinfo,
					       buffer,
					       cinfo.rec_outbuf_height);
    //    g_debug("just read scanline %d", cinfo.output_scanline - rows_read);
    //    g_debug(" rows read: %d", rows_read);
    int cur_buffer = 0;
    while (rows_read > 0 && rows_left > 0) {
      // copy a row
      if (rows_to_skip == 0) {
	int32_t i;
	for (i = 0; i < w && i < ((int32_t) cinfo.output_width - d_x); i++) {
	  dest[i] = 0xFF000000 |                          // A
	    buffer[cur_buffer][(d_x + i) * 3 + 0] << 16 | // R
	    buffer[cur_buffer][(d_x + i) * 3 + 1] << 8 |  // G
	    buffer[cur_buffer][(d_x + i) * 3 + 2];        // B
	}
	pixels_wasted += d_x + cinfo.output_width - i;
      }

      // advance everything 1 row
      rows_read--;
      cur_buffer++;

      if (rows_to_skip > 0) {
	rows_to_skip--;
      } else {
	rows_left--;
	dest += stride;
      }
    }
  }

  //  g_debug("pixels wasted: %llu", pixels_wasted);

  // free buffers
  for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
    g_slice_free1(row_size, buffer[i]);
  }
  g_slice_free1(sizeof(JSAMPROW) * cinfo.rec_outbuf_height, buffer);

  // last thing, stop jpeg
  jpeg_destroy_decompress(&cinfo);
}

static void read_region(openslide_t *osr, uint32_t *dest,
			int64_t x, int64_t y,
			int32_t layer,
			int64_t w, int64_t h) {
  //  g_debug("jpeg ops read_region: x: %" PRId64 ", y: %" PRId64 ", layer: %d, w: %" PRId64 ", h: %" PRId64 "",
  //	  x, y, layer, w, h);

  struct jpegops_data *data = osr->data;

  // get the layer
  struct layer *l = data->layers + layer;
  int32_t scale_denom = l->scale_denom;
  double rel_downsample = l->no_scale_denom_downsample;
  //  g_debug("layer: %d, rel_downsample: %g, scale_denom: %d",
  //	  layer, rel_downsample, scale_denom);


  // all things with scale_denom are accounted for in the JPEG library
  // so, we don't adjust here for it, except in w,h


  // go file by file

  int64_t src_y = y / rel_downsample;  // scale into this jpeg's space
  src_y = (src_y / scale_denom) * scale_denom; // round down to scaled pixel boundary
  int64_t dest_y = 0;
  int64_t end_src_y = MIN(src_y + h * scale_denom, l->pixel_h);  // set the end

  while (src_y < end_src_y) {
    int32_t file_y = src_y / l->image00_h;
    int64_t origin_src_segment_y = file_y * (int64_t) l->image00_h;
    int32_t end_in_src_segment_y = MIN((file_y + 1) * (int64_t) l->image00_h,
				       end_src_y) - origin_src_segment_y;
    int32_t start_in_src_segment_y = src_y - origin_src_segment_y;

    int32_t dest_h = (end_in_src_segment_y - start_in_src_segment_y)
      / scale_denom;

    int64_t src_x = x / rel_downsample;
    src_x = (src_x / scale_denom) * scale_denom;
    int64_t dest_x = 0;
    int64_t end_src_x = MIN(src_x + w * scale_denom, l->pixel_w);

    //    g_debug("for (%" PRId64 ",%" PRId64 ") to (%" PRId64 ",%" PRId64 "):",
    //    	    src_x, src_y, end_src_x, end_src_y);

    while (src_x < end_src_x) {
      int32_t file_x = src_x / l->image00_w;
      int64_t origin_src_segment_x = file_x * l->image00_w;
      int32_t end_in_src_segment_x = MIN((file_x + 1) * (int64_t) l->image00_w,
					 end_src_x) - origin_src_segment_x;
      int32_t start_in_src_segment_x = src_x - origin_src_segment_x;

      int32_t dest_w = (end_in_src_segment_x - start_in_src_segment_x)
	/ scale_denom;

      //      g_debug(" copy image(%" PRId32 ",%" PRId32 "), "
      //	      "from (%" PRId32 ",%" PRId32 ") to (%" PRId32 ",%" PRId32 ")",
      //	      file_x, file_y,
      //	      start_in_src_segment_x, start_in_src_segment_y,
      //	      end_in_src_segment_x, end_in_src_segment_y);
      //      g_debug(" -> (%" PRId64 ",%" PRId64 "), w: %" PRId32 ", h: %" PRId32,
      //	      dest_x, dest_y, dest_w, dest_h);

      struct one_jpeg *jpeg = l->layer_jpegs[file_y * l->jpegs_across + file_x];
      uint32_t *cur_dest = dest + (dest_y * w + dest_x);

      //      g_debug(" jpeg w: %d, jpeg h: %d", jpeg->width, jpeg->height);

      read_from_one_jpeg(jpeg, cur_dest,
			 start_in_src_segment_x, start_in_src_segment_y,
			 scale_denom, dest_w, dest_h, w);

      // advance dest by amount already copied
      dest_x += dest_w;
      src_x = end_in_src_segment_x + origin_src_segment_x;
    }
    dest_y += dest_h;
    src_y = end_in_src_segment_y + origin_src_segment_y;
  }
}


static void destroy(openslide_t *osr) {
  struct jpegops_data *data = osr->data;

  // each jpeg in turn
  for (int32_t i = 0; i < data->jpeg_count; i++) {
    struct one_jpeg *jpeg = data->all_jpegs + i;

    fclose(jpeg->f);
    g_free(jpeg->mcu_starts);
    g_free(jpeg->comment);
  }

  // each layer in turn
  for (int32_t i = 0; i < osr->layer_count; i++) {
    struct layer *l = data->layers + i;

    //    g_debug("g_free(%p)", (void *) l->layer_jpegs);
    g_free(l->layer_jpegs);
  }

  // the JPEG array
  g_free(data->all_jpegs);

  // the layer array
  g_free(data->layers);

  // the cache
  _openslide_cache_destroy(data->cache);

  // the structure
  g_slice_free(struct jpegops_data, data);
}

static void get_dimensions(openslide_t *osr, int32_t layer,
			   int64_t *w, int64_t *h) {
  struct jpegops_data *data = osr->data;

  // check bounds
  if (layer >= osr->layer_count) {
    *w = 0;
    *h = 0;
    return;
  }

  struct layer *l = data->layers + layer;
  *w = l->pixel_w / l->scale_denom;
  *h = l->pixel_h / l->scale_denom;

  //  g_debug("dimensions of layer %" PRId32 ": (%" PRId32 ",%" PRId32 ")", layer, *w, *h);
}

static const char* get_comment(openslide_t *osr) {
  struct jpegops_data *data = osr->data;
  return data->all_jpegs[0].comment;
}

static struct _openslide_ops jpeg_ops = {
  .read_region = read_region,
  .destroy = destroy,
  .get_dimensions = get_dimensions,
  .get_comment = get_comment,
};


static void compute_optimization(FILE *f,
				 int64_t *mcu_starts_count,
				 int64_t **mcu_starts) {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  // generate the optimization list, by finding restart markers
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  rewind(f);
  _openslide_jpeg_fancy_src(&cinfo, f, NULL, 0, 0, 0, 0);

  jpeg_read_header(&cinfo, TRUE);
  jpeg_start_decompress(&cinfo);

  int64_t MCUs = cinfo.MCUs_per_row * cinfo.MCU_rows_in_scan;
  *mcu_starts_count = MCUs / cinfo.restart_interval;
  *mcu_starts = g_new0(int64_t, *mcu_starts_count);

  // the first entry
  (*mcu_starts)[0] = _openslide_jpeg_fancy_src_get_filepos(&cinfo);

  // now find the rest of the MCUs
  bool last_was_ff = false;
  int64_t marker = 0;
  while (marker < *mcu_starts_count) {
    if (cinfo.src->bytes_in_buffer == 0) {
      (cinfo.src->fill_input_buffer)(&cinfo);
    }
    uint8_t b = *(cinfo.src->next_input_byte++);
    cinfo.src->bytes_in_buffer--;

    if (last_was_ff) {
      // EOI?
      if (b == JPEG_EOI) {
	// we're done
	break;
      } else if (b >= 0xD0 && b < 0xD8) {
	// marker
	(*mcu_starts)[1 + marker++] = _openslide_jpeg_fancy_src_get_filepos(&cinfo);
      }
    }
    last_was_ff = b == 0xFF;
  }

  /*
  for (int64_t i = 0; i < *mcu_starts_count; i++) {
    g_debug(" %lld", (*mcu_starts)[i]);
  }
  */

  // success, now clean up
  jpeg_destroy_decompress(&cinfo);
}

static void init_one_jpeg(struct one_jpeg *onej,
			  struct _openslide_jpeg_fragment *fragment) {
  FILE *f = onej->f = fragment->f;
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  // optimization
  compute_optimization(fragment->f, &onej->mcu_starts_count, &onej->mcu_starts);

  // init jpeg
  rewind(f);

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  _openslide_jpeg_fancy_src(&cinfo, f, NULL, 0, 0, 0, 0);

  // extract comment
  jpeg_save_markers(&cinfo, JPEG_COM, 0xFFFF);
  jpeg_read_header(&cinfo, FALSE);
  if (cinfo.marker_list) {
    // copy everything out
    char *com = g_strndup((const gchar *) cinfo.marker_list->data,
			  cinfo.marker_list->data_length);
    // but only really save everything up to the first '\0'
    onej->comment = g_strdup(com);
    g_free(com);
  }
  jpeg_save_markers(&cinfo, JPEG_COM, 0);  // stop saving

  // save dimensions
  jpeg_calc_output_dimensions(&cinfo);
  onej->width = cinfo.output_width;
  onej->height = cinfo.output_height;

  //  g_debug(" w: %d, h: %d", cinfo.output_width, cinfo.output_height);

  // save "tile" dimensions
  jpeg_start_decompress(&cinfo);
  onej->tile_width = onej->width /
    (cinfo.MCUs_per_row / cinfo.restart_interval);
  onej->tile_height = onej->height / cinfo.MCU_rows_in_scan;

  //  g_debug("jpeg \"tile\" dimensions: %dx%d", onej->tile_width, onej->tile_height);

  // destroy jpeg
  jpeg_destroy_decompress(&cinfo);
}

static gint width_compare(gconstpointer a, gconstpointer b) {
  int64_t w1 = *((const int64_t *) a);
  int64_t w2 = *((const int64_t *) b);

  g_assert(w1 >= 0 && w2 >= 0);

  return (w1 < w2) - (w1 > w2);
}

static void get_keys(gpointer key, gpointer value,
		     gpointer user_data) {
  GList *keys = *((GList **) user_data);
  keys = g_list_prepend(keys, key);
  *((GList **) user_data) = keys;
}

void _openslide_add_jpeg_ops(openslide_t *osr,
			     int32_t count,
			     struct _openslide_jpeg_fragment **fragments) {
  //  g_debug("count: %d", count);
  //  for (int32_t i = 0; i < count; i++) {
    //    struct _openslide_jpeg_fragment *frag = fragments[i];
    //    g_debug("%d: file: %p, x: %d, y: %d, z: %d",
    //	    i, (void *) frag->f, frag->x, frag->y, frag->z);
  //  }

  if (osr == NULL) {
    // free now and return
    for (int32_t i = 0; i < count; i++) {
      fclose(fragments[i]->f);
      g_slice_free(struct _openslide_jpeg_fragment, fragments[i]);
    }
    g_free(fragments);
    return;
  }

  g_assert(osr->data == NULL);


  // allocate private data
  struct jpegops_data *data = g_slice_new0(struct jpegops_data);
  osr->data = data;

  // load all jpegs (assume all are useful)
  data->jpeg_count = count;
  data->all_jpegs = g_new0(struct one_jpeg, count);
  for (int32_t i = 0; i < data->jpeg_count; i++) {
    g_debug("init JPEG %d", i);
    init_one_jpeg(&data->all_jpegs[i], fragments[i]);
  }

  // create map from width to layers, using the fragments
  GHashTable *width_to_layer_map = create_width_to_layer_map(count,
							     fragments,
							     data->all_jpegs);

  //  g_hash_table_foreach(width_to_layer_map, print_wlmap_entry, NULL);

  // delete all the fragments
  for (int32_t i = 0; i < count; i++) {
    g_slice_free(struct _openslide_jpeg_fragment, fragments[i]);
  }
  g_free(fragments);

  // get sorted keys
  GList *layer_keys = NULL;
  g_hash_table_foreach(width_to_layer_map, get_keys, &layer_keys);
  layer_keys = g_list_sort(layer_keys, width_compare);

  //  g_debug("number of keys: %d", g_list_length(layer_keys));


  // populate the layer_count
  osr->layer_count = g_hash_table_size(width_to_layer_map);

  // load into data array
  data->layers = g_new(struct layer, g_hash_table_size(width_to_layer_map));
  GList *tmp_list = layer_keys;

  int i = 0;

  //  g_debug("copying sorted layers");
  while(tmp_list != NULL) {
    // get a key and value
    struct layer *l = g_hash_table_lookup(width_to_layer_map, tmp_list->data);

    //    print_wlmap_entry(tmp_list->data, l, NULL);

    // copy
    struct layer *dest = data->layers + i;
    *dest = *l;    // shallow copy

    // manually free some things, because of that shallow copy
    g_hash_table_steal(width_to_layer_map, tmp_list->data);
    int64_free(tmp_list->data);  // key
    g_slice_free(struct layer, l); // shallow deletion of layer

    // consume the head and continue
    tmp_list = g_list_delete_link(tmp_list, tmp_list);
    i++;
  }

  // init cache
  data->cache = _openslide_cache_create(1024*1024*16);

  // unref the hash table
  g_hash_table_unref(width_to_layer_map);

  // set ops
  osr->ops = &jpeg_ops;
}



/*
 * Source manager for doing fancy things with libjpeg and restart markers,
 * initially copied from jdatasrc.c from IJG libjpeg.
 */
struct my_src_mgr {
  struct jpeg_source_mgr pub;   /* public fields */

  FILE *infile;                 /* source stream */
  JOCTET *buffer;               /* start of buffer */
  bool start_of_file;
  uint8_t next_restart_marker;

  int64_t next_start_offset;
  int64_t next_start_position;
  int64_t stop_position;

  int64_t header_length;
  int64_t *start_positions;
  int64_t start_positions_count;
  int64_t topleft;
  int32_t width;
  int32_t stride;
};

#define INPUT_BUF_SIZE  4096    /* choose an efficiently fread'able size */

static void compute_next_positions (struct my_src_mgr *src) {
  if (src->start_positions_count == 0) {
    // no positions given, do the whole file
    src->next_start_position = 0;
    src->stop_position = INT64_MAX;

    //    g_debug("next start offset: %lld", src->next_start_offset);
    //    g_debug("(count==0) next start: %lld, stop: %lld", src->next_start_position, src->stop_position);

    return;
  }

  // do special case for header
  if (src->start_of_file) {
    src->next_start_offset = src->topleft - src->stride;  // next time, start at topleft
    src->stop_position = src->start_positions[0];         // stop at data start
    g_assert(src->next_start_offset < (int64_t) src->start_positions_count);
    src->next_start_position = 0;

    //    g_debug("next start offset: %lld", src->next_start_offset);
    //    g_debug("(start_of_file) next start: %lld, stop: %lld", src->next_start_position, src->stop_position);

    return;
  }

  // advance
  src->next_start_offset += src->stride;

  // compute next jump point
  g_assert(src->next_start_offset >= 0
	   && src->next_start_offset < (int64_t) src->start_positions_count);
  src->next_start_position = src->start_positions[src->next_start_offset];

  // compute stop point, or end of file
  int64_t stop_offset = src->next_start_offset + src->width;
  if (stop_offset < src->start_positions_count) {
    src->stop_position = src->start_positions[stop_offset];
  } else {
    src->stop_position = INT64_MAX;
  }

  //  g_debug("next start offset: %lld", src->next_start_offset);
  //  g_debug("next start: %lld, stop: %lld", src->next_start_position, src->stop_position);
}

static void init_source (j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;
  src->start_of_file = true;
  src->next_restart_marker = 0;
  compute_next_positions(src);
}

static boolean fill_input_buffer (j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;
  size_t nbytes;

  off_t pos = ftello(src->infile);

  boolean rewrite_markers = true;
  if (src->start_positions_count == 0 || pos < src->start_positions[0]) {
    rewrite_markers = false; // we are in the header, or we don't know where it is
  }

  g_assert(pos <= src->stop_position);

  size_t bytes_to_read = INPUT_BUF_SIZE;
  if (pos < src->stop_position) {
    // don't read past
    bytes_to_read = MIN((int64_t) (src->stop_position - pos), bytes_to_read);
  } else if (pos == src->stop_position) {
    // skip to the jump point
    compute_next_positions(src);
    //    g_debug("at %lld, jump to %lld, will stop again at %lld", pos, src->next_start_position, src->stop_position);

    fseeko(src->infile, src->next_start_position, SEEK_SET);

    // figure out new stop position
    bytes_to_read = MIN((int64_t) (src->stop_position - src->next_start_position),
			bytes_to_read);
  }

  //  g_debug(" bytes_to_read: %d", bytes_to_read);

  nbytes = fread(src->buffer, 1, bytes_to_read, src->infile);

  if (nbytes <= 0) {
    if (src->start_of_file) {
      ERREXIT(cinfo, JERR_INPUT_EMPTY);
    }
    WARNMS(cinfo, JWRN_JPEG_EOF);

    /* Insert a fake EOI marker */
    src->buffer[0] = (JOCTET) 0xFF;
    src->buffer[1] = (JOCTET) JPEG_EOI;
    nbytes = 2;
  } else if (rewrite_markers) {
    // rewrite the restart markers if we know for sure we are not in the header
    bool last_was_ff = false;

    for (size_t i = 0; i < nbytes; i++) {
      uint8_t b = src->buffer[i];
      if (last_was_ff && b >= 0xD0 && b < 0xD8) {
	src->buffer[i] = 0xD0 | src->next_restart_marker;
	//	g_debug("rewrite %x -> %x", b, src->buffer[i]);
	src->next_restart_marker = (src->next_restart_marker + 1) % 8;
      }
      last_was_ff = b == 0xFF;
    }

    // don't end on ff, unless it is the very last byte
    if (last_was_ff && nbytes > 1) {
      nbytes--;
      fseek(src->infile, -1, SEEK_CUR);
    }
  }

  src->pub.next_input_byte = src->buffer;
  src->pub.bytes_in_buffer = nbytes;
  src->start_of_file = false;

  return TRUE;
}


static void skip_input_data (j_decompress_ptr cinfo, long num_bytes) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;

  /* Just a dumb implementation for now.  Could use fseek() except
   * it doesn't work on pipes.  Not clear that being smart is worth
   * any trouble anyway --- large skips are infrequent.
   */
  if (num_bytes > 0) {
    while (num_bytes > (long) src->pub.bytes_in_buffer) {
      num_bytes -= (long) src->pub.bytes_in_buffer;
      (void) fill_input_buffer(cinfo);
      /* note we assume that fill_input_buffer will never return FALSE,
       * so suspension need not be handled.
       */
    }
    src->pub.next_input_byte += (size_t) num_bytes;
    src->pub.bytes_in_buffer -= (size_t) num_bytes;
  }
}


static void term_source (j_decompress_ptr cinfo) {
  /* no work necessary here */
}

int64_t _openslide_jpeg_fancy_src_get_filepos(j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;

  return ftello(src->infile) - src->pub.bytes_in_buffer;
}

void _openslide_jpeg_fancy_src (j_decompress_ptr cinfo, FILE *infile,
				int64_t *start_positions,
				int64_t start_positions_count,
				int64_t topleft,
				int32_t width, int32_t stride) {
  struct my_src_mgr *src;

  if (cinfo->src == NULL) {     /* first time for this JPEG object? */
    cinfo->src = (struct jpeg_source_mgr *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  sizeof(struct my_src_mgr));
    src = (struct my_src_mgr *) cinfo->src;
    src->buffer = (JOCTET *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  INPUT_BUF_SIZE * sizeof(JOCTET));
    //    g_debug("init fancy src with %p", src);
  }

  //  g_debug("fancy: start_positions_count: %llu, topleft: %llu, width: %d, stride: %d",
  //	 start_positions_count, topleft, width, stride);

  src = (struct my_src_mgr *) cinfo->src;
  src->pub.init_source = init_source;
  src->pub.fill_input_buffer = fill_input_buffer;
  src->pub.skip_input_data = skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src->pub.term_source = term_source;
  src->infile = infile;
  src->start_positions = start_positions;
  src->start_positions_count = start_positions_count;
  src->topleft = topleft;
  src->width = width;
  src->stride = stride;
  src->pub.bytes_in_buffer = 0; /* forces fill_input_buffer on first read */
  src->pub.next_input_byte = NULL; /* until buffer loaded */
}