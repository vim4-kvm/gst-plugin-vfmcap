/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exclusive hardware diagnostic; never registered as an automatic test.
 * The raw-file mode compares GPU and CPU conversion of the exact same frame.
 * Timings include import/dispatch/fence/destruction, not capture or encoding. */
#include "amly-gpu.h"
#include "amly.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    int fd;
    gsize size;
    guint8 *map;
} DmaBuffer;

static gboolean dma_sync(DmaBuffer *buffer, guint64 flags)
{
    struct dma_buf_sync sync = {.flags = flags};
    int result;
    do {
        result = ioctl(buffer->fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        g_printerr("DMA_BUF_IOCTL_SYNC: %s\n", g_strerror(errno));
    return result == 0;
}

static gboolean allocate_dma(DmaBuffer *buffer, gsize bytes)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return FALSE;
    buffer->size = (bytes + page - 1) & ~(gsize)(page - 1);
    int heap = open("/dev/dma_heap/heap-codecmm", O_RDWR | O_CLOEXEC);
    if (heap < 0) {
        g_printerr("Opening codecmm CMA heap: %s\n", g_strerror(errno));
        return FALSE;
    }
    struct dma_heap_allocation_data allocation = {
        .len = buffer->size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };
    int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation);
    int saved_errno = errno;
    close(heap);
    if (result < 0) {
        g_printerr("Allocating CMA DMA-buf: %s\n", g_strerror(saved_errno));
        return FALSE;
    }
    buffer->fd = allocation.fd;
    buffer->map = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        buffer->fd, 0);
    if (buffer->map == MAP_FAILED) {
        buffer->map = NULL;
        g_printerr("Mapping CMA DMA-buf: %s\n", g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static void free_dma(DmaBuffer *buffer)
{
    if (buffer->map)
        munmap(buffer->map, buffer->size);
    if (buffer->fd >= 0)
        close(buffer->fd);
}

static void fill_synthetic(guint8 *data, guint width, guint height, guint stride)
{
    guint32 random = 0x824e319b;
    memset(data, 0x97, (gsize)stride * height);
    for (guint row = 0; row < height; row++) {
        for (guint x = 0; x < width; x += 2) {
            guint16 sample[4];
            for (guint i = 0; i < 4; i++) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                sample[i] = random & 1023;
            }
            // Include extrema and odd chroma sums beside pseudo-random colors.
            if (x == 0) {
                sample[0] = 1023 - (row & 1);
                sample[1] = 0;
                sample[2] = row & 1;
                sample[3] = 1023;
            }
            guint64 pair = sample[0] | ((guint64)sample[1] << 10) |
                ((guint64)sample[2] << 20) | ((guint64)sample[3] << 30);
            for (guint byte = 0; byte < 5; byte++)
                data[(gsize)row * stride + x * 5 / 2 + byte] = pair >> (byte * 8);
        }
    }
}

static gboolean run_frame(AmlyGpu *gpu, const char *raw_file, guint width,
                           guint height, guint input_stride, guint output_stride,
                           guint iterations)
{
    DmaBuffer input = {.fd = -1}, output = {.fd = -1};
    guint64 input_bytes = (guint64)input_stride * height;
    guint64 output_luma_size = (guint64)output_stride * height;
    guint64 output_bytes = output_luma_size <= G_MAXUINT32 ? output_luma_size * 3 / 2 : G_MAXUINT64;
    gboolean ok = FALSE;
    guint8 *expected = NULL;
    gchar *raw = NULL;
    GError *error = NULL;
    if (!width || !height || (width & 1) || (height & 1) || !iterations ||
        (guint64)input_stride < (guint64)width * 5 / 2 ||
        (guint64)output_stride < (guint64)width * 2 || output_stride % 16 ||
        input_bytes > G_MAXUINT32 - 4095 || output_bytes > G_MAXUINT32 - 4095) {
        g_printerr("Invalid frame dimensions/pitches/iteration count\n");
        goto cleanup;
    }
    if (raw_file) {
        gsize raw_size = 0;
        if (!g_file_get_contents(raw_file, &raw, &raw_size, &error)) {
            g_printerr("Reading raw frame: %s\n", error->message);
            goto cleanup;
        }
        if (raw_size != input_bytes) {
            g_printerr("Raw file has %zu bytes; expected exactly %" G_GUINT64_FORMAT "\n",
                        raw_size, input_bytes);
            goto cleanup;
        }
    }
    if (!allocate_dma(&input, input_bytes) || !allocate_dma(&output, output_bytes))
        goto cleanup;
    if (!dma_sync(&input, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
        goto cleanup;
    memset(input.map, 0x97, input.size);
    if (raw)
        memcpy(input.map, raw, input_bytes);
    else
        fill_synthetic(input.map, width, height, input_stride);
    if (!dma_sync(&input, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
        goto cleanup;

    expected = g_malloc(output_bytes);
    memset(expected, 0xa5, output_bytes);
    if (!dma_sync(&input, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
        goto cleanup;
    gint64 before = g_get_monotonic_time();
    amly_to_p010((uint16_t *)expected,
        (uint16_t *)(expected + (gsize)output_stride * height), input.map,
        width, height, input_stride, output_stride / 2);
    double cpu_ms = (g_get_monotonic_time() - before) / 1000.0;
    if (!dma_sync(&input, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
        goto cleanup;

    double total_ms = 0, max_ms = 0;
    for (guint iteration = 0; iteration < iterations; iteration++) {
        if (!dma_sync(&output, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
            goto cleanup;
        memset(output.map, 0xa5, output.size);
        if (!dma_sync(&output, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
            goto cleanup;
        before = g_get_monotonic_time();
        if (!amly_gpu_convert(gpu, input.fd, input.size, output.fd, output.size,
                                width, height, input_stride, output_stride, &error)) {
            g_printerr("GPU conversion: %s\n", error->message);
            goto cleanup;
        }
        double ms = (g_get_monotonic_time() - before) / 1000.0;
        total_ms += ms;
        max_ms = MAX(max_ms, ms);
        if (!dma_sync(&output, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
            goto cleanup;
        gsize mismatch = G_MAXSIZE;
        for (gsize byte = 0; byte < output_bytes; byte++) {
            if (output.map[byte] != expected[byte]) {
                mismatch = byte;
                break;
            }
        }
        // Verify that the shader also preserves allocation tail padding.
        for (gsize byte = output_bytes; mismatch == G_MAXSIZE && byte < output.size; byte++) {
            if (output.map[byte] != 0xa5) {
                mismatch = byte;
                break;
            }
        }
        if (mismatch != G_MAXSIZE)
            g_printerr("Mismatch iteration=%u offset=%zu gpu=0x%02x cpu=0x%02x\n",
                iteration, mismatch, output.map[mismatch],
                mismatch < output_bytes ? expected[mismatch] : 0xa5);
        gboolean synced = dma_sync(&output, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        if (!synced || mismatch != G_MAXSIZE)
            goto cleanup;
    }
    g_print("same_frame=PASS width=%u height=%u input_pitch=%u output_pitch=%u "
        "iterations=%u cpu_conversion_ms=%.3f gpu_total_avg_ms=%.3f gpu_total_max_ms=%.3f\n",
        width, height, input_stride, output_stride, iterations,
        cpu_ms, total_ms / iterations, max_ms);
    ok = TRUE;

cleanup:
    g_clear_error(&error);
    g_free(raw);
    g_free(expected);
    free_dma(&input);
    free_dma(&output);
    return ok;
}

static gboolean number(const char *text, guint *value)
{
    char *end;
    errno = 0;
    guint64 parsed = g_ascii_strtoull(text, &end, 10);
    if (errno || !*text || *end || !parsed || parsed > G_MAXUINT)
        return FALSE;
    *value = parsed;
    return TRUE;
}

int main(int argc, char **argv)
{
    const char *raw_file = NULL;
    guint dimensions[5] = {0, 0, 0, 0, 1};
    if (argc != 1) {
        gboolean raw_mode = !strcmp(argv[1], "--raw");
        gboolean synthetic_mode = !strcmp(argv[1], "--synthetic");
        int first = raw_mode ? 3 : 2;
        if ((!raw_mode && !synthetic_mode) ||
            (argc != first + 4 && argc != first + 5))
            goto usage;
        if (raw_mode)
            raw_file = argv[2];
        for (int i = first; i < argc; i++)
            if (!number(argv[i], &dimensions[i - first]))
                goto usage;
    }
    GError *error = NULL;
    AmlyGpu *gpu = amly_gpu_new(&error);
    if (!gpu) {
        g_printerr("Initializing GPU: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }
    gboolean ok = TRUE;
    if (argc == 1) {
        // Cross 32/128-bit source words, shader workgroups and NEON tails.
        const guint widths[] = {2, 4, 6, 30, 32, 34, 66, 130, 1920, 2560, 3440, 3840};
        for (guint i = 0; i < G_N_ELEMENTS(widths) && ok; i++) {
            guint width = widths[i];
            guint height = width < 1920 ? 6 : (width == 3840 ? 2160 : 1440);
            guint input_stride = width * 5 / 2 + (i % 3);
            guint output_stride = (width * 2 + 63) & ~63u;
            ok = run_frame(gpu, NULL, width, height, input_stride, output_stride, 1);
        }
    } else {
        ok = run_frame(gpu, raw_file, dimensions[0], dimensions[1], dimensions[2],
                         dimensions[3], dimensions[4]);
    }
    amly_gpu_free(gpu);
    return ok ? 0 : 1;

usage:
    g_printerr("usage: %s [--raw FILE | --synthetic] WIDTH HEIGHT INPUT_PITCH OUTPUT_PITCH [ITERATIONS]\n"
        "       %s  # synthetic geometry/color/padding suite on real Mali-G52\n",
        argv[0], argv[0]);
    return 2;
}
