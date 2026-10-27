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

static guint sample(const guint8 *input, guint stride, guint x, guint y, guint channel)
{
    const guint8 *bytes = input + (gsize)y * stride + x * 5;
    guint64 pair = 0;
    for (guint i = 0; i < 5; i++)
        pair |= (guint64)bytes[i] << (8 * i);
    return (pair >> (10 * channel)) & 1023;
}

static double filtered_sample(const guint8 *input, guint stride, guint width,
    guint height, double x, double y, gint channel)
{
    guint columns = channel < 0 ? width : width / 2;
    x = CLAMP(x, 0.0, columns - 1.0);
    y = CLAMP(y, 0.0, height - 1.0);
    guint left = x, top = y;
    guint right = MIN(left + 1, columns - 1), bottom = MIN(top + 1, height - 1);
    double result = 0;
    for (guint row = 0; row < 2; row++) {
        for (guint col = 0; col < 2; col++) {
            guint sx = col ? right : left, sy = row ? bottom : top;
            guint value = channel < 0 ? sample(input, stride, sx / 2, sy, 1 + 2 * (sx & 1)) :
                sample(input, stride, sx, sy, channel);
            result += value * (col ? x - left : 1 - (x - left)) *
                (row ? y - top : 1 - (y - top));
        }
    }
    return result;
}

static void reference_scale(guint8 *output, const guint8 *input, guint width,
    guint height, guint out_width, guint out_height, guint input_stride, guint output_stride)
{
    double rx = (double)width / out_width, ry = (double)height / out_height;
    for (guint y = 0; y < out_height; y++) {
        guint16 *row = (guint16 *)(output + (gsize)y * output_stride);
        for (guint x = 0; x < out_width; x++)
            row[x] = (guint16)(filtered_sample(input, input_stride, width, height,
                (x + 0.5) * rx - 0.5, (y + 0.5) * ry - 0.5, -1) + 0.5) << 6;
    }
    for (guint y = 0; y < out_height / 2; y++) {
        guint16 *row = (guint16 *)(output + (gsize)(out_height + y) * output_stride);
        for (guint x = 0; x < out_width / 2; x++) {
            for (guint c = 0; c < 2; c++)
                row[x * 2 + c] = (guint16)(filtered_sample(input, input_stride, width, height,
                    (x + 0.5) * rx - 0.5, (y * 2 + 1) * ry - 0.5, c * 2) + 0.5) << 6;
        }
    }
}

static gboolean run_frame(AmlyGpu *gpu, const char *raw_file, guint width,
                           guint height, guint out_width, guint out_height,
                           guint input_stride, guint output_stride, guint iterations)
{
    DmaBuffer input = {.fd = -1}, output = {.fd = -1};
    guint64 input_bytes = (guint64)input_stride * height;
    guint64 output_luma_size = (guint64)output_stride * out_height;
    guint64 output_bytes = output_luma_size <= G_MAXUINT32 ? output_luma_size * 3 / 2 : G_MAXUINT64;
    gboolean ok = FALSE;
    guint8 *expected = NULL;
    gchar *raw = NULL;
    GError *error = NULL;
    gboolean scaled = width != out_width || height != out_height;
    if (!width || !height || (width & 1) || (height & 1) || !iterations ||
        !out_width || !out_height || (out_width & 1) || (out_height & 1) ||
        out_width > width || out_height > height ||
        (guint64)input_stride < (guint64)width * 5 / 2 ||
        (guint64)output_stride < (guint64)out_width * 2 || output_stride % 16 ||
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
        /* vfm_cap exports a page-aligned allocation, larger than visible rows. */
        if (raw_size < input_bytes || raw_size > ((input_bytes + 4095) & ~(guint64)4095)) {
            g_printerr("Raw file has %zu bytes; expected visible/page-aligned %" G_GUINT64_FORMAT "\n",
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
    if (scaled)
        reference_scale(expected, input.map, width, height, out_width, out_height,
                        input_stride, output_stride);
    else
        amly_to_p010((uint16_t *)expected,
            (uint16_t *)(expected + (gsize)output_stride * height), input.map,
            width, height, input_stride, output_stride / 2);
    double cpu_ms = (g_get_monotonic_time() - before) / 1000.0;
    if (!dma_sync(&input, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
        goto cleanup;

    double total_ms = 0, max_ms = 0;
    guint max_sample_error = 0;
    for (guint iteration = 0; iteration < iterations; iteration++) {
        if (!iteration) {
            if (!dma_sync(&output, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
                goto cleanup;
            memset(output.map, 0xa5, output.size);
            if (!dma_sync(&output, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
                goto cleanup;
        }
        before = g_get_monotonic_time();
        if (!amly_gpu_convert_scaled(gpu, input.fd, input.size, output.fd, output.size,
                width, height, out_width, out_height, input_stride, output_stride, &error)) {
            g_printerr("GPU conversion: %s\n", error->message);
            goto cleanup;
        }
        double ms = (g_get_monotonic_time() - before) / 1000.0;
        total_ms += ms;
        max_ms = MAX(max_ms, ms);
        // Compare the first and final job. Scanning a large mapped DMA frame
        // between every dispatch prevents measuring sustained GPU throughput.
        if (iteration && iteration + 1 < iterations)
            continue;
        if (!dma_sync(&output, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
            goto cleanup;
        gsize mismatch = G_MAXSIZE;
        for (gsize byte = 0; byte < output_bytes; byte++) {
            if (scaled && byte % output_stride < out_width * 2) {
                // Mali's texture filter uses finite subtexel precision. Bound
                // the difference against full double interpolation, while
                // checking that output remains ten-bit with zero low bits.
                guint16 actual, reference;
                memcpy(&actual, output.map + byte, 2);
                memcpy(&reference, expected + byte, 2);
                guint delta = ABS((int)actual - reference) >> 6;
                max_sample_error = MAX(max_sample_error, delta);
                if (!(actual & 63) && delta <= 6) {
                    byte++;
                    continue;
                }
                g_printerr("Scaled sample offset=%zu GPU=%u reference=%u\n", byte, actual >> 6, reference >> 6);
                mismatch = byte;
                break;
            }
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
    g_print("same_frame=PASS width=%u height=%u output=%ux%u input_pitch=%u output_pitch=%u "
        "iterations=%u max_sample_error=%u cpu_conversion_ms=%.3f gpu_total_avg_ms=%.3f gpu_total_max_ms=%.3f\n",
        width, height, out_width, out_height, input_stride, output_stride, iterations,
        max_sample_error, cpu_ms, total_ms / iterations, max_ms);
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
    gboolean scale_mode = argc > 1 && !strcmp(argv[1], "--scale");
    if (argc != 1) {
        gboolean raw_mode = !strcmp(argv[1], "--raw");
        gboolean synthetic_mode = !strcmp(argv[1], "--synthetic");
        int first = raw_mode ? 3 : 2;
        if ((!raw_mode && !synthetic_mode && !scale_mode) ||
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
            ok = run_frame(gpu, NULL, width, height, width, height, input_stride, output_stride, 1);
        }
        const guint sizes[][4] = {{6, 6, 2, 2}, {130, 34, 66, 18},
            {3840, 2160, 3022, 1700}, {3840, 2160, 1920, 1080}, {3440, 1440, 2560, 1070}};
        for (guint i = 0; i < G_N_ELEMENTS(sizes) && ok; i++)
            ok = run_frame(gpu, NULL, sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3],
                ((sizes[i][0] * 5 / 2 + 15) & ~15u) + i % 3,
                (sizes[i][2] * 2 + 63) & ~63u, 2);
    } else if (scale_mode) {
        ok = run_frame(gpu, NULL, dimensions[0], dimensions[1], dimensions[2], dimensions[3],
            (dimensions[0] * 5 / 2 + 15) & ~15u, (dimensions[2] * 2 + 63) & ~63u, dimensions[4]);
    } else {
        ok = run_frame(gpu, raw_file, dimensions[0], dimensions[1], dimensions[0], dimensions[1],
                         dimensions[2], dimensions[3], dimensions[4]);
    }
    amly_gpu_free(gpu);
    return ok ? 0 : 1;

usage:
    g_printerr("usage: %s [--raw FILE | --synthetic] WIDTH HEIGHT INPUT_PITCH OUTPUT_PITCH [ITERATIONS]\n"
        "       %s --scale WIDTH HEIGHT OUT_WIDTH OUT_HEIGHT [ITERATIONS]\n"
        "       %s  # synthetic geometry/color/padding suite on real Mali-G52\n",
        argv[0], argv[0], argv[0]);
    return 2;
}
