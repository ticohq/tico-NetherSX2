#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tico/tico_entry.h"
#include "tico/tico_picture.h"

/* "TNP1", width, height (little endian u32), then RGBA rows top first */
#define PICTURE_MAGIC "TNP1"
#define PICTURE_WIDTH 384

char tico_picture_path[1024];
volatile int tico_picture_requested;

void tico_picture_write(const unsigned char *pixels, int width, int height, int stride,
                        int bgra, int flip) {
  __atomic_store_n(&tico_picture_requested, 0, __ATOMIC_RELEASE);
  if (!pixels || width <= 0 || height <= 0 || !tico_picture_path[0]) return;
  const int out_width = width < PICTURE_WIDTH ? width : PICTURE_WIDTH;
  const int out_height = (int)((int64_t)height * out_width / width);
  if (out_height <= 0) return;
  unsigned char *out = malloc((size_t)out_width * out_height * 4);
  if (!out) return;
  /* nearest pixel: the picture is a small preview */
  for (int y = 0; y < out_height; y++) {
    int source_y = (int)((int64_t)y * height / out_height);
    if (flip) source_y = height - 1 - source_y;
    const unsigned char *row = pixels + (size_t)source_y * stride;
    unsigned char *target = out + (size_t)y * out_width * 4;
    for (int x = 0; x < out_width; x++) {
      const unsigned char *pixel = row + (size_t)((int64_t)x * width / out_width) * 4;
      target[x * 4 + 0] = bgra ? pixel[2] : pixel[0];
      target[x * 4 + 1] = pixel[1];
      target[x * 4 + 2] = bgra ? pixel[0] : pixel[2];
      target[x * 4 + 3] = 255;
    }
  }
  FILE *file = fopen(tico_picture_path, "wb");
  if (file) {
    const uint32_t size[2] = {(uint32_t)out_width, (uint32_t)out_height};
    fwrite(PICTURE_MAGIC, 1, 4, file);
    fwrite(size, sizeof(size), 1, file);
    fwrite(out, 4, (size_t)out_width * out_height, file);
    fclose(file);
    tico_log("picture: %dx%d written to %s\n", out_width, out_height, tico_picture_path);
  }
  free(out);
}

unsigned char *tico_picture_read(const char *path, int *width, int *height) {
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  char magic[4];
  uint32_t size[2];
  unsigned char *pixels = NULL;
  if (fread(magic, 1, 4, file) == 4 && !memcmp(magic, PICTURE_MAGIC, 4) &&
      fread(size, sizeof(size), 1, file) == 1 && size[0] && size[1] && size[0] <= 4096 &&
      size[1] <= 4096) {
    pixels = malloc((size_t)size[0] * size[1] * 4);
    if (pixels && fread(pixels, 4, (size_t)size[0] * size[1], file) ==
                      (size_t)size[0] * size[1]) {
      *width = (int)size[0];
      *height = (int)size[1];
    } else {
      free(pixels);
      pixels = NULL;
    }
  }
  fclose(file);
  return pixels;
}
