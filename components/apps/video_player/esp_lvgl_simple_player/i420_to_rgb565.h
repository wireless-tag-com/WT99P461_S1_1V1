/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert planar I420 (Y then U then V) to RGB565 little-endian.
 * width/height must be even. dst size = width * height * 2.
 */
void i420_to_rgb565_le(const uint8_t *i420, uint16_t width, uint16_t height, uint8_t *rgb565);

#ifdef __cplusplus
}
#endif
