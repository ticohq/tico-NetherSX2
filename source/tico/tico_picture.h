/* A small picture of a frame, kept beside a save state the core wrote without
 * one (its resume state, written while the game shuts down). */
#ifndef NETHERSX2_TICO_PICTURE_H
#define NETHERSX2_TICO_PICTURE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Set before the menu lets go of the game for Exit or Restart: the presenting
 * thread then writes the frozen frame to tico_picture_path. */
extern char tico_picture_path[1024];
extern volatile int tico_picture_requested;

/* Writes width x height pixels (rows of stride bytes, RGBA, or BGRA when bgra
 * is set; bottom row first when flip is set) scaled down to tico_picture_path,
 * then clears the request. */
void tico_picture_write(const unsigned char *pixels, int width, int height, int stride,
                        int bgra, int flip);

/* The picture at path as RGBA (free() it), or NULL. */
unsigned char *tico_picture_read(const char *path, int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif
