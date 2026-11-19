/* Experimental CPU fallback for Pilot 2.0 Real3D RPCs on One Pro.
 * The vendor's 480-pixel depth map drives horizontal stereo disparity.
 * A failed ABI/layout check leaves the existing HDR path untouched.
 */
#include "runtime.h"
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
#include <arm_neon.h>
#endif
#ifdef XREAL_COMPARE_ONEPRO_WARP
#include "onepro_warp.h"
#endif
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned long size_t;
#define NULL ((void *)0)
extern int strcmp(const char *, const char *);
extern int snprintf(char *, size_t, const char *, ...);
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int rename(const char *, const char *);
extern int pthread_create(u64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(u64, void **);
struct timespec { long sec, nsec; };
extern int clock_gettime(int, struct timespec *);
extern int nanosleep(const struct timespec *, struct timespec *);

/* perf8 frame pacing. Pilot's 2D->3D path delivers ~48 frames/s, which the
 * 90 Hz OLED shows as an irregular mix of 1 and 2 refreshes (judder). Each
 * Process call now returns no earlier than PACE_US after the previous call
 * began, slightly under two 60 Hz source frames, so the pipeline locks onto
 * every second source frame: 30 frames/s, each shown exactly 3 refreshes.
 * The warp itself is unchanged (it finishes in ~15 ms). */
/* perf11: pacing DISABLED. On the device the even 30/s cadence raised
 * Pilot's synth latency from ~70 ms to 100-177 ms (frames queue while the
 * call sleeps) and head-locked 3D visibly lagged and jumped. 2D->3D frames
 * must return as soon as the warp is done. */
#define PACE_US 0ULL
static u64 pace_last_start, pace_slept_us, pace_late;

extern void *ar_hal_sys_mmap_cache(u64, u32);
extern int ar_hal_sys_munmap(void *, u32);
extern int ar_hal_sys_mmz_invalid_cache(u64, void *, u32);
extern int ar_hal_sys_mmz_flush_cache_pa(u64, u32);

struct rpc {
    char name[32];
    u64 reserved;
    const void *args;
    u32 length;
};

/* Recovered from the Pilot 2.0 ARM64 call at 0xdb160..0xdb208. */
struct real3d_frame {
    u32 width, height;
    u32 src_stride_y, src_stride_uv;
    u32 dst_stride_y, dst_stride_uv;
    u32 depth_width, fractional_bits;
    u64 src[3];
    u64 dst[3];
    u64 depth;
    u64 result;
};
_Static_assert(sizeof(struct real3d_frame) == 96, "Real3D RPC ABI");
_Static_assert(__builtin_offsetof(struct rpc, args) == 40, "RPC pointer ABI");

static int initialized;
static u32 frames;
static u32 depth_min, depth_max, shifted_samples;
static u32 map_us, warp_us, flush_us;

#ifdef XREAL_CAPTURE_REAL3D
/* One bounded capture per process, at the same RPC and before unmapping.
 * These are input/warped frames, NOT a capture of the final eye displays. */
static int captured;
static int write_all(int fd, const void *data, size_t size) {
    const u8 *p = data;
    while (size) {
        long n = write(fd, p, size);
        if (n <= 0 || (size_t)n > size) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}
static int capture_plane(const char *path, const u8 *data,
                         u32 width, u32 height, u32 stride) {
    char header[64];
    int n = snprintf(header, sizeof(header), "P5\n%u %u\n255\n", width, height);
    if (n <= 0 || n >= (int)sizeof(header)) return -1;
    int fd = open(path, 1 | 64 | 512, 0600);
    if (fd < 0) return -1;
    int rc = write_all(fd, header, (size_t)n);
    for (u32 y = 0; !rc && y < height; ++y)
        rc = write_all(fd, data+(size_t)y*stride, width);
    close(fd);
    return rc;
}
#endif

static u64 monotonic_us(void) {
    struct timespec t;
    if (clock_gettime(1, &t)) return 0;
    return (u64)t.sec * 1000000ULL + (u64)t.nsec / 1000;
}

static u64 real3d_log_bytes;
#define REAL3D_LOG_MAX (512U * 1024U)
static void real3d_log(const char *line, size_t n) {
    if (real3d_log_bytes >= REAL3D_LOG_MAX) {
        (void)rename("/usrdata/real3d-cpu.log", "/usrdata/real3d-cpu.log.prev");
        real3d_log_bytes = 0;
    }
    int fd = open("/usrdata/real3d-cpu.log", 1 | 64 | 1024, 0600);
    if (fd >= 0) { (void)write(fd, line, n); (void)close(fd); real3d_log_bytes += n; }
}

static void diagnostic(const char *event, const struct real3d_frame *f, int rc) {
    char line[550];
    int n;
    if (f) {
        n = snprintf(line, sizeof(line),
            "Real3D CPU %s rc=%d frame=%u %ux%u strides=%u/%u -> %u/%u depth_width=%u fractional_bits=%u "
            "src=%lx,%lx,%lx dst=%lx,%lx,%lx map=%lx result=%lx "
            "depth_range=%u..%u shifted=%u time_us=%u/%u/%u stamp_us=%lu workers=%u\n",
            event, rc, frames, f->width, f->height,
            f->src_stride_y, f->src_stride_uv,
            f->dst_stride_y, f->dst_stride_uv,
            f->depth_width, f->fractional_bits,
            f->src[0], f->src[1], f->src[2],
            f->dst[0], f->dst[1], f->dst[2], f->depth, f->result,
            depth_min, depth_max, shifted_samples, map_us, warp_us, flush_us,
            monotonic_us(), xreal_pool_workers());
    } else {
        n = snprintf(line, sizeof(line), "Real3D CPU %s rc=%d frames=%u\n",
                     event, rc, frames);
    }
    if (n <= 0 || n >= (int)sizeof(line)) return;
    real3d_log(line, (size_t)n);
}

/* One Pro's MMZ pool used by the already validated HDR fallback. */
static int valid(u64 address, u32 bytes) {
    return bytes && address >= 0x33000000ULL && address < 0x40000000ULL &&
           bytes <= 0x40000000ULL - address;
}

static void *map(u64 address, u32 bytes) {
    if (!valid(address, bytes)) return NULL;
    void *p = ar_hal_sys_mmap_cache(address, bytes);
    if (!p || p == (void *)-1) return NULL;
    int rc = ar_hal_sys_mmz_invalid_cache(address, p, bytes);
    if (rc && (u32)rc != 0x80028003U) {
        ar_hal_sys_munmap(p, bytes);
        return NULL;
    }
    return p;
}

/* Destination planes are written completely (whole 64-byte lines: widths
 * 1920/960 and strides 2048/1920/1024/960 are multiples of 64) and never
 * read, so their cache lines need no invalidation before the warp; the
 * flush after the warp writes them back. Unaligned layouts keep the
 * invalidate. */
static void *map_output(u64 address, u32 bytes, u32 stride) {
    if ((address & 63) || (stride & 63)) return map(address, bytes);
    if (!valid(address, bytes)) return NULL;
    void *p = ar_hal_sys_mmap_cache(address, bytes);
    if (!p || p == (void *)-1) return NULL;
    return p;
}

static int set_result(u64 address, int result) {
    int *p = map(address, 64);
    if (!p) return -5;
    *p = result;
    int rc = ar_hal_sys_mmz_flush_cache_pa(address, 64);
    ar_hal_sys_munmap(p, 64);
    return rc ? -5 : 0;
}

static int depth_shift(u8 value, u32 width, u32 depth_width, u32 fractional_bits) {
    /* Pilot has already scaled NPU depth by the pose/mode multiplier and
     * encoded displacement with 2, 3 or 4 fractional bits in luma pixels.
     * Q5 preserves even a Q4 luma step after chroma subsampling. */
    return value << (5 - fractional_bits - (width / depth_width == 2));
}

static u8 sample_q5(const u8 *s, int q, u32 width) {
    if (q <= 0) return s[0];
    u32 x = (u32)q / 32, frac = (u32)q & 31;
    if (x >= width - 1) return s[width - 1];
    return (u8)((s[x] * (32-frac) + s[x+1] * frac + 16) >> 5);
}

/* Four neighbouring pixels share one fractional displacement. Keep the
 * scalar Q5 weights/rounding exactly, using four independent 16-bit lanes.
 * Call only when both four-byte loads are wholly inside the source row. */
static void sample_cell4(u8 *dst, const u8 *src, u32 frac) {
    typedef u8 bytes4 __attribute__((vector_size(4)));
    typedef u16 words4 __attribute__((vector_size(8)));
    bytes4 left, right;
    __builtin_memcpy(&left, src, 4);
    __builtin_memcpy(&right, src + 1, 4);
    words4 a = __builtin_convertvector(left, words4);
    words4 b = __builtin_convertvector(right, words4);
    words4 value = (a * (u16)(32-frac) + b * (u16)frac + (u16)16) >> 5;
    bytes4 result = __builtin_convertvector(value, bytes4);
    __builtin_memcpy(dst, &result, 4);
}

/* Two 16-bit lanes packed into a register; each sum is <= 8176 so neither
 * multiplication nor the rounding addition can carry into the next lane. */
static void sample_cell2(u8 *dst, const u8 *src, u32 frac) {
    u16 left, right;
    __builtin_memcpy(&left, src, 2);
    __builtin_memcpy(&right, src + 1, 2);
    u32 a = (left & 255U) | ((u32)(left & 0xff00U) << 8);
    u32 b = (right & 255U) | ((u32)(right & 0xff00U) << 8);
    u32 value = (a * (32-frac) + b * frac + 0x00100010U) >> 5;
    u16 result = (u16)((value & 255U) | ((value >> 8) & 0xff00U));
    __builtin_memcpy(dst, &result, 2);
}

/* Eight neighbouring pixels sharing one fractional displacement (128-bit NEON).
 * Identical Q5 weights/rounding to two sample_cell4 calls. */
static inline void sample_cell8(u8 *dst, const u8 *src, u32 frac) {
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
    uint16x8_t a = vmovl_u8(vld1_u8(src));
    uint16x8_t b = vmovl_u8(vld1_u8(src + 1));
    uint16x8_t v = vmlaq_n_u16(vmulq_n_u16(a, (u16)(32 - frac)), b, (u16)frac);
    vst1_u8(dst, vmovn_u16(vrshrq_n_u16(v, 5)));
#else
    sample_cell4(dst, src, frac);
    sample_cell4(dst + 4, src + 4, frac);
#endif
}

/* Joint U+V two-pixel interpolation sharing one fractional displacement:
 * packs [U0, U1, V0, V1] into one 4-lane NEON vector. */
static inline void sample_cell2_pair(u8 *d1, const u8 *s1, u8 *d2, const u8 *s2, u32 frac) {
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
    u16 l1, r1, l2, r2;
    __builtin_memcpy(&l1, s1, 2); __builtin_memcpy(&r1, s1 + 1, 2);
    __builtin_memcpy(&l2, s2, 2); __builtin_memcpy(&r2, s2 + 1, 2);
    uint8x8_t bl = vcreate_u8((u64)l1 | ((u64)l2 << 16));
    uint8x8_t br = vcreate_u8((u64)r1 | ((u64)r2 << 16));
    uint16x4_t a = vget_low_u16(vmovl_u8(bl));
    uint16x4_t b = vget_low_u16(vmovl_u8(br));
    uint16x4_t v = vmla_n_u16(vmul_n_u16(a, (u16)(32 - frac)), b, (u16)frac);
    uint8x8_t out = vmovn_u16(vcombine_u16(vrshr_n_u16(v, 5), vdup_n_u16(0)));
    u32 packed = vget_lane_u32(vreinterpret_u32_u8(out), 0);
    u16 o1 = (u16)packed, o2 = (u16)(packed >> 16);
    __builtin_memcpy(d1, &o1, 2);
    __builtin_memcpy(d2, &o2, 2);
#else
    sample_cell2(d1, s1, frac);
    sample_cell2(d2, s2, frac);
#endif
}

/* The live Real3D frame is 1920x1080 with a 480x270 depth map.  Each
 * depth sample covers four luma pixels or two chroma pixels.  Process a
 * complete depth cell at once so that division and disparity calculation
 * are not repeated for every output pixel. */
/* smooth=0 is the rc7 behaviour: one displacement per depth row, so a
 * depth step moves a whole 4-row (luma) / 2-row (chroma) band and slanted
 * object edges show a staircase. smooth=1 blends each output row's
 * displacement with the neighbouring depth row by distance to the row
 * centres (weights 1/8, 3/8 for luma; 1/4 for chroma), clamped at the top
 * and bottom. smooth=2 additionally blends horizontally with the
 * neighbouring depth column by distance to the cell centres (the same
 * weights), so every output pixel gets its own displacement: bilinear
 * interpolation of the 480x270 map instead of 4-pixel steps along slanted
 * edges. Cells whose neighbours carry the same displacement keep the shared
 * fast path. */
/* perf15 luma sharpness: 4-tap Catmull-Rom instead of 2-tap linear when the
 * Q5 source position is fractional (integer positions are still copied).
 * Q8 weights for the 32 phases, each row sums to 256. Anti-ringing: the
 * result is clamped to the two centre samples, so subtitles and hard edges
 * get no halo, while gradients/texture keep more contrast than linear. */
static const short CR_Q8[32][4] = {
    {0, 256, 0, 0},
    {-4, 256, 4, 0},
    {-7, 253, 10, 0},
    {-10, 251, 16, -1},
    {-12, 247, 23, -2},
    {-14, 242, 31, -3},
    {-16, 237, 39, -4},
    {-17, 230, 48, -5},
    {-18, 222, 58, -6},
    {-19, 214, 68, -7},
    {-19, 206, 78, -9},
    {-19, 196, 89, -10},
    {-19, 186, 100, -11},
    {-18, 176, 111, -13},
    {-18, 166, 122, -14},
    {-17, 155, 133, -15},
    {-16, 144, 144, -16},
    {-15, 133, 155, -17},
    {-14, 122, 166, -18},
    {-13, 111, 176, -18},
    {-11, 100, 186, -19},
    {-10, 89, 196, -19},
    {-9, 78, 206, -19},
    {-7, 68, 214, -19},
    {-6, 58, 222, -18},
    {-5, 48, 230, -17},
    {-4, 39, 237, -16},
    {-3, 31, 242, -14},
    {-2, 23, 247, -12},
    {-1, 16, 251, -10},
    {0, 10, 253, -7},
    {0, 4, 256, -4}
};

static u8 sample_cubic_q5(const u8 *s, int q, u32 width) {
    if (q <= 0) return s[0];
    u32 x = (u32)q >> 5, f = (u32)q & 31;
    if (x >= width - 1) return s[width - 1];
    if (!f) return s[x];
    int a = s[x ? x - 1 : 0], b = s[x], c = s[x + 1], d = s[x + 2 < width ? x + 2 : width - 1];
    int v = (CR_Q8[f][0] * a + CR_Q8[f][1] * b + CR_Q8[f][2] * c + CR_Q8[f][3] * d + 128) >> 8;
    int lo = b < c ? b : c, hi = b < c ? c : b;
    return (u8)(v < lo ? lo : v > hi ? hi : v);
}

/* Four luma pixels sharing one fractional phase (the cell fast path).
 * Caller guarantees 1 <= sx and sx + 7 <= width. Same arithmetic as
 * sample_cubic_q5. */
static void sample_cell4_cubic(u8 *dst, const u8 *s, u32 sx, u32 f) {
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
    uint8x8_t win = vld1_u8(s + sx - 1);                 /* s[sx-1 .. sx+6] */
    int16x4_t t0 = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(win)));
    int16x4_t t1 = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(vext_u8(win, win, 1))));
    int16x4_t t2 = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(vext_u8(win, win, 2))));
    int16x4_t t3 = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(vext_u8(win, win, 3))));
    int32x4_t acc = vmull_n_s16(t0, CR_Q8[f][0]);
    acc = vmlal_n_s16(acc, t1, CR_Q8[f][1]);
    acc = vmlal_n_s16(acc, t2, CR_Q8[f][2]);
    acc = vmlal_n_s16(acc, t3, CR_Q8[f][3]);
    int16x4_t v = vmovn_s32(vshrq_n_s32(vaddq_s32(acc, vdupq_n_s32(128)), 8));
    v = vmax_s16(v, vmin_s16(t1, t2));
    v = vmin_s16(v, vmax_s16(t1, t2));
    uint8x8_t out = vqmovun_s16(vcombine_s16(v, v));
    vst1_lane_u32((uint32_t *)(void *)dst, vreinterpret_u32_u8(out), 0);
#else
    for (u32 k = 0; k < 4; ++k) dst[k] = sample_cubic_q5(s, (int)((sx + k) * 32 + f), sx + 8);
#endif
}

/* perf18: per-pixel cubic for four luma pixels with individual phases, one
 * 16-byte window + TBL gathers and a 4x4 weight transpose. Returns 0 when the
 * window does not fit (caller uses sample_cubic_q5). Same arithmetic. */
static inline int sample_pixels4_cubic(u8 *d, const u8 *s, u32 x, const u32 sh[4], u32 width) {
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
    u32 i[4], f[4];
    for (u32 k = 0; k < 4; ++k) { u32 q = (x + k) * 32 + sh[k]; i[k] = q >> 5; f[k] = q & 31; }
    u32 lo = i[0], hi = i[0];
    for (u32 k = 1; k < 4; ++k) { if (i[k] < lo) lo = i[k]; if (i[k] > hi) hi = i[k]; }
    if (lo < 1 || hi - lo > 12 || lo - 1 + 16 > width) return 0;
    uint8x16_t win = vld1q_u8(s + lo - 1);
    u64 base = (u64)(i[0] - lo) | (u64)(i[1] - lo) << 8 | (u64)(i[2] - lo) << 16 | (u64)(i[3] - lo) << 24;
    int16x4_t tap[4];
    for (u32 t = 0; t < 4; ++t) {
        uint8x8_t idx = vcreate_u8(base + 0x01010101ULL * t);
        tap[t] = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(vqtbl1_u8(win, idx))));
    }
    /* Rows = pixels, columns = taps; transpose to tap-major weight vectors. */
    int16x4_t r0 = vld1_s16(CR_Q8[f[0]]), r1 = vld1_s16(CR_Q8[f[1]]);
    int16x4_t r2 = vld1_s16(CR_Q8[f[2]]), r3 = vld1_s16(CR_Q8[f[3]]);
    int16x4x2_t a = vtrn_s16(r0, r1), b = vtrn_s16(r2, r3);
    int32x2x2_t c = vtrn_s32(vreinterpret_s32_s16(a.val[0]), vreinterpret_s32_s16(b.val[0]));
    int32x2x2_t e = vtrn_s32(vreinterpret_s32_s16(a.val[1]), vreinterpret_s32_s16(b.val[1]));
    int16x4_t w0 = vreinterpret_s16_s32(c.val[0]), w2 = vreinterpret_s16_s32(c.val[1]);
    int16x4_t w1 = vreinterpret_s16_s32(e.val[0]), w3 = vreinterpret_s16_s32(e.val[1]);
    int32x4_t acc = vmull_s16(tap[0], w0);
    acc = vmlal_s16(acc, tap[1], w1);
    acc = vmlal_s16(acc, tap[2], w2);
    acc = vmlal_s16(acc, tap[3], w3);
    int16x4_t v = vmovn_s32(vshrq_n_s32(vaddq_s32(acc, vdupq_n_s32(128)), 8));
    v = vmax_s16(v, vmin_s16(tap[1], tap[2]));
    v = vmin_s16(v, vmax_s16(tap[1], tap[2]));
    vst1_lane_u32((uint32_t *)(void *)d, vreinterpret_u32_u8(vqmovun_s16(vcombine_s16(v, v))), 0);
    return 1;
#else
    (void)d; (void)s; (void)x; (void)sh; (void)width;
    return 0;
#endif
}

#define HFLAT 4U

/* perf16 edge-aware depth (joint bilateral upsampling, luma only). Where the
 * 480x270 displacement jumps by more than JBU_EDGE (1/2 pixel) around a
 * cell, each pixel takes a weighted mix of the 2x2 nearest cells' raw
 * displacements: spatial (bilinear) weight times a range weight from how
 * close the pixel's source luma is to each cell's centre luma. The depth
 * boundary then follows the object contour instead of a smooth ramp across
 * foreground and background. Elsewhere the perf2/perf5 smoothing is kept. */
#define JBU_EDGE 32U   /* perf18: 1 pixel (was 1/2): object boundaries, not depth noise */
static const unsigned short JBU_RANGE_Q8[256] = {256, 254, 249, 240, 229, 215, 199, 182, 164, 146, 128, 110, 94, 79, 66, 54, 43, 34, 27, 21, 16, 12, 9, 6, 5, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
/* Four luma pixels with individual Q5 displacements: one 16-byte load and a
 * TBL gather instead of eight byte loads. Same arithmetic as sample_q5.
 * Returns 0 (caller falls back) when the window does not fit. */
static inline int sample_pixels4(u8 *d, const u8 *s, u32 x, const u32 sh[4], u32 width) {
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
    u32 i0 = sh[0] >> 5, i1 = sh[1] >> 5, i2 = sh[2] >> 5, i3 = sh[3] >> 5;
    u32 lo = i0 < i1 ? i0 : i1;
    if (i2 < lo) lo = i2;
    if (i3 < lo) lo = i3;
    u32 hi = i0 > i1 ? i0 : i1;
    if (i2 > hi) hi = i2;
    if (i3 > hi) hi = i3;
    u32 base = x + lo;
    if (hi - lo + 4 > 15 || base + 16 > width) return 0;
    u64 a = (u64)(i0 - lo) | (u64)(i1 - lo + 1) << 8 | (u64)(i2 - lo + 2) << 16 | (u64)(i3 - lo + 3) << 24;
    uint8x8_t index = vcreate_u8(a | (a + 0x01010101ULL) << 32);
    uint8x8_t got = vqtbl1_u8(vld1q_u8(s + base), index);
    uint16x8_t wide = vmovl_u8(got);
    uint16x4_t f = vcreate_u16((u64)(sh[0] & 31) | (u64)(sh[1] & 31) << 16 |
                               (u64)(sh[2] & 31) << 32 | (u64)(sh[3] & 31) << 48);
    uint16x4_t v = vmul_u16(vget_low_u16(wide), vsub_u16(vdup_n_u16(32), f));
    v = vmla_u16(v, vget_high_u16(wide), f);
    v = vrshr_n_u16(v, 5);
    vst1_lane_u32((uint32_t *)(void *)d, vreinterpret_u32_u8(vmovn_u16(vcombine_u16(v, v))), 0);
    return 1;
#else
    (void)d; (void)s; (void)x; (void)sh; (void)width;
    return 0;
#endif
}

/* src2/dst2 (optional): a second plane with identical geometry (U with V),
 * warped in the same pass; only for smooth != 2. cubic: luma 4-tap. */
static int warp_plane_cells_impl(const u8 *src, u8 *dst, const u8 *depth,
                                 u32 width, u32 height, u32 src_stride,
                                 u32 dst_stride, u32 depth_width,
                                 u32 depth_height, u32 begin, u32 end,
                                 u32 fractional_bits, int smooth,
                                 const u8 *src2, u8 *dst2, u32 src2_stride, u32 dst2_stride,
                                 int cubic, int jbu, int reproject) {
    if (fractional_bits < 2 || fractional_bits > 4 ||
        !depth_width || !depth_height || !width || !height ||
        !src || !dst || !depth || src_stride < width || dst_stride < width) return -22;
    u32 pixels = width / depth_width;
    u32 rows = height / depth_height;
    if ((pixels != 2 && pixels != 4) || !rows ||
        width % depth_width || height % depth_height ||
        begin > end || end > depth_height) return -22;
    if (depth_width > 480) return -22;
    u32 den_shift = rows == 4 ? 3 : rows == 2 ? 2 : 0;
    if (smooth && !den_shift) return -22;
    if (dst2 && (!src2 || smooth == 2 || src2_stride < width || dst2_stride < width)) return -22;
    cubic = cubic && pixels == 4;
    jbu = jbu && smooth == 2 && pixels == 4 && rows == 4;
    for (u32 zy = begin; zy < end; ++zy) {
        const u8 *z = depth + (size_t)zy * depth_width;
        const u8 *up = depth + (size_t)(zy ? zy - 1 : zy) * depth_width;
        const u8 *down = depth + (size_t)(zy + 1 < depth_height ? zy + 1 : zy) * depth_width;
        u16 source_x[480], here[480], above[480], below[480], blend[480];
        u8 cen_cur[480], cen_up[480], cen_dn[480], edge_up[480], edge_dn[480];
        u32 any_edge = 0;
        if (jbu) {
            /* perf18: edge flags once per depth row (for the upper and lower
             * output-row halves) instead of per cell per output row. Same
             * test as before: max-min of here/left/right in this row and in
             * the other row > JBU_EDGE. */
            const u8 *zu = depth + (size_t)(zy ? zy - 1 : zy) * depth_width;
            const u8 *zd = depth + (size_t)(zy + 1 < depth_height ? zy + 1 : zy) * depth_width;
            for (u32 zx = 0; zx < depth_width; ++zx) {
                u32 l = zx ? zx - 1 : zx, r = zx + 1 < depth_width ? zx + 1 : zx;
                u32 h0 = (u32)depth_shift(z[zx], width, depth_width, fractional_bits);
                u32 h1 = (u32)depth_shift(z[l], width, depth_width, fractional_bits);
                u32 h2 = (u32)depth_shift(z[r], width, depth_width, fractional_bits);
                u32 lo = h0 < h1 ? h0 : h1, hi = h0 < h1 ? h1 : h0;
                if (h2 < lo) lo = h2;
                if (h2 > hi) hi = h2;
                const u8 *o2[2] = { zu, zd };
                u8 *flag[2] = { edge_up, edge_dn };
                for (u32 t = 0; t < 2; ++t) {
                    u32 a0 = (u32)depth_shift(o2[t][zx], width, depth_width, fractional_bits);
                    u32 a1 = (u32)depth_shift(o2[t][l], width, depth_width, fractional_bits);
                    u32 a2 = (u32)depth_shift(o2[t][r], width, depth_width, fractional_bits);
                    u32 l2 = lo, h3 = hi;
                    if (a0 < l2) l2 = a0;
                    if (a0 > h3) h3 = a0;
                    if (a1 < l2) l2 = a1;
                    if (a1 > h3) h3 = a1;
                    if (a2 < l2) l2 = a2;
                    if (a2 > h3) h3 = a2;
                    flag[t][zx] = h3 - l2 > JBU_EDGE;
                    any_edge |= flag[t][zx];
                }
            }
        }
        if (jbu && any_edge) {
            /* Centre luma of each cell: mean of its 2x2 centre pixels. */
            u32 rz[3] = { zy ? zy - 1 : zy, zy, zy + 1 < depth_height ? zy + 1 : zy };
            u8 *cens[3] = { cen_up, cen_cur, cen_dn };
            for (u32 r = 0; r < 3; ++r) {
                const u8 *r1 = src + (size_t)(rz[r] * 4 + 1) * src_stride;
                const u8 *r2 = src + (size_t)(rz[r] * 4 + 2) * src_stride;
                for (u32 zx = 0; zx < depth_width; ++zx) {
                    u32 c = zx * 4 + 1;
                    cens[r][zx] = (u8)((r1[c] + r1[c + 1] + r2[c] + r2[c + 1] + 2) >> 2);
                }
            }
        }
        for (u32 zx = 0; zx < depth_width; ++zx) {
            here[zx] = (u16)depth_shift(z[zx], width, depth_width, fractional_bits);
            source_x[zx] = (u16)(zx * pixels * 32 + here[zx]);
            if (smooth) {
                above[zx] = (u16)depth_shift(up[zx], width, depth_width, fractional_bits);
                below[zx] = (u16)depth_shift(down[zx], width, depth_width, fractional_bits);
            }
        }
        for (u32 ry = 0; ry < rows; ++ry) {
            if (smooth) {
                /* Offset of this row's centre from the depth row's centre,
                 * in units of 1/(2*rows) depth rows. Never zero for even rows. */
                int num = 2 * (int)ry + 1 - (int)rows;
                const u16 *other = num < 0 ? above : below;
                u32 w = (u32)(num < 0 ? -num : num), den = 2 * rows;
                for (u32 zx = 0; zx < depth_width; ++zx) {
                    blend[zx] = (u16)((here[zx] * (den - w) + other[zx] * w + rows) >> den_shift);
                    source_x[zx] = (u16)(zx * pixels * 32 + blend[zx]);
                }
            }
            if (reproject && smooth) {
                /* perf23: the depth map is in source coordinates, but this row
                 * is built in output coordinates (dst[x] = src[x + d]). Read the
                 * displacement where the pixel actually comes from: one fixed-
                 * point step d = D(x + D(x)), per cell. Without it, output pixels
                 * next to a near object take the background's displacement and
                 * repeat the object's edge (halo that grows with depth). */
                u16 moved[480];
                u32 half = 16 * pixels, unit = 32 * pixels;
                for (u32 zx = 0; zx < depth_width; ++zx) {
                    u32 to = zx + (blend[zx] + half) / unit;
                    moved[zx] = blend[to < depth_width ? to : depth_width - 1];
                }
                for (u32 zx = 0; zx < depth_width; ++zx) {
                    blend[zx] = moved[zx];
                    source_x[zx] = (u16)(zx * pixels * 32 + blend[zx]);
                }
            }
            u32 y = zy * rows + ry;
            const u8 *s = src + (size_t)y * src_stride;
            u8 *d = dst + (size_t)y * dst_stride;
            const u8 *s2 = dst2 ? src2 + (size_t)y * src2_stride : NULL;
            u8 *d2 = dst2 ? dst2 + (size_t)y * dst2_stride : NULL;
            for (u32 zx = 0, x = 0; zx < depth_width; ++zx, x += pixels) {
                if (jbu && any_edge && (2 * ry + 1 < rows ? edge_up : edge_dn)[zx]) {
                    u32 l = zx ? zx - 1 : zx, r = zx + 1 < depth_width ? zx + 1 : zx;
                    int num = 2 * (int)ry + 1 - (int)rows;
                    const u16 *oth = num < 0 ? above : below;
                    const u8 *ocen = num < 0 ? cen_up : cen_dn;
                    u32 vw = (u32)(num < 0 ? -num : num);
                    {
                        for (u32 k = 0; k < 4; ++k) {
                            int hnum = 2 * (int)k + 1 - 4;
                            u32 hw = (u32)(hnum < 0 ? -hnum : hnum), side = hnum < 0 ? l : r;
                            u32 dv[4] = { here[zx], here[side], oth[zx], oth[side] };
                            u32 cv[4] = { cen_cur[zx], cen_cur[side], ocen[zx], ocen[side] };
                            u32 sw[4] = { (8 - hw) * (8 - vw), hw * (8 - vw), (8 - hw) * vw, hw * vw };
                            /* perf19 sharp boundaries: split the four cells into a
                             * near and a far layer (around the mid displacement).
                             * When the pixel's luma clearly belongs to one layer
                             * (its weight >= 2x the other), use only that layer,
                             * so objects separate from the background as with the
                             * native mesh; ambiguous luma keeps the soft mix. */
                            u32 g = s[x + k], dmin = dv[0], dmax = dv[0];
                            for (u32 i = 1; i < 4; ++i) { if (dv[i] < dmin) dmin = dv[i]; if (dv[i] > dmax) dmax = dv[i]; }
                            u32 mid2 = dmin + dmax;                 /* 2 * mid */
                            u32 wn = 0, an = 0, wf = 0, af = 0;
                            for (u32 i = 0; i < 4; ++i) {
                                u32 diff = g > cv[i] ? g - cv[i] : cv[i] - g;
                                u32 w = sw[i] * JBU_RANGE_Q8[diff];
                                if (2 * dv[i] >= mid2) { wn += w; an += w * dv[i]; }
                                else { wf += w; af += w * dv[i]; }
                            }
                            u32 wsum, acc;
                            if (wf && wn >= 2 * wf) { wsum = wn; acc = an; }
                            else if (wn && wf >= 2 * wn) { wsum = wf; acc = af; }
                            else { wsum = wn + wf; acc = an + af; }
                            u32 shift = (acc + wsum / 2) / wsum;
                            int q = (int)((x + k) * 32 + shift);
                            d[x + k] = cubic ? sample_cubic_q5(s, q, width) : sample_q5(s, q, width);
                        }
                        continue;
                    }
                }
                if (smooth == 2) {
                    u32 mid = blend[zx];
                    u32 left = blend[zx ? zx - 1 : zx];
                    u32 right = blend[zx + 1 < depth_width ? zx + 1 : zx];
                    /* Neighbours within HFLAT (1/8 pixel): the cell keeps one
                     * displacement; per-pixel blending would move pixels by
                     * at most 0.05 px there, which is invisible. */
                    u32 dl = left > mid ? left - mid : mid - left;
                    u32 dr = right > mid ? right - mid : mid - right;
                    if (dl > HFLAT || dr > HFLAT) {
                        u32 sh[4];
                        if (pixels == 4) {
                            sh[0] = (5 * mid + 3 * left + 4) >> 3;
                            sh[1] = (7 * mid + left + 4) >> 3;
                            sh[2] = (7 * mid + right + 4) >> 3;
                            sh[3] = (5 * mid + 3 * right + 4) >> 3;
                        } else {
                            sh[0] = (3 * mid + left + 2) >> 2;
                            sh[1] = (3 * mid + right + 2) >> 2;
                        }
                        u32 top = mid > left ? mid : left;
                        if (right > top) top = right;
                        if (cubic) {
                            if (!sample_pixels4_cubic(d + x, s, x, sh, width))
                                for (u32 k = 0; k < pixels; ++k)
                                    d[x + k] = sample_cubic_q5(s, (int)((x + k) * 32 + sh[k]), width);
                        } else if (pixels == 4 && sample_pixels4(d + x, s, x, sh, width)) {
                        } else if ((((x + pixels - 1) * 32 + top) >> 5) + 1 < width) {
                            /* Every sample and its right neighbour lie inside the
                             * row: the same arithmetic as sample_q5, unchecked. */
                            for (u32 k = 0; k < pixels; ++k) {
                                u32 q = (x + k) * 32 + sh[k], i = q >> 5, f = q & 31;
                                d[x + k] = (u8)((s[i] * (32 - f) + s[i + 1] * f + 16) >> 5);
                            }
                        } else {
                            for (u32 k = 0; k < pixels; ++k)
                                d[x + k] = sample_q5(s, (int)((x + k) * 32 + sh[k]), width);
                        }
                        continue;
                    }
                }
                u32 q = source_x[zx], sx = q / 32;
                if (!(q & 31) && sx + pixels <= width) {
                    if (pixels == 4)
                        __builtin_memcpy(d + x, s + sx, 4);
                    else
                        __builtin_memcpy(d + x, s + sx, 2);
                    if (d2) __builtin_memcpy(d2 + x, s2 + sx, pixels);
                } else if (cubic) {
                    if (sx >= 1 && sx + 7 <= width)
                        sample_cell4_cubic(d + x, s, sx, q & 31);
                    else
                        for (u32 k = 0; k < pixels; ++k)
                            d[x + k] = sample_cubic_q5(s, (int)q + (int)k * 32, width);
                } else if (sx + pixels < width) {
                    if (pixels == 4)
                        sample_cell4(d + x, s + sx, q & 31);
                    else
                        sample_cell2(d + x, s + sx, q & 31);
                    if (d2) {
                        if (pixels == 4) sample_cell4(d2 + x, s2 + sx, q & 31);
                        else sample_cell2(d2 + x, s2 + sx, q & 31);
                    }
                } else {
                    for (u32 k = 0; k < pixels; ++k)
                        d[x + k] = sample_q5(s, (int)q + (int)k * 32, width);
                    if (d2)
                        for (u32 k = 0; k < pixels; ++k)
                            d2[x + k] = sample_q5(s2, (int)q + (int)k * 32, width);
                }
            }
        }
    }
    return 0;
}

static int warp_plane_cells_range(const u8 *src, u8 *dst, const u8 *depth,
                                  u32 width, u32 height, u32 src_stride,
                                  u32 dst_stride, u32 depth_width,
                                  u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_impl(src, dst, depth, width, height, src_stride, dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits, 0, NULL, NULL, 0, 0, 0, 0, 0);
}

static int warp_plane_cells_smooth_range(const u8 *src, u8 *dst, const u8 *depth,
                                         u32 width, u32 height, u32 src_stride,
                                         u32 dst_stride, u32 depth_width,
                                         u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_impl(src, dst, depth, width, height, src_stride, dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits, 1, NULL, NULL, 0, 0, 0, 0, 0);
}

static int warp_plane_cells(const u8 *src, u8 *dst, const u8 *depth,
                            u32 width, u32 height, u32 src_stride,
                            u32 dst_stride, u32 depth_width,
                            u32 depth_height, u32 fractional_bits) {
    return warp_plane_cells_range(src, dst, depth, width, height, src_stride,
                                  dst_stride, depth_width, depth_height,
                                  0, depth_height, fractional_bits);
}

/* perf25: symmetric two-eye DIBR. The native One Pro mesh writes BOTH eyes
 * (src and dst), each shifted by half the disparity; our Pro+ path warped only
 * one eye by the full disparity, so to stay clean we had to keep it shallow.
 * Here each eye is warped independently from a clean copy of the source, in
 * OPPOSITE directions (dir = +1 / -1), so the total disparity is the sum of two
 * half-sized per-eye shifts: more volume at the same per-eye edge cost. Keeps
 * perf24's anti-staircase smoothing; sample_q5 clamps both ends, so a leftward
 * (dir=-1) shift past the left edge is safe. Not the hot NEON path (spike). */
static int warp_eye_range(const u8 *src, u8 *dst, const u8 *depth,
                          u32 width, u32 height, u32 src_stride, u32 dst_stride,
                          u32 depth_width, u32 depth_height, u32 begin, u32 end,
                          u32 fractional_bits, int dir) {
    if (fractional_bits < 2 || fractional_bits > 4 || !depth_width || !depth_height ||
        !width || !height || !src || !dst || !depth ||
        src_stride < width || dst_stride < width) return -22;
    u32 pixels = width / depth_width, rows = height / depth_height;
    if ((pixels != 2 && pixels != 4) || !rows || width % depth_width ||
        height % depth_height || begin > end || end > depth_height) return -22;
    u32 den_shift = rows == 4 ? 3 : rows == 2 ? 2 : 0;
    if (!den_shift) return -22;
    for (u32 zy = begin; zy < end; ++zy) {
        const u8 *z = depth + (size_t)zy * depth_width;
        const u8 *up = depth + (size_t)(zy ? zy - 1 : zy) * depth_width;
        const u8 *down = depth + (size_t)(zy + 1 < depth_height ? zy + 1 : zy) * depth_width;
        u16 here[480], above[480], below[480], blend[480];
        for (u32 zx = 0; zx < depth_width; ++zx) {
            here[zx] = (u16)depth_shift(z[zx], width, depth_width, fractional_bits);
            above[zx] = (u16)depth_shift(up[zx], width, depth_width, fractional_bits);
            below[zx] = (u16)depth_shift(down[zx], width, depth_width, fractional_bits);
        }
        for (u32 ry = 0; ry < rows; ++ry) {
            int num = 2 * (int)ry + 1 - (int)rows;
            const u16 *other = num < 0 ? above : below;
            u32 vw = (u32)(num < 0 ? -num : num), den = 2 * rows;
            for (u32 zx = 0; zx < depth_width; ++zx)
                blend[zx] = (u16)((here[zx] * (den - vw) + other[zx] * vw + rows) >> den_shift);
            u32 y = zy * rows + ry;
            const u8 *s = src + (size_t)y * src_stride;
            u8 *d = dst + (size_t)y * dst_stride;
            for (u32 zx = 0, x = 0; zx < depth_width; ++zx, x += pixels) {
                u32 mid = blend[zx];
                u32 left = blend[zx ? zx - 1 : zx];
                u32 right = blend[zx + 1 < depth_width ? zx + 1 : zx];
                u32 dl = left > mid ? left - mid : mid - left;
                u32 dr = right > mid ? right - mid : mid - right;
                u32 sh[4];
                if (dl > HFLAT || dr > HFLAT) {
                    if (pixels == 4) {
                        sh[0] = (5 * mid + 3 * left + 4) >> 3;
                        sh[1] = (7 * mid + left + 4) >> 3;
                        sh[2] = (7 * mid + right + 4) >> 3;
                        sh[3] = (5 * mid + 3 * right + 4) >> 3;
                    } else {
                        sh[0] = (3 * mid + left + 2) >> 2;
                        sh[1] = (3 * mid + right + 2) >> 2;
                    }
                } else {
                    for (u32 k = 0; k < pixels; ++k) sh[k] = mid;
                }
                for (u32 k = 0; k < pixels; ++k)
                    d[x + k] = sample_q5(s, (int)((x + k) * 32) + dir * (int)sh[k], width);
            }
        }
    }
    return 0;
}

int real3d_warp_eye_range_for_test(const u8 *src, u8 *dst, const u8 *depth,
                                   u32 width, u32 height, u32 src_stride, u32 dst_stride,
                                   u32 depth_width, u32 depth_height, u32 begin, u32 end,
                                   u32 fractional_bits, int dir) {
    return warp_eye_range(src, dst, depth, width, height, src_stride, dst_stride,
                          depth_width, depth_height, begin, end, fractional_bits, dir);
}

struct segment_job {
    const u8 *src[3], *depth;
    const u16 *depth_q5;    /* perf29: 1D Z-buffer projected Q5 luma shift map (0..1408) */
    u8 *dst[3];
    u8 *eye2[3];            /* perf25: second eye output (the mapped source planes); NULL = single eye */
    u32 width[3], height[3], src_stride[3], dst_stride[3];
    u32 depth_width, depth_height, fractional_bits;
    u32 begin, end;
    u32 *next;
    int rc;
};

static inline void depth_shift_row(u16 *out, const u8 *in, u32 n, u32 shift) {
    u32 i = 0;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
    int16x8_t vsh = vdupq_n_s16((int16_t)shift);
    for (; i + 8 <= n; i += 8)
        vst1q_u16(out + i, vshlq_u16(vmovl_u8(vld1_u8(in + i)), vsh));
#endif
    for (; i < n; ++i) out[i] = (u16)((u32)in[i] << shift);
}

/* Specialized live Real3D Pro+ luma path (pixels=4, rows=4, smooth=2, 2-tap):
 * avoids the 8.8 KB stack / 3753-instruction monolithic warp_plane_cells_impl,
 * vectorizes depth-row prep, and coalesces adjacent flat cells to 8 px NEON. */
static int warp_luma_bilinear_fast(const u8 *src, u8 *dst, const u8 *depth,
                                   u32 width, u32 height, u32 src_stride,
                                   u32 dst_stride, u32 depth_width,
                                   u32 depth_height, u32 begin, u32 end,
                                   u32 fractional_bits) {
    if (fractional_bits < 2 || fractional_bits > 4 ||
        !depth_width || !depth_height || depth_width > 480 ||
        !src || !dst || !depth || src_stride < width || dst_stride < width ||
        width != depth_width * 4 || height != depth_height * 4 ||
        begin > end || end > depth_height) return -22;
    u32 shift = 5 - fractional_bits;
    u16 here[480], above[480], below[480], blend[480];
    for (u32 zy = begin; zy < end; ++zy) {
        const u8 *z = depth + (size_t)zy * depth_width;
        const u8 *up = depth + (size_t)(zy ? zy - 1 : zy) * depth_width;
        const u8 *down = depth + (size_t)(zy + 1 < depth_height ? zy + 1 : zy) * depth_width;
        depth_shift_row(here, z, depth_width, shift);
        depth_shift_row(above, up, depth_width, shift);
        depth_shift_row(below, down, depth_width, shift);
        for (u32 ry = 0; ry < 4; ++ry) {
            const u16 *other = ry < 2 ? above : below;
            u32 w = (ry == 0 || ry == 3) ? 3U : 1U;
            u32 inv = 8U - w;
            u32 zx = 0;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
            for (; zx + 8 <= depth_width; zx += 8) {
                uint16x8_t h = vld1q_u16(here + zx);
                uint16x8_t o = vld1q_u16(other + zx);
                uint16x8_t b = vmlaq_n_u16(vmulq_n_u16(h, (u16)inv), o, (u16)w);
                vst1q_u16(blend + zx, vrshrq_n_u16(b, 3));
            }
#endif
            for (; zx < depth_width; ++zx)
                blend[zx] = (u16)((here[zx] * inv + other[zx] * w + 4) >> 3);
            u32 y = zy * 4 + ry;
            const u8 *s = src + (size_t)y * src_stride;
            u8 *d = dst + (size_t)y * dst_stride;
            for (u32 cx = 0, x = 0; cx < depth_width; ++cx, x += 4) {
                u32 mid = blend[cx];
                u32 left = blend[cx ? cx - 1 : cx];
                u32 right = blend[cx + 1 < depth_width ? cx + 1 : cx];
                u32 dl = left > mid ? left - mid : mid - left;
                u32 dr = right > mid ? right - mid : mid - right;
                if (dl > HFLAT || dr > HFLAT) {
                    u32 sh[4] = {
                        (5 * mid + 3 * left + 4) >> 3,
                        (7 * mid + left + 4) >> 3,
                        (7 * mid + right + 4) >> 3,
                        (5 * mid + 3 * right + 4) >> 3
                    };
                    u32 top = mid > left ? mid : left;
                    if (right > top) top = right;
                    if (sample_pixels4(d + x, s, x, sh, width)) {
                    } else if ((((x + 3) * 32 + top) >> 5) + 1 < width) {
                        for (u32 k = 0; k < 4; ++k) {
                            u32 q = (x + k) * 32 + sh[k], i = q >> 5, f = q & 31;
                            d[x + k] = (u8)((s[i] * (32 - f) + s[i + 1] * f + 16) >> 5);
                        }
                    } else {
                        for (u32 k = 0; k < 4; ++k)
                            d[x + k] = sample_q5(s, (int)((x + k) * 32 + sh[k]), width);
                    }
                    continue;
                }
                u32 q = x * 32 + mid, sx = q >> 5, frac = q & 31;
                if (cx + 1 < depth_width && right == mid) {
                    u32 r2 = blend[cx + 2 < depth_width ? cx + 2 : cx + 1];
                    u32 dr2 = r2 > mid ? r2 - mid : mid - r2;
                    if (dr2 <= HFLAT) {
                        if (!frac && sx + 8 <= width) {
                            __builtin_memcpy(d + x, s + sx, 8);
                            ++cx; x += 4;
                            continue;
                        }
                        if (frac && sx + 8 < width) {
                            sample_cell8(d + x, s + sx, frac);
                            ++cx; x += 4;
                            continue;
                        }
                    }
                }
                if (!frac && sx + 4 <= width) {
                    __builtin_memcpy(d + x, s + sx, 4);
                } else if (sx + 4 < width) {
                    sample_cell4(d + x, s + sx, frac);
                } else {
                    for (u32 k = 0; k < 4; ++k)
                        d[x + k] = sample_q5(s, (int)q + (int)k * 32, width);
                }
            }
        }
    }
    return 0;
}

/* Specialized live Real3D Pro+ joint U/V path (pixels=2, rows=2, smooth=1):
 * vectorizes depth prep, uses joint U+V NEON interpolation, and coalesces
 * adjacent equal-shift cells to 4 px per plane. */
static int warp_chroma_pair_fast(const u8 *u_src, u8 *u_dst, const u8 *v_src, u8 *v_dst,
                                 const u8 *depth, u32 width, u32 height,
                                 u32 u_src_stride, u32 u_dst_stride,
                                 u32 v_src_stride, u32 v_dst_stride, u32 depth_width,
                                 u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    if (fractional_bits < 2 || fractional_bits > 4 ||
        !depth_width || !depth_height || depth_width > 480 ||
        !u_src || !u_dst || !v_src || !v_dst || !depth ||
        u_src_stride < width || u_dst_stride < width ||
        v_src_stride < width || v_dst_stride < width ||
        width != depth_width * 2 || height != depth_height * 2 ||
        begin > end || end > depth_height) return -22;
    u32 shift = 4 - fractional_bits;
    u16 here[480], above[480], below[480], blend[480];
    for (u32 zy = begin; zy < end; ++zy) {
        const u8 *z = depth + (size_t)zy * depth_width;
        const u8 *up = depth + (size_t)(zy ? zy - 1 : zy) * depth_width;
        const u8 *down = depth + (size_t)(zy + 1 < depth_height ? zy + 1 : zy) * depth_width;
        depth_shift_row(here, z, depth_width, shift);
        depth_shift_row(above, up, depth_width, shift);
        depth_shift_row(below, down, depth_width, shift);
        for (u32 ry = 0; ry < 2; ++ry) {
            const u16 *other = ry == 0 ? above : below;
            u32 zx = 0;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
            for (; zx + 8 <= depth_width; zx += 8) {
                uint16x8_t h = vld1q_u16(here + zx);
                uint16x8_t o = vld1q_u16(other + zx);
                uint16x8_t b = vaddq_u16(vaddq_u16(h, vshlq_n_u16(h, 1)), o);
                vst1q_u16(blend + zx, vrshrq_n_u16(b, 2));
            }
#endif
            for (; zx < depth_width; ++zx)
                blend[zx] = (u16)((here[zx] * 3 + other[zx] + 2) >> 2);
            u32 y = zy * 2 + ry;
            const u8 *s1 = u_src + (size_t)y * u_src_stride;
            u8 *d1 = u_dst + (size_t)y * u_dst_stride;
            const u8 *s2 = v_src + (size_t)y * v_src_stride;
            u8 *d2 = v_dst + (size_t)y * v_dst_stride;
            for (u32 cx = 0, x = 0; cx < depth_width; ++cx, x += 2) {
                u32 mid = blend[cx];
                u32 q = x * 32 + mid, sx = q >> 5, frac = q & 31;
                if (cx + 1 < depth_width && blend[cx + 1] == mid) {
                    if (!frac && sx + 4 <= width) {
                        __builtin_memcpy(d1 + x, s1 + sx, 4);
                        __builtin_memcpy(d2 + x, s2 + sx, 4);
                        ++cx; x += 2;
                        continue;
                    }
                    if (frac && sx + 4 < width) {
                        sample_cell4(d1 + x, s1 + sx, frac);
                        sample_cell4(d2 + x, s2 + sx, frac);
                        ++cx; x += 2;
                        continue;
                    }
                }
                if (!frac && sx + 2 <= width) {
                    __builtin_memcpy(d1 + x, s1 + sx, 2);
                    __builtin_memcpy(d2 + x, s2 + sx, 2);
                } else if (sx + 2 < width) {
                    sample_cell2_pair(d1 + x, s1 + sx, d2 + x, s2 + sx, frac);
                } else {
                    for (u32 k = 0; k < 2; ++k) {
                        d1[x + k] = sample_q5(s1, (int)q + (int)k * 32, width);
                        d2[x + k] = sample_q5(s2, (int)q + (int)k * 32, width);
                    }
                }
            }
        }
    }
    return 0;
}

static int warp_plane_cells_bilinear_range(const u8 *src, u8 *dst, const u8 *depth,
                                           u32 width, u32 height, u32 src_stride,
                                           u32 dst_stride, u32 depth_width,
                                           u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    if (depth_width && depth_height && width == depth_width * 4 && height == depth_height * 4)
        return warp_luma_bilinear_fast(src, dst, depth, width, height, src_stride, dst_stride,
                                       depth_width, depth_height, begin, end, fractional_bits);
    return warp_plane_cells_impl(src, dst, depth, width, height, src_stride, dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits, 2, NULL, NULL, 0, 0, 0, 0, 0);
}

static int warp_luma_cubic_range(const u8 *src, u8 *dst, const u8 *depth,
                                 u32 width, u32 height, u32 src_stride,
                                 u32 dst_stride, u32 depth_width,
                                 u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_impl(src, dst, depth, width, height, src_stride, dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits, 2,
                                 NULL, NULL, 0, 0, 1, 0, 0);
}

static int warp_luma_jbu_range(const u8 *src, u8 *dst, const u8 *depth,
                               u32 width, u32 height, u32 src_stride,
                               u32 dst_stride, u32 depth_width,
                               u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_impl(src, dst, depth, width, height, src_stride, dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits, 2,
                                 NULL, NULL, 0, 0, 1, 1, 0);
}

static int warp_luma_reproject_range(const u8 *src, u8 *dst, const u8 *depth,
                                     u32 width, u32 height, u32 src_stride,
                                     u32 dst_stride, u32 depth_width,
                                     u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_impl(src, dst, depth, width, height, src_stride, dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits, 2,
                                 NULL, NULL, 0, 0, 0, 0, 1);
}

static int warp_chroma_pair_range(const u8 *u_src, u8 *u_dst, const u8 *v_src, u8 *v_dst,
                                  const u8 *depth, u32 width, u32 height,
                                  u32 u_src_stride, u32 u_dst_stride,
                                  u32 v_src_stride, u32 v_dst_stride, u32 depth_width,
                                  u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_impl(u_src, u_dst, depth, width, height, u_src_stride, u_dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits, 1,
                                 v_src, v_dst, v_src_stride, v_dst_stride, 0, 0, 1);
}

/* perf24: live chroma path with reprojection off (clean perf15-20 joint U/V). */
static int warp_chroma_pair_clean_range(const u8 *u_src, u8 *u_dst, const u8 *v_src, u8 *v_dst,
                                  const u8 *depth, u32 width, u32 height,
                                  u32 u_src_stride, u32 u_dst_stride,
                                  u32 v_src_stride, u32 v_dst_stride, u32 depth_width,
                                  u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    if (depth_width && depth_height && width == depth_width * 2 && height == depth_height * 2)
        return warp_chroma_pair_fast(u_src, u_dst, v_src, v_dst, depth, width, height,
                                     u_src_stride, u_dst_stride, v_src_stride, v_dst_stride,
                                     depth_width, depth_height, begin, end, fractional_bits);
    return warp_plane_cells_impl(u_src, u_dst, depth, width, height, u_src_stride, u_dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits, 1,
                                 v_src, v_dst, v_src_stride, v_dst_stride, 0, 0, 0);
}

/* Dynamic scheduling: workers take SEGMENT_ROWS depth rows at a time from a
 * shared counter, so a core busy with Pilot's own threads does not hold up
 * the frame. [begin, end) bounds the depth rows this job set covers. */
#define SEGMENT_ROWS 6
__attribute__((unused)) static void *symmetric_worker(void *arg) {
    struct segment_job *j = arg;
    j->rc = 0;
    for (;;) {
        u32 chunk = __atomic_fetch_add(j->next, 1, __ATOMIC_RELAXED);
        u32 first = j->begin + chunk * SEGMENT_ROWS;
        if (first >= j->end) break;
        u32 last = first + SEGMENT_ROWS < j->end ? first + SEGMENT_ROWS : j->end;
        for (u32 c = 0; c < 3; ++c) {
            int rc = warp_eye_range(j->src[c], j->dst[c], j->depth, j->width[c], j->height[c],
                                    j->src_stride[c], j->dst_stride[c], j->depth_width,
                                    j->depth_height, first, last, j->fractional_bits, +1);
            if (!rc) rc = warp_eye_range(j->src[c], j->eye2[c], j->depth, j->width[c], j->height[c],
                                    j->src_stride[c], j->src_stride[c], j->depth_width,
                                    j->depth_height, first, last, j->fractional_bits, -1);
            if (rc && !j->rc) j->rc = rc;
        }
    }
    return NULL;
}

/* perf29: specialized Q5 luma warp with cliff-guarded horizontal smoothing.
 * Reads pre-projected Q5 luma shifts (0..1408, i.e. 0..44 px) directly from
 * depth_q5, avoiding u8 255 clipping. Smooth slopes (<= CLIFF_Q5 = 4 px/cell)
 * get full bilinear smoothing (zero staircasing), while sharp depth cliffs
 * (> 4 px/cell) do not blend across foreground/background boundaries (zero
 * ghosts on thin details). */
#define CLIFF_Q5 128U
static int warp_luma_q5_fast(const u8 *src, u8 *dst, const u16 *depth_q5,
                             u32 width, u32 height, u32 src_stride,
                             u32 dst_stride, u32 depth_width,
                             u32 depth_height, u32 begin, u32 end) {
    if (!depth_width || !depth_height || depth_width > 480 ||
        !src || !dst || !depth_q5 || src_stride < width || dst_stride < width ||
        width != depth_width * 4 || height != depth_height * 4 ||
        begin > end || end > depth_height) return -22;
    u16 blend[480];
    for (u32 zy = begin; zy < end; ++zy) {
        const u16 *here = depth_q5 + (size_t)zy * depth_width;
        const u16 *above = depth_q5 + (size_t)(zy ? zy - 1 : zy) * depth_width;
        const u16 *below = depth_q5 + (size_t)(zy + 1 < depth_height ? zy + 1 : zy) * depth_width;
        for (u32 ry = 0; ry < 4; ++ry) {
            const u16 *other = ry < 2 ? above : below;
            u32 w = (ry == 0 || ry == 3) ? 3U : 1U;
            u32 inv = 8U - w;
            u32 zx = 0;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
            for (; zx + 8 <= depth_width; zx += 8) {
                uint16x8_t h = vld1q_u16(here + zx);
                uint16x8_t o = vld1q_u16(other + zx);
                uint16x8_t b = vmlaq_n_u16(vmulq_n_u16(h, (u16)inv), o, (u16)w);
                vst1q_u16(blend + zx, vrshrq_n_u16(b, 3));
            }
#endif
            for (; zx < depth_width; ++zx)
                blend[zx] = (u16)((here[zx] * inv + other[zx] * w + 4) >> 3);
            u32 y = zy * 4 + ry;
            const u8 *s = src + (size_t)y * src_stride;
            u8 *d = dst + (size_t)y * dst_stride;
            for (u32 cx = 0, x = 0; cx < depth_width; ++cx, x += 4) {
                u32 mid = blend[cx];
                u32 left = blend[cx ? cx - 1 : cx];
                u32 right = blend[cx + 1 < depth_width ? cx + 1 : cx];
                u32 dl = left > mid ? left - mid : mid - left;
                u32 dr = right > mid ? right - mid : mid - right;
                u32 l_eff = dl > CLIFF_Q5 ? mid : left;
                u32 r_eff = dr > CLIFF_Q5 ? mid : right;
                u32 dl_eff = l_eff > mid ? l_eff - mid : mid - l_eff;
                u32 dr_eff = r_eff > mid ? r_eff - mid : mid - r_eff;
                if (dl_eff > HFLAT || dr_eff > HFLAT) {
                    u32 sh[4] = {
                        (5 * mid + 3 * l_eff + 4) >> 3,
                        (7 * mid + l_eff + 4) >> 3,
                        (7 * mid + r_eff + 4) >> 3,
                        (5 * mid + 3 * r_eff + 4) >> 3
                    };
                    u32 top = mid > l_eff ? mid : l_eff;
                    if (r_eff > top) top = r_eff;
                    if (sample_pixels4(d + x, s, x, sh, width)) {
                    } else if ((((x + 3) * 32 + top) >> 5) + 1 < width) {
                        for (u32 k = 0; k < 4; ++k) {
                            u32 q = (x + k) * 32 + sh[k], i = q >> 5, f = q & 31;
                            d[x + k] = (u8)((s[i] * (32 - f) + s[i + 1] * f + 16) >> 5);
                        }
                    } else {
                        for (u32 k = 0; k < 4; ++k)
                            d[x + k] = sample_q5(s, (int)((x + k) * 32 + sh[k]), width);
                    }
                    continue;
                }
                u32 q = x * 32 + mid, sx = q >> 5, frac = q & 31;
                if (cx + 1 < depth_width && right == mid) {
                    u32 r2 = blend[cx + 2 < depth_width ? cx + 2 : cx + 1];
                    u32 dr2 = r2 > mid ? r2 - mid : mid - r2;
                    if (dr2 <= HFLAT || dr2 > CLIFF_Q5) {
                        if (!frac && sx + 8 <= width) {
                            __builtin_memcpy(d + x, s + sx, 8);
                            ++cx; x += 4;
                            continue;
                        }
                        if (frac && sx + 8 < width) {
                            sample_cell8(d + x, s + sx, frac);
                            ++cx; x += 4;
                            continue;
                        }
                    }
                }
                if (!frac && sx + 4 <= width) {
                    __builtin_memcpy(d + x, s + sx, 4);
                } else if (sx + 4 < width) {
                    sample_cell4(d + x, s + sx, frac);
                } else {
                    for (u32 k = 0; k < 4; ++k)
                        d[x + k] = sample_q5(s, (int)q + (int)k * 32, width);
                }
            }
        }
    }
    return 0;
}

/* perf29: specialized joint U/V warp reading depth_q5 >> 1 (exact half of luma
 * shift for 4:2:0 chroma, aligned with warp_luma_q5_fast). */
static int warp_chroma_pair_q5_fast(const u8 *u_src, u8 *u_dst, const u8 *v_src, u8 *v_dst,
                                    const u16 *depth_q5, u32 width, u32 height,
                                    u32 u_src_stride, u32 u_dst_stride,
                                    u32 v_src_stride, u32 v_dst_stride, u32 depth_width,
                                    u32 depth_height, u32 begin, u32 end) {
    if (!depth_width || !depth_height || depth_width > 480 ||
        !u_src || !u_dst || !v_src || !v_dst || !depth_q5 ||
        u_src_stride < width || u_dst_stride < width ||
        v_src_stride < width || v_dst_stride < width ||
        width != depth_width * 2 || height != depth_height * 2 ||
        begin > end || end > depth_height) return -22;
    u16 blend[480];
    for (u32 zy = begin; zy < end; ++zy) {
        const u16 *here = depth_q5 + (size_t)zy * depth_width;
        const u16 *above = depth_q5 + (size_t)(zy ? zy - 1 : zy) * depth_width;
        const u16 *below = depth_q5 + (size_t)(zy + 1 < depth_height ? zy + 1 : zy) * depth_width;
        for (u32 ry = 0; ry < 2; ++ry) {
            const u16 *other = ry == 0 ? above : below;
            u32 zx = 0;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
            for (; zx + 8 <= depth_width; zx += 8) {
                uint16x8_t h = vld1q_u16(here + zx);
                uint16x8_t o = vld1q_u16(other + zx);
                uint16x8_t b = vaddq_u16(vaddq_u16(h, vshlq_n_u16(h, 1)), o);
                vst1q_u16(blend + zx, vrshrq_n_u16(b, 3));  /* >>2 vertical blend, >>1 chroma half */
            }
#endif
            for (; zx < depth_width; ++zx)
                blend[zx] = (u16)((here[zx] * 3 + other[zx] + 4) >> 3);
            u32 y = zy * 2 + ry;
            const u8 *s1 = u_src + (size_t)y * u_src_stride;
            u8 *d1 = u_dst + (size_t)y * u_dst_stride;
            const u8 *s2 = v_src + (size_t)y * v_src_stride;
            u8 *d2 = v_dst + (size_t)y * v_dst_stride;
            for (u32 cx = 0, x = 0; cx < depth_width; ++cx, x += 2) {
                u32 mid = blend[cx];
                u32 q = x * 32 + mid, sx = q >> 5, frac = q & 31;
                if (cx + 1 < depth_width && blend[cx + 1] == mid) {
                    if (!frac && sx + 4 <= width) {
                        __builtin_memcpy(d1 + x, s1 + sx, 4);
                        __builtin_memcpy(d2 + x, s2 + sx, 4);
                        ++cx; x += 2;
                        continue;
                    }
                    if (frac && sx + 4 < width) {
                        sample_cell4(d1 + x, s1 + sx, frac);
                        sample_cell4(d2 + x, s2 + sx, frac);
                        ++cx; x += 2;
                        continue;
                    }
                }
                if (!frac && sx + 2 <= width) {
                    __builtin_memcpy(d1 + x, s1 + sx, 2);
                    __builtin_memcpy(d2 + x, s2 + sx, 2);
                } else if (sx + 2 < width) {
                    sample_cell2_pair(d1 + x, s1 + sx, d2 + x, s2 + sx, frac);
                } else {
                    for (u32 k = 0; k < 2; ++k) {
                        d1[x + k] = sample_q5(s1, (int)q + (int)k * 32, width);
                        d2[x + k] = sample_q5(s2, (int)q + (int)k * 32, width);
                    }
                }
            }
        }
    }
    return 0;
}

static void *segment_worker(void *arg) {
    struct segment_job *j = arg;
    j->rc = 0;
    for (;;) {
        u32 chunk = __atomic_fetch_add(j->next, 1, __ATOMIC_RELAXED);
        u32 first = j->begin + chunk * SEGMENT_ROWS;
        if (first >= j->end) break;
        u32 last = first + SEGMENT_ROWS < j->end ? first + SEGMENT_ROWS : j->end;
        int rc;
        if (j->depth_q5) {
            rc = warp_luma_q5_fast(j->src[0], j->dst[0], j->depth_q5, j->width[0],
                                   j->height[0], j->src_stride[0], j->dst_stride[0],
                                   j->depth_width, j->depth_height, first, last);
            if (rc && !j->rc) j->rc = rc;
            rc = warp_chroma_pair_q5_fast(j->src[1], j->dst[1], j->src[2], j->dst[2],
                                          j->depth_q5, j->width[1], j->height[1],
                                          j->src_stride[1], j->dst_stride[1],
                                          j->src_stride[2], j->dst_stride[2],
                                          j->depth_width, j->depth_height, first, last);
            if (rc && !j->rc) j->rc = rc;
            continue;
        }
        rc = warp_plane_cells_bilinear_range(j->src[0], j->dst[0], j->depth, j->width[0],
                                       j->height[0], j->src_stride[0], j->dst_stride[0],
                                       j->depth_width, j->depth_height, first, last,
                                       j->fractional_bits);
        if (rc && !j->rc) j->rc = rc;
        rc = warp_chroma_pair_clean_range(j->src[1], j->dst[1], j->src[2], j->dst[2], j->depth,
                                    j->width[1], j->height[1], j->src_stride[1], j->dst_stride[1],
                                    j->src_stride[2], j->dst_stride[2], j->depth_width,
                                    j->depth_height, first, last, j->fractional_bits);
        if (rc && !j->rc) j->rc = rc;
    }
    return NULL;
}

/* Process one colour plane.  The vendor may request one warped eye or a
 * side-by-side buffer; both layouts are bounded by the validated strides. */
static int warp_plane(const u8 *src, u8 *dst, const u8 *depth,
                      u32 width, u32 height, u32 src_stride, u32 dst_stride,
                      u32 depth_width, u32 depth_stride, u32 depth_height,
                      int side_by_side, u32 fractional_bits) {
    if (fractional_bits < 2 || fractional_bits > 4 ||
        !src || !dst || !depth || !width || !height ||
        src_stride < width || dst_stride < width * (side_by_side ? 2U : 1U) ||
        depth_width < 16 || depth_stride < depth_width || !depth_height)
        return -22;
    for (u32 y = 0; y < height; ++y) {
        const u8 *s = src + (size_t)y * src_stride;
        u8 *d = dst + (size_t)y * dst_stride;
        const u8 *z = depth + (size_t)(y * depth_height / height) * depth_stride;
        for (u32 x = 0; x < width; ++x) {
            u8 near = z[x * depth_width / width];
            int shift = depth_shift(near, width, depth_width, fractional_bits);
            if (side_by_side) {
                d[x] = sample_q5(s, (int)x * 32 + shift / 2, width);
                d[x + width] = sample_q5(s, (int)x * 32 - shift / 2, width);
            } else {
                d[x] = sample_q5(s, (int)x * 32 + shift, width);
            }
        }
    }
    return 0;
}

static int validate_frame(const struct real3d_frame *f) {
    if (!f || f->width != 1920 || f->height != 1080 ||
        f->depth_width != 480 || f->fractional_bits < 2 || f->fractional_bits > 4 ||
        (f->src_stride_y != 1920 && f->src_stride_y != 2048) ||
        (f->dst_stride_y != 1920 && f->dst_stride_y != 2048) ||
        (f->src_stride_uv != 960 && f->src_stride_uv != 1024) ||
        (f->dst_stride_uv != 960 && f->dst_stride_uv != 1024)) return -22;
    /* No format plane, depth map or result slot may alias another. Validate
     * all ranges before the first mapping, cache operation or write. */
    u64 addresses[8];
    u32 sizes[8];
    for (u32 i = 0; i < 3; ++i) {
        addresses[i] = f->src[i]; addresses[i+3] = f->dst[i];
        sizes[i] = i ? f->src_stride_uv*540 : f->src_stride_y*1080;
        sizes[i+3] = i ? f->dst_stride_uv*540 : f->dst_stride_y*1080;
    }
    addresses[6] = f->depth; sizes[6] = 480*270;
    addresses[7] = f->result; sizes[7] = 64;
    for (u32 i = 0; i < 8; ++i) {
        if (!valid(addresses[i], sizes[i])) return -22;
        for (u32 j = 0; j < i; ++j)
            if (addresses[i] < addresses[j]+sizes[j] &&
                addresses[j] < addresses[i]+sizes[i]) return -22;
    }
    return 0;
}

int real3d_validate_frame_for_test(const struct real3d_frame *f) {
    return validate_frame(f);
}

/* perf22 depth pre-pass for the Pro+ path (480x270 map, not the image):
 * 1. Horizontal dilation (max of left/self/right): the vendor Pro+ pipeline
 *    has a dilate stage (dilate_ms in libnr_image_post_processor's WarpCost
 *    fields) that our port skipped. Near objects widen by one cell, so their
 *    silhouette moves whole and the smoothed displacement no longer drags the
 *    object's edge colour into the background (halo at strong depth).
 * 2. Gain: displacement x REAL3D_GAIN_Q8/256 (1.3), clamped to 255, for more
 *    depth than the menu's strongest level. Interior smoothing is unchanged,
 *    so no staircase returns. */
#define REAL3D_GAIN_Q8 256U   /* kept at 256 for real3d_depth_prepass_for_test compatibility */
static void depth_prepass(const u8 *z, u8 *out, u32 dw, u32 dh) {
    for (u32 y = 0; y < dh; ++y) {
        const u8 *row = z + (size_t)y * dw;
        u8 *o = out + (size_t)y * dw;
        for (u32 x = 0; x < dw; ++x) {
            u32 m = (row[x] * REAL3D_GAIN_Q8 + 128) >> 8;
            o[x] = (u8)(m > 255 ? 255 : m);
        }
    }
}

int real3d_depth_prepass_for_test(const u8 *z, u8 *out, u32 dw, u32 dh) {
    if (!z || !out || !dw || !dh) return -22;
    depth_prepass(z, out, dw, dh);
    return 0;
}

/* perf28: 1D Forward Z-Buffer depth projection + background-side disocclusion
 * fill + x1.35 depth gain (346/256).
 * 1. Scale raw NPU depth by x1.35 (clamped to 255) and apply 1-cell horizontal
 *    max dilation so an NPU depth mask 1 cell narrower than the RGB silhouette
 *    covers the true object edge.
 * 2. Forward-project each source cell xs to destination cell xd = xs - c0
 *    (c0 = ceil(d / cell_unit)) with a Z-buffer (max depth wins). Steep
 *    intermediate falling-edge ramp cells (> 1 cell drop on both sides) are
 *    omitted so soft NPU right edges do not drag foreground into background.
 *    Continuous surfaces (c0 <= c1 + 1) bridge 1-cell expansion gaps.
 * 3. Fill empty disocclusion holes from the right (background side) with
 *    (last_src_xs - xd) * cell_unit so disoccluded pixels sample the clean
 *    background to the right of the foreground object (zero foreground ghost,
 *    zero width loss, smooth ramp into warp_luma_bilinear_fast). */
#define REAL3D_ZBUF_GAIN_Q8 346U

static void zbuf_project_row(const u8 *z_row, u8 *out_row, u32 dw, u32 fractional_bits, u32 gain_q8) {
    u32 cell_shift = fractional_bits + 2;
    u32 cell_unit = 1U << cell_shift;
    u8 scaled[480], zr[480];
    short out[480], src_of[480];
    for (u32 x = 0; x < dw; ++x) {
        u32 m = ((u32)z_row[x] * gain_q8 + 128U) >> 8;
        scaled[x] = (u8)(m > 255U ? 255U : m);
        out[x] = -2;
        src_of[x] = -2;
        __asm__ volatile("" : "+m"(out[x]), "+m"(src_of[x]));
    }
    for (u32 x = 0; x < dw; ++x) {
        u8 a = scaled[x ? x - 1 : x], b = scaled[x], c = scaled[x + 1 < dw ? x + 1 : x];
        u8 mx = a > b ? a : b;
        zr[x] = c > mx ? c : mx;
    }
    for (u32 xs = 0; xs < dw; ++xs) {
        if (xs > 0 && xs + 1 < dw &&
            (u32)zr[xs - 1] > (u32)zr[xs] + cell_unit &&
            (u32)zr[xs] > (u32)zr[xs + 1] + cell_unit)
            continue;
        u32 d0 = zr[xs];
        u32 c0 = (d0 + cell_unit - 1U) >> cell_shift;
        int xd0 = (int)xs - (int)c0;
        if (xd0 >= 0 && xd0 < (int)dw && (short)d0 > out[xd0]) {
            out[xd0] = (short)d0;
            src_of[xd0] = (short)xs;
        }
        if (xs + 1 < dw) {
            u32 d1 = zr[xs + 1];
            u32 c1 = (d1 + cell_unit - 1U) >> cell_shift;
            if (c0 <= c1 + 1U) {
                int xd1 = (int)(xs + 1) - (int)c1;
                if (xd1 > xd0 + 1) {
                    short dm = (short)((d0 + d1) >> 1);
                    for (int xm = xd0 + 1; xm < xd1 && xm < (int)dw; ++xm) {
                        if (xm >= 0 && dm > out[xm]) {
                            out[xm] = dm;
                            src_of[xm] = (short)(xs + 1);
                        }
                    }
                }
            }
        }
    }
    u32 c_right = ((u32)zr[dw - 1] + cell_unit - 1U) >> cell_shift;
    u32 tail_start = c_right < dw - 1 ? (dw - 1 - c_right) : 0;
    for (u32 xd = tail_start; xd < dw; ++xd) {
        if (out[xd] < 0) {
            out[xd] = (short)zr[dw - 1];
            src_of[xd] = (short)(dw - 1);
        }
    }
    u32 last_s = dw - 1;
    for (int xd = (int)dw - 1; xd >= 0; --xd) {
        if (out[xd] >= 0) {
            last_s = (u32)src_of[xd];
            out_row[xd] = (u8)out[xd];
        } else {
            u32 fill = last_s > (u32)xd ? (last_s - (u32)xd) << cell_shift : 0;
            out_row[xd] = (u8)(fill > 255U ? 255U : fill);
        }
    }
}

int real3d_zbuf_project_for_test(const u8 *z, u8 *out, u32 dw, u32 dh, u32 fractional_bits, u32 gain_q8) {
    if (!z || !out || !dw || dw > 480 || !dh || fractional_bits < 2 || fractional_bits > 4) return -22;
    for (u32 y = 0; y < dh; ++y)
        zbuf_project_row(z + (size_t)y * dw, out + (size_t)y * dw, dw, fractional_bits, gain_q8);
    return 0;
}

/* perf29: adaptive strong 3D in Q5 (0..1408, up to 44 px — no u8 255 cap!).
 * Computes a 256-bin histogram of the 480x270 depth map, tracks p05/p95 with
 * a smooth EMA (alpha = 1/16, no temporal pumping), and targets ~38 px (1216 Q5)
 * relative stereo disparity between p05 and p95 (gain 1.8x..3.2x, soft-clamped
 * at 44 px, fading to 1.0x on flat 2D menus/title cards where span < 8).
 * No blind dilation -> thin details (wires, hair, text) keep exact width and
 * zero background-grab; paired with cliff-guarded warp_luma_q5_fast -> zero
 * ghosting and zero staircasing. */
#define REAL3D_Q5_TARGET 1216U   /* 38.0 luma px between p05 and p95 */
#define REAL3D_Q5_MAX    1408U   /* 44.0 luma px hard cap */
#define REAL3D_GAIN_MIN  460U    /* 1.80x minimum boost on 3D scenes */
#define REAL3D_GAIN_MAX  820U    /* 3.20x maximum boost on low-contrast scenes */
static u16 depth_q5_buf[480 * 270];
static int ema_ready, ema_p05_q8, ema_p95_q8;

static void compute_adaptive_depth_params(const u8 *z, u32 size, u32 fractional_bits,
                                          u32 *z_floor_out, u32 *scale_q8_out) {
    u32 hist[256];
    for (u32 i = 0; i < 256; ++i) {
        hist[i] = 0;
        __asm__ volatile("" : "+m"(hist[i]));
    }
    for (u32 i = 0; i < size; ++i) ++hist[z[i]];
    u32 lo_target = size / 20U, hi_target = size - lo_target;
    u32 acc = 0, p05 = 0, p95 = 255;
    int got_lo = 0;
    for (u32 i = 0; i < 256; ++i) {
        acc += hist[i];
        if (!got_lo && acc >= lo_target) { p05 = i; got_lo = 1; }
        if (acc >= hi_target) { p95 = i; break; }
    }
    if (!ema_ready) {
        ema_p05_q8 = (int)(p05 << 8);
        ema_p95_q8 = (int)(p95 << 8);
        ema_ready = 1;
    } else {
        ema_p05_q8 += ((int)(p05 << 8) - ema_p05_q8) >> 4;
        ema_p95_q8 += ((int)(p95 << 8) - ema_p95_q8) >> 4;
    }
    int span = (ema_p95_q8 - ema_p05_q8) >> 8;
    if (span < 8) {
        *z_floor_out = 0;
        *scale_q8_out = 256U;
        return;
    }
    u32 raw_q5 = (u32)span << (5 - fractional_bits);
    u32 scale_q8 = (REAL3D_Q5_TARGET * 256U) / (raw_q5 ? raw_q5 : 1U);
    if (scale_q8 < REAL3D_GAIN_MIN) scale_q8 = REAL3D_GAIN_MIN;
    if (scale_q8 > REAL3D_GAIN_MAX) scale_q8 = REAL3D_GAIN_MAX;
    *z_floor_out = (u32)(ema_p05_q8 > 0 ? (ema_p05_q8 >> 8) : 0);
    *scale_q8_out = scale_q8;
}

static void zbuf_q5_project_row(const u8 *z_row, u16 *out_row, u32 dw,
                                u32 fractional_bits, u32 z_floor, u32 scale_q8) {
    u32 sh = 5 - fractional_bits;
    u16 d_src[480];
    short out[480], src_of[480];
    for (u32 x = 0; x < dw; ++x) {
        u32 dz = z_row[x] > z_floor ? (u32)z_row[x] - z_floor : 0U;
        u32 dq = ((dz << sh) * scale_q8 + 128U) >> 8;
        d_src[x] = (u16)(dq > REAL3D_Q5_MAX ? REAL3D_Q5_MAX : dq);
        out[x] = -2;
        src_of[x] = -2;
        __asm__ volatile("" : "+m"(out[x]), "+m"(src_of[x]));
    }
    for (u32 xs = 0; xs < dw; ++xs) {
        if (xs > 0 && xs + 1 < dw &&
            (u32)d_src[xs - 1] > (u32)d_src[xs] + 128U &&
            (u32)d_src[xs] > (u32)d_src[xs + 1] + 128U)
            continue;
        u32 d0 = d_src[xs];
        u32 c0 = (d0 + 64U) >> 7;
        int xd0 = (int)xs - (int)c0;
        if (xd0 >= 0 && xd0 < (int)dw && (short)d0 > out[xd0]) {
            out[xd0] = (short)d0;
            src_of[xd0] = (short)xs;
        }
        if (xs + 1 < dw) {
            u32 d1 = d_src[xs + 1];
            u32 c1 = (d1 + 64U) >> 7;
            if (c0 <= c1 + 1U) {
                int xd1 = (int)(xs + 1) - (int)c1;
                if (xd1 > xd0 + 1) {
                    short dm = (short)((d0 + d1) >> 1);
                    for (int xm = xd0 + 1; xm < xd1 && xm < (int)dw; ++xm) {
                        if (xm >= 0 && dm > out[xm]) {
                            out[xm] = dm;
                            src_of[xm] = (short)(xs + 1);
                        }
                    }
                }
            }
        }
    }
    u32 c_right = ((u32)d_src[dw - 1] + 64U) >> 7;
    u32 tail_start = c_right < dw - 1 ? (dw - 1 - c_right) : 0;
    for (u32 xd = tail_start; xd < dw; ++xd) {
        if (out[xd] < 0) {
            out[xd] = (short)d_src[dw - 1];
            src_of[xd] = (short)(dw - 1);
        }
    }
    u32 last_s = dw - 1;
    for (int xd = (int)dw - 1; xd >= 0; --xd) {
        if (out[xd] >= 0) {
            last_s = (u32)src_of[xd];
            out_row[xd] = (u16)out[xd];
        } else {
            u32 fill = last_s > (u32)xd ? (last_s - (u32)xd) << 7 : 0;
            out_row[xd] = (u16)(fill > REAL3D_Q5_MAX ? REAL3D_Q5_MAX : fill);
        }
    }
}

struct zbuf_q5_job {
    const u8 *z;
    u16 *out;
    u32 dw, dh, fractional_bits, z_floor, scale_q8;
    u32 *next;
};

static void *zbuf_q5_worker(void *arg) {
    struct zbuf_q5_job *j = arg;
    for (;;) {
        u32 chunk = __atomic_fetch_add(j->next, 1, __ATOMIC_RELAXED);
        u32 first = chunk * SEGMENT_ROWS;
        if (first >= j->dh) break;
        u32 last = first + SEGMENT_ROWS < j->dh ? first + SEGMENT_ROWS : j->dh;
        for (u32 y = first; y < last; ++y)
            zbuf_q5_project_row(j->z + (size_t)y * j->dw, j->out + (size_t)y * j->dw,
                                j->dw, j->fractional_bits, j->z_floor, j->scale_q8);
    }
    return NULL;
}

int real3d_zbuf_q5_warp_for_test(const u8 *src, u8 *dst, const u8 *z,
                                 u32 width, u32 height, u32 stride,
                                 u32 dw, u32 dh, u32 fractional_bits,
                                 u32 z_floor, u32 scale_q8) {
    if (!src || !dst || !z || !dw || dw > 480 || !dh || dw * dh > 480 * 270 ||
        fractional_bits < 2 || fractional_bits > 4) return -22;
    for (u32 y = 0; y < dh; ++y)
        zbuf_q5_project_row(z + (size_t)y * dw, depth_q5_buf + (size_t)y * dw,
                            dw, fractional_bits, z_floor, scale_q8);
    return warp_luma_q5_fast(src, dst, depth_q5_buf, width, height, stride, stride,
                             dw, dh, 0, dh);
}

static int process_frame(const struct real3d_frame *f) {
    /* Recovered Process ABI Q2/Q3/Q4: one warped I420 frame, 480x270 depth.
     * Stride is padding, never evidence of side-by-side or a pixel format. */
    if (!initialized || validate_frame(f)) return -22;
    int side_by_side = 0;
    int chroma_half = 1;
    u32 depth_height = (f->height + 3) / 4;
    u32 depth_size = f->depth_width * depth_height;

    u64 started_at = monotonic_us();
    u8 *z = map(f->depth, depth_size);
    if (!z) return -5;
    if (frames < 3 || frames % 300 == 0) {
        depth_min = 255;
        depth_max = shifted_samples = 0;
        for (u32 i = 0; i < depth_size; ++i) {
            u8 v = z[i];
            if (v < depth_min) depth_min = v;
            if (v > depth_max) depth_max = v;
            if (depth_shift(v, f->width, f->depth_width, f->fractional_bits)) ++shifted_samples;
        }
    }
    int rc = 0;
    u8 *source[3] = {NULL, NULL, NULL};
    u8 *destination[3] = {NULL, NULL, NULL};
    u32 widths[3], heights[3], src_strides[3], dst_strides[3];
    u32 src_sizes[3], dst_sizes[3];
    for (u32 channel = 0; channel < 3; ++channel) {
        widths[channel] = channel && chroma_half ? (f->width + 1) / 2 : f->width;
        heights[channel] = channel && chroma_half ? (f->height + 1) / 2 : f->height;
        src_strides[channel] = channel ? f->src_stride_uv : f->src_stride_y;
        dst_strides[channel] = channel ? f->dst_stride_uv : f->dst_stride_y;
        src_sizes[channel] = src_strides[channel] * heights[channel];
        dst_sizes[channel] = dst_strides[channel] * heights[channel];
    }
    for (u32 channel = 0; channel < 3; ++channel) {
        source[channel] = map(f->src[channel], src_sizes[channel]);
        destination[channel] = map_output(f->dst[channel], dst_sizes[channel], dst_strides[channel]);
        if (!source[channel] || !destination[channel]) { rc = -5; goto done; }
    }
    u64 mapped_at = monotonic_us();

#ifdef XREAL_COMPARE_ONEPRO_WARP
    int legacy = xreal_compare_algorithm() == 1;
    if (legacy) {
        rc = xreal_onepro_warp(source, destination, src_strides, dst_strides,
                               z, f->fractional_bits);
    } else
#endif
    if (!side_by_side && chroma_half && f->width == 1920 && f->height == 1080) {
        /* Four independent depth-row segments on the four Cortex-A53 cores. */
        const u16 *dq5 = NULL;
        if (f->depth_width <= 480 && f->depth_width * depth_height <= 480U * 270U) {
            u32 z_floor = 0, scale_q8 = 256U;
            compute_adaptive_depth_params(z, f->depth_width * depth_height,
                                          f->fractional_bits, &z_floor, &scale_q8);
            struct zbuf_q5_job zjobs[4];
            u32 znext = 0;
            for (u32 i = 0; i < 4; ++i)
                zjobs[i] = (struct zbuf_q5_job){
                    .z = z, .out = depth_q5_buf, .dw = f->depth_width, .dh = depth_height,
                    .fractional_bits = f->fractional_bits, .z_floor = z_floor,
                    .scale_q8 = scale_q8, .next = &znext,
                };
            xreal_parallel_run(zbuf_q5_worker, zjobs, sizeof(zjobs[0]), 4);
            dq5 = depth_q5_buf;
        }
        struct segment_job jobs[4];
        u32 next_chunk = 0;
        for (u32 i = 0; i < 4; ++i) {
            jobs[i] = (struct segment_job){
                .depth = z, .depth_q5 = dq5, .depth_width = f->depth_width,
                .depth_height = depth_height, .fractional_bits = f->fractional_bits,
                .begin = 0, .end = depth_height, .next = &next_chunk, .rc = 0,
            };
            for (u32 channel = 0; channel < 3; ++channel) {
                jobs[i].src[channel] = source[channel];
                jobs[i].dst[channel] = destination[channel];
                jobs[i].eye2[channel] = (u8 *)0;
                jobs[i].width[channel] = widths[channel];
                jobs[i].height[channel] = heights[channel];
                jobs[i].src_stride[channel] = src_strides[channel];
                jobs[i].dst_stride[channel] = dst_strides[channel];
            }
        }
        xreal_parallel_run(segment_worker, jobs, sizeof(jobs[0]), 4);
        for (u32 i = 0; i < 4; ++i)
            if (jobs[i].rc) rc = jobs[i].rc;
    } else for (u32 channel = 0; channel < 3; ++channel) {
        int sbs = side_by_side && dst_strides[channel] >= 2 * widths[channel];
        if (!sbs && widths[channel] % f->depth_width == 0 &&
            heights[channel] % depth_height == 0 &&
            (widths[channel] / f->depth_width == 2 ||
             widths[channel] / f->depth_width == 4))
            rc = warp_plane_cells(source[channel], destination[channel], z,
                                  widths[channel], heights[channel],
                                  src_strides[channel], dst_strides[channel],
                                  f->depth_width, depth_height, f->fractional_bits);
        else rc = warp_plane(source[channel], destination[channel], z,
                             widths[channel], heights[channel],
                             src_strides[channel], dst_strides[channel],
                             f->depth_width, f->depth_width, depth_height, sbs, f->fractional_bits);
        if (rc) break;
    }
    u64 warped_at = monotonic_us();
    if (!rc) {
        /* Vendor HAL performs a global cache flush for sizes above 1 MiB
         * (same as AI HDR); dst[0] is 2.07 MiB for 1080p Y, covering all
         * modified planes (including legacy src planes) in one ioctl. */
        if (dst_sizes[0] > 0x100000U) {
            if (ar_hal_sys_mmz_flush_cache_pa(f->dst[0], dst_sizes[0])) rc = -5;
        } else {
#ifdef XREAL_COMPARE_ONEPRO_WARP
            if (legacy) for (u32 channel = 0; channel < 3; ++channel)
                if (ar_hal_sys_mmz_flush_cache_pa(f->src[channel], src_sizes[channel])) {
                    rc = -5;
                    break;
                }
#endif
            if (!rc) for (u32 channel = 0; channel < 3; ++channel)
                if (ar_hal_sys_mmz_flush_cache_pa(f->dst[channel], dst_sizes[channel])) {
                    rc = -5;
                    break;
                }
        }
    }
    if (frames < 3 || frames % 300 == 0) {
        map_us = (u32)(mapped_at - started_at);
        warp_us = (u32)(warped_at - mapped_at);
        flush_us = (u32)(monotonic_us() - warped_at);
    }
#ifdef XREAL_CAPTURE_REAL3D
    if (!rc && !captured && frames == 120) {
        captured = 1;
        int capture_rc = capture_plane("/usrdata/real3d-input.pgm", source[0],
                                       f->width, f->height, f->src_stride_y);
        capture_rc |= capture_plane("/usrdata/real3d-warped.pgm", destination[0],
                                    f->width, f->height, f->dst_stride_y);
        capture_rc |= capture_plane("/usrdata/real3d-displacement.pgm", z,
                                    f->depth_width, depth_height, f->depth_width);
        char meta[128];
        int n = snprintf(meta, sizeof(meta),
            "{\"frame\":%u,\"fractional_bits\":%u,\"capture_rc\":%d}\n",
            frames, f->fractional_bits, capture_rc);
        int fd = open("/usrdata/real3d-capture.json", 1 | 64 | 512, 0600);
        if (fd >= 0) {
            if (n <= 0 || n >= (int)sizeof(meta) || write_all(fd, meta, (size_t)n))
                capture_rc = -1;
            close(fd);
        } else capture_rc = -1;
        diagnostic("capture", f, capture_rc);
    }
#endif
done:
    for (u32 channel = 0; channel < 3; ++channel) {
        if (destination[channel]) ar_hal_sys_munmap(destination[channel], dst_sizes[channel]);
        if (source[channel]) ar_hal_sys_munmap(source[channel], src_sizes[channel]);
    }
    ar_hal_sys_munmap(z, depth_size);
    return rc;
}

int xreal_real3d_rpc(u32 core, const struct rpc *call, u32 sync) {
    if (core || !call || sync != 1 || !call->args) return -22;
    if (!strcmp(call->name, "ArArmWarp2DTo3DInit")) {
        if (call->length != 8) return -22;
        u64 result = *(const u64 *)call->args;
        int rc = set_result(result, 0);
        initialized = rc == 0;
        frames = 0;
        ema_ready = ema_p05_q8 = ema_p95_q8 = 0;
        pace_last_start = pace_slept_us = pace_late = 0;
        diagnostic("init", NULL, rc);
        return rc;
    }
    if (!strcmp(call->name, "ArArmWarp2DTo3DClose")) {
        if (call->length != 8) return -22;
        u64 result = *(const u64 *)call->args;
        initialized = 0;
        int rc = set_result(result, 0);
        diagnostic("close", NULL, rc);
        return rc;
    }
    if (!strcmp(call->name, "ArArmWarp2DTo3DProcess")) {
        if (call->length != sizeof(struct real3d_frame)) return -22;
        const struct real3d_frame *f = call->args;
        u64 call_start = monotonic_us();
        int rc = process_frame(f);
        if (rc || frames < 3 || frames % 300 == 0) {
#ifdef XREAL_COMPARE_ONEPRO_WARP
            diagnostic(xreal_compare_algorithm() == 1 ?
                       "process-onepro-mesh" : "process-proplus-port", f, rc);
#else
            diagnostic("process", f, rc);
#endif
        }
        if (rc) {
            /* A rejected descriptor may alias result with a frame plane.
             * Only a fully validated descriptor permits a status write.
             * Runtime map/flush failures must not leave an old success flag. */
            if (!validate_frame(f)) (void)set_result(f->result, rc);
            return rc;
        }
        ++frames;
        int status = set_result(f->result, 0);
        if (pace_last_start) {
            /* perf10 fix: from THIS call's start (perf9 used the previous
             * call's start, which let every other frame through unpaced). */
            u64 elapsed = monotonic_us() - call_start;
            if (elapsed < PACE_US) {
                u64 wait = PACE_US - elapsed;
                struct timespec t = { (long)(wait / 1000000ULL), (long)(wait % 1000000ULL) * 1000 };
                (void)nanosleep(&t, NULL);
                pace_slept_us += wait;
            } else {
                ++pace_late;
            }
        }
        pace_last_start = call_start;
        if (frames % 300 == 0) {
            char line[160];
            int n = snprintf(line, sizeof(line), "Real3D CPU pace frame=%u slept_avg_us=%lu late=%lu target_us=%lu\n",
                             frames, (unsigned long)(pace_slept_us / 300), (unsigned long)pace_late,
                             (unsigned long)PACE_US);
            pace_slept_us = pace_late = 0;
            if (n > 0 && n < (int)sizeof(line)) real3d_log(line, (size_t)n);
        }
        return status;
    }
    return -95;
}

int real3d_warp_plane_for_test(const u8 *src, u8 *dst, const u8 *depth,
                               u32 width, u32 height, u32 src_stride,
                               u32 dst_stride, u32 depth_width,
                               u32 depth_stride, u32 depth_height, int sbs, u32 fractional_bits) {
    return warp_plane(src, dst, depth, width, height, src_stride, dst_stride,
                      depth_width, depth_stride, depth_height, sbs, fractional_bits);
}

int real3d_warp_cells_for_test(const u8 *src, u8 *dst, const u8 *depth,
                               u32 width, u32 height, u32 src_stride,
                               u32 dst_stride, u32 depth_width,
                               u32 depth_height, u32 fractional_bits) {
    return warp_plane_cells(src, dst, depth, width, height, src_stride,
                            dst_stride, depth_width, depth_height, fractional_bits);
}

int real3d_warp_cells_range_for_test(const u8 *src, u8 *dst, const u8 *depth,
                                     u32 width, u32 height, u32 src_stride,
                                     u32 dst_stride, u32 depth_width,
                                     u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_range(src, dst, depth, width, height,
                                  src_stride, dst_stride, depth_width,
                                  depth_height, begin, end, fractional_bits);
}

int real3d_warp_cells_smooth_range_for_test(const u8 *src, u8 *dst, const u8 *depth,
                                            u32 width, u32 height, u32 src_stride,
                                            u32 dst_stride, u32 depth_width,
                                            u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_smooth_range(src, dst, depth, width, height,
                                         src_stride, dst_stride, depth_width,
                                         depth_height, begin, end, fractional_bits);
}

int real3d_warp_cells_bilinear_range_for_test(const u8 *src, u8 *dst, const u8 *depth,
                                              u32 width, u32 height, u32 src_stride,
                                              u32 dst_stride, u32 depth_width,
                                              u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_plane_cells_bilinear_range(src, dst, depth, width, height,
                                           src_stride, dst_stride, depth_width,
                                           depth_height, begin, end, fractional_bits);
}

int real3d_warp_luma_cubic_range_for_test(const u8 *src, u8 *dst, const u8 *depth,
                                          u32 width, u32 height, u32 src_stride,
                                          u32 dst_stride, u32 depth_width,
                                          u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_luma_cubic_range(src, dst, depth, width, height, src_stride, dst_stride,
                                 depth_width, depth_height, begin, end, fractional_bits);
}

int real3d_warp_chroma_pair_range_for_test(const u8 *u_src, u8 *u_dst, const u8 *v_src, u8 *v_dst,
                                           const u8 *depth, u32 width, u32 height, u32 stride,
                                           u32 depth_width, u32 depth_height, u32 begin, u32 end,
                                           u32 fractional_bits) {
    return warp_chroma_pair_range(u_src, u_dst, v_src, v_dst, depth, width, height,
                                  stride, stride, stride, stride, depth_width, depth_height,
                                  begin, end, fractional_bits);
}

int real3d_warp_luma_jbu_range_for_test(const u8 *src, u8 *dst, const u8 *depth,
                                        u32 width, u32 height, u32 src_stride,
                                        u32 dst_stride, u32 depth_width,
                                        u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_luma_jbu_range(src, dst, depth, width, height, src_stride, dst_stride,
                               depth_width, depth_height, begin, end, fractional_bits);
}

int real3d_warp_luma_reproject_range_for_test(const u8 *src, u8 *dst, const u8 *depth,
                                              u32 width, u32 height, u32 src_stride,
                                              u32 dst_stride, u32 depth_width,
                                              u32 depth_height, u32 begin, u32 end, u32 fractional_bits) {
    return warp_luma_reproject_range(src, dst, depth, width, height, src_stride, dst_stride,
                                     depth_width, depth_height, begin, end, fractional_bits);
}
