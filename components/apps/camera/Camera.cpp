/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_video_init.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/ppa.h"

#include "bsp/esp-bsp.h"

#include "esp_lcd_touch_gt911.h"

#include "app_video.h"
#include "app_pedestrian_detect.h"
#include "app_humanface_detect.h"
#include "app_camera_pipeline.hpp"
#include "Camera.hpp"
#include "ui/ui.h"

#define ALIGN_UP_BY(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

#define CAMERA_INIT_TASK_WAIT_MS            (1000)
#define DETECT_NUM_MAX                      (10)
#define FPS_PRINT                           (1)

/*
 * 行内水平环移校正（RGB565）：
 *   >0  右旋 N 列（左边竖条跑到右边时用）
 *   <0  左旋 |N| 列（右边竖条跑到左边时用）
 * 画布已与 1024x600 对齐后，先观察；若仍有环移再改此值。
 */
#ifndef CAMERA_HWRAP_FIX_PX
#define CAMERA_HWRAP_FIX_PX                 (0)
#endif

using namespace std;

typedef enum {
    CAMERA_EVENT_TASK_RUN = BIT(0),
    CAMERA_EVENT_DELETE = BIT(1),
    CAMERA_EVENT_PED_DETECT = BIT(2),
    CAMERA_EVENT_HUMAN_DETECT = BIT(3),
} camera_event_id_t;

LV_IMG_DECLARE(img_app_camera);

static const char *TAG = "Camera";

// AI detection variables
// static void **detect_buf;
static vector<vector<int>> detect_bound;
static vector<vector<int>> detect_keypoints;
static PedestrianDetect *ped_detect = NULL;
static HumanFaceDetect *hum_detect = NULL;
static pipeline_handle_t feed_pipeline;
static pipeline_handle_t detect_pipeline;

// Other variables
static lv_obj_t *btn_label = NULL;
static size_t data_cache_line_size = 0;
static ppa_client_handle_t ppa_client_srm_handle = NULL;
static EventGroupHandle_t camera_event_group;

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, 
                                       uint32_t camera_buf_hes, uint32_t camera_buf_ves, 
                                       size_t camera_buf_len);

static bool ppa_trans_done_cb(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data);

#if CAMERA_HWRAP_FIX_PX != 0
static void camera_fix_hwrap_rgb565(uint16_t *frame, uint32_t width, uint32_t height, int32_t shift)
{
    if (frame == NULL || shift == 0) {
        return;
    }

    uint32_t n = (uint32_t)(shift > 0 ? shift : -shift);
    if (n >= width || n > 64) {
        return;
    }

    uint16_t tmp[64];
    for (uint32_t y = 0; y < height; y++) {
        uint16_t *row = frame + y * width;
        if (shift > 0) {
            /* 右旋：左边竖条在右边时 */
            memcpy(tmp, row + (width - n), n * sizeof(uint16_t));
            memmove(row + n, row, (width - n) * sizeof(uint16_t));
            memcpy(row, tmp, n * sizeof(uint16_t));
        } else {
            /* 左旋：右边竖条在左边时 */
            memcpy(tmp, row, n * sizeof(uint16_t));
            memmove(row, row + n, (width - n) * sizeof(uint16_t));
            memcpy(row + (width - n), tmp, n * sizeof(uint16_t));
        }
    }
}
#endif

Camera::Camera(uint16_t hor_res, uint16_t ver_res):
    ESP_Brookesia_PhoneApp("Camera", &img_app_camera, false),  // auto_resize_visual_area
    _screen_index(SCREEN_CAMERA_SHOT),
    _hor_res(hor_res),
    _ver_res(ver_res),
    _img_album_dsc_size(hor_res > ver_res ? ver_res : hor_res),
    _img_album_buffer(NULL),
    _camera_init_sem(NULL),
    _camera_ctlr_handle(0),
    _hw_ready(false)
{
    _img_album_buf_bytes = _img_album_dsc_size * _img_album_dsc_size * sizeof(lv_color_t);
}

Camera::~Camera()
{
}

bool Camera::run(void)
{
    /* 首次打开再初始化 CSI/缓冲，避免开机占满内部 SRAM 导致 SD SPI DMA 失败。 */
    if (!ensureHardwareReady()) {
        ESP_LOGE(TAG, "Camera hardware init failed");
        return false;
    }

    /* 未识别到传感器时不启动采集任务，避免对无效设备执行 ioctl。 */
    if (_camera_ctlr_handle < 0) {
        ESP_LOGE(TAG, "Camera device is unavailable");
        return false;
    }
    if (_camera_init_sem == NULL) {
        _camera_init_sem = xSemaphoreCreateBinary();
        assert(_camera_init_sem != NULL);

        xTaskCreatePinnedToCore((TaskFunction_t)taskCameraInit, "Camera Init", 4096, this, 2, NULL, 0);
        if (xSemaphoreTake(_camera_init_sem, pdMS_TO_TICKS(CAMERA_INIT_TASK_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Camera init timeout");
            return false;
        }
        vSemaphoreDelete(_camera_init_sem); /* FreeRTOS 信号量须通过配套接口释放。 */
        _camera_init_sem = NULL;
    }

    /* AI 模型较大且可能抛异常（exceptions 关闭时会 abort）。
     * 预览阶段不加载，等用户切到 Detect 模式再按需创建。 */
    ped_detect = NULL;
    hum_detect = NULL;

    xTaskCreatePinnedToCore((TaskFunction_t)camera_dectect_task, "Camera Detect", 1024 * 8, this, 5, &_detect_task_handle, 1);

    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);

    // UI initialization
    ui_camera_init();

    // The following is the additional UI initialization
    _img_album_buffer = (uint8_t *)heap_caps_aligned_alloc(128, _img_refresh_dsc.data_size, MALLOC_CAP_SPIRAM);
    if (_img_album_buffer == NULL) {
        ESP_LOGE(TAG, "Allocate memory for album buffer failed");
        return false;
    }
    lv_img_dsc_t img_dsc = {
        .header = {
            .cf = LV_IMG_CF_TRUE_COLOR,
            .always_zero = 0,
            .reserved = 0,
            .w = _hor_res,
            .h = _ver_res,
        },
        .data_size = _img_album_buf_bytes,
        .data = (const uint8_t *)_img_album_buffer,
    };
    memcpy(&_img_album_dsc, &img_dsc, sizeof(lv_img_dsc_t));

    lv_obj_refr_size(ui_PanelCameraShotAlbum);
    lv_obj_clear_flag(ui_PanelCameraShotAlbum, LV_OBJ_FLAG_CLICKABLE);

    _img_album = lv_imgbtn_create(ui_PanelCameraShotAlbum);
    lv_obj_add_flag(_img_album, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(_img_album, 100, 100);
    lv_obj_center(_img_album);
    lv_obj_add_event_cb(_img_album, onScreenCameraShotAlbumClick, LV_EVENT_CLICKED, this);

    img_dsc.header.w = _hor_res;
    img_dsc.header.h = _ver_res;
    img_dsc.data_size = _img_refresh_dsc.data_size;
    memcpy(&_img_photo_dsc, &img_dsc, sizeof(lv_img_dsc_t));
    memcpy(_img_album_buffer, _img_refresh_dsc.data, _img_refresh_dsc.data_size);
    lv_obj_set_width(ui_ImageCameraPhotoImage, _hor_res);
    lv_obj_set_height(ui_ImageCameraPhotoImage, _ver_res);
    lv_img_set_src(ui_ImageCameraPhotoImage, &_img_photo_dsc);

    lv_obj_add_event_cb(ui_ButtonCameraShotBtn, onScreenCameraShotBtnClick, LV_EVENT_CLICKED, this);

    lv_obj_add_flag(ui_PanelCameraShotTitle, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *mode_switch_btn = lv_btn_create(ui_ImageCameraShotImage);
    lv_obj_set_style_bg_color(mode_switch_btn, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_size(mode_switch_btn, 130, 50);
    btn_label = lv_label_create(mode_switch_btn);
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(btn_label, "  Normal \n   Detect");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(mode_switch_btn, LV_ALIGN_TOP_RIGHT, -150, 0);
    lv_obj_add_event_cb(mode_switch_btn, [](lv_event_t *e) {
        Camera *camera = (Camera *)e->user_data;

        if (xEventGroupGetBits(camera_event_group) & CAMERA_EVENT_PED_DETECT) {
            if (hum_detect == NULL) {
                hum_detect = get_humanface_detect();
                if (hum_detect == NULL) {
                    ESP_LOGE(TAG, "Human face detect model load failed");
                    return;
                }
            }
            xEventGroupClearBits(camera_event_group, CAMERA_EVENT_PED_DETECT);
            xEventGroupSetBits(camera_event_group, CAMERA_EVENT_HUMAN_DETECT);
            lv_label_set_text(btn_label, "    Face \n   Detect");

            lv_obj_add_flag(ui_ButtonCameraShotBtn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_PanelCameraShotControlBg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(camera->_img_album, LV_OBJ_FLAG_HIDDEN);
            camera->_screen_index = SCREEN_CAMERA_AI;
        } else if (xEventGroupGetBits(camera_event_group) & CAMERA_EVENT_HUMAN_DETECT) {
            xEventGroupClearBits(camera_event_group, CAMERA_EVENT_HUMAN_DETECT);
            lv_label_set_text(btn_label, "  Normal \n   Detect");

            lv_obj_clear_flag(ui_ButtonCameraShotBtn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui_PanelCameraShotControlBg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(camera->_img_album, LV_OBJ_FLAG_HIDDEN);
            camera->_screen_index = SCREEN_CAMERA_SHOT;
        } else {
            if (ped_detect == NULL) {
                ped_detect = get_pedestrian_detect();
                if (ped_detect == NULL) {
                    ESP_LOGE(TAG, "Pedestrian detect model load failed");
                    return;
                }
            }
            xEventGroupSetBits(camera_event_group, CAMERA_EVENT_PED_DETECT);
            lv_label_set_text(btn_label, "Pedestrian \n   Detect");

            lv_obj_add_flag(ui_ButtonCameraShotBtn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_PanelCameraShotControlBg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(camera->_img_album, LV_OBJ_FLAG_HIDDEN);
            camera->_screen_index = SCREEN_CAMERA_AI;
        }

    }, LV_EVENT_CLICKED, this);

    return true;
}

bool Camera::pause(void)
{
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    
    return true;
}

bool Camera::resume(void)
{
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);

    return true;
}

bool Camera::back(void)
{
    notifyCoreClosed();

    return true;
}

bool Camera::close(void)
{
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_DELETE);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_PED_DETECT);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_HUMAN_DETECT);
    
    app_video_stream_task_stop(_camera_ctlr_handle);
    app_video_stream_wait_stop();

    if (_img_album_buffer) {
        heap_caps_free(_img_album_buffer);
        _img_album_buffer = NULL;
    }

    return true;
}

bool Camera::init(void)
{
    /* installApp 阶段只建事件组，重资源延后到首次 run()。 */
    camera_event_group = xEventGroupCreate();
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_PED_DETECT);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_HUMAN_DETECT);
    _hw_ready = false;
    _camera_ctlr_handle = -1;
    return true;
}

bool Camera::ensureHardwareReady(void)
{
    if (_hw_ready) {
        return _camera_ctlr_handle >= 0;
    }

    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
    esp_err_t ret = app_video_main(i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video main init failed with error 0x%x", ret);
    }

    _camera_ctlr_handle = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT_RGB565);
    if (_camera_ctlr_handle < 0) {
        ESP_LOGE(TAG, "video cam open failed");
        if (ESP_OK == i2c_master_probe(i2c_bus_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) ||
                ESP_OK == i2c_master_probe(i2c_bus_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100)) {
            ESP_LOGI(TAG, "gt911 touch found");
        } else {
            ESP_LOGE(TAG, "Touch not found");
        }
        _hw_ready = true;
        return false;
    }

    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        _cam_buffer[i] = (uint8_t *)heap_caps_aligned_alloc(data_cache_line_size,
                            _hor_res * _ver_res * BSP_LCD_BITS_PER_PIXEL / 8, MALLOC_CAP_SPIRAM);
        _cam_buffer_size[i] = _hor_res * _ver_res * BSP_LCD_BITS_PER_PIXEL / 8;
    }

    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_video_frame_operation));

    lv_img_dsc_t img_dsc = {
        .header = {
            .cf = LV_IMG_CF_TRUE_COLOR,
            .always_zero = 0,
            .reserved = 0,
            .w = _hor_res,
            .h = _ver_res,
        },
        .data_size = _cam_buffer_size[0],
        .data = (const uint8_t *)_cam_buffer[0],
    };
    memcpy(&_img_refresh_dsc, &img_dsc, sizeof(lv_img_dsc_t));

    size_t detect_buf_size = ALIGN_UP_BY(_hor_res * _ver_res * BSP_LCD_BITS_PER_PIXEL / 8, data_cache_line_size);

    ppa_client_config_t srm_config =  {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&srm_config, &ppa_client_srm_handle));

    ppa_event_callbacks_t cbs = {
        .on_trans_done = ppa_trans_done_cb,
    };
    ppa_client_register_event_callbacks(ppa_client_srm_handle, &cbs);

    camera_pipeline_cfg_t PPA_feed_cfg = {
        .elem_num = 4,
        .elements = NULL,
        .align_size = 1,
        .caps = MALLOC_CAP_SPIRAM,
        .buffer_size = detect_buf_size,
    };
    camera_element_pipeline_new(&PPA_feed_cfg, &feed_pipeline);

    camera_pipeline_cfg_t detect_feed_cfg = {
        .elem_num = 4,
        .elements = NULL,
        .align_size = 1,
        .caps = MALLOC_CAP_SPIRAM,
        .buffer_size = 20 * sizeof(int),
    };
    camera_element_pipeline_new(&detect_feed_cfg, &detect_pipeline);

    _hw_ready = true;
    return true;
}

void Camera::taskCameraInit(Camera *app)
{
    ESP_ERROR_CHECK(app_video_set_bufs(app->_camera_ctlr_handle, EXAMPLE_CAM_BUF_NUM, (const void **)app->_cam_buffer));

    ESP_LOGI(TAG, "Start camera stream task");
    ESP_ERROR_CHECK(app_video_stream_task_start(app->_camera_ctlr_handle, 0));

    xSemaphoreGive(app->_camera_init_sem);

    vTaskDelete(NULL);
}

void Camera::onScreenCameraShotAlbumClick(lv_event_t *e)
{
    lv_obj_invalidate(ui_ImageCameraPhotoImage);
}

void Camera::onScreenCameraShotBtnClick(lv_event_t *e)
{
    Camera *camera = (Camera *)e->user_data;

    if (camera == NULL) {
        return;
    }

    lv_obj_add_flag(camera->_img_album, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_PanelCameraShotAlbum, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_src(camera->_img_album, &camera->_img_album_dsc);

    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    memcpy(camera->_img_album_buffer, camera->_img_refresh_dsc.data,
           camera->_img_refresh_dsc.data_size);
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
}

static bool ppa_trans_done_cb(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    camera_pipeline_buffer_element *p = (camera_pipeline_buffer_element *)user_data;
    camera_pipeline_done_element(feed_pipeline, p);

    return (xHigherPriorityTaskWoken == pdTRUE);
}

#if FPS_PRINT
typedef struct {
    int64_t start;
    int64_t acc;
    char str1[15];
    char str2[15];
} PerfCounter;
static PerfCounter perf_counters[1] = {0};

static void perfmon_start(int ctr, const char *fmt1, const char *fmt2, ...)
{
    va_list args;
    va_start(args, fmt2);
    vsnprintf(perf_counters[ctr].str1, sizeof(perf_counters[ctr].str1), fmt1, args);
    vsnprintf(perf_counters[ctr].str2, sizeof(perf_counters[ctr].str2), fmt2, args);
    va_end(args);

    perf_counters[ctr].start = esp_timer_get_time();
}

static void perfmon_end(int ctr, int count)
{
    int64_t time_diff = esp_timer_get_time() - perf_counters[ctr].start;
    float time_in_sec = (float)time_diff / 1000000;
    float frequency = count / time_in_sec;

    printf("Perf ctr[%d], [%15s][%15s]: %.2f FPS (%.2f ms per operation)\n",
           ctr, perf_counters[ctr].str1, perf_counters[ctr].str2, frequency, time_in_sec * 1000 / count);
}
#endif

void Camera::camera_dectect_task(Camera *app)
{
    int res = 0;
    while (1) {
        xEventGroupWaitBits(camera_event_group, CAMERA_EVENT_TASK_RUN, pdFALSE, pdTRUE, portMAX_DELAY);
        
        if (xEventGroupGetBits(camera_event_group) & (CAMERA_EVENT_PED_DETECT | CAMERA_EVENT_HUMAN_DETECT)) {
            camera_pipeline_buffer_element *p = camera_pipeline_recv_element(feed_pipeline, portMAX_DELAY);
            if (p) {
                std::list<dl::detect::result_t> detect_results; // 当前帧的线程私有结果。
                if (xEventGroupGetBits(camera_event_group) & CAMERA_EVENT_PED_DETECT) {
                    if (ped_detect) {
                        detect_results = app_pedestrian_detect((uint16_t *)p->buffer, app->_hor_res, app->_ver_res);
                    }
                } else if (hum_detect) {
                    detect_results = app_humanface_detect((uint16_t *)p->buffer, app->_hor_res, app->_ver_res);
                }

                camera_pipeline_queue_element_index(feed_pipeline, p->index);

                camera_pipeline_buffer_element *element = camera_pipeline_get_queued_element(detect_pipeline);
                if (element) {
                    element->detect_results = std::move(detect_results); // 入队后由显示线程独占读取。

                    camera_pipeline_done_element(detect_pipeline, element);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (xEventGroupGetBits(camera_event_group) & CAMERA_EVENT_DELETE) {
            delete_pedestrian_detect();
            delete_humanface_detect();
            ped_detect = NULL;
            hum_detect = NULL;

            ESP_LOGI(TAG, "Camera detect task exit");
            vTaskDelete(NULL);
        }
    }
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, 
                                       uint32_t camera_buf_hes, uint32_t camera_buf_ves, 
                                       size_t camera_buf_len)
{
    // Wait for task run event
    xEventGroupWaitBits(camera_event_group, CAMERA_EVENT_TASK_RUN, pdFALSE, pdTRUE, portMAX_DELAY);

#if CAMERA_HWRAP_FIX_PX != 0
    if (camera_buf && camera_buf_hes > 0 && camera_buf_ves > 0) {
        camera_fix_hwrap_rgb565(reinterpret_cast<uint16_t *>(camera_buf),
                                  camera_buf_hes, camera_buf_ves, CAMERA_HWRAP_FIX_PX);
    }
#endif

    // Check if AI detection is needed
    EventBits_t current_bits = xEventGroupGetBits(camera_event_group);
    bool is_detect_mode = current_bits & (CAMERA_EVENT_PED_DETECT | CAMERA_EVENT_HUMAN_DETECT);
    
    if (is_detect_mode) {
        // Process input frame
        camera_pipeline_buffer_element *input_element = camera_pipeline_get_queued_element(feed_pipeline);
        if (input_element) {
            // 采集缓冲区在回调结束后会复用，将快照复制到检测队列自己的缓冲区。
            if (camera_buf_len <= input_element->valid_size) {
                memcpy(input_element->buffer, camera_buf, camera_buf_len);
                camera_pipeline_done_element(feed_pipeline, input_element);
            } else {
                ESP_LOGE(TAG, "Camera frame exceeds detection buffer");
                camera_pipeline_queue_element_index(feed_pipeline, input_element->index);
            }
        }

        // Get detection results
        camera_pipeline_buffer_element *detect_element = camera_pipeline_recv_element(detect_pipeline, 0);
        if (detect_element) {
            // Process detection results
            detect_keypoints.clear();
            detect_bound.clear();
            
            for (const auto& res : detect_element->detect_results) {
                const auto& box = res.box;
                // Check if bounding box is valid
                if (box.size() >= 4 && std::any_of(box.begin(), box.end(), [](int v) { return v != 0; })) {
                    detect_bound.push_back(box);

                    // Process keypoints only in face detection mode
                    if ((current_bits & CAMERA_EVENT_HUMAN_DETECT) && 
                        res.keypoint.size() >= 10 && 
                        std::any_of(res.keypoint.begin(), res.keypoint.end(), [](int v) { return v != 0; })) {
                        detect_keypoints.push_back(res.keypoint);
                    }
                }
            }

            camera_pipeline_queue_element_index(detect_pipeline, detect_element->index);
        }

        // Draw detection results
        uint16_t *rgb_buf = reinterpret_cast<uint16_t*>(camera_buf);
        for (size_t i = 0; i < detect_bound.size(); i++) {
            const auto& bound = detect_bound[i];
            // Check if current bounding box is valid
            if (bound.size() >= 4 && std::any_of(bound.begin(), bound.end(), [](int v) { return v != 0; })) {
                // Draw bounding box
                draw_rectangle_rgb(rgb_buf, camera_buf_hes, camera_buf_ves,
                                 bound[0], bound[1], bound[2], bound[3],
                                 0, 0, 255, 0, 0, 3);

                // Draw keypoints in face detection mode
                if ((current_bits & CAMERA_EVENT_HUMAN_DETECT) && 
                    i < detect_keypoints.size() && 
                    detect_keypoints[i].size() >= 10) {
                    draw_green_points(rgb_buf, detect_keypoints[i]);
                }
            }
        }
    }

    // Update display if not in delete state
    if (!(current_bits & CAMERA_EVENT_DELETE) && bsp_display_lock(100)) {
        if (ui_ImageCameraShotImage) {
            lv_canvas_set_buffer(ui_ImageCameraShotImage, camera_buf,
                               camera_buf_hes, camera_buf_ves,
                               LV_IMG_CF_TRUE_COLOR);
            /* canvas_set_buffer 不会改控件尺寸，必须与帧分辨率对齐避免行步长错位。 */
            lv_obj_set_size(ui_ImageCameraShotImage, camera_buf_hes, camera_buf_ves);
        }
        lv_refr_now(NULL);
        bsp_display_unlock();
    }

#if FPS_PRINT
    static int count = 0;
    if (count % 10 == 0) {
        perfmon_start(0, "PFS", "camera");
    } else if (count % 10 == 9) {
        perfmon_end(0, 10);
    }
    count++;
#endif
}
