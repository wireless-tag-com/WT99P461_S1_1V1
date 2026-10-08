/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void                *sub_src;   /*!< Sub source to keep media source extra data */
} media_src_t;

int media_src_storage_open(media_src_t *src);
int media_src_storage_connect(media_src_t *src, const char *uri);
int media_src_storage_disconnect(media_src_t *src);
int media_src_storage_read(media_src_t *src, void *data, size_t len);
/* 查询锁存的存储读错误，区别于正常文件结束。 */
bool media_src_storage_read_failed(media_src_t *src);
int media_src_storage_seek(media_src_t *src, uint64_t position);
int media_src_storage_get_position(media_src_t *src, uint64_t *position);
int media_src_storage_get_size(media_src_t *src, uint64_t *size);
int media_src_storage_close(media_src_t *src);

/** Reset / query SD read timing stats (for decode-path profiling). */
void media_src_storage_stats_reset(void);
void media_src_storage_stats_get(uint64_t *bytes, int64_t *us);

#ifdef __cplusplus
}
#endif
