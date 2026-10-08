/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "i420_to_rgb565.h"

#define CLAMP(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

static inline void put_rgb565_le(uint8_t r, uint8_t g, uint8_t b, uint8_t *dst)
{
    dst[1] = (uint8_t)((r & 0xf8) | (g >> 5));
    dst[0] = (uint8_t)(((g & 0x1c) << 3) | (b >> 3));
}

void i420_to_rgb565_le(const uint8_t *i420, uint16_t width, uint16_t height, uint8_t *rgb565)
{
    const uint8_t *y_plane = i420;
    const uint8_t *u_plane = i420 + (size_t)width * height;
    const uint8_t *v_plane = u_plane + ((size_t)width * height / 4);

    for (uint16_t j = 0; j < height; j++) {
        for (uint16_t i = 0; i < width; i++) {
            int y = y_plane[j * width + i];
            int u = u_plane[(j / 2) * (width / 2) + (i / 2)];
            int v = v_plane[(j / 2) * (width / 2) + (i / 2)];

            int c = y - 16;
            int d = u - 128;
            int e = v - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            put_rgb565_le((uint8_t)CLAMP(r), (uint8_t)CLAMP(g), (uint8_t)CLAMP(b),
                          rgb565 + ((size_t)j * width + i) * 2);
        }
    }
}
