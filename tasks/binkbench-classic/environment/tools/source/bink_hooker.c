/*
 * ============================================================================
 *  BinkHooker v2.7
 *  Universal frame capture hook for BinkPlayer64
 * ============================================================================
 * 
 *  Build Instructions:
 *    gcc -shared -fPIC -O2 -fno-omit-frame-pointer \
 *        -o bink_hooker.so bink_hooker.c \
 *        -ldl -lX11 -lXext -lGL -lz -lpthread
 * 
 *  Run Environment Examples (Capture cropped PNGs, discarding BMPs/Metadata):
 *    BINK_DEBUG=1 BINK_TRACE=1 BINK_BACKTRACE=1 \
 *    BINK_DUMP_PNG=1 BINK_DUMP_BMP=0 BINK_WRITE_META=0 \
 *    BINK_CROP_WIDTH=1920 BINK_CROP_HEIGHT=1080 \
 *    LD_PRELOAD=./bink_hooker.so \
 *    xvfb-run -a ./BinkPlayer64 -n -a file.bk2
 * 
 * ============================================================================
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <zlib.h>

#include <dlfcn.h>
#include <execinfo.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <GL/gl.h>
#include <GL/glx.h>

/*
 * ============================================================================
 *  CONFIGURATION & GLOBALS
 * ============================================================================
 */

typedef enum {
    GL_CAPTURE_BEFORE,
    GL_CAPTURE_AFTER
} GLCapturePoint;

typedef struct {
    char output_dir[PATH_MAX];
    int debug;
    int trace;
    int backtrace;
    int dump_bmp;
    int dump_raw;
    int dump_png;
    int write_meta;
    int max_frames;
    int filter_window;
    int capture_every_n;
    GLCapturePoint gl_capture_point;
    int crop_width;
    int crop_height;
} HookConfig;

static HookConfig g_cfg = {
    .output_dir = "./bink_frames",
    .debug = 0,
    .trace = 0,
    .backtrace = 0,
    .dump_bmp = 1,
    .dump_raw = 0,
    .dump_png = 0,
    .write_meta = 0, /* Disabled by default */
    .max_frames = 0,
    .filter_window = 1,
    .capture_every_n = 1,
    .gl_capture_point = GL_CAPTURE_BEFORE,
    .crop_width = 0,
    .crop_height = 0
};

static unsigned long g_frame = 0;
static unsigned long g_seen = 0;
static unsigned long g_capture_call_count = 0;

static int g_initialized = 0;
static int g_is_target = 0;
static int g_padding_logged = 0;
static int g_mask_warning_logged = 0;

static Drawable g_video_window = 0;
static Display *g_main_display = NULL;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;

/* Background Fallback Thread Controls */
static pthread_t g_fallback_thread;
static int g_run_fallback = 0;

/* Thread-local reentrancy guard for symbol resolution */
static __thread int g_resolving_symbols = 0;

/*
 * ============================================================================
 *  REAL FUNCTION SYMBOLS
 * ============================================================================
 */

static Display *(*real_XOpenDisplay)(const char *) = NULL;

static int (*real_XPutImage)(
    Display *, Drawable, GC, XImage *, int, int, int, int, unsigned int, unsigned int
) = NULL;

static Status (*real_XShmPutImage)(
    Display *, Drawable, GC, XImage *, int, int, int, int, unsigned int, unsigned int, Bool
) = NULL;

static void (*real_glXSwapBuffers)(
    Display *, GLXDrawable
) = NULL;

static void (*real_glGetIntegerv)(
    GLenum, GLint *
) = NULL;

static void (*real_glPixelStorei)(
    GLenum, GLint
) = NULL;

static void (*real_glReadPixels)(
    GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *
) = NULL;

static void (*real_glFinish)(
    void
) = NULL;

static void (*real_glTexImage2D)(
    GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *
) = NULL;

static void (*real_glTexSubImage2D)(
    GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *
) = NULL;

/* Forward Declarations */
static void capture_ximage(XImage *image, const char *source, const char *caller);
static void format_caller_string(void *ret, char *buf, size_t buf_len);
static void capture_gl_texture_data(int width, int height, GLenum format, GLenum type, const void *pixels, const char *source, const char *caller);

/*
 * ============================================================================
 *  SYMBOL RESOLUTION
 * ============================================================================
 */

static void resolve_symbols() {
    if (g_resolving_symbols) {
        return;
    }
    g_resolving_symbols = 1;

    if (!real_XOpenDisplay)
        real_XOpenDisplay = dlsym(RTLD_NEXT, "XOpenDisplay");

    if (!real_XPutImage) 
        real_XPutImage = dlsym(RTLD_NEXT, "XPutImage");

    if (!real_XShmPutImage)
        real_XShmPutImage = (Status (*)(Display *, Drawable, GC, XImage *, int, int, int, int, unsigned int, unsigned int, Bool))dlsym(RTLD_NEXT, "XShmPutImage");

    if (!real_glXSwapBuffers)
        real_glXSwapBuffers = (void (*)(Display *, GLXDrawable))dlsym(RTLD_NEXT, "glXSwapBuffers");

    if (!real_glGetIntegerv)
        real_glGetIntegerv = (void (*)(GLenum, GLint *))dlsym(RTLD_NEXT, "glGetIntegerv");

    if (!real_glPixelStorei)
        real_glPixelStorei = (void (*)(GLenum, GLint))dlsym(RTLD_NEXT, "glPixelStorei");

    if (!real_glReadPixels)
        real_glReadPixels = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))dlsym(RTLD_NEXT, "glReadPixels");

    if (!real_glFinish)
        real_glFinish = dlsym(RTLD_NEXT, "glFinish");

    if (!real_glTexImage2D)
        real_glTexImage2D = dlsym(RTLD_NEXT, "glTexImage2D");

    if (!real_glTexSubImage2D)
        real_glTexSubImage2D = dlsym(RTLD_NEXT, "glTexSubImage2D");

    g_resolving_symbols = 0;
}

/*
 * ============================================================================
 *  PROCESS DETECTION & WINDOW TRACKING
 * ============================================================================
 */

static int is_target_process() {
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) {
        fprintf(stderr, "[HOOK] Failed to resolve /proc/self/exe\n");
        return 0;
    }
    path[len] = '\0';

    fprintf(stderr, "[HOOK] Loaded into %s\n", path);

    if (getenv("BINK_FORCE_HOOK")) {
        return 1;
    }

    if (strstr(path, "BinkPlayer64") || strstr(path, "BinkPlayer")) {
        return 1;
    }

    return 0;
}

static void track_drawable(Drawable d, int width, int height) {
    pthread_mutex_lock(&g_lock);
    if (g_video_window == 0) {
        if (width >= 320 && height >= 240) {
            g_video_window = d;
            fprintf(stderr, "[HOOK] Auto-detected video window Drawable ID: 0x%lx (%dx%d)\n", (unsigned long)d, width, height);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static int should_capture_drawable(Drawable d) {
    pthread_mutex_lock(&g_lock);
    Drawable target = g_video_window;
    pthread_mutex_unlock(&g_lock);

    if (target == 0) {
        return 1;
    }
    return (d == target);
}

/*
 * ============================================================================
 *  UTILITIES
 * ============================================================================
 */

static uint32_t calculate_crc32(const unsigned char *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static void format_caller_string(void *ret, char *buf, size_t buf_len) {
    Dl_info info;
    if (dladdr(ret, &info)) {
        const char *fname = info.dli_fname;
        const char *slash = strrchr(fname, '/');
        if (slash) {
            fname = slash + 1;
        }
        uintptr_t offset = (uintptr_t)ret - (uintptr_t)info.dli_fbase;
        snprintf(buf, buf_len, "%s+0x%lX", fname, (unsigned long)offset);
    } else {
        snprintf(buf, buf_len, "%p", ret);
    }
}

static void log_caller(const char *hook_name, void *ret, const char *caller_str) {
    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "[HOOK][%s] Caller: %s (Address: %p)\n", hook_name, caller_str, ret);

    if (g_cfg.backtrace) {
        void *frames[16];
        int n = backtrace(frames, 16);
        backtrace_symbols_fd(frames, n, STDERR_FILENO);
    }
    pthread_mutex_unlock(&g_lock);
}

static void *get_ximage_pixels(XImage *image) {
    if (!image) {
        return NULL;
    }
    /* Look up MIT-SHM memory mappings prior to general structure offsets */
    if (image->obdata) {
        XShmSegmentInfo *shm = (XShmSegmentInfo *)image->obdata;
        if (shm && shm->shmaddr) {
            return shm->shmaddr;
        }
    }
    return image->data;
}

/*
 * ============================================================================
 *  XIMAGE DECODER
 * ============================================================================
 */

static uint8_t scale_channel(uint32_t value, uint32_t mask) {
    if (!mask) return 0;

    int shift = 0;
    while (!(mask & 1)) {
        mask >>= 1;
        shift++;
    }

    uint32_t max = mask;
    value = (value >> shift) & max;

    return (uint8_t)((value * 255) / max);
}

static void decode_pixel(uint32_t pixel, XImage *img, unsigned char *rgb) {
    rgb[0] = scale_channel(pixel, img->red_mask);
    rgb[1] = scale_channel(pixel, img->green_mask);
    rgb[2] = scale_channel(pixel, img->blue_mask);
}

/*
 * ============================================================================
 *  IMAGE WRITERS
 * ============================================================================
 */

static void write_u32(unsigned char *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static int write_bmp(const char *file, int width, int height, unsigned char *rgb) {
    FILE *fp = fopen(file, "wb");
    if (!fp) return -1;

    int stride = (width * 3 + 3) & ~3;
    int size = stride * height;
    unsigned char hdr[54] = {0};

    hdr[0] = 'B';
    hdr[1] = 'M';
    write_u32(hdr + 2, 54 + size);
    write_u32(hdr + 10, 54);
    write_u32(hdr + 14, 40);
    write_u32(hdr + 18, width);
    write_u32(hdr + 22, height);
    hdr[26] = 1;
    hdr[28] = 24;

    if (fwrite(hdr, 1, 54, fp) != 54) {
        fclose(fp);
        return -1;
    }

    unsigned char *row = calloc(1, stride);
    if (!row) {
        fclose(fp);
        return -1;
    }

    int write_error = 0;
    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = rgb[(y * width + x) * 3 + 2]; // B
            row[x * 3 + 1] = rgb[(y * width + x) * 3 + 1]; // G
            row[x * 3 + 2] = rgb[(y * width + x) * 3 + 0]; // R
        }
        if (fwrite(row, 1, stride, fp) != (size_t)stride) {
            write_error = 1;
            break;
        }
    }

    free(row);
    fclose(fp);
    return write_error ? -1 : 0;
}

static int write_raw(const char *file, int width, int height, unsigned char *rgb) {
    FILE *fp = fopen(file, "wb");
    if (!fp) return -1;

    size_t size = (size_t)width * height * 3;
    size_t written = fwrite(rgb, 1, size, fp);
    fclose(fp);
    return (written == size) ? 0 : -1;
}

static void png_write_chunk(FILE *fp, const char *type, const unsigned char *data, uint32_t len) {
    unsigned char len_be[4];
    len_be[0] = (len >> 24) & 0xFF;
    len_be[1] = (len >> 16) & 0xFF;
    len_be[2] = (len >> 8) & 0xFF;
    len_be[3] = len & 0xFF;
    fwrite(len_be, 1, 4, fp);

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const unsigned char *)type, 4);
    if (len > 0) {
        crc = crc32(crc, data, len);
    }

    fwrite(type, 1, 4, fp);
    if (len > 0) {
        fwrite(data, 1, len, fp);
    }

    unsigned char crc_be[4];
    crc_be[0] = (crc >> 24) & 0xFF;
    crc_be[1] = (crc >> 16) & 0xFF;
    crc_be[2] = (crc >> 8) & 0xFF;
    crc_be[3] = crc & 0xFF;
    fwrite(crc_be, 1, 4, fp);
}

static int write_png(const char *file, int width, int height, unsigned char *rgb) {
    FILE *fp = fopen(file, "wb");
    if (!fp) {
        fprintf(stderr, "[HOOK][PNG-ERROR] Failed to open target file %s\n", file);
        return -1;
    }

    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (fwrite(sig, 1, 8, fp) != 8) {
        fprintf(stderr, "[HOOK][PNG-ERROR] Failed to write file signature\n");
        fclose(fp);
        return -1;
    }

    unsigned char ihdr[13];
    ihdr[0] = (width >> 24) & 0xFF;
    ihdr[1] = (width >> 16) & 0xFF;
    ihdr[2] = (width >> 8) & 0xFF;
    ihdr[3] = width & 0xFF;
    ihdr[4] = (height >> 24) & 0xFF;
    ihdr[5] = (height >> 16) & 0xFF;
    ihdr[6] = (height >> 8) & 0xFF;
    ihdr[7] = height & 0xFF;
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* color type: RGB */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    png_write_chunk(fp, "IHDR", ihdr, 13);

    /* Construct scanlines with a filter-byte prefix (0 = no filter) */
    size_t row_bytes = (size_t)width * 3;
    size_t raw_size = (row_bytes + 1) * (size_t)height;
    unsigned char *raw = malloc(raw_size);
    if (!raw) {
        fprintf(stderr, "[HOOK][PNG-ERROR] Failed to allocate raw scratch memory of size %lu\n", (unsigned long)raw_size);
        fclose(fp);
        return -1;
    }

    for (int y = 0; y < height; y++) {
        unsigned char *dst_row = raw + y * (row_bytes + 1);
        dst_row[0] = 0;
        memcpy(dst_row + 1, rgb + (size_t)y * row_bytes, row_bytes);
    }

    uLongf comp_bound = compressBound(raw_size);
    unsigned char *comp = malloc(comp_bound);
    if (!comp) {
        fprintf(stderr, "[HOOK][PNG-ERROR] Failed to allocate compressed container of size %lu\n", (unsigned long)comp_bound);
        free(raw);
        fclose(fp);
        return -1;
    }

    /* Optimized level to prevent CPU hogging/timing context interrupts */
    int zret = compress2(comp, &comp_bound, raw, raw_size, Z_BEST_SPEED);
    free(raw);
    if (zret != Z_OK) {
        fprintf(stderr, "[HOOK][PNG-ERROR] compress2 failed with code %d\n", zret);
        free(comp);
        fclose(fp);
        return -1;
    }

    png_write_chunk(fp, "IDAT", comp, (uint32_t)comp_bound);
    free(comp);

    png_write_chunk(fp, "IEND", NULL, 0);

    fclose(fp);
    return 0;
}

static int write_frame_meta(const char *file, int width, int height, int uncropped_w, int uncropped_h, uint32_t crc, const char *source, const char *caller) {
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s.meta", file);

    FILE *fp = fopen(meta_path, "w");
    if (!fp) return -1;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t pts_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    fprintf(fp, "width=%d\n", width);
    fprintf(fp, "height=%d\n", height);
    if (uncropped_w != width || uncropped_h != height) {
        fprintf(fp, "uncropped_width=%d\n", uncropped_w);
        fprintf(fp, "uncropped_height=%d\n", uncropped_h);
    }
    fprintf(fp, "format=RGB24\n");
    fprintf(fp, "stride=%d\n", width * 3);
    fprintf(fp, "crc=%08X\n", crc);
    fprintf(fp, "source=%s\n", source);
    fprintf(fp, "caller=%s\n", caller);
    fprintf(fp, "pts_ms=%lu\n", (unsigned long)pts_ms);

    fclose(fp);
    return 0;
}

/*
 * ============================================================================
 *  UNIFIED CAPTURE API
 * ============================================================================
 */

static void capture_rgb_frame(int width, int height, unsigned char *rgb, const char *source, const char *caller) {
    pthread_mutex_lock(&g_lock);
    unsigned long call_id = g_capture_call_count++;
    int every_n = g_cfg.capture_every_n;
    pthread_mutex_unlock(&g_lock);

    /* Early decimation to preserve pipeline pacing */
    if (every_n > 1 && (call_id % every_n) != 0) {
        return;
    }

    pthread_mutex_lock(&g_lock);
    if (g_cfg.max_frames > 0 && g_frame >= g_cfg.max_frames) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_mutex_unlock(&g_lock);

    /* Sequential write phase protected by a dedicated write mutex to prevent sequence gaps */
    pthread_mutex_lock(&g_write_lock);
    
    pthread_mutex_lock(&g_lock);
    if (g_cfg.max_frames > 0 && g_frame >= g_cfg.max_frames) {
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_unlock(&g_write_lock);
        return;
    }
    unsigned long current_frame = g_frame;
    int crop_target_w = g_cfg.crop_width;
    int crop_target_h = g_cfg.crop_height;
    pthread_mutex_unlock(&g_lock);

    /* Apply top-left window cropping dynamically if user parameters are specified */
    unsigned char *processed_rgb = rgb;
    int final_w = width;
    int final_h = height;
    int allocated_processed = 0;

    if (crop_target_w > 0 && crop_target_h > 0 && crop_target_w <= width && crop_target_h <= height) {
        processed_rgb = malloc((size_t)crop_target_w * crop_target_h * 3);
        if (processed_rgb) {
            final_w = crop_target_w;
            final_h = crop_target_h;
            allocated_processed = 1;
            for (int y = 0; y < crop_target_h; y++) {
                memcpy(processed_rgb + y * crop_target_w * 3, rgb + y * width * 3, crop_target_w * 3);
            }
        } else {
            processed_rgb = rgb;
        }
    }

    uint32_t crc = calculate_crc32(processed_rgb, (size_t)final_w * final_h * 3);
    char filename[PATH_MAX];
    int write_ok = 0;

    if (g_cfg.dump_bmp) {
        snprintf(filename, sizeof(filename), "%s/frame_%06lu.bmp", g_cfg.output_dir, current_frame);
        if (write_bmp(filename, final_w, final_h, processed_rgb) == 0) {
            if (g_cfg.write_meta) {
                write_frame_meta(filename, final_w, final_h, width, height, crc, source, caller);
            }
            write_ok = 1;
            if (g_cfg.debug) {
                fprintf(stderr, "[HOOK] Saved %s (%dx%d) CRC:%08X\n", filename, final_w, final_h, crc);
            }
        } else {
            fprintf(stderr, "[HOOK] Error writing BMP payload: %s\n", filename);
        }
    }

    if (g_cfg.dump_raw) {
        snprintf(filename, sizeof(filename), "%s/frame_%06lu.rgb", g_cfg.output_dir, current_frame);
        if (write_raw(filename, final_w, final_h, processed_rgb) == 0) {
            if (g_cfg.write_meta) {
                write_frame_meta(filename, final_w, final_h, width, height, crc, source, caller);
            }
            write_ok = 1;
            if (g_cfg.debug) {
                fprintf(stderr, "[HOOK] Saved raw data payload and metadata companion for frame index %06lu\n", current_frame);
            }
        } else {
            fprintf(stderr, "[HOOK] Error writing raw dump file: %s\n", filename);
        }
    }

    if (g_cfg.dump_png) {
        snprintf(filename, sizeof(filename), "%s/frame_%06lu.png", g_cfg.output_dir, current_frame);

        fprintf(stderr, "[HOOK] PNG attempt %s (%dx%d)\n", filename, final_w, final_h);

        int png_result = write_png(filename, final_w, final_h, processed_rgb);

        fprintf(stderr, "[HOOK] PNG result=%d\n", png_result);

        if (png_result == 0) {
            if (g_cfg.write_meta) {
                write_frame_meta(filename, final_w, final_h, width, height, crc, source, caller);
            }
            write_ok = 1;
        } else {
            fprintf(stderr, "[HOOK] Error writing PNG payload: %s\n", filename);
        }
    }

    /* Only increment frame counter upon verified disk I/O confirmation */
    if (write_ok || (!g_cfg.dump_bmp && !g_cfg.dump_raw && !g_cfg.dump_png)) {
        pthread_mutex_lock(&g_lock);
        g_frame++;
        pthread_mutex_unlock(&g_lock);
    }

    pthread_mutex_lock(&g_lock);
    if (height != final_h && !g_padding_logged) {
        g_padding_logged = 1;
        fprintf(stderr, "[HOOK] Codec padding detected: actual=%dx%d, cropped output to %dx%d\n", width, height, final_w, final_h);
    }
    pthread_mutex_unlock(&g_lock);

    if (allocated_processed) {
        free(processed_rgb);
    }

    pthread_mutex_unlock(&g_write_lock);
}

static void capture_ximage(XImage *image, const char *source, const char *caller) {
    if (!image) return;

    void *pixel_data = get_ximage_pixels(image);
    if (!pixel_data) return;

    int width = image->width;
    int height = image->height;

    pthread_mutex_lock(&g_lock);
    unsigned long current_seen = g_seen;
    pthread_mutex_unlock(&g_lock);

    if (g_cfg.debug) {
        fprintf(stderr,
            "[HOOK][X11]\n"
            "  Frame Seen: %lu\n"
            "  Image:      %p\n"
            "  Data Ref:   %p\n"
            "  Size:       %dx%d\n"
            "  Depth:      %d\n"
            "  BPP:        %d\n"
            "  Stride:     %d\n"
            "  Byte Order: %s\n"
            "  Masks:      R=%08lx G=%08lx B=%08lx\n",
            current_seen,
            (void*)image,
            pixel_data,
            width,
            height,
            image->depth,
            image->bits_per_pixel,
            image->bytes_per_line,
            image->byte_order == MSBFirst ? "MSBFirst" : "LSBFirst",
            image->red_mask,
            image->green_mask,
            image->blue_mask
        );
    }

    /* Rate-limited visual inspection log for zero-mask profiles */
    if (image->red_mask == 0 && image->green_mask == 0 && image->blue_mask == 0) {
        pthread_mutex_lock(&g_lock);
        if (!g_mask_warning_logged) {
            g_mask_warning_logged = 1;
            fprintf(stderr, "[HOOK] WARNING: No RGB masks populated in XImage. Assuming default XRGB8888/BGRA (32bpp) or RGB24 (24bpp) visual layout.\n");
            
            unsigned char *p = (unsigned char *)pixel_data;
            if (p && width > 0 && height > 0) {
                fprintf(stderr,
                    "[HOOK] First raw bytes in buffer: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    p[0], p[1], p[2], p[3],
                    p[4], p[5], p[6], p[7]);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }

    unsigned char *rgb = malloc((size_t)width * height * 3);
    if (!rgb) return;

    int bytes_per_pixel = image->bits_per_pixel / 8;
    if (bytes_per_pixel == 0) bytes_per_pixel = 1;

    for (int y = 0; y < height; y++) {
        unsigned char *src = (unsigned char *)pixel_data + y * image->bytes_per_line;
        for (int x = 0; x < width; x++) {
            unsigned char *out = rgb + (y * width + x) * 3;

            /* Check and intercept unmapped visual layouts */
            if (image->bits_per_pixel == 32 &&
                image->red_mask == 0 &&
                image->green_mask == 0 &&
                image->blue_mask == 0)
            {
                /* Fallback decoder for typical 32-bit XRGB8888 / BGRA on LSBFirst */
                unsigned char *src_pixel = src + x * 4;
                out[0] = src_pixel[2]; // R
                out[1] = src_pixel[1]; // G
                out[2] = src_pixel[0]; // B
            }
            else if (image->bits_per_pixel == 24 &&
                     image->red_mask == 0 &&
                     image->green_mask == 0 &&
                     image->blue_mask == 0)
            {
                /* Fallback decoder for standard 24-bit RGB888 / BGR888 on LSBFirst */
                unsigned char *src_pixel = src + x * 3;
                out[0] = src_pixel[2]; // R
                out[1] = src_pixel[1]; // G
                out[2] = src_pixel[0]; // B
            }
            else
            {
                uint32_t pixel = 0;
                int copy_len = bytes_per_pixel < 4 ? bytes_per_pixel : 4;
                memcpy(&pixel, src + x * bytes_per_pixel, copy_len);

                /* Correct host vs X Server pixel configurations */
                if (image->byte_order == MSBFirst) {
                    if (bytes_per_pixel == 4) {
                        pixel = __builtin_bswap32(pixel);
                    } else if (bytes_per_pixel == 2) {
                        pixel = __builtin_bswap16((uint16_t)pixel);
                    } else if (bytes_per_pixel == 3) {
                        uint32_t b0 = pixel & 0xFF;
                        uint32_t b1 = (pixel >> 8) & 0xFF;
                        uint32_t b2 = (pixel >> 16) & 0xFF;
                        pixel = (b0 << 16) | (b1 << 8) | b2;
                    }
                }

                decode_pixel(pixel, image, out);
            }
        }
    }

    capture_rgb_frame(width, height, rgb, source, caller);
    free(rgb);
}

/*
 * ============================================================================
 *  FALLBACK THREAD CAPTURE (XGETIMAGE)
 * ============================================================================
 */

static void *fallback_capture_thread(void *arg) {
    Display *dpy = NULL;
    int poll_ms = atoi(getenv("BINK_FALLBACK_POLL_MS") ? getenv("BINK_FALLBACK_POLL_MS") : "0");
    if (poll_ms <= 0) {
        return NULL;
    }

    /* Wait briefly for process initialization and Display hooks to register */
    usleep(500000);

    pthread_mutex_lock(&g_lock);
    dpy = g_main_display;
    pthread_mutex_unlock(&g_lock);

    /* Fallback to dedicated display instance if main handle hasn't registered */
    if (!dpy) {
        dpy = XOpenDisplay(NULL);
        if (!dpy) {
            fprintf(stderr, "[HOOK][FALLBACK] Could not establish local display connection\n");
            return NULL;
        }
    }

    fprintf(stderr, "[HOOK][FALLBACK] Polling active video window every %d ms\n", poll_ms);

    while (g_run_fallback) {
        usleep(poll_ms * 1000);

        pthread_mutex_lock(&g_lock);
        Drawable target = g_video_window;
        pthread_mutex_unlock(&g_lock);

        if (!target) {
            continue;
        }

        Window root;
        int x, y;
        unsigned int width = 0, height = 0, border = 0, depth = 0;

        /* Verify active geometry bounds on display reference safely */
        if (XGetGeometry(dpy, target, &root, &x, &y, &width, &height, &border, &depth)) {
            if (width > 0 && height > 0) {
                XImage *img = XGetImage(dpy, target, 0, 0, width, height, AllPlanes, ZPixmap);
                if (img) {
                    char caller_str[64];
                    snprintf(caller_str, sizeof(caller_str), "FallbackThread+0x0");

                    pthread_mutex_lock(&g_lock);
                    g_seen++;
                    pthread_mutex_unlock(&g_lock);

                    capture_ximage(img, "XGetImageFallback", caller_str);
                    XDestroyImage(img);
                }
            }
        }
    }

    /* Close only if we opened a unique instance */
    pthread_mutex_lock(&g_lock);
    int is_unique = (dpy != g_main_display);
    pthread_mutex_unlock(&g_lock);

    if (is_unique && dpy) {
        XCloseDisplay(dpy);
    }
    return NULL;
}

/*
 * ============================================================================
 *  OPENGL TEXTURE CAPTURE
 * ============================================================================
 */

static void capture_gl_texture_data(int width, int height, GLenum format, GLenum type, const void *pixels, const char *source, const char *caller) {
    if (type != GL_UNSIGNED_BYTE) {
        return;
    }

    unsigned char *rgb = malloc((size_t)width * height * 3);
    if (!rgb) return;

    const unsigned char *src = (const unsigned char *)pixels;
    int valid = 0;

    if (format == GL_RGB) {
        memcpy(rgb, src, (size_t)width * height * 3);
        valid = 1;
    } else if (format == GL_BGR) {
        for (int i = 0; i < width * height; i++) {
            rgb[i * 3 + 0] = src[i * 3 + 2];
            rgb[i * 3 + 1] = src[i * 3 + 1];
            rgb[i * 3 + 2] = src[i * 3 + 0];
        }
        valid = 1;
    } else if (format == GL_RGBA) {
        for (int i = 0; i < width * height; i++) {
            rgb[i * 3 + 0] = src[i * 4 + 0];
            rgb[i * 3 + 1] = src[i * 4 + 1];
            rgb[i * 3 + 2] = src[i * 4 + 2];
        }
        valid = 1;
    } else if (format == GL_BGRA) {
        for (int i = 0; i < width * height; i++) {
            rgb[i * 3 + 0] = src[i * 4 + 2];
            rgb[i * 3 + 1] = src[i * 4 + 1];
            rgb[i * 3 + 2] = src[i * 4 + 0];
        }
        valid = 1;
    } else if (format == GL_RED || format == GL_LUMINANCE) {
        /* Standard gray channel translation mapping (typical of single YUV plane decodes) */
        for (int i = 0; i < width * height; i++) {
            rgb[i * 3 + 0] = src[i];
            rgb[i * 3 + 1] = src[i];
            rgb[i * 3 + 2] = src[i];
        }
        valid = 1;
    }

    if (valid) {
        capture_rgb_frame(width, height, rgb, source, caller);
    }

    free(rgb);
}

/*
 * ============================================================================
 *  INTERCEPTED FUNCTIONS (HOOKS)
 * ============================================================================
 */

Display *XOpenDisplay(const char *display_name) {
    resolve_symbols();
    if (!real_XOpenDisplay) return NULL;

    Display *dpy = real_XOpenDisplay(display_name);
    if (dpy && g_is_target) {
        pthread_mutex_lock(&g_lock);
        if (!g_main_display) {
            g_main_display = dpy;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return dpy;
}

int XPutImage(
    Display *display, Drawable drawable, GC gc, XImage *image,
    int src_x, int src_y, int dst_x, int dst_y,
    unsigned int width, unsigned int height
) {
    resolve_symbols();
    if (!real_XPutImage) return 0;

    track_drawable(drawable, image->width, image->height);

    if (!g_is_target || (g_cfg.filter_window && !should_capture_drawable(drawable))) {
        return real_XPutImage(display, drawable, gc, image, src_x, src_y, dst_x, dst_y, width, height);
    }

    void *ret = __builtin_return_address(0);
    char caller_str[256];
    format_caller_string(ret, caller_str, sizeof(caller_str));

    pthread_mutex_lock(&g_lock);
    g_seen++;
    pthread_mutex_unlock(&g_lock);

    if (g_cfg.trace || g_cfg.debug) {
        log_caller("XPutImage", ret, caller_str);
    }

    capture_ximage(image, "XPutImage", caller_str);

    return real_XPutImage(display, drawable, gc, image, src_x, src_y, dst_x, dst_y, width, height);
}

Status XShmPutImage(
    Display *display, Drawable drawable, GC gc, XImage *image,
    int src_x, int src_y, int dst_x, int dst_y,
    unsigned int width, unsigned int height, Bool send_event
) {
    resolve_symbols();
    if (!real_XShmPutImage) return 0;

    track_drawable(drawable, image->width, image->height);

    if (!g_is_target || (g_cfg.filter_window && !should_capture_drawable(drawable))) {
        return real_XShmPutImage(display, drawable, gc, image, src_x, src_y, dst_x, dst_y, width, height, send_event);
    }

    void *ret = __builtin_return_address(0);
    char caller_str[256];
    format_caller_string(ret, caller_str, sizeof(caller_str));

    pthread_mutex_lock(&g_lock);
    g_seen++;
    pthread_mutex_unlock(&g_lock);

    if (g_cfg.trace || g_cfg.debug) {
        log_caller("XShmPutImage", ret, caller_str);
    }

    capture_ximage(image, "XShmPutImage", caller_str);

    return real_XShmPutImage(display, drawable, gc, image, src_x, src_y, dst_x, dst_y, width, height, send_event);
}

/*
 * ============================================================================
 *  GLSWAP / GL PIXEL & TEXTURE HOOKS
 * ============================================================================
 */

static void trigger_gl_capture(const char *caller_str) {
    if (real_glGetIntegerv && real_glReadPixels) {
        GLint viewport[4];
        real_glGetIntegerv(GL_VIEWPORT, viewport);

        int width = viewport[2];
        int height = viewport[3];

        if (width > 0 && height > 0) {
            unsigned char *pixels = malloc((size_t)width * height * 3);
            if (pixels) {
                GLint prev_alignment = 4;
                if (real_glGetIntegerv) {
                    real_glGetIntegerv(GL_PACK_ALIGNMENT, &prev_alignment);
                }
                if (real_glPixelStorei) {
                    real_glPixelStorei(GL_PACK_ALIGNMENT, 1);
                }

                if (real_glFinish) {
                    real_glFinish();
                }

                real_glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);

                if (real_glPixelStorei) {
                    real_glPixelStorei(GL_PACK_ALIGNMENT, prev_alignment);
                }

                /* glReadPixels captures bottom-up; flip vertically to align with standard top-down assumptions */
                unsigned char *flipped = malloc((size_t)width * height * 3);
                if (flipped) {
                    for (int y = 0; y < height; y++) {
                        memcpy(flipped + y * width * 3, pixels + (height - 1 - y) * width * 3, width * 3);
                    }
                    capture_rgb_frame(width, height, flipped, "glXSwapBuffers", caller_str);
                    free(flipped);
                } else {
                    capture_rgb_frame(width, height, pixels, "glXSwapBuffers", caller_str);
                }

                free(pixels);
            }
        }
    }
}

void glXSwapBuffers(Display *display, GLXDrawable drawable) {
    resolve_symbols();

    if (!g_is_target) {
        if (real_glXSwapBuffers) {
            real_glXSwapBuffers(display, drawable);
        }
        return;
    }

    void *ret = __builtin_return_address(0);
    char caller_str[256];
    format_caller_string(ret, caller_str, sizeof(caller_str));

    pthread_mutex_lock(&g_lock);
    g_seen++;
    pthread_mutex_unlock(&g_lock);

    if (g_cfg.trace || g_cfg.debug) {
        log_caller("glXSwapBuffers", ret, caller_str);
    }

    if (g_cfg.gl_capture_point == GL_CAPTURE_BEFORE) {
        trigger_gl_capture(caller_str);
    }

    if (real_glXSwapBuffers) {
        real_glXSwapBuffers(display, drawable);
    }

    if (g_cfg.gl_capture_point == GL_CAPTURE_AFTER) {
        trigger_gl_capture(caller_str);
    }
}

void glTexImage2D(
    GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
    GLint border, GLenum format, GLenum type, const void *pixels
) {
    resolve_symbols();
    if (!real_glTexImage2D) return;

    if (g_is_target && pixels && width > 0 && height > 0) {
        void *ret = __builtin_return_address(0);
        char caller_str[256];
        format_caller_string(ret, caller_str, sizeof(caller_str));

        pthread_mutex_lock(&g_lock);
        g_seen++;
        pthread_mutex_unlock(&g_lock);

        if (g_cfg.trace || g_cfg.debug) {
            pthread_mutex_lock(&g_lock);
            fprintf(stderr, "[HOOK][glTexImage2D] target=0x%x internalformat=0x%x size=%dx%d format=0x%x type=0x%x caller=%s\n",
                    target, internalformat, width, height, format, type, caller_str);
            pthread_mutex_unlock(&g_lock);
        }

        if (width >= 320 && height >= 240) {
            capture_gl_texture_data(width, height, format, type, pixels, "glTexImage2D", caller_str);
        }
    }

    real_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

void glTexSubImage2D(
    GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *pixels
) {
    resolve_symbols();
    if (!real_glTexSubImage2D) return;

    if (g_is_target && pixels && width > 0 && height > 0) {
        void *ret = __builtin_return_address(0);
        char caller_str[256];
        format_caller_string(ret, caller_str, sizeof(caller_str));

        pthread_mutex_lock(&g_lock);
        g_seen++;
        pthread_mutex_unlock(&g_lock);

        if (g_cfg.trace || g_cfg.debug) {
            pthread_mutex_lock(&g_lock);
            fprintf(stderr, "[HOOK][glTexSubImage2D] target=0x%x offset=%d,%d size=%dx%d format=0x%x type=0x%x caller=%s\n",
                    target, xoffset, yoffset, width, height, format, type, caller_str);
            pthread_mutex_unlock(&g_lock);
        }

        if (width >= 320 && height >= 240) {
            capture_gl_texture_data(width, height, format, type, pixels, "glTexSubImage2D", caller_str);
        }
    }

    real_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

/*
 * ============================================================================
 *  INITIALIZATION & DESTRUCTION
 * ============================================================================
 */

__attribute__((constructor)) static void hook_init() {
    pthread_mutex_lock(&g_lock);
    if (g_initialized) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_initialized = 1;
    pthread_mutex_unlock(&g_lock);

    /* Run process detection outside of locked state */
    int is_target = is_target_process();
    g_is_target = is_target;

    if (!is_target) {
        resolve_symbols();
        return;
    }

    /* Locked environment variables mapping */
    pthread_mutex_lock(&g_lock);
    const char *v;
    if ((v = getenv("BINK_DUMP_DIR"))) {
        strncpy(g_cfg.output_dir, v, sizeof(g_cfg.output_dir) - 1);
        g_cfg.output_dir[sizeof(g_cfg.output_dir) - 1] = '\0';
    }

    if ((v = getenv("BINK_DEBUG"))) {
        g_cfg.debug = atoi(v);
    }
    if ((v = getenv("BINK_TRACE"))) {
        g_cfg.trace = atoi(v);
    }
    if ((v = getenv("BINK_BACKTRACE"))) {
        g_cfg.backtrace = atoi(v);
    }
    if ((v = getenv("BINK_MAX_FRAMES"))) {
        g_cfg.max_frames = atoi(v);
    }
    if ((v = getenv("BINK_DUMP_BMP"))) {
        g_cfg.dump_bmp = atoi(v);
    }
    if ((v = getenv("BINK_DUMP_RAW"))) {
        g_cfg.dump_raw = atoi(v);
    }
    if ((v = getenv("BINK_DUMP_PNG"))) {
        g_cfg.dump_png = atoi(v);
    }
    if ((v = getenv("BINK_WRITE_META"))) {
        g_cfg.write_meta = atoi(v);
    }
    if ((v = getenv("BINK_FILTER_WINDOW"))) {
        g_cfg.filter_window = atoi(v);
    }
    if ((v = getenv("BINK_CAPTURE_EVERY_N"))) {
        g_cfg.capture_every_n = atoi(v);
    }
    if ((v = getenv("BINK_WINDOW_ID"))) {
        g_video_window = (Drawable)strtoul(v, NULL, 0);
        fprintf(stderr, "[HOOK] Explicitly filtering to Window ID: 0x%lx\n", (unsigned long)g_video_window);
    }
    if ((v = getenv("BINK_GL_CAPTURE_POINT"))) {
        if (strcmp(v, "after") == 0) {
            g_cfg.gl_capture_point = GL_CAPTURE_AFTER;
        }
    }
    if ((v = getenv("BINK_CROP_WIDTH"))) {
        g_cfg.crop_width = atoi(v);
    }
    if ((v = getenv("BINK_CROP_HEIGHT"))) {
        g_cfg.crop_height = atoi(v);
    }
    pthread_mutex_unlock(&g_lock);

    /* Create target frames directory */
    mkdir(g_cfg.output_dir, 0755);

    /* Resolve required dynamic link library pointers safely */
    resolve_symbols();

    /* Spin up isolated background polling thread if specified */
    if (getenv("BINK_FALLBACK_POLL_MS")) {
        g_run_fallback = 1;
        pthread_create(&g_fallback_thread, NULL, fallback_capture_thread, NULL);
    }

    pthread_mutex_lock(&g_lock);
    fprintf(stderr,
        "[HOOK] BinkHooker v2.7 Loaded\n"
        "  Target Status:      Active\n"
        "  Output Directory:    %s\n"
        "  Dump BMP:            %s\n"
        "  Dump RAW:            %s\n"
        "  Dump PNG:            %s\n"
        "  Write Metadata:      %s\n"
        "  Max Frames:          %d\n"
        "  Window Filter:       %s\n"
        "  Every Nth Frame:     %d\n"
        "  GL Capture Phase:    %s\n"
        "  Cropping Target:     %dx%d\n"
        "  XPutImage:           %p\n"
        "  XShmPutImage:        %p\n"
        "  glXSwapBuffers:      %p\n"
        "  glTexImage2D:        %p\n"
        "  glTexSubImage2D:     %p\n",
        g_cfg.output_dir,
        g_cfg.dump_bmp ? "ON" : "OFF",
        g_cfg.dump_raw ? "ON" : "OFF",
        g_cfg.dump_png ? "ON" : "OFF",
        g_cfg.write_meta ? "ON" : "OFF",
        g_cfg.max_frames,
        g_cfg.filter_window ? "ON" : "OFF",
        g_cfg.capture_every_n,
        g_cfg.gl_capture_point == GL_CAPTURE_AFTER ? "AFTER" : "BEFORE",
        g_cfg.crop_width, g_cfg.crop_height,
        (void*)real_XPutImage,
        (void*)real_XShmPutImage,
        (void*)real_glXSwapBuffers,
        (void*)real_glTexImage2D,
        (void*)real_glTexSubImage2D
    );
    pthread_mutex_unlock(&g_lock);
}

__attribute__((destructor)) static void hook_exit() {
    if (g_is_target) {
        if (g_run_fallback) {
            g_run_fallback = 0;
            pthread_join(g_fallback_thread, NULL);
        }

        pthread_mutex_lock(&g_lock);
        fprintf(stderr,
            "[HOOK] Shutdown\n"
            "  Total Intercepted:  %lu\n"
            "  Total Stored:       %lu\n",
            g_seen,
            g_frame
        );
        pthread_mutex_unlock(&g_lock);
    }
}