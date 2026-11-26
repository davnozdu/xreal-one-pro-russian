/* Sole-owner VDEC harness for Artosyn AR9481 (XREAL One Pro).
 * Decodes H.264/H.265 from a raw elementary-stream file, no display.
 * Freestanding: raw syscalls, links the vendor MPI libs directly.
 *
 * usage: poc_dec <es-file> <max-frames>
 * prints: init results, pool/chn results, decoded frame count, first frame info,
 * elapsed ms and fps.
 */
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef unsigned char u8;

__attribute__((unused)) static inline i64 sys1(i64 n, i64 a)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}
static inline i64 sys3(i64 n, i64 a, i64 b, i64 c)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static inline i64 sys4(i64 n, i64 a, i64 b, i64 c, i64 d)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}
static i64 out(const char *s, u64 n) { return sys3(64, 1, (i64)s, (i64)n); }
static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { out(s, slen(s)); }
static void putdec(long v)
{
    char b[24]; int i = 23; int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[23] = '\n';
    do { b[--i] = (char)('0' + (u % 10)); u /= 10; } while (u && i > 0);
    if (neg) b[--i] = '-';
    out(b + i, (u64)(23 - i + 1));
}
static void puthex(u64 v)
{
    char b[20]; int i = 19;
    b[19] = '\n';
    if (!v) { out("0x0\n", 4); return; }
    while (v && i > 0) {
        int d = (int)(v & 15);
        b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    out("0x", 2);
    out(b + i, (u64)(20 - i));
}
__attribute__((unused)) static void die(long code)
{
    register i64 x8 __asm__("x8") = 93;
    register i64 x0 __asm__("x0") = code;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    for (;;) { }
}

static u64 now_ms(void)
{
    struct { long sec; long nsec; } ts = {0, 0};
    sys3(113 /*clock_gettime*/, 1 /*CLOCK_MONOTONIC*/, (i64)&ts, 0);
    return (u64)ts.sec * 1000u + (u64)ts.nsec / 1000000u;
}

struct attr_s {                    /* VDEC_CHN_ATTR on AR9481: no enVBSource, 4 bytes tighter than the Proxima doc */
    u32 enType;                    /* +0  : 96 = H.264, 265 = H.265, 26 = JPEG */
    u32 enMode;                    /* +4  : 0 = frame mode */
    u32 picWidth;                  /* +8  : max width  */
    u32 picHeight;                 /* +12 : max height */
    u32 streamBufSize;             /* +16 : must exceed 32 KiB (checked in CreateChn) */
    u32 frameBufSize;              /* +20 */
    u32 frameBufCnt;               /* +24 */
    u32 refFrameNum;               /* +28 */
    u32 temporalMvp;               /* +32 */
    u32 tmvBufSize;                /* +36 */
    u32 cmdQueueDepth;             /* +40 */
    u32 reserved[11];              /* +44..+87 */
};

struct vb_pool_cfg_s {             /* 40 bytes */
    u64 blkSize;                   /* +0 */
    u32 blkCnt;                    /* +8 */
    u32 remapMode;                 /* +12 */
    char mmzName[24];              /* +16 */
};

struct vdec_stream_s {             /* 48 bytes */
    u32 len;                       /* +0  */
    u32 pad0;
    u64 phyAddr;                   /* +8  */
    u64 pts;                       /* +16 */
    u32 endOfFrame;                /* +24 (doc: +20; kept both non-zero below) */
    u32 rsv0;
    u32 virLo;                     /* +32 */
    u32 virHi;                     /* +36 */
    u32 rsv[2];
};

struct frame_s {                   /* >= 448 bytes */
    u8 raw[512];
};

extern int AR_MPI_SYS_Init(void);
extern int AR_MPI_VDEC_Init(int chnNum);
extern int AR_MPI_VDEC_CreateChn(int chn, void *attr);
extern int AR_MPI_VDEC_DestroyChn(int chn);
extern int AR_MPI_VDEC_StartRecvStream(int chn);
extern int AR_MPI_VDEC_StopRecvStream(int chn);
extern int AR_MPI_VDEC_AttachVbPool(int chn, int pool);
extern int AR_MPI_VDEC_DetachVbPool(int chn);
extern int AR_MPI_VDEC_SendStream(int chn, void *stream, int milliSec);
extern int AR_MPI_VDEC_GetFrame(int chn, void *frame, int milliSec);
extern int AR_MPI_VDEC_ReleaseFrame(int chn, void *frame);
extern int AR_MPI_VB_SetConfig(void *cfg);
extern int AR_MPI_VB_Init(void);
extern int AR_MPI_VB_SetModPoolConfig(int uid, void *cfg);
extern int AR_MPI_VB_InitModCommPool(int uid);
extern int AR_MPI_VB_CreatePool(void *cfg);
extern int AR_MPI_VB_DestroyPool(int pool);

struct vb_config_s {               /* 648 bytes: u32MaxPoolCnt + 16 * VB_POOL_CONFIG_S(40) */
    u32 maxPoolCnt;
    u32 pad;
    struct vb_pool_cfg_s commPool[16];
};

static struct vb_config_s vbconf;
static struct attr_s attr;
static struct vb_pool_cfg_s vbcfg;
static struct vdec_stream_s st;
static struct frame_s frame;
static char iobuf[65536];

__attribute__((used)) long run(void)
{
    /* argv via /proc/self/cmdline: <self> <file> <maxframes> */
    char cmd[256];
    i64 fd = sys4(56, -100, (i64)"/proc/self/cmdline", 0, 0);
    if (fd < 0) { put("no cmdline\n"); return 1; }
    i64 n = sys3(63, fd, (i64)cmd, (i64)sizeof(cmd) - 1);
    sys3(57, fd, 0, 0);
    if (n <= 0) { put("cmdline empty\n"); return 1; }
    cmd[n] = 0;
    char *path = 0, *arg2 = 0;
    int field = 0;
    for (i64 i = 0; i < n; i++) {
        if (cmd[i] == 0) { field++; if (field == 1) path = &cmd[i + 1]; if (field == 2) { arg2 = &cmd[i + 1]; break; } }
    }
    if (!path || !*path) { put("usage: poc_dec <es> <maxframes>\n"); return 1; }
    long maxFrames = 0;
    if (arg2) for (char *p = arg2; *p >= '0' && *p <= '9'; p++) maxFrames = maxFrames * 10 + (*p - '0');
    if (maxFrames <= 0) maxFrames = 120;

    int chn = 20;

    put("SYS_Init -> "); putdec(AR_MPI_SYS_Init());

    /* scan mode: find which VB_UID accepts a module common pool */
    if (path[0] == 's' && path[1] == 'c') {
        vbconf.maxPoolCnt = 16;
        for (int i = 0; i < 16; i++) {
            vbconf.commPool[i].blkSize = 4096;
            vbconf.commPool[i].blkCnt = 2;
        }
        for (int uid = 0; uid < 20; uid++) {
            int a = AR_MPI_VB_SetModPoolConfig(uid, &vbconf);
            int b = AR_MPI_VB_InitModCommPool(uid);
            put("uid="); putdec(uid);
            put("  SetModPoolConfig="); putdec(a);
            put("  InitModCommPool="); putdec(b);
        }
        return 0;
    }

    vbconf.maxPoolCnt = 4;
    vbconf.commPool[0].blkSize = 1280u * 720u * 3u / 2u;
    vbconf.commPool[0].blkCnt = 12;
    put("VB_SetConfig -> "); putdec(AR_MPI_VB_SetConfig(&vbconf));
    put("VB_Init -> "); putdec(AR_MPI_VB_Init());
    put("VB_InitModCommPool(4=VDEC) -> "); putdec(AR_MPI_VB_InitModCommPool(4));

    put("VDEC_Init(40) -> "); putdec(AR_MPI_VDEC_Init(40));

    attr.enType = 96;              /* H.264 */
    attr.enMode = 0;
    attr.picWidth = 1280;
    attr.picHeight = 720;
    attr.streamBufSize = 1024 * 1024;
    attr.frameBufSize = 1280u * 720u * 3u / 2u;
    attr.frameBufCnt = 8;
    attr.refFrameNum = 4;
    attr.temporalMvp = 0;
    attr.tmvBufSize = 0;
    attr.cmdQueueDepth = 8;

    int rc = AR_MPI_VDEC_CreateChn(chn, &attr);
    put("CreateChn -> "); putdec(rc);
    if (rc != 0) { put("abort: no channel\n"); return 3; }

    vbcfg.blkSize = 1280u * 720u * 3u / 2u;
    vbcfg.blkCnt = 12;
    int pool = AR_MPI_VB_CreatePool(&vbcfg);
    put("VB_CreatePool -> "); putdec(pool);
    if (pool >= 0) {
        put("AttachVbPool -> "); putdec(AR_MPI_VDEC_AttachVbPool(chn, pool));
    }
    put("StartRecvStream -> "); putdec(AR_MPI_VDEC_StartRecvStream(chn));

    i64 f = sys4(56, -100, (i64)path, 0, 0);
    if (f < 0) { put("cannot open es file\n"); return 2; }

    u64 t0 = now_ms();
    long frames = 0, sent = 0;
    for (;;) {
        i64 got = sys3(63, f, (i64)iobuf, (i64)sizeof(iobuf));
        if (got <= 0) break;
        sent++;
        st.len = (u32)got;
        st.phyAddr = 0;
        st.pts = 0;
        st.endOfFrame = 0;
        st.virLo = (u32)(u64)(unsigned long)iobuf;
        st.virHi = (u32)((u64)(unsigned long)iobuf >> 32);
        int s = AR_MPI_VDEC_SendStream(chn, &st, 200);
        if (s != 0) { put("SendStream err "); putdec(s); break; }

        int g;
        while ((g = AR_MPI_VDEC_GetFrame(chn, &frame, 0)) == 0) {
            frames++;
            if (frames == 1) {
                put("frame: w="); putdec(*(u32 *)(frame.raw + 4));
                put(" h="); putdec(*(u32 *)(frame.raw + 8));
                put(" pixfmt="); putdec(*(u32 *)(frame.raw + 16));
                put(" phys_y="); puthex(*(u64 *)(frame.raw + 144));
                put(" stride="); putdec(*(u32 *)(frame.raw + 356));
            }
            AR_MPI_VDEC_ReleaseFrame(chn, &frame);
            if (frames >= maxFrames) break;
        }
        if (frames >= maxFrames) break;
    }
    /* drain: keep pulling until nothing comes for a while */
    for (int i = 0; i < 50 && frames < maxFrames; i++) {
        if (AR_MPI_VDEC_GetFrame(chn, &frame, 20) != 0) break;
        frames++;
        AR_MPI_VDEC_ReleaseFrame(chn, &frame);
    }
    u64 t1 = now_ms();
    sys3(57, f, 0, 0);

    put("frames="); putdec(frames);
    put(" chunks="); putdec(sent);
    put(" ms="); putdec((long)(t1 - t0));
    if (t1 > t0) {
        put(" fps="); putdec((long)((u64)frames * 1000u / (t1 - t0)));
    }
    put("StopRecvStream -> "); putdec(AR_MPI_VDEC_StopRecvStream(chn));
    put("DestroyChn -> "); putdec(AR_MPI_VDEC_DestroyChn(chn));
    if (pool >= 0) {
        put("DetachVbPool -> "); putdec(AR_MPI_VDEC_DetachVbPool(chn));
        put("VB_DestroyPool -> "); putdec(AR_MPI_VB_DestroyPool(pool));
    }
    return 0;
}

__attribute__((naked)) void _start(void)
{
    __asm__ volatile(
        "mov x29, #0\n"
        "mov x30, #0\n"
        "bl run\n"
        "mov x8, #93\n"
        "svc #0\n");
}
