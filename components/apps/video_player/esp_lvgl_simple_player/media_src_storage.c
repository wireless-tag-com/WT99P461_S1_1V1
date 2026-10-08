#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "media_src_storage.h"

/* 每个视频独立管理描述符，避免混用 stdio 缓冲位置和 POSIX 文件位置。 */
typedef struct {
    int fd; /* -1 表示尚未打开或已经关闭。 */
    bool read_failed; /* 锁存 I/O 错误，不能把损坏数据当作文件结束。 */
} storage_src_t;
static const char *TAG = "media_src_storage";
static uint64_t s_read_bytes; /* 实际向文件系统请求并成功读取的字节数。 */
static int64_t s_read_us; /* 累计读取时间。 */
void media_src_storage_stats_reset(void) { s_read_bytes = 0; s_read_us = 0; }
void media_src_storage_stats_get(uint64_t *bytes, int64_t *us)
{
    if (bytes) *bytes = s_read_bytes;
    if (us) *us = s_read_us;
}
/* 创建一次文件会话。 */
int media_src_storage_open(media_src_t *src)
{
    if (!src || src->sub_src) return -1;
    storage_src_t *m = calloc(1, sizeof(*m));
    if (!m) return -1;
    m->fd = -1;
    src->sub_src = m;
    return 0;
}
/* 打开只读文件，重新建立错误状态。 */
int media_src_storage_connect(media_src_t *src, const char *uri)
{
    storage_src_t *m = src ? src->sub_src : NULL;
    if (!m || !uri) return -1;
    if (m->fd >= 0) close(m->fd);
    m->fd = open(uri, O_RDONLY);
    m->read_failed = false;
    if (m->fd < 0) ESP_LOGE(TAG, "Open %s failed: errno=%d", uri, errno);
    return m->fd < 0 ? -1 : 0;
}
/* 关闭可重复调用，支持初始化任意阶段失败后的统一清理。 */
int media_src_storage_disconnect(media_src_t *src)
{
    storage_src_t *m = src ? src->sub_src : NULL;
    if (m && m->fd >= 0) { close(m->fd); m->fd = -1; }
    return 0;
}
/* 大块读取直接交给 IDF SDMMC 的 DMA 对齐/中转逻辑，不再人为拆成4KB。 */
int media_src_storage_read(media_src_t *src, void *data, size_t len)
{
    storage_src_t *m = src ? src->sub_src : NULL;
    if (!m || m->fd < 0 || m->read_failed || len > INT_MAX) return -1;
    const int64_t start = esp_timer_get_time();
    ssize_t n;
    do { n = read(m->fd, data, len); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        m->read_failed = true;
        ESP_LOGE(TAG, "Video read failed: errno=%d; stopping this file", errno);
        return -1;
    }
    if (n > 0) { s_read_bytes += n; s_read_us += esp_timer_get_time() - start; }
    return (int)n;
}
/* 查询本文件会话的读失败状态。 */
bool media_src_storage_read_failed(media_src_t *src)
{
    storage_src_t *m = src ? src->sub_src : NULL;
    return m && m->read_failed;
}
/* seek/read 使用同一描述符，不混用 fseek/ftell。 */
int media_src_storage_seek(media_src_t *src, uint64_t position)
{
    storage_src_t *m = src ? src->sub_src : NULL;
    off_t offset = (off_t)position;
    if (!m || m->fd < 0 || offset < 0 || (uint64_t)offset != position) return -1;
    return lseek(m->fd, offset, SEEK_SET) < 0 ? -1 : 0;
}
/* 查询实际文件位置。 */
int media_src_storage_get_position(media_src_t *src, uint64_t *position)
{
    storage_src_t *m = src ? src->sub_src : NULL;
    if (!m || m->fd < 0 || !position) return -1;
    off_t offset = lseek(m->fd, 0, SEEK_CUR);
    if (offset < 0) return -1;
    *position = (uint64_t)offset;
    return 0;
}
/* fstat 获取大小，不改变读取位置。 */
int media_src_storage_get_size(media_src_t *src, uint64_t *size)
{
    storage_src_t *m = src ? src->sub_src : NULL;
    struct stat st;
    if (!m || m->fd < 0 || !size || fstat(m->fd, &st) || st.st_size < 0) return -1;
    *size = (uint64_t)st.st_size;
    return 0;
}
/* 清空句柄，防止打开失败或重复清理使用悬空指针。 */
int media_src_storage_close(media_src_t *src)
{
    if (!src) return -1;
    media_src_storage_disconnect(src);
    free(src->sub_src);
    src->sub_src = NULL;
    return 0;
}
