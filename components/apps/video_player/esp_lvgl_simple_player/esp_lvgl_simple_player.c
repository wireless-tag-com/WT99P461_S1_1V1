/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_private/esp_cache_private.h"
#include "esp_dma_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include "driver/jpeg_decode.h"
#include "media_src_storage.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "esp_lvgl_simple_player.h"
#include "i420_to_rgb565.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_alloc.h"

#define CACHE_BUF_ALIGN         (1024)
/* 硬件 JPEG + 屏显合理上限；超出说明文件非原始 MJPEG 或头解析错误。 */
#define PLAYER_MAX_FRAME_WIDTH  (1920)
#define PLAYER_MAX_FRAME_HEIGHT (1080)
/* 软解性能友好上限；更大仍可尝试，但可能卡顿。 */
#define PLAYER_H264_SOFT_MAX_W  (800)
#define PLAYER_H264_SOFT_MAX_H  (600)
#define PLAYER_H264_STREAM_CAP  (256 * 1024)
#define PLAYER_FPS_LOG_INTERVAL_US (2000000LL)

#define ALIGN_UP(num, align)      (((num) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(num, align)    ((num) & ~((align) - 1))

static const char *TAG = "esp_lvgl_player";

typedef struct {
    int64_t last_us;
    uint32_t frames;
    int64_t sum_read_us;
    int64_t sum_decode_us;
    int64_t sum_ui_us;
    int64_t sum_total_us;
    uint64_t sum_jpeg_bytes;
} player_fps_t;

static void player_fps_tick(player_fps_t *fps, const char *codec,
                            int64_t read_us, int64_t decode_us, int64_t ui_us,
                            int64_t total_us, uint32_t jpeg_bytes)
{
    fps->frames++;
    fps->sum_read_us += read_us;
    fps->sum_decode_us += decode_us;
    fps->sum_ui_us += ui_us;
    fps->sum_total_us += total_us;
    fps->sum_jpeg_bytes += jpeg_bytes;

    ESP_LOGI(TAG,
             "%s frame#%" PRIu32 ": jpeg=%" PRIu32 "B read=%" PRId64 "ms decode=%" PRId64
             "ms ui=%" PRId64 "ms total=%" PRId64 "ms",
             codec, fps->frames, jpeg_bytes,
             read_us / 1000, decode_us / 1000, ui_us / 1000, total_us / 1000);

    const int64_t now = esp_timer_get_time();
    if (fps->last_us == 0) {
        fps->last_us = now;
        return;
    }
    const int64_t elapsed = now - fps->last_us;
    if (elapsed < PLAYER_FPS_LOG_INTERVAL_US) {
        return;
    }
    const float sec = (float)elapsed / 1000000.0f;
    const uint32_t n = fps->frames;
    ESP_LOGI(TAG,
             "%s FPS: %.2f | avg ms: read=%.1f decode=%.1f ui=%.1f total=%.1f | avg jpeg=%.1fKB",
             codec, (float)n / sec,
             n ? (fps->sum_read_us / 1000.0f) / n : 0,
             n ? (fps->sum_decode_us / 1000.0f) / n : 0,
             n ? (fps->sum_ui_us / 1000.0f) / n : 0,
             n ? (fps->sum_total_us / 1000.0f) / n : 0,
             n ? (fps->sum_jpeg_bytes / 1024.0f) / n : 0);
    fps->frames = 0;
    fps->sum_read_us = 0;
    fps->sum_decode_us = 0;
    fps->sum_ui_us = 0;
    fps->sum_total_us = 0;
    fps->sum_jpeg_bytes = 0;
    fps->last_us = now;
}

/* 小端内存中的 JPEG 标记：FFD8 / FFD9 */
static const uint16_t SOI = 0xd8ff;
static const uint16_t EOI = 0xd9ff;
static BaseType_t player_task_handle = NULL;

typedef struct
{
    bool is_init;
    const char              *video_path;
    const char              *bgm_path;
    media_src_t             file;
    uint64_t                filesize;
    jpeg_decoder_handle_t   jpeg;

    uint32_t    screen_width;   /* Width of the video player object */
    uint32_t    screen_height;  /* Height of the video player object */
    uint32_t    video_width;      /* Maximum width of the video */
    uint32_t    video_height;     /* Maximum height of the video  */

    player_state_t  state;
    bool            loop;
    bool            hide_controls;
    bool            hide_slider;
    bool            hide_status;
    bool            auto_width;
    bool            auto_height;

    /* Buffers */
    uint8_t     *in_buff;
    uint32_t    in_buff_size;
    /* 双缓冲：解码写 back，锁内再绑到 canvas，避免与 LVGL 读同一帧撕裂闪屏。 */
    uint8_t     *out_buff[2];
    uint32_t    out_buff_size;
    uint8_t     out_front; /* 当前已绑定到 canvas 的下标 */
    uint16_t    out_w;
    uint16_t    out_h;
    uint8_t     *cache_buff;
    uint32_t    cache_buff_size;
    bool        cache_buff_in_psram;
    uint32_t    cache_file_start; /* 缓存对应的文件起点。 */
    uint32_t    cache_valid; /* 本轮缓存的有效字节数，跨帧复用。 */

    /* LVGL objects */
    lv_obj_t    *main;
    lv_obj_t    *canvas;
    lv_obj_t    *slider;
    lv_obj_t    *btn_play;
    lv_obj_t    *btn_pause;
    lv_obj_t    *btn_stop;
    lv_obj_t    *btn_repeat;
    lv_obj_t    *img_pause;
    lv_obj_t    *img_stop;
    lv_obj_t    *controls;
} player_ctx_t;

static player_ctx_t player_ctx;


static const jpeg_decode_cfg_t jpeg_decode_cfg = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    /* 本板 LVGL/面板路径与 JPEG 硬解默认 RGB 元件序相反，用 BGR 对齐显示。 */
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};



static void play_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        esp_lvgl_simple_player_play();
    }
}

static void stop_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        esp_lvgl_simple_player_stop();

        bsp_display_unlock();
        if (esp_lvgl_simple_player_wait_task_stop(100) != ESP_OK) {
            ESP_LOGE(TAG, "Player task stop timeout");
        }
        bsp_display_lock(100);
    }
}

static void pause_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        if(player_ctx.state == PLAYER_STATE_PAUSED) {
            esp_lvgl_simple_player_play();
        } else if (player_ctx.state == PLAYER_STATE_PLAYING) {
            esp_lvgl_simple_player_pause();
        }
    }
}

static void repeat_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        bool loop = lv_obj_get_state(obj) & LV_STATE_CHECKED ? true : false;
        esp_lvgl_simple_player_repeat(loop);
    }
}

static lv_obj_t * create_lvgl_objects(lv_obj_t * screen)
{
    bsp_display_lock(0);

    /* Rows */
    lv_obj_t *cont_col = lv_obj_create(screen);
    lv_obj_set_size(cont_col, player_ctx.screen_width, player_ctx.screen_height);
    lv_obj_set_flex_flow(cont_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont_col, 0, 0);
    lv_obj_set_flex_align(cont_col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(cont_col, lv_color_black(), 0);
    lv_obj_clear_flag(cont_col, LV_OBJ_FLAG_SCROLLABLE);
    player_ctx.main = cont_col;

    /* Video canvas */
    player_ctx.canvas = lv_canvas_create(cont_col);
    lv_obj_add_event_cb(player_ctx.canvas, pause_event_cb, LV_EVENT_CLICKED, NULL);

    /*Create a slider in the center of the display*/
    lv_obj_t * slider = lv_slider_create(cont_col);
    lv_obj_set_size(slider, player_ctx.screen_width, 5);
    lv_obj_add_state(slider, LV_STATE_DISABLED);
    lv_obj_set_style_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_CLICKABLE);
    player_ctx.slider = slider;

    /* Buttons */
    lv_obj_t *cont_row = lv_obj_create(cont_col);
    lv_obj_set_size(cont_row, player_ctx.screen_width - 20, 80);
    lv_obj_set_flex_flow(cont_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_top(cont_row, 2, 0);
    lv_obj_set_style_pad_bottom(cont_row, 2, 0);
    lv_obj_set_flex_align(cont_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(cont_row, lv_color_black(), 0);
    lv_obj_set_style_border_width(cont_row, 0, 0);
    player_ctx.controls = cont_row;

    /* Play button */
    lv_obj_t * play_btn = lv_btn_create(cont_row);
    lv_obj_t * label = lv_label_create(play_btn);
    lv_label_set_text_static(label, LV_SYMBOL_PLAY);
    lv_obj_add_event_cb(play_btn, play_event_cb, LV_EVENT_CLICKED, NULL);
    player_ctx.btn_play = play_btn;

    /* Pause button */
    lv_obj_t * pause_btn = lv_btn_create(cont_row);
    label = lv_label_create(pause_btn);
    lv_label_set_text_static(label, LV_SYMBOL_PAUSE);
    lv_obj_add_event_cb(pause_btn, pause_event_cb, LV_EVENT_CLICKED, NULL);
    player_ctx.btn_pause = pause_btn;

    /* Stop button */
    lv_obj_t * stop_btn = lv_btn_create(cont_row);
    label = lv_label_create(stop_btn);
    lv_label_set_text_static(label, LV_SYMBOL_STOP);
    lv_obj_add_event_cb(stop_btn, stop_event_cb, LV_EVENT_CLICKED, NULL);
    player_ctx.btn_stop = stop_btn;

    /* Repeat button */
    lv_obj_t * repeat_btn = lv_btn_create(cont_row);
    lv_obj_add_flag(repeat_btn, LV_OBJ_FLAG_CHECKABLE);
    label = lv_label_create(repeat_btn);
    lv_label_set_text_static(label, LV_SYMBOL_REFRESH);
    lv_obj_add_event_cb(repeat_btn, repeat_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    player_ctx.btn_repeat = repeat_btn;

    /* Pause image */
    lv_obj_t * img_pause = lv_label_create(player_ctx.canvas);
    lv_obj_set_style_text_font(img_pause, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(img_pause, lv_color_white(), 0);
    lv_label_set_text_static(img_pause, LV_SYMBOL_PAUSE);
    lv_obj_center(img_pause);
    lv_obj_add_flag(img_pause, LV_OBJ_FLAG_HIDDEN);
    player_ctx.img_pause = img_pause;

    /* Stop image */
    lv_obj_t * img_stop = lv_label_create(player_ctx.canvas);
    lv_obj_set_style_text_font(img_stop, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(img_stop, lv_color_white(), 0);
    lv_label_set_text_static(img_stop, LV_SYMBOL_STOP);
    lv_obj_center(img_stop);
    lv_obj_add_flag(img_stop, LV_OBJ_FLAG_HIDDEN);
    player_ctx.img_stop = img_stop;

    /* Hide control buttons */
    if (player_ctx.hide_controls) {
        lv_obj_add_flag(cont_row, LV_OBJ_FLAG_HIDDEN);
    }
    /* Hide slider */
    if (player_ctx.hide_slider) {
        lv_obj_add_flag(slider, LV_OBJ_FLAG_HIDDEN);
    }
    /* Hide status icons */
    if (player_ctx.hide_status) {
        lv_obj_add_flag(img_pause, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(img_stop, LV_OBJ_FLAG_HIDDEN);
    }

    bsp_display_unlock();

    return cont_col;
}

static esp_err_t get_video_size(uint32_t * width, uint32_t * height)
{
    esp_err_t err;
    jpeg_decode_picture_info_t header = {0};
    assert(width && height);

    /* 从头扫描第一帧 JPEG（部分文件在 SOI 前有填充/容器残片）。 */
    ESP_RETURN_ON_FALSE(media_src_storage_seek(&player_ctx.file, 0) == 0, ESP_FAIL, TAG, "seek 0 failed");

    int size = media_src_storage_read(&player_ctx.file, player_ctx.cache_buff, player_ctx.cache_buff_size);
    if (size < 4) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *soi = memmem(player_ctx.cache_buff, size, &SOI, 2);
    if (soi == NULL) {
        ESP_LOGE(TAG, "No JPEG SOI (FFD8) in first %d bytes; need raw .mjpeg (concatenated JPEG), not AVI/MP4", size);
        return ESP_ERR_INVALID_ARG;
    }

    size_t jpeg_off = (size_t)(soi - player_ctx.cache_buff);
    size_t jpeg_len = (size_t)size - jpeg_off;
    err = jpeg_decoder_get_info(soi, jpeg_len, &header);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_decoder_get_info failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "header parsed, width=%" PRIu32 " height=%" PRIu32 " soi_off=%u read=%d",
             header.width, header.height, (unsigned)jpeg_off, size);
    if (header.width * header.height > 1024 * 600) {
        ESP_LOGW(TAG,
                 "Frame %" PRIu32 "x%" PRIu32 " is large for SPI-SD+LVGL; expect low FPS. Prefer <=1024x600",
                 header.width, header.height);
    }

    if (header.width == 0 || header.height == 0 ||
            header.width > PLAYER_MAX_FRAME_WIDTH || header.height > PLAYER_MAX_FRAME_HEIGHT) {
        ESP_LOGE(TAG,
                 "Invalid/unsupported frame size %" PRIu32 "x%" PRIu32
                 " (max %dx%d). Re-encode: ffmpeg -i in.mp4 -vf scale=1024:600 -q:v 5 -vcodec mjpeg out.mjpeg",
                 header.width, header.height, PLAYER_MAX_FRAME_WIDTH, PLAYER_MAX_FRAME_HEIGHT);
        return ESP_ERR_INVALID_SIZE;
    }

    *width = header.width;
    *height = header.height;
    return ESP_OK;
}

static esp_err_t video_decoder_init(void)
{
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = -1,
    };
    return jpeg_new_decoder_engine(&engine_cfg, &player_ctx.jpeg);
}

static void video_decoder_deinit(void)
{
    if (player_ctx.jpeg) {
        jpeg_del_decoder_engine(player_ctx.jpeg);
        player_ctx.jpeg = NULL; /* 清除已释放句柄，下一次提前失败不能重复释放。 */
    }
}

static uint8_t * video_decoder_malloc(uint32_t size, bool inbuff, uint32_t * outsize)
{
    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    return (uint8_t *)jpeg_alloc_decoder_mem(size, (inbuff ? &tx_mem_cfg : &rx_mem_cfg), (size_t*)outsize);
}

static uint8_t *player_out_back(void)
{
    return player_ctx.out_buff[1U - player_ctx.out_front];
}

static void player_out_free_pair(void)
{
    for (int i = 0; i < 2; ++i) {
        if (player_ctx.out_buff[i]) {
            heap_caps_free(player_ctx.out_buff[i]);
            player_ctx.out_buff[i] = NULL;
        }
    }
    player_ctx.out_buff_size = 0;
    player_ctx.out_front = 0;
    player_ctx.out_w = 0;
    player_ctx.out_h = 0;
}

/* JPEG 硬解输出缓冲：两块同等大小，均走 jpeg_alloc。 */
static esp_err_t player_out_alloc_pair_jpeg(uint32_t need)
{
    uint32_t sz0 = need;
    uint32_t sz1 = need;

    player_out_free_pair();
    player_ctx.out_buff[0] = video_decoder_malloc(need, false, &sz0);
    ESP_RETURN_ON_FALSE(player_ctx.out_buff[0], ESP_ERR_NO_MEM, TAG,
                        "Allocation out_buff[0] failed (need ~%" PRIu32 ")", need);
    player_ctx.out_buff_size = sz0;
    sz1 = sz0;
    player_ctx.out_buff[1] = video_decoder_malloc(sz0, false, &sz1);
    if (!player_ctx.out_buff[1] || sz1 < sz0) {
        ESP_LOGE(TAG, "Allocation out_buff[1] failed (need ~%" PRIu32 ")", sz0);
        player_out_free_pair();
        return ESP_ERR_NO_MEM;
    }
    player_ctx.out_front = 0;
    return ESP_OK;
}

/* H.264 RGB565：两块 PSRAM 对齐缓冲。 */
static esp_err_t player_out_alloc_pair_rgb565(uint32_t need)
{
    player_out_free_pair();
    player_ctx.out_buff_size = need;
    player_ctx.out_buff[0] = heap_caps_aligned_calloc(64, 1, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    player_ctx.out_buff[1] = heap_caps_aligned_calloc(64, 1, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!player_ctx.out_buff[0] || !player_ctx.out_buff[1]) {
        ESP_LOGE(TAG, "RGB565 out_buff pair alloc failed (need 2x%" PRIu32 ")", need);
        player_out_free_pair();
        return ESP_ERR_NO_MEM;
    }
    player_ctx.out_front = 0;
    return ESP_OK;
}

/* 调用方须已持有 display lock。把刚解完的 back 切到 canvas。 */
static void player_out_present_locked(void)
{
    const uint8_t back = (uint8_t)(1U - player_ctx.out_front);
    if (!player_ctx.canvas || !player_ctx.out_buff[back] || !player_ctx.out_w || !player_ctx.out_h) {
        return;
    }
    lv_canvas_set_buffer(player_ctx.canvas, player_ctx.out_buff[back],
                         player_ctx.out_w, player_ctx.out_h, LV_IMG_CF_TRUE_COLOR);
    player_ctx.out_front = back;
    lv_obj_invalidate(player_ctx.canvas);
}

static int video_decoder_read_jpeg_image(uint32_t *file_seek_start, uint32_t *file_seek_offset)
{
    (void)file_seek_offset;
    uint32_t jpeg_image_size = 0;
    bool found_soi = false;
    uint8_t *cache_buff = player_ctx.cache_buff;
    const uint32_t cache_buff_size = player_ctx.cache_buff_size;
    uint32_t abs_pos = *file_seek_start;

    while (1) {
        /* 复用上一帧预读的剩余数据；保留边界字节以识别跨块 JPEG 标记。 */
        uint32_t offset = abs_pos - player_ctx.cache_file_start;
        if (abs_pos < player_ctx.cache_file_start || offset >= player_ctx.cache_valid ||
            (player_ctx.cache_valid - offset < 2 && abs_pos + 1 < player_ctx.filesize)) {
            if (media_src_storage_seek(&player_ctx.file, abs_pos) != 0) return -1;
            int n = media_src_storage_read(&player_ctx.file, cache_buff, cache_buff_size);
            player_ctx.cache_valid = n > 0 ? (uint32_t)n : 0;
            player_ctx.cache_file_start = abs_pos;
            if (n < 0) return -1;
            if (n == 0) break;
            offset = 0;
        }
        uint8_t *chunk = cache_buff + offset;
        uint32_t chunk_len = player_ctx.cache_valid - offset;

        if (!found_soi) {
            uint8_t *soi = memmem(chunk, chunk_len, &SOI, 2);
            if (soi == NULL) {
                abs_pos += chunk_len > 1 ? chunk_len - 1 : chunk_len;
                continue;
            }
            found_soi = true;
            uint32_t skip = (uint32_t)(soi - chunk);
            abs_pos += skip;
            chunk = soi;
            chunk_len -= skip;
        }

        uint8_t *eoi = memmem(chunk, chunk_len, &EOI, 2);
        uint8_t *next_soi = NULL;
        if (chunk_len > 2) {
            next_soi = memmem(chunk + 2, chunk_len - 2, &SOI, 2);
        }

        uint32_t take = chunk_len;
        bool frame_done = false;
        if (eoi != NULL && (next_soi == NULL || eoi < next_soi)) {
            take = (uint32_t)(eoi + 2 - chunk);
            frame_done = true;
        } else if (next_soi != NULL) {
            take = (uint32_t)(next_soi - chunk);
            frame_done = true;
            ESP_LOGW(TAG, "JPEG frame missing EOI, cut at next SOI");
        }

        if (!frame_done && take > 1) --take; /* 留一个字节与下一块联合匹配标记。 */
        if (jpeg_image_size + take > player_ctx.in_buff_size) {
            ESP_LOGE(TAG, "JPEG frame > in_buff (%" PRIu32 ")", player_ctx.in_buff_size);
            *file_seek_start = abs_pos;
            return -1;
        }
        memcpy(player_ctx.in_buff + jpeg_image_size, chunk, take);
        jpeg_image_size += take;
        abs_pos += take;

        if (frame_done) {
            *file_seek_start = abs_pos;
            return (int)jpeg_image_size;
        }
    }

    *file_seek_start = abs_pos;
    if (!found_soi || jpeg_image_size == 0) {
        return 0;
    }
    if (jpeg_image_size >= 2 &&
            player_ctx.in_buff[jpeg_image_size - 2] == 0xff &&
            player_ctx.in_buff[jpeg_image_size - 1] == 0xd9) {
        return (int)jpeg_image_size;
    }
    ESP_LOGE(TAG, "JPEG EOI not found before EOF (got %" PRIu32 " bytes)", jpeg_image_size);
    return -1;
}

static int video_decoder_decode(uint32_t jpeg_image_size)
{
    esp_err_t err;
    uint32_t ret_size = 0;
    uint32_t jpeg_image_size_aligned = ALIGN_UP(jpeg_image_size, 16);

    if (jpeg_image_size_aligned > player_ctx.in_buff_size) {
        ESP_LOGE(TAG, "JPEG image size is bigger than input buffer size");
        return -1;
    }

    /* 对齐填充用 0xFF，避免 HW 把未定义尾部当成码流继续解析。 */
    if (jpeg_image_size_aligned > jpeg_image_size) {
        memset(player_ctx.in_buff + jpeg_image_size, 0xff,
               jpeg_image_size_aligned - jpeg_image_size);
    }

    ret_size = player_ctx.out_buff_size;
    uint8_t *dst = player_out_back();
    ESP_RETURN_ON_FALSE(dst, -1, TAG, "out_buff back is NULL");
    err = jpeg_decoder_process(player_ctx.jpeg, &jpeg_decode_cfg, player_ctx.in_buff, jpeg_image_size_aligned,
                               dst, player_ctx.out_buff_size, &ret_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed (frame=%" PRIu32 " need_out=%" PRIu32 " have_out=%" PRIu32 ")",
                 jpeg_image_size, ret_size, player_ctx.out_buff_size);
        return -1;
    }

    if (ret_size > player_ctx.out_buff_size) {
        ESP_LOGE(TAG, "Output buffer is too small");
        return -1;
    }

    return (int)jpeg_image_size;
}

static bool path_is_h264(const char *path)
{
    if (path == NULL) {
        return false;
    }
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return false;
    }
    return (strcasecmp(ext, ".h264") == 0) || (strcasecmp(ext, ".264") == 0);
}

static void player_ui_set_playing(uint32_t width, uint32_t height)
{
    bsp_display_lock(0);
    player_ctx.out_w = (uint16_t)width;
    player_ctx.out_h = (uint16_t)height;
    if (player_ctx.out_buff[player_ctx.out_front] && width && height) {
        lv_canvas_set_buffer(player_ctx.canvas, player_ctx.out_buff[player_ctx.out_front],
                             width, height, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(player_ctx.canvas, width, height);
        lv_obj_invalidate(player_ctx.canvas);
    }
    if (player_ctx.auto_width || player_ctx.auto_height) {
        uint32_t h = (player_ctx.auto_height ? (height + 120) : lv_obj_get_height(player_ctx.main));
        uint32_t w = (player_ctx.auto_width ? width : lv_obj_get_width(player_ctx.main));
        lv_obj_set_size(player_ctx.main, w, h);
    }
    lv_obj_clear_state(player_ctx.slider, LV_STATE_DISABLED);
    lv_obj_add_state(player_ctx.btn_play, LV_STATE_DISABLED);
    lv_obj_clear_state(player_ctx.btn_stop, LV_STATE_DISABLED);
    lv_obj_clear_state(player_ctx.btn_pause, LV_STATE_DISABLED);
    lv_obj_clear_state(player_ctx.btn_repeat, LV_STATE_DISABLED);
    lv_obj_add_flag(player_ctx.img_stop, LV_OBJ_FLAG_HIDDEN);
    lv_slider_set_range(player_ctx.slider, 0, 1000);
    bsp_display_unlock();
}

/**
 * Annex-B .h264 软解（tinyH264，需 constrained baseline，无 B 帧）。
 * 输出 I420 → RGB565 → LVGL canvas。
 */
static esp_err_t play_h264_stream(void)
{
    esp_err_t ret = ESP_OK;
    esp_h264_err_t hret;
    esp_h264_dec_handle_t dec = NULL;
    esp_h264_dec_param_sw_handle_t param = NULL;
    uint8_t *stream = NULL;
    size_t stream_len = 0;
    size_t stream_off = 0;
    size_t file_pos = 0;
    bool canvas_ready = false;
    uint16_t width = 0;
    uint16_t height = 0;
    int frames = 0;

    esp_h264_dec_cfg_sw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_I420,
    };

    stream = heap_caps_malloc(PLAYER_H264_STREAM_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(stream, ESP_ERR_NO_MEM, TAG, "h264 stream buffer alloc failed");

    hret = esp_h264_dec_sw_new(&cfg, &dec);
    ESP_GOTO_ON_FALSE(hret == ESP_H264_ERR_OK, ESP_FAIL, err, TAG, "esp_h264_dec_sw_new failed %d", hret);
    hret = esp_h264_dec_open(dec);
    ESP_GOTO_ON_FALSE(hret == ESP_H264_ERR_OK, ESP_FAIL, err, TAG, "esp_h264_dec_open failed %d", hret);
    hret = esp_h264_dec_sw_get_param_hd(dec, &param);
    ESP_GOTO_ON_FALSE(hret == ESP_H264_ERR_OK, ESP_FAIL, err, TAG, "get_param_hd failed %d", hret);

    player_ctx.state = PLAYER_STATE_PLAYING;
    ESP_LOGI(TAG, "H.264 soft decode start: %s", player_ctx.video_path);
    player_fps_t fps = {0};

    if ((player_ctx.bgm_path != NULL) && bsp_extra_player_play_file(player_ctx.bgm_path) != ESP_OK) {
        ESP_LOGE(TAG, "Play bgm failed");
    }

    while (player_ctx.state != PLAYER_STATE_STOPPED) {
        if (player_ctx.state == PLAYER_STATE_PAUSED) {
            if (bsp_display_lock(10)) {
                lv_obj_clear_flag(player_ctx.img_pause, LV_OBJ_FLAG_HIDDEN);
                bsp_display_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* 紧凑未消费数据并继续读文件 */
        if (stream_off > 0 && stream_len > stream_off) {
            memmove(stream, stream + stream_off, stream_len - stream_off);
            stream_len -= stream_off;
            stream_off = 0;
        } else if (stream_off >= stream_len) {
            stream_len = 0;
            stream_off = 0;
        }

        if (stream_len < (PLAYER_H264_STREAM_CAP / 4)) {
            int n = media_src_storage_read(&player_ctx.file, stream + stream_len,
                                           PLAYER_H264_STREAM_CAP - stream_len);
            ESP_GOTO_ON_FALSE(n >= 0, ESP_FAIL, err, TAG, "H.264 storage read failed");
            if (n > 0) {
                stream_len += (size_t)n;
                file_pos += (size_t)n;
            } else if (stream_len == 0) {
                ESP_LOGI(TAG, "H.264 EOF, frames=%d", frames);
                if (player_ctx.loop) {
                    media_src_storage_seek(&player_ctx.file, 0);
                    file_pos = 0;
                    stream_len = 0;
                    stream_off = 0;
                    /* 重新打开解码器以复位状态 */
                    esp_h264_dec_close(dec);
                    esp_h264_dec_del(dec);
                    dec = NULL;
                    hret = esp_h264_dec_sw_new(&cfg, &dec);
                    ESP_GOTO_ON_FALSE(hret == ESP_H264_ERR_OK, ESP_FAIL, err, TAG, "re-new dec failed");
                    hret = esp_h264_dec_open(dec);
                    ESP_GOTO_ON_FALSE(hret == ESP_H264_ERR_OK, ESP_FAIL, err, TAG, "re-open dec failed");
                    hret = esp_h264_dec_sw_get_param_hd(dec, &param);
                    ESP_GOTO_ON_FALSE(hret == ESP_H264_ERR_OK, ESP_FAIL, err, TAG, "re-param failed");
                    frames = 0;
                    continue;
                }
                esp_lvgl_simple_player_stop();
                continue;
            }
        }

        esp_h264_dec_in_frame_t in_frame = {
            .raw_data = {
                .buffer = stream + stream_off,
                .len = (uint32_t)(stream_len - stream_off),
            },
            .consume = 0,
        };
        bool need_more = false;

        while (in_frame.raw_data.len > 0 && player_ctx.state == PLAYER_STATE_PLAYING) {
            esp_h264_dec_out_frame_t out_frame = {0};
            const int64_t t_dec0 = esp_timer_get_time();
            hret = esp_h264_dec_process(dec, &in_frame, &out_frame);
            if (hret != ESP_H264_ERR_OK) {
                ESP_LOGE(TAG, "H.264 decode failed ret=%d (need baseline Annex-B, no B-frames)", hret);
                ret = ESP_FAIL;
                goto err;
            }
            if (in_frame.consume == 0) {
                need_more = true;
                break;
            }
            in_frame.raw_data.buffer += in_frame.consume;
            in_frame.raw_data.len -= in_frame.consume;
            stream_off += in_frame.consume;

            if (out_frame.out_size == 0 || out_frame.outbuf == NULL) {
                continue; /* SPS/PPS */
            }

            if (!canvas_ready) {
                esp_h264_resolution_t res = {0};
                hret = esp_h264_dec_get_resolution(param, &res);
                ESP_GOTO_ON_FALSE(hret == ESP_H264_ERR_OK && res.width && res.height, ESP_ERR_INVALID_SIZE,
                                  err, TAG, "get resolution failed");
                width = res.width;
                height = res.height;
                ESP_LOGI(TAG, "H.264 resolution %ux%u", width, height);
                if (width > PLAYER_H264_SOFT_MAX_W || height > PLAYER_H264_SOFT_MAX_H) {
                    ESP_LOGW(TAG, "Resolution large for soft decode; expect low FPS. Prefer <=%dx%d baseline",
                             PLAYER_H264_SOFT_MAX_W, PLAYER_H264_SOFT_MAX_H);
                }
                if (width > PLAYER_MAX_FRAME_WIDTH || height > PLAYER_MAX_FRAME_HEIGHT) {
                    ESP_LOGE(TAG, "Frame too large");
                    ret = ESP_ERR_INVALID_SIZE;
                    goto err;
                }

                uint64_t need = (uint64_t)width * height * 2;
                ESP_GOTO_ON_ERROR(player_out_alloc_pair_rgb565((uint32_t)need), err, TAG, "RGB565 out_buff failed");
                player_ctx.out_w = width;
                player_ctx.out_h = height;
                player_ui_set_playing(width, height);
                canvas_ready = true;
            }

            const int64_t t_cvt0 = esp_timer_get_time();
            i420_to_rgb565_le(out_frame.outbuf, width, height, player_out_back());
            const int64_t t_cvt1 = esp_timer_get_time();
            frames++;
            int64_t ui_us = 0;
            if (bsp_display_lock(10)) {
                const int64_t t_ui0 = esp_timer_get_time();
                player_out_present_locked();
                if (player_ctx.filesize) {
                    lv_slider_set_value(player_ctx.slider,
                                        (int)((file_pos * 1000ULL) / player_ctx.filesize), LV_ANIM_OFF);
                }
                bsp_display_unlock();
                ui_us = esp_timer_get_time() - t_ui0;
            }
            /* 软解较慢，略延时避免 UI 饿死 */
            vTaskDelay(pdMS_TO_TICKS(1));
            const int64_t t_end = esp_timer_get_time();
            player_fps_tick(&fps, "H.264",
                            0,
                            (t_cvt1 - t_dec0),
                            ui_us,
                            t_end - t_dec0,
                            out_frame.out_size);
            (void)t_cvt0;
        }

        if (need_more && stream_len >= PLAYER_H264_STREAM_CAP && stream_off == 0) {
            ESP_LOGE(TAG, "H.264 NAL larger than stream buffer");
            ret = ESP_ERR_NO_MEM;
            goto err;
        }
        (void)need_more;
    }

err:
    if (dec) {
        esp_h264_dec_close(dec);
        esp_h264_dec_del(dec);
    }
    if (stream) {
        heap_caps_free(stream);
    }
    return ret;
}

static void show_video_task(void *arg)
{
    esp_err_t ret = ESP_OK;
    int processed = 0;
    int all_size = 0;
    uint32_t file_seek_pos = 0;
    uint32_t file_seek_offset = 0;
    int jpeg_image_size = 0;
    const bool is_h264 = path_is_h264(player_ctx.video_path);

    /* Open video file */
    ESP_LOGI(TAG, "Opening video file %s ...", player_ctx.video_path);
    ESP_GOTO_ON_FALSE(media_src_storage_open(&player_ctx.file) == 0, ESP_ERR_NO_MEM, err, TAG, "Storage open failed");
    ESP_GOTO_ON_FALSE(media_src_storage_connect(&player_ctx.file, player_ctx.video_path) == 0, ESP_FAIL, err, TAG, "Storage connect failed");

    if (player_ctx.bgm_path != NULL) {
        ESP_LOGI(TAG, "Opening bgm file %s ...", player_ctx.bgm_path);
    }

    /* Get file size */
    ESP_GOTO_ON_FALSE(media_src_storage_get_size(&player_ctx.file, &player_ctx.filesize) == 0, ESP_FAIL, err, TAG, "Get file size failed");

    if (is_h264) {
        ret = play_h264_stream();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "H.264 play failed: %s", esp_err_to_name(ret));
        }
        goto err;
    }

    /* Create input buffer */
    player_ctx.in_buff = video_decoder_malloc(player_ctx.in_buff_size, true, &player_ctx.in_buff_size);
    ESP_GOTO_ON_FALSE(player_ctx.in_buff, ESP_ERR_NO_MEM, err, TAG, "Allocation in_buff failed");

    /* Init video decoder */
    ESP_GOTO_ON_ERROR(video_decoder_init(), err, TAG, "Initialize video decoder failed");

    /* Get video output size：HW JPEG 输出宽高会 16 对齐，缓冲必须按对齐后尺寸分配。 */
    uint32_t height = 0;
    uint32_t width = 0;
    ESP_GOTO_ON_ERROR(get_video_size(&width, &height), err, TAG, "Read JPEG header / first frame failed");
    const uint32_t pic_w = width;
    const uint32_t pic_h = height;
    width = ALIGN_UP(width, 16);
    height = ALIGN_UP(height, 16);
    ESP_LOGI(TAG, "MJPEG frame %ux%u -> aligned out %ux%u", pic_w, pic_h, width, height);

    /* Create output buffers：RGB565 双缓冲，避免解码写与 LVGL 读同一块。 */
    {
        uint64_t need = (uint64_t)width * (uint64_t)height * (BSP_LCD_BITS_PER_PIXEL / 8);
        ESP_GOTO_ON_FALSE(need > 0 && need <= (32 * 1024 * 1024), ESP_ERR_NO_MEM, err, TAG,
                          "out_buff size overflow (%" PRIu32 "x%" PRIu32 ")", width, height);
        ESP_GOTO_ON_ERROR(player_out_alloc_pair_jpeg((uint32_t)need), err, TAG,
                          "Allocation out_buff pair failed (need 2x%" PRIu32 " bytes)", (uint32_t)need);
    }
    player_ctx.out_w = (uint16_t)width;
    player_ctx.out_h = (uint16_t)height;

    bsp_display_lock(0);
    /* 先绑 front（尚未解码内容）；每帧解到 back 后再 present 切换。 */
    lv_canvas_set_buffer(player_ctx.canvas, player_ctx.out_buff[player_ctx.out_front],
                         width, height, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(player_ctx.canvas, width, height);
    lv_obj_invalidate(player_ctx.canvas);

    if (player_ctx.auto_width || player_ctx.auto_height) {
        uint32_t h = (player_ctx.auto_height ? (height + 120) : lv_obj_get_height(player_ctx.main));
        uint32_t w = (player_ctx.auto_width ? width : lv_obj_get_width(player_ctx.main));
        lv_obj_set_size(player_ctx.main, w, h);
    }

    lv_obj_clear_state(player_ctx.slider, LV_STATE_DISABLED);
    /* Enable/disable buttons */
    lv_obj_add_state(player_ctx.btn_play, LV_STATE_DISABLED);
    lv_obj_clear_state(player_ctx.btn_stop, LV_STATE_DISABLED);
    lv_obj_clear_state(player_ctx.btn_pause, LV_STATE_DISABLED);
    lv_obj_clear_state(player_ctx.btn_repeat, LV_STATE_DISABLED);
    /* Hide Stop button */
    lv_obj_add_flag(player_ctx.img_stop, LV_OBJ_FLAG_HIDDEN);
    /* Set slider range */
    lv_slider_set_range(player_ctx.slider, 0, 1000);
    bsp_display_unlock();

    player_ctx.state = PLAYER_STATE_PLAYING;

    ESP_LOGI(TAG, "Video player initialized");

    if ((player_ctx.bgm_path != NULL) && bsp_extra_player_play_file(player_ctx.bgm_path) != ESP_OK) {
        ESP_LOGE(TAG, "Play bgm failed");
    }
    media_src_storage_seek(&player_ctx.file, 0);

    player_fps_t fps = {0};
    media_src_storage_stats_reset();
    player_ctx.cache_valid = 0; /* 首帧从文件起点重新建立流缓存。 */
    uint32_t frame_idx = 0;
    while (player_ctx.state != PLAYER_STATE_STOPPED) {
        if (player_ctx.state == PLAYER_STATE_PAUSED) {
            if (bsp_display_lock(10)) {
                lv_obj_clear_flag(player_ctx.img_pause, LV_OBJ_FLAG_HIDDEN);
                bsp_display_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        const int64_t t_frame0 = esp_timer_get_time();
        uint64_t rd_bytes0 = 0;
        int64_t rd_us0 = 0;
        media_src_storage_stats_get(&rd_bytes0, &rd_us0);

        jpeg_image_size = video_decoder_read_jpeg_image(&file_seek_pos, &file_seek_offset);

        uint64_t rd_bytes1 = 0;
        int64_t rd_us1 = 0;
        media_src_storage_stats_get(&rd_bytes1, &rd_us1);
        const int64_t t_after_read = esp_timer_get_time();
        const int64_t read_us = (rd_us1 - rd_us0);
        (void)rd_bytes1;
        (void)rd_bytes0;

        /* 存储错误不能按坏帧跳过，否则会持续 seek/read 并刷屏。 */
        if (media_src_storage_read_failed(&player_ctx.file)) {
            ESP_LOGE(TAG, "SD read failed; stopping playback");
            ret = ESP_FAIL;
            goto err;
        }
        if (jpeg_image_size < 0) {
            ESP_LOGW(TAG, "Read JPEG image failed. Skip and continue.");
            /* 尝试从下一潜在 SOI 继续，避免整段退出 */
            file_seek_pos += 2;
            file_seek_offset = 0;
            continue;
        } else if (jpeg_image_size == 0) {
            ESP_LOGI(TAG, "Playing finished.");
            if (player_ctx.loop) {
                ESP_LOGI(TAG, "Playing loop enabled. Play again...");
                media_src_storage_seek(&player_ctx.file, 0);
                file_seek_pos = 0;
                file_seek_offset = 0;
                all_size = 0;
                continue;
            } else {
                esp_lvgl_simple_player_stop();
                continue;
            }
        }

        /* Decode one frame */
        processed = video_decoder_decode(jpeg_image_size);
        const int64_t t_after_decode = esp_timer_get_time();
        if (processed < 0) {
            ESP_LOGW(TAG, "Decode JPEG failed. Skip frame.");
            continue;
        }
        all_size += processed;

        int64_t ui_us = 0;
        if (bsp_display_lock(10)) {
            const int64_t t_ui0 = esp_timer_get_time();
            /* 切到刚解完的 back，再刷新；解码不再写 LVGL 正在显示的那块。 */
            player_out_present_locked();
            lv_slider_set_value(player_ctx.slider, ((float)all_size/(float)player_ctx.filesize)*1000, LV_ANIM_OFF);
            bsp_display_unlock();
            ui_us = esp_timer_get_time() - t_ui0;
        }
        const int64_t t_end = esp_timer_get_time();
        frame_idx++;
        player_fps_tick(&fps, "MJPEG",
                        read_us > 0 ? read_us : (t_after_read - t_frame0),
                        t_after_decode - t_after_read,
                        ui_us,
                        t_end - t_frame0,
                        (uint32_t)jpeg_image_size);
        (void)frame_idx;
    }

err:
    bsp_display_lock(0);
    /* 对齐 P4C5：先刷黑再 invalidate。禁止 lv_canvas_set_buffer(..., NULL)：
     * LV_ASSERT_NULL 会 while(1) 卡死 video task，触发 Task WDT。 */
    if (player_ctx.out_buff[player_ctx.out_front] && player_ctx.out_buff_size) {
        memset(player_ctx.out_buff[player_ctx.out_front], 0, player_ctx.out_buff_size);
    }
    if (player_ctx.auto_height && player_ctx.main) {
        lv_obj_set_height(player_ctx.main, 320);
    }
    if (player_ctx.canvas) {
        lv_obj_invalidate(player_ctx.canvas);
    }
    if (player_ctx.slider) {
        lv_slider_set_value(player_ctx.slider, 0, LV_ANIM_OFF);
    }
    bsp_display_unlock();

    if (player_ctx.bgm_path != NULL) {
        bsp_extra_player_register_callback(NULL, NULL);
        if (audio_player_stop() != ESP_OK) {
            ESP_LOGE(TAG, "Stop bgm failed");
        }
    }

    /* Close storage */
    media_src_storage_disconnect(&player_ctx.file);
    media_src_storage_close(&player_ctx.file);

    /* Deinit video decoder */
    video_decoder_deinit();

    if (player_ctx.in_buff) {
        heap_caps_free(player_ctx.in_buff);
        player_ctx.in_buff = NULL;
    }
    player_out_free_pair();

    /* 资源清理完成后才允许再次开始播放。 */
    player_ctx.state = PLAYER_STATE_STOPPED;
    ESP_LOGI(TAG, "Video player task finished.");

    /* Close task */
    vTaskDelete(NULL);
}

lv_obj_t * esp_lvgl_simple_player_create(esp_lvgl_simple_player_cfg_t * params)
{
    ESP_RETURN_ON_FALSE(params->video_path, NULL, TAG, "File path must be filled");
    ESP_RETURN_ON_FALSE(params->screen, NULL, TAG, "LVGL screen must be filled");
    ESP_RETURN_ON_FALSE(params->buff_size, NULL, TAG, "Size of the video frame buffer must be filled");
    ESP_RETURN_ON_FALSE(params->screen_width > 0 && params->screen_height > 0, NULL, TAG, "Object size must be filled");

    player_ctx.video_path = params->video_path;
    player_ctx.bgm_path = params->bgm_path;
    player_ctx.in_buff_size = params->buff_size;

    player_ctx.cache_buff_size = ALIGN_UP(params->cache_buff_size, CACHE_BUF_ALIGN);
    player_ctx.cache_buff_in_psram = params->cache_buff_in_psram;
    /* Create split buffer */
    uint32_t flag = player_ctx.cache_buff_in_psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
    player_ctx.cache_buff = (uint8_t *)heap_caps_aligned_alloc(128, player_ctx.cache_buff_size, flag);
    if (!player_ctx.cache_buff) {
        ESP_LOGE(TAG, "Malloc cache buffer failed");
        return NULL;
    }

    player_ctx.screen_width = params->screen_width;
    player_ctx.screen_height = params->screen_height;
    player_ctx.hide_controls = params->flags.hide_controls;
    player_ctx.hide_slider = params->flags.hide_slider;
    player_ctx.hide_status = params->flags.hide_status;
    player_ctx.auto_width = params->flags.auto_width;
    player_ctx.auto_height = params->flags.auto_height;
    player_ctx.is_init = true;

    /* Create LVGL objects */
    lv_obj_t * player_screen = create_lvgl_objects(params->screen);

    /* Default player state */
    esp_lvgl_simple_player_stop();

    return player_screen;
}

player_state_t esp_lvgl_simple_player_get_state(void)
{
    return player_ctx.state;
}

void esp_lvgl_simple_player_hide_controls(bool hide)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return;
    }

    if (hide) {
        lv_obj_add_flag(player_ctx.controls, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(player_ctx.controls, LV_OBJ_FLAG_HIDDEN);
    }
}

void esp_lvgl_simple_player_change_file(const char *video_file)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return;
    }

    if (player_ctx.state != PLAYER_STATE_STOPPED) {
        ESP_LOGW(TAG, "Playing file can be changed only when video is stopped.");
    }
    player_ctx.video_path = video_file;

    ESP_LOGI(TAG, "Video file changed to %s", video_file);
}

void esp_lvgl_simple_player_play(void)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return;
    }

    if (player_ctx.state == PLAYER_STATE_STOPPED) {
        ESP_LOGI(TAG, "Player starting playing.");
        /* Create video task */
        xTaskCreate(show_video_task, "video task", 16 * 1024, NULL, 4, &player_task_handle);
    } else if(player_ctx.state == PLAYER_STATE_PAUSED) {
        esp_lvgl_simple_player_resume();
    }
}

void esp_lvgl_simple_player_pause(void)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return;
    }

    if (player_ctx.state == PLAYER_STATE_PLAYING) {
        ESP_LOGI(TAG, "Player paused.");
        player_ctx.state = PLAYER_STATE_PAUSED;
        if (player_ctx.bgm_path != NULL) {
            if (audio_player_pause() != ESP_OK) {
                ESP_LOGE(TAG, "Pause bgm failed");
            }
        }

        bsp_display_lock(0);
        lv_obj_clear_state(player_ctx.btn_play, LV_STATE_DISABLED);
        lv_obj_clear_state(player_ctx.btn_stop, LV_STATE_DISABLED);
        lv_obj_clear_state(player_ctx.btn_pause, LV_STATE_DISABLED);
        lv_obj_clear_state(player_ctx.btn_repeat, LV_STATE_DISABLED);
        bsp_display_unlock();
    }
}

void esp_lvgl_simple_player_resume(void)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return;
    }

    if (player_ctx.state == PLAYER_STATE_PAUSED) {
        ESP_LOGI(TAG, "Player resume playing.");
        player_ctx.state = PLAYER_STATE_PLAYING;
        if (player_ctx.bgm_path != NULL) {
            if (!bsp_extra_player_is_playing_by_path(player_ctx.bgm_path)) {
                ESP_LOGW(TAG, "BGM is not playing. Start playing.");
                if (bsp_extra_player_play_file(player_ctx.bgm_path) != ESP_OK) {
                    ESP_LOGE(TAG, "Play bgm failed");
                }
            } else if (audio_player_resume() != ESP_OK) {
                ESP_LOGE(TAG, "Pause bgm failed");
            }
        }

        bsp_display_lock(0);
        lv_obj_add_flag(player_ctx.img_pause, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(player_ctx.btn_play, LV_STATE_DISABLED);
        bsp_display_unlock();
    }
}

void esp_lvgl_simple_player_stop(void)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return;
    }

    ESP_LOGI(TAG, "Player stopped.");
    player_ctx.state = PLAYER_STATE_STOPPED;

    bsp_display_lock(0);
    lv_obj_clear_state(player_ctx.btn_play, LV_STATE_DISABLED);
    lv_obj_add_state(player_ctx.btn_stop, LV_STATE_DISABLED);
    lv_obj_add_state(player_ctx.btn_pause, LV_STATE_DISABLED);
    lv_obj_add_state(player_ctx.btn_repeat, LV_STATE_DISABLED);
    lv_obj_clear_flag(player_ctx.img_stop, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

void esp_lvgl_simple_player_repeat(bool repeat)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return;
    }

    ESP_LOGI(TAG, "Player repeat %s.", (repeat ? "enabled" : "disabled"));
    player_ctx.loop = repeat;
}

esp_err_t esp_lvgl_simple_player_del(void)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return ESP_OK;
    }

    if (player_task_handle != 0) {
        esp_lvgl_simple_player_stop();
        if (esp_lvgl_simple_player_wait_task_stop(-1) != ESP_OK) {
            ESP_LOGE(TAG, "Player task stop timeout");
        }
    }

    if (player_ctx.cache_buff) {
        heap_caps_free(player_ctx.cache_buff);
        player_ctx.cache_buff = NULL;
    }

    player_ctx.is_init = false;

    return ESP_OK;
}

esp_err_t esp_lvgl_simple_player_wait_task_stop(int timeout_ms)
{
    if (!player_ctx.is_init) {
        ESP_LOGW(TAG, "Not init");
        return ESP_OK;
    }

    if (player_task_handle == 0) {
        return ESP_OK;
    }

    uint32_t i = 0;
    while (eTaskGetState(player_task_handle) != eDeleted && (timeout_ms < 0 || i < timeout_ms)) {
        vTaskDelay(pdMS_TO_TICKS(1));
        i++;
    }
    player_task_handle = NULL;

    if (i >= timeout_ms) {
        ESP_LOGE(TAG, "Player task stop timeout");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
