/*
 * AVI files with ZMBV video and PCM audio, for the recorder
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_AVI_H
#define UI_AVI_H

typedef struct AviFile AviFile;

/*
 * Create @path, which must not exist yet, for ZMBV video of @width x
 * @height pixels at @fps frames a second and, if @audio_rate is not 0,
 * 16-bit stereo PCM audio at that rate.
 */
AviFile *avi_create(const char *path, int width, int height, int fps,
                    int audio_rate, Error **errp);

/* Add a frame, as zmbv_encode() made it; @key for a key frame */
bool avi_add_video(AviFile *a, const void *data, size_t len, bool key);

/* Add @frames frames of interleaved 16-bit stereo samples */
bool avi_add_audio(AviFile *a, const int16_t *samples, size_t frames);

/*
 * Update the headers so that the file plays as it is, should QEMU die
 * before avi_close().  Players then read it without its index.
 */
bool avi_sync(AviFile *a);

/* Write the index, finish the headers and close; frees @a */
bool avi_close(AviFile *a, Error **errp);

/* Bytes written so far */
uint64_t avi_bytes(AviFile *a);

/* Whether a write failed, and why */
const char *avi_error(AviFile *a);

/*
 * ZMBV, the lossless screen codec of DOSBox: zlib over the XOR of each
 * 16x16 block with the frame before.  ffmpeg, VLC, mpv and most video
 * editors play it, and it needs nothing but zlib.  Pixels are 32 bits,
 * blue, green, red and a byte that does not count, in this order in
 * memory: x8r8g8b8 on a little-endian host.
 */
#define ZMBV_BLOCK      16

typedef struct ZmbvEncoder ZmbvEncoder;

ZmbvEncoder *zmbv_encoder_new(int width, int height, int level);
void zmbv_encoder_free(ZmbvEncoder *z);

/*
 * Compress one frame into @out, replacing what it held.  For a key
 * frame, @raw is the whole frame, width * height pixels.  Otherwise
 * it is a delta from zmbv_delta(), or NULL for a frame that is the
 * same as the one before.
 */
bool zmbv_encode(ZmbvEncoder *z, bool key, const void *raw, size_t len,
                 GByteArray *out);

/*
 * The ZMBV delta from @prev to @cur, both @width x @height pixels,
 * @stride pixels apart, looking only at the blocks that meet the
 * rectangle @x, @y, @w, @h.  It appends the delta to @out and copies
 * the blocks that changed from @cur to @prev.  Returns how many pixels
 * changed in the bits of @mask, and their bounding box in @box (x, y,
 * w, h) if any did.
 */
int zmbv_delta(int width, int height, int stride, uint32_t *prev,
               const uint32_t *cur, int x, int y, int w, int h,
               uint32_t mask, GByteArray *out, int box[4]);

#endif
