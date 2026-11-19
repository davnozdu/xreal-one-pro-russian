/* Full RGB cube, not three diagonal curves.
 * Expand R and G once at mode initialization, retain 17 B slices in Q2.
 * Two 32-bit table reads per pixel. 4.25 MiB (half the former table).
 * Q2 and rounded interpolation stay within one RGB8 level versus the cube. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef u32 pixels4 __attribute__((vector_size(16)));
typedef u8 bytes4 __attribute__((vector_size(4)));
#define TABLE_WORDS (17U * 256U * 256U)


unsigned long long xreal_rgb_table_bytes(void) {
    return (u64)(TABLE_WORDS+256) * sizeof(u32);
}

int xreal_build_rgb_table_range(const u16 *cube, u64 *table,
                                 u32 begin_b, u32 end_b) {
    if (!cube || !table || begin_b > end_b || end_b > 17) return -22;
    u32 *compact=(u32 *)table;
    if (!begin_b) for (u32 blue=0;blue<256;++blue) {
        u32 v=blue*16, slice=v/255, f=v-slice*255;
        if(slice==16){slice=15;f=255;}
        compact[TABLE_WORDS+blue]=(slice<<16) | ((f*256+127)/255);
    }
    for (u32 b = begin_b; b < end_b; ++b) {
        for (u32 g = 0; g < 256; ++g) {
            u32 gi = g*16/255, gf = g*16-gi*255;
            if (gi == 16) { gi=15; gf=255; }
            for (u32 r = 0; r < 256; ++r) {
                u32 ri=r*16/255, rf=r*16-ri*255;
                if (ri == 16) { ri=15; rf=255; }
                u32 base=((b*17+gi)*17+ri)*3;
                u32 packed=0;
                for (u32 c=0; c<3; ++c) {
                    u32 a=(cube[base+c]*(255-rf)+cube[base+3+c]*rf+127)/255;
                    u32 d=(cube[base+51+c]*(255-rf)+cube[base+54+c]*rf+127)/255;
                    u32 value=(a*(255-gf)+d*gf+127)/255;
                    packed |= ((value+32)/64) << (10*c);
                }
                compact[(b*256+g)*256+r]=packed;
            }
        }
    }
    return 0;
}

int xreal_full_rgb_lut(const u64 *table, u8 *r, u8 *g, u8 *b,
                       u32 rs, u32 gs, u32 bs, u32 width, u32 height) {
    if (!table || !r || !g || !b || !width || !height ||
        width>1920 || height>1080 || rs<width || gs<width || bs<width ||
        rs>4096 || gs>4096 || bs>4096) return -22;
    const u32 *compact=(const u32 *)table;
    for (u32 y=0; y<height; ++y) {
        u8 *rp=r+(u64)y*rs, *gp=g+(u64)y*gs, *bp=b+(u64)y*bs;
        u32 x=0;
        for (; x+4<=width; x+=4) {
            pixels4 a,d,weight;
            for (u32 lane=0;lane<4;++lane) {
                u32 mapping=compact[TABLE_WORDS+bp[x+lane]];
                u32 index=(mapping&0xffff0000) |
                          ((u32)gp[x+lane]<<8) | rp[x+lane];
                a[lane]=compact[index];d[lane]=compact[index+65536];
                weight[lane]=mapping&511;
            }
            pixels4 inverse=256-weight;
            pixels4 red=((a&1023)*inverse+(d&1023)*weight+512)>>10;
            pixels4 green=(((a>>10)&1023)*inverse+((d>>10)&1023)*weight+512)>>10;
            pixels4 blue=((a>>20)*inverse+(d>>20)*weight+512)>>10;
            bytes4 rr=__builtin_convertvector(red,bytes4);
            bytes4 gg=__builtin_convertvector(green,bytes4);
            bytes4 bb=__builtin_convertvector(blue,bytes4);
            __builtin_memcpy(rp+x,&rr,4);
            __builtin_memcpy(gp+x,&gg,4);
            __builtin_memcpy(bp+x,&bb,4);
        }
        for (; x<width; ++x) {
            u32 mapping=compact[TABLE_WORDS+bp[x]], weight=mapping&511;
            u32 index=(mapping&0xffff0000) | ((u32)gp[x]<<8) | rp[x];
            u32 a=compact[index], d=compact[index+65536];
            rp[x]=(u8)(((a&1023)*(256-weight)+(d&1023)*weight+512)>>10);
            gp[x]=(u8)((((a>>10)&1023)*(256-weight)+((d>>10)&1023)*weight+512)>>10);
            bp[x]=(u8)(((a>>20)*(256-weight)+(d>>20)*weight+512)>>10);
        }
    }
    return 0;
}

/* Compact cache-resident variant (perf branch).
 * The table above is 4.25 MiB and is read at random positions; the device
 * (4x Cortex-A53 @ 1.27 GHz, 512 KiB shared L2, 4 KiB pages, no THP) misses
 * L2 and the TLB on most pixels. Here only R is expanded, for each of the
 * 17x17 (B,G) cube nodes: 17*17*256 words = 289 KiB, which fits in L2.
 * G and B use exact 1/255 weights, so the only rounding is the Q2 entry
 * (<= 1/8 RGB8 level) and the final division: <= 1 level versus the cube.
 * Words after the table: 256 G-axis and 256 B-axis maps, (offset<<8)|frac. */
#define COMPACT_WORDS (17U * 17U * 256U)
#define COMPACT_G_STEP 256U
#define COMPACT_B_STEP (17U * 256U)

unsigned long long xreal_compact_table_bytes(void) {
    return (u64)(COMPACT_WORDS + 512U) * sizeof(u32);
}

int xreal_build_compact_table_range(const u16 *cube, u32 *table,
                                    u32 begin_b, u32 end_b) {
    if (!cube || !table || begin_b > end_b || end_b > 17) return -22;
    if (!begin_b) for (u32 v = 0; v < 256; ++v) {
        u32 i = v*16/255, f = v*16 - i*255;
        if (i == 16) { i = 15; f = 255; }
        table[COMPACT_WORDS+v] = ((i*COMPACT_G_STEP) << 8) | f;
        table[COMPACT_WORDS+256+v] = ((i*COMPACT_B_STEP) << 8) | f;
    }
    for (u32 b = begin_b; b < end_b; ++b)
        for (u32 g = 0; g < 17; ++g)
            for (u32 r = 0; r < 256; ++r) {
                u32 ri = r*16/255, rf = r*16 - ri*255;
                if (ri == 16) { ri = 15; rf = 255; }
                u32 base = ((b*17+g)*17+ri)*3, packed = 0;
                for (u32 c = 0; c < 3; ++c) {
                    u32 v = (cube[base+c]*(255-rf) + cube[base+3+c]*rf + 127)/255;
                    packed |= ((v+32)/64) << (10*c);
                }
                table[(b*17+g)*256+r] = packed;
            }
    return 0;
}

/* (x0*(255-wb) + x1*wb) has 255*255*4 units per RGB8 level; <= 66.3e6. */
#define COMPACT_DIVISOR 260100U
#define COMPACT_CHANNEL(p00,p01,p10,p11,shift,ig,wg,ib,wb) \
    ((((((p00)>>(shift))&1023)*(ig) + (((p01)>>(shift))&1023)*(wg))*(ib) + \
      ((((p10)>>(shift))&1023)*(ig) + (((p11)>>(shift))&1023)*(wg))*(wb) + \
      COMPACT_DIVISOR/2) / COMPACT_DIVISOR)

/* prefetch=1: while converting a 4-pixel group, request the table lines of
 * the group 8 pixels ahead (PRFM, no fault on any address). The in-order A53
 * otherwise stalls on each table miss; whether the extra instructions pay
 * off depends on L2 pressure, so the adapter's autotune decides. */
static inline __attribute__((always_inline))
int compact_impl(const u32 *table, u8 *r, u8 *g, u8 *b,
                 u32 rs, u32 gs, u32 bs, u32 width, u32 height, int prefetch) {
    if (!table || !r || !g || !b || !width || !height ||
        width>1920 || height>1080 || rs<width || gs<width || bs<width ||
        rs>4096 || gs>4096 || bs>4096) return -22;
    const u32 *gmap = table + COMPACT_WORDS, *bmap = gmap + 256;
    for (u32 y = 0; y < height; ++y) {
        u8 *rp = r+(u64)y*rs, *gp = g+(u64)y*gs, *bp = b+(u64)y*bs;
        /* Flat areas (letterbox bars, UI, sky) repeat whole 4-pixel groups. */
        u32 last_r = 0, last_g = 0, last_b = 0, out_r = 0, out_g = 0, out_b = 0;
        int have_last = 0;
        u32 x = 0;
        for (; x+4 <= width; x += 4) {
            u32 kr, kg, kb;
            __builtin_memcpy(&kr, rp+x, 4);
            __builtin_memcpy(&kg, gp+x, 4);
            __builtin_memcpy(&kb, bp+x, 4);
            if (prefetch && x+12 <= width)
                for (u32 lane = 0; lane < 4; ++lane) {
                    u32 index = (gmap[gp[x+8+lane]] >> 8) + (bmap[bp[x+8+lane]] >> 8) + rp[x+8+lane];
                    __builtin_prefetch(table+index, 0, 3);
                    __builtin_prefetch(table+index+COMPACT_G_STEP, 0, 3);
                    __builtin_prefetch(table+index+COMPACT_B_STEP, 0, 3);
                    __builtin_prefetch(table+index+COMPACT_B_STEP+COMPACT_G_STEP, 0, 3);
                }
            if (have_last && kr == last_r && kg == last_g && kb == last_b) {
                __builtin_memcpy(rp+x, &out_r, 4);
                __builtin_memcpy(gp+x, &out_g, 4);
                __builtin_memcpy(bp+x, &out_b, 4);
                continue;
            }
            pixels4 p00, p01, p10, p11, wg, wb;
            for (u32 lane = 0; lane < 4; ++lane) {
                u32 gm = gmap[gp[x+lane]], bm = bmap[bp[x+lane]];
                u32 index = (gm >> 8) + (bm >> 8) + rp[x+lane];
                p00[lane] = table[index];
                p01[lane] = table[index+COMPACT_G_STEP];
                p10[lane] = table[index+COMPACT_B_STEP];
                p11[lane] = table[index+COMPACT_B_STEP+COMPACT_G_STEP];
                wg[lane] = gm & 255; wb[lane] = bm & 255;
            }
            pixels4 ig = 255-wg, ib = 255-wb;
            pixels4 red = COMPACT_CHANNEL(p00,p01,p10,p11,0,ig,wg,ib,wb);
            pixels4 green = COMPACT_CHANNEL(p00,p01,p10,p11,10,ig,wg,ib,wb);
            pixels4 blue = COMPACT_CHANNEL(p00,p01,p10,p11,20,ig,wg,ib,wb);
            bytes4 rr = __builtin_convertvector(red, bytes4);
            bytes4 gg = __builtin_convertvector(green, bytes4);
            bytes4 bb = __builtin_convertvector(blue, bytes4);
            __builtin_memcpy(rp+x, &rr, 4);
            __builtin_memcpy(gp+x, &gg, 4);
            __builtin_memcpy(bp+x, &bb, 4);
            last_r = kr; last_g = kg; last_b = kb; have_last = 1;
            __builtin_memcpy(&out_r, &rr, 4);
            __builtin_memcpy(&out_g, &gg, 4);
            __builtin_memcpy(&out_b, &bb, 4);
        }
        for (; x < width; ++x) {
            u32 gm = gmap[gp[x]], bm = bmap[bp[x]];
            u32 index = (gm >> 8) + (bm >> 8) + rp[x];
            u32 p00 = table[index], p01 = table[index+COMPACT_G_STEP];
            u32 p10 = table[index+COMPACT_B_STEP];
            u32 p11 = table[index+COMPACT_B_STEP+COMPACT_G_STEP];
            u32 wg = gm & 255, wb = bm & 255, ig = 255-wg, ib = 255-wb;
            rp[x] = (u8)COMPACT_CHANNEL(p00,p01,p10,p11,0,ig,wg,ib,wb);
            gp[x] = (u8)COMPACT_CHANNEL(p00,p01,p10,p11,10,ig,wg,ib,wb);
            bp[x] = (u8)COMPACT_CHANNEL(p00,p01,p10,p11,20,ig,wg,ib,wb);
        }
    }
    return 0;
}

int xreal_compact_rgb_lut(const u32 *table, u8 *r, u8 *g, u8 *b,
                          u32 rs, u32 gs, u32 bs, u32 width, u32 height) {
    return compact_impl(table, r, g, b, rs, gs, bs, width, height, 0);
}

/* Same table and bit-identical output as xreal_compact_rgb_lut. */
int xreal_compact_pf_rgb_lut(const u32 *table, u8 *r, u8 *g, u8 *b,
                             u32 rs, u32 gs, u32 bs, u32 width, u32 height) {
    return compact_impl(table, r, g, b, rs, gs, bs, width, height, 1);
}

/* Compact2: same values and arithmetic as the compact kernel (bit-identical
 * output), laid out [b][r][g] so that each pixel's G and G+1 neighbours sit
 * in adjacent words: two cache lines per pixel instead of four. The 8-pixel
 * body issues all 32 table loads before any arithmetic, which suits the
 * in-order Cortex-A53 (it stalls on the first use of a missing load). */
#define C2_R_STEP 17U
#define C2_B_STEP (256U * 17U)

unsigned long long xreal_compact2_table_bytes(void) {
    return (u64)(COMPACT_WORDS + 512U) * sizeof(u32);
}

int xreal_build_compact2_table_range(const u16 *cube, u32 *table,
                                     u32 begin_b, u32 end_b) {
    if (!cube || !table || begin_b > end_b || end_b > 17) return -22;
    if (!begin_b) for (u32 v = 0; v < 256; ++v) {
        u32 i = v*16/255, f = v*16 - i*255;
        if (i == 16) { i = 15; f = 255; }
        table[COMPACT_WORDS+v] = (i << 8) | f;                       /* G: +i */
        table[COMPACT_WORDS+256+v] = ((i*C2_B_STEP) << 8) | f;       /* B */
    }
    for (u32 b = begin_b; b < end_b; ++b)
        for (u32 r = 0; r < 256; ++r) {
            u32 ri = r*16/255, rf = r*16 - ri*255;
            if (ri == 16) { ri = 15; rf = 255; }
            for (u32 g = 0; g < 17; ++g) {
                u32 base = ((b*17+g)*17+ri)*3, packed = 0;
                for (u32 c = 0; c < 3; ++c) {
                    u32 v = (cube[base+c]*(255-rf) + cube[base+3+c]*rf + 127)/255;
                    packed |= ((v+32)/64) << (10*c);
                }
                table[b*C2_B_STEP + r*C2_R_STEP + g] = packed;
            }
        }
    return 0;
}

static inline void c2_gather(const u32 *table, const u32 *gmap, const u32 *bmap,
                             const u8 *rp, const u8 *gp, const u8 *bp,
                             pixels4 *p00, pixels4 *p01, pixels4 *p10, pixels4 *p11,
                             pixels4 *wg, pixels4 *wb) {
    for (u32 lane = 0; lane < 4; ++lane) {
        u32 gm = gmap[gp[lane]], bm = bmap[bp[lane]];
        const u32 *e = table + (gm >> 8) + (bm >> 8) + rp[lane]*C2_R_STEP;
        (*p00)[lane] = e[0]; (*p01)[lane] = e[1];
        (*p10)[lane] = e[C2_B_STEP]; (*p11)[lane] = e[C2_B_STEP+1];
        (*wg)[lane] = gm & 255; (*wb)[lane] = bm & 255;
    }
}

static inline void c2_store(u8 *rp, u8 *gp, u8 *bp, pixels4 p00, pixels4 p01,
                            pixels4 p10, pixels4 p11, pixels4 wg, pixels4 wb) {
    pixels4 ig = 255-wg, ib = 255-wb;
    pixels4 red = COMPACT_CHANNEL(p00,p01,p10,p11,0,ig,wg,ib,wb);
    pixels4 green = COMPACT_CHANNEL(p00,p01,p10,p11,10,ig,wg,ib,wb);
    pixels4 blue = COMPACT_CHANNEL(p00,p01,p10,p11,20,ig,wg,ib,wb);
    bytes4 rr = __builtin_convertvector(red, bytes4);
    bytes4 gg = __builtin_convertvector(green, bytes4);
    bytes4 bb = __builtin_convertvector(blue, bytes4);
    __builtin_memcpy(rp, &rr, 4);
    __builtin_memcpy(gp, &gg, 4);
    __builtin_memcpy(bp, &bb, 4);
}

int xreal_compact2_rgb_lut(const u32 *table, u8 *r, u8 *g, u8 *b,
                           u32 rs, u32 gs, u32 bs, u32 width, u32 height) {
    if (!table || !r || !g || !b || !width || !height ||
        width>1920 || height>1080 || rs<width || gs<width || bs<width ||
        rs>4096 || gs>4096 || bs>4096) return -22;
    const u32 *gmap = table + COMPACT_WORDS, *bmap = gmap + 256;
    for (u32 y = 0; y < height; ++y) {
        u8 *rp = r+(u64)y*rs, *gp = g+(u64)y*gs, *bp = b+(u64)y*bs;
        u64 last_r = 0, last_g = 0, last_b = 0, out_r = 0, out_g = 0, out_b = 0;
        int have_last = 0;
        u32 x = 0;
        for (; x+8 <= width; x += 8) {
            u64 kr, kg, kb;
            __builtin_memcpy(&kr, rp+x, 8);
            __builtin_memcpy(&kg, gp+x, 8);
            __builtin_memcpy(&kb, bp+x, 8);
            if (have_last && kr == last_r && kg == last_g && kb == last_b) {
                __builtin_memcpy(rp+x, &out_r, 8);
                __builtin_memcpy(gp+x, &out_g, 8);
                __builtin_memcpy(bp+x, &out_b, 8);
                continue;
            }
            pixels4 a00, a01, a10, a11, awg, awb, b00, b01, b10, b11, bwg, bwb;
            c2_gather(table, gmap, bmap, rp+x, gp+x, bp+x, &a00, &a01, &a10, &a11, &awg, &awb);
            c2_gather(table, gmap, bmap, rp+x+4, gp+x+4, bp+x+4, &b00, &b01, &b10, &b11, &bwg, &bwb);
            c2_store(rp+x, gp+x, bp+x, a00, a01, a10, a11, awg, awb);
            c2_store(rp+x+4, gp+x+4, bp+x+4, b00, b01, b10, b11, bwg, bwb);
            last_r = kr; last_g = kg; last_b = kb; have_last = 1;
            __builtin_memcpy(&out_r, rp+x, 8);
            __builtin_memcpy(&out_g, gp+x, 8);
            __builtin_memcpy(&out_b, bp+x, 8);
        }
        for (; x < width; ++x) {
            u32 gm = gmap[gp[x]], bm = bmap[bp[x]];
            const u32 *e = table + (gm >> 8) + (bm >> 8) + rp[x]*C2_R_STEP;
            u32 p00 = e[0], p01 = e[1], p10 = e[C2_B_STEP], p11 = e[C2_B_STEP+1];
            u32 wg = gm & 255, wb = bm & 255, ig = 255-wg, ib = 255-wb;
            rp[x] = (u8)COMPACT_CHANNEL(p00,p01,p10,p11,0,ig,wg,ib,wb);
            gp[x] = (u8)COMPACT_CHANNEL(p00,p01,p10,p11,10,ig,wg,ib,wb);
            bp[x] = (u8)COMPACT_CHANNEL(p00,p01,p10,p11,20,ig,wg,ib,wb);
        }
    }
    return 0;
}

/* Tetrahedral interpolation (perf4). The 17^3 cube itself, one u32 per node
 * (R | G<<8 | B<<16, 8-bit values): 19.7 KiB, resident in each core's L1
 * instead of a 289 KiB table in the shared L2. Four nodes per pixel, weights
 * are differences of the sorted fractions (exact /255 units), so each
 * channel is sum(w*node) <= 65025 and is computed exactly, then rounded.
 * Output equals exact tetrahedral interpolation of the cube. Versus the
 * trilinear reference on the vendor cubes (86^3 grid): AI HDR 75.4% equal,
 * 24.6% 1 level, 0.04% 2, 0.001% 3 (saturated violet); Movie <= 1 level. Three 256-word axis maps follow the
 * nodes: (node offset << 8) | fraction. */
#define TETRA_NODES 4913U
#define TETRA_G 17U
#define TETRA_B 289U

unsigned long long xreal_tetra_table_bytes(void) {
    return (u64)(TETRA_NODES + 768U) * sizeof(u32);
}

int xreal_build_tetra_table(const u16 *cube, u32 *table) {
    if (!cube || !table) return -22;
    for (u32 i = 0; i < TETRA_NODES; ++i) {
        u32 packed = 0;
        for (u32 c = 0; c < 3; ++c) {
            u32 v = ((u32)cube[i*3+c] + 128) >> 8;
            packed |= (v > 255 ? 255 : v) << (8*c);
        }
        table[i] = packed;
    }
    for (u32 v = 0; v < 256; ++v) {
        u32 lo = v*16/255, f = v*16 - lo*255;
        if (lo == 16) { lo = 15; f = 255; }
        table[TETRA_NODES+v] = (lo << 8) | f;
        table[TETRA_NODES+256+v] = ((lo*TETRA_G) << 8) | f;
        table[TETRA_NODES+512+v] = ((lo*TETRA_B) << 8) | f;
    }
    return 0;
}

static inline __attribute__((always_inline))
void tetra_pixel(const u32 *table, const u32 *rmap, const u32 *gmap, const u32 *bmap,
                 u8 *rp, u8 *gp, u8 *bp) {
    u32 mr = rmap[*rp], mg = gmap[*gp], mb = bmap[*bp];
    u32 base = (mr >> 8) + (mg >> 8) + (mb >> 8);
    u32 f1 = mr & 255, s1 = 1, f2 = mg & 255, s2 = TETRA_G, f3 = mb & 255, s3 = TETRA_B, t;
    /* Three compare-exchanges, written for csel; ties carry zero weight. */
    if (f2 > f1) { t = f1; f1 = f2; f2 = t; t = s1; s1 = s2; s2 = t; }
    if (f3 > f2) { t = f2; f2 = f3; f3 = t; t = s2; s2 = s3; s3 = t; }
    if (f2 > f1) { t = f1; f1 = f2; f2 = t; t = s1; s1 = s2; s2 = t; }
    const u32 *e = table + base;
    u32 n0 = e[0], na = e[s1], nb = e[s1+s2], n1 = e[1+TETRA_G+TETRA_B];
    u32 w0 = 255-f1, w1 = f1-f2, w2 = f2-f3, w3 = f3;
    /* R and B in 16-bit fields of one register, G separately; no carries. */
    u32 rb = w0*(n0 & 0xff00ffU) + w1*(na & 0xff00ffU) + w2*(nb & 0xff00ffU) + w3*(n1 & 0xff00ffU);
    u32 g = w0*((n0>>8)&255) + w1*((na>>8)&255) + w2*((nb>>8)&255) + w3*((n1>>8)&255);
    /* round(x/255) for x <= 65025: (x+128 + ((x+128)>>8)) >> 8, per field. */
    rb += 0x00800080U; rb += (rb >> 8) & 0x00ff00ffU;
    g += 128; g += g >> 8;
    *rp = (u8)(rb >> 8); *gp = (u8)(g >> 8); *bp = (u8)(rb >> 24);
}

#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
#include <arm_neon.h>
#endif

int xreal_tetra_rgb_lut(const u32 *table, u8 *r, u8 *g, u8 *b,
                        u32 rs, u32 gs, u32 bs, u32 width, u32 height) {
    if (!table || !r || !g || !b || !width || !height ||
        width>1920 || height>1080 || rs<width || gs<width || bs<width ||
        rs>4096 || gs>4096 || bs>4096) return -22;
    const u32 *rmap = table + TETRA_NODES, *gmap = rmap + 256, *bmap = gmap + 256;
    for (u32 y = 0; y < height; ++y) {
        u8 *rp = r+(u64)y*rs, *gp = g+(u64)y*gs, *bp = b+(u64)y*bs;
        u32 last_r = 0, last_g = 0, last_b = 0, out_r = 0, out_g = 0, out_b = 0;
        int have_last = 0;
        u32 x = 0;
        for (; x+4 <= width; x += 4) {
            u32 kr, kg, kb;
            __builtin_memcpy(&kr, rp+x, 4);
            __builtin_memcpy(&kg, gp+x, 4);
            __builtin_memcpy(&kb, bp+x, 4);
            if (have_last && kr == last_r && kg == last_g && kb == last_b) {
                __builtin_memcpy(rp+x, &out_r, 4);
                __builtin_memcpy(gp+x, &out_g, 4);
                __builtin_memcpy(bp+x, &out_b, 4);
                continue;
            }
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
            u32 mr[4], mg[4], mb[4];
            for (u32 lane = 0; lane < 4; ++lane) {
                mr[lane] = rmap[(kr >> (8 * lane)) & 255];
                mg[lane] = gmap[(kg >> (8 * lane)) & 255];
                mb[lane] = bmap[(kb >> (8 * lane)) & 255];
            }
            uint32x4_t vmr = vld1q_u32(mr), vmg = vld1q_u32(mg), vmb = vld1q_u32(mb);
            uint32x4_t vbase = vaddq_u32(vaddq_u32(vshrq_n_u32(vmr, 8), vshrq_n_u32(vmg, 8)), vshrq_n_u32(vmb, 8));
            uint16x4_t mask8 = vdup_n_u16(255);
            uint16x4_t f1 = vand_u16(vmovn_u32(vmr), mask8);
            uint16x4_t f2 = vand_u16(vmovn_u32(vmg), mask8);
            uint16x4_t f3 = vand_u16(vmovn_u32(vmb), mask8);
            uint16x4_t s1 = vdup_n_u16(1), s2 = vdup_n_u16(TETRA_G), s3 = vdup_n_u16(TETRA_B);
            uint16x4_t gt, tf, ts;
            gt = vcgt_u16(f2, f1);
            tf = vbsl_u16(gt, f2, f1); f2 = vbsl_u16(gt, f1, f2); f1 = tf;
            ts = vbsl_u16(gt, s2, s1); s2 = vbsl_u16(gt, s1, s2); s1 = ts;
            gt = vcgt_u16(f3, f2);
            tf = vbsl_u16(gt, f3, f2); f3 = vbsl_u16(gt, f2, f3); f2 = tf;
            ts = vbsl_u16(gt, s3, s2); s3 = vbsl_u16(gt, s2, s3); s2 = ts;
            gt = vcgt_u16(f2, f1);
            tf = vbsl_u16(gt, f2, f1); f2 = vbsl_u16(gt, f1, f2); f1 = tf;
            ts = vbsl_u16(gt, s2, s1); s2 = vbsl_u16(gt, s1, s2); s1 = ts;
            uint16x4_t w0 = vsub_u16(mask8, f1);
            uint16x4_t w1 = vsub_u16(f1, f2);
            uint16x4_t w2 = vsub_u16(f2, f3);
            uint16x4_t w3 = f3;
            u32 base[4]; u16 o1[4], o2[4];
            vst1q_u32(base, vbase);
            vst1_u16(o1, s1);
            vst1_u16(o2, vadd_u16(s1, s2));
            u32 n0[4], na[4], nb[4], n1[4];
            for (u32 lane = 0; lane < 4; ++lane) {
                const u32 *e = table + base[lane];
                n0[lane] = e[0];
                na[lane] = e[o1[lane]];
                nb[lane] = e[o2[lane]];
                n1[lane] = e[1 + TETRA_G + TETRA_B];
            }
            uint32x4_t q0 = vld1q_u32(n0), qa = vld1q_u32(na);
            uint32x4_t qb = vld1q_u32(nb), q1 = vld1q_u32(n1);
            uint16x4_t c0[3] = {
                vand_u16(vmovn_u32(q0), mask8),
                vand_u16(vshrn_n_u32(q0, 8), mask8),
                vshrn_n_u32(q0, 16)
            };
            uint16x4_t ca[3] = {
                vand_u16(vmovn_u32(qa), mask8),
                vand_u16(vshrn_n_u32(qa, 8), mask8),
                vshrn_n_u32(qa, 16)
            };
            uint16x4_t cb[3] = {
                vand_u16(vmovn_u32(qb), mask8),
                vand_u16(vshrn_n_u32(qb, 8), mask8),
                vshrn_n_u32(qb, 16)
            };
            uint16x4_t c1[3] = {
                vand_u16(vmovn_u32(q1), mask8),
                vand_u16(vshrn_n_u32(q1, 8), mask8),
                vshrn_n_u32(q1, 16)
            };
            uint16x4_t half = vdup_n_u16(128);
            u32 outv[3];
            for (u32 c = 0; c < 3; ++c) {
                uint16x4_t acc = vmul_u16(w0, c0[c]);
                acc = vmla_u16(acc, w1, ca[c]);
                acc = vmla_u16(acc, w2, cb[c]);
                acc = vmla_u16(acc, w3, c1[c]);
                acc = vadd_u16(acc, half);
                acc = vadd_u16(acc, vshr_n_u16(acc, 8));
                uint8x8_t o = vshrn_n_u16(vcombine_u16(acc, acc), 8);
                outv[c] = vget_lane_u32(vreinterpret_u32_u8(o), 0);
            }
            out_r = outv[0]; out_g = outv[1]; out_b = outv[2];
            __builtin_memcpy(rp+x, &out_r, 4);
            __builtin_memcpy(gp+x, &out_g, 4);
            __builtin_memcpy(bp+x, &out_b, 4);
            last_r = kr; last_g = kg; last_b = kb; have_last = 1;
#else
            for (u32 lane = 0; lane < 4; ++lane)
                tetra_pixel(table, rmap, gmap, bmap, rp+x+lane, gp+x+lane, bp+x+lane);
            last_r = kr; last_g = kg; last_b = kb; have_last = 1;
            __builtin_memcpy(&out_r, rp+x, 4);
            __builtin_memcpy(&out_g, gp+x, 4);
            __builtin_memcpy(&out_b, bp+x, 4);
#endif
        }
        for (; x < width; ++x)
            tetra_pixel(table, rmap, gmap, bmap, rp+x, gp+x, bp+x);
    }
    return 0;
}

/* compact16 (perf6): the compact table and lookups, 16-bit arithmetic.
 * Each 4-pixel group extracts one channel of all four corners into two
 * 8-lane vectors ([p00|p10] and [p01|p11]); the G lerp runs once over both
 * B rows, then the B lerp. Lerp: a + round((b-a)*w/32768) (SQRDMULH) with
 * w = (f*32897+64)>>8 ~ f*32768/255 (f=255 -> 32767). Error versus the cube: Q2 table
 * <= 1/8 level, each lerp <= 1/8, final rounding 1/2: within 1 level.
 * The portable path computes exactly the same lanes.
 * Precomputed axis maps pack (table_offset << 16) | c16_weight(f) so the hot
 * loop computes zero weight arithmetic. */
static const u32 C16_GMAP[256] = {
    0x00000000U, 0x00000808U, 0x00001010U, 0x00001818U, 0x00002020U, 0x00002828U, 0x00003030U, 0x00003838U,
    0x00004040U, 0x00004848U, 0x00005050U, 0x00005858U, 0x00006061U, 0x00006869U, 0x00007071U, 0x00007879U,
    0x01000080U, 0x01000888U, 0x01001090U, 0x01001898U, 0x010020a1U, 0x010028a9U, 0x010030b1U, 0x010038b9U,
    0x010040c1U, 0x010048c9U, 0x010050d1U, 0x010058d9U, 0x010060e1U, 0x010068e9U, 0x010070f1U, 0x010078f9U,
    0x02000101U, 0x02000909U, 0x02001111U, 0x02001919U, 0x02002121U, 0x02002929U, 0x02003131U, 0x02003939U,
    0x02004141U, 0x02004949U, 0x02005151U, 0x02005959U, 0x02006162U, 0x0200696aU, 0x02007172U, 0x0200797aU,
    0x03000181U, 0x03000989U, 0x03001191U, 0x03001999U, 0x030021a2U, 0x030029aaU, 0x030031b2U, 0x030039baU,
    0x030041c2U, 0x030049caU, 0x030051d2U, 0x030059daU, 0x030061e2U, 0x030069eaU, 0x030071f2U, 0x030079faU,
    0x04000202U, 0x04000a0aU, 0x04001212U, 0x04001a1aU, 0x04002222U, 0x04002a2aU, 0x04003232U, 0x04003a3aU,
    0x04004242U, 0x04004a4aU, 0x04005252U, 0x04005a5aU, 0x04006263U, 0x04006a6bU, 0x04007273U, 0x04007a7bU,
    0x05000282U, 0x05000a8aU, 0x05001292U, 0x05001a9aU, 0x050022a3U, 0x05002aabU, 0x050032b3U, 0x05003abbU,
    0x050042c3U, 0x05004acbU, 0x050052d3U, 0x05005adbU, 0x050062e3U, 0x05006aebU, 0x050072f3U, 0x05007afbU,
    0x06000303U, 0x06000b0bU, 0x06001313U, 0x06001b1bU, 0x06002323U, 0x06002b2bU, 0x06003333U, 0x06003b3bU,
    0x06004343U, 0x06004b4bU, 0x06005353U, 0x06005b5bU, 0x06006364U, 0x06006b6cU, 0x06007374U, 0x06007b7cU,
    0x07000383U, 0x07000b8bU, 0x07001393U, 0x07001b9bU, 0x070023a4U, 0x07002bacU, 0x070033b4U, 0x07003bbcU,
    0x070043c4U, 0x07004bccU, 0x070053d4U, 0x07005bdcU, 0x070063e4U, 0x07006becU, 0x070073f4U, 0x07007bfcU,
    0x08000404U, 0x08000c0cU, 0x08001414U, 0x08001c1cU, 0x08002424U, 0x08002c2cU, 0x08003434U, 0x08003c3cU,
    0x08004444U, 0x08004c4cU, 0x08005454U, 0x08005c5cU, 0x08006465U, 0x08006c6dU, 0x08007475U, 0x08007c7dU,
    0x09000484U, 0x09000c8cU, 0x09001494U, 0x09001c9cU, 0x090024a5U, 0x09002cadU, 0x090034b5U, 0x09003cbdU,
    0x090044c5U, 0x09004ccdU, 0x090054d5U, 0x09005cddU, 0x090064e5U, 0x09006cedU, 0x090074f5U, 0x09007cfdU,
    0x0a000505U, 0x0a000d0dU, 0x0a001515U, 0x0a001d1dU, 0x0a002525U, 0x0a002d2dU, 0x0a003535U, 0x0a003d3dU,
    0x0a004545U, 0x0a004d4dU, 0x0a005555U, 0x0a005d5dU, 0x0a006566U, 0x0a006d6eU, 0x0a007576U, 0x0a007d7eU,
    0x0b000585U, 0x0b000d8dU, 0x0b001595U, 0x0b001d9dU, 0x0b0025a6U, 0x0b002daeU, 0x0b0035b6U, 0x0b003dbeU,
    0x0b0045c6U, 0x0b004dceU, 0x0b0055d6U, 0x0b005ddeU, 0x0b0065e6U, 0x0b006deeU, 0x0b0075f6U, 0x0b007dfeU,
    0x0c000606U, 0x0c000e0eU, 0x0c001616U, 0x0c001e1eU, 0x0c002626U, 0x0c002e2eU, 0x0c003636U, 0x0c003e3eU,
    0x0c004646U, 0x0c004e4eU, 0x0c005656U, 0x0c005e5eU, 0x0c006667U, 0x0c006e6fU, 0x0c007677U, 0x0c007e7fU,
    0x0d000686U, 0x0d000e8eU, 0x0d001696U, 0x0d001e9eU, 0x0d0026a7U, 0x0d002eafU, 0x0d0036b7U, 0x0d003ebfU,
    0x0d0046c7U, 0x0d004ecfU, 0x0d0056d7U, 0x0d005edfU, 0x0d0066e7U, 0x0d006eefU, 0x0d0076f7U, 0x0d007effU,
    0x0e000707U, 0x0e000f0fU, 0x0e001717U, 0x0e001f1fU, 0x0e002727U, 0x0e002f2fU, 0x0e003737U, 0x0e003f3fU,
    0x0e004747U, 0x0e004f4fU, 0x0e005757U, 0x0e005f5fU, 0x0e006768U, 0x0e006f70U, 0x0e007778U, 0x0e007f80U,
    0x0f000787U, 0x0f000f8fU, 0x0f001797U, 0x0f001f9fU, 0x0f0027a8U, 0x0f002fb0U, 0x0f0037b8U, 0x0f003fc0U,
    0x0f0047c8U, 0x0f004fd0U, 0x0f0057d8U, 0x0f005fe0U, 0x0f0067e8U, 0x0f006ff0U, 0x0f0077f8U, 0x0f007fffU
};
static const u32 C16_BMAP[256] = {
    0x00000000U, 0x00000808U, 0x00001010U, 0x00001818U, 0x00002020U, 0x00002828U, 0x00003030U, 0x00003838U,
    0x00004040U, 0x00004848U, 0x00005050U, 0x00005858U, 0x00006061U, 0x00006869U, 0x00007071U, 0x00007879U,
    0x11000080U, 0x11000888U, 0x11001090U, 0x11001898U, 0x110020a1U, 0x110028a9U, 0x110030b1U, 0x110038b9U,
    0x110040c1U, 0x110048c9U, 0x110050d1U, 0x110058d9U, 0x110060e1U, 0x110068e9U, 0x110070f1U, 0x110078f9U,
    0x22000101U, 0x22000909U, 0x22001111U, 0x22001919U, 0x22002121U, 0x22002929U, 0x22003131U, 0x22003939U,
    0x22004141U, 0x22004949U, 0x22005151U, 0x22005959U, 0x22006162U, 0x2200696aU, 0x22007172U, 0x2200797aU,
    0x33000181U, 0x33000989U, 0x33001191U, 0x33001999U, 0x330021a2U, 0x330029aaU, 0x330031b2U, 0x330039baU,
    0x330041c2U, 0x330049caU, 0x330051d2U, 0x330059daU, 0x330061e2U, 0x330069eaU, 0x330071f2U, 0x330079faU,
    0x44000202U, 0x44000a0aU, 0x44001212U, 0x44001a1aU, 0x44002222U, 0x44002a2aU, 0x44003232U, 0x44003a3aU,
    0x44004242U, 0x44004a4aU, 0x44005252U, 0x44005a5aU, 0x44006263U, 0x44006a6bU, 0x44007273U, 0x44007a7bU,
    0x55000282U, 0x55000a8aU, 0x55001292U, 0x55001a9aU, 0x550022a3U, 0x55002aabU, 0x550032b3U, 0x55003abbU,
    0x550042c3U, 0x55004acbU, 0x550052d3U, 0x55005adbU, 0x550062e3U, 0x55006aebU, 0x550072f3U, 0x55007afbU,
    0x66000303U, 0x66000b0bU, 0x66001313U, 0x66001b1bU, 0x66002323U, 0x66002b2bU, 0x66003333U, 0x66003b3bU,
    0x66004343U, 0x66004b4bU, 0x66005353U, 0x66005b5bU, 0x66006364U, 0x66006b6cU, 0x66007374U, 0x66007b7cU,
    0x77000383U, 0x77000b8bU, 0x77001393U, 0x77001b9bU, 0x770023a4U, 0x77002bacU, 0x770033b4U, 0x77003bbcU,
    0x770043c4U, 0x77004bccU, 0x770053d4U, 0x77005bdcU, 0x770063e4U, 0x77006becU, 0x770073f4U, 0x77007bfcU,
    0x88000404U, 0x88000c0cU, 0x88001414U, 0x88001c1cU, 0x88002424U, 0x88002c2cU, 0x88003434U, 0x88003c3cU,
    0x88004444U, 0x88004c4cU, 0x88005454U, 0x88005c5cU, 0x88006465U, 0x88006c6dU, 0x88007475U, 0x88007c7dU,
    0x99000484U, 0x99000c8cU, 0x99001494U, 0x99001c9cU, 0x990024a5U, 0x99002cadU, 0x990034b5U, 0x99003cbdU,
    0x990044c5U, 0x99004ccdU, 0x990054d5U, 0x99005cddU, 0x990064e5U, 0x99006cedU, 0x990074f5U, 0x99007cfdU,
    0xaa000505U, 0xaa000d0dU, 0xaa001515U, 0xaa001d1dU, 0xaa002525U, 0xaa002d2dU, 0xaa003535U, 0xaa003d3dU,
    0xaa004545U, 0xaa004d4dU, 0xaa005555U, 0xaa005d5dU, 0xaa006566U, 0xaa006d6eU, 0xaa007576U, 0xaa007d7eU,
    0xbb000585U, 0xbb000d8dU, 0xbb001595U, 0xbb001d9dU, 0xbb0025a6U, 0xbb002daeU, 0xbb0035b6U, 0xbb003dbeU,
    0xbb0045c6U, 0xbb004dceU, 0xbb0055d6U, 0xbb005ddeU, 0xbb0065e6U, 0xbb006deeU, 0xbb0075f6U, 0xbb007dfeU,
    0xcc000606U, 0xcc000e0eU, 0xcc001616U, 0xcc001e1eU, 0xcc002626U, 0xcc002e2eU, 0xcc003636U, 0xcc003e3eU,
    0xcc004646U, 0xcc004e4eU, 0xcc005656U, 0xcc005e5eU, 0xcc006667U, 0xcc006e6fU, 0xcc007677U, 0xcc007e7fU,
    0xdd000686U, 0xdd000e8eU, 0xdd001696U, 0xdd001e9eU, 0xdd0026a7U, 0xdd002eafU, 0xdd0036b7U, 0xdd003ebfU,
    0xdd0046c7U, 0xdd004ecfU, 0xdd0056d7U, 0xdd005edfU, 0xdd0066e7U, 0xdd006eefU, 0xdd0076f7U, 0xdd007effU,
    0xee000707U, 0xee000f0fU, 0xee001717U, 0xee001f1fU, 0xee002727U, 0xee002f2fU, 0xee003737U, 0xee003f3fU,
    0xee004747U, 0xee004f4fU, 0xee005757U, 0xee005f5fU, 0xee006768U, 0xee006f70U, 0xee007778U, 0xee007f80U,
    0xff000787U, 0xff000f8fU, 0xff001797U, 0xff001f9fU, 0xff0027a8U, 0xff002fb0U, 0xff0037b8U, 0xff003fc0U,
    0xff0047c8U, 0xff004fd0U, 0xff0057d8U, 0xff005fe0U, 0xff0067e8U, 0xff006ff0U, 0xff0077f8U, 0xff007fffU
};

static inline short c16_mulh(short a, short w) {  /* SQRDMULH, no saturation case */
    return (short)(((int)a * w * 2 + 0x8000) >> 16);
}
static inline u32 c16_weight(u32 f) {     /* ~ f*32768/255, f=255 -> 32767 */
    u32 w = (f * 32897U + 64U) >> 8;
    return w - (w >> 15);
}
static inline u8 c16_channel(u32 p00, u32 p01, u32 p10, u32 p11, u32 shift, short wg, short wb) {
    short a0 = (short)((p00 >> shift) & 1023), a1 = (short)((p01 >> shift) & 1023);
    short b0 = (short)((p10 >> shift) & 1023), b1 = (short)((p11 >> shift) & 1023);
    short g0 = (short)(a0 + c16_mulh((short)(a1 - a0), wg));
    short g1 = (short)(b0 + c16_mulh((short)(b1 - b0), wg));
    short v = (short)(g0 + c16_mulh((short)(g1 - g0), wb));
    u32 out = ((u32)v + 2) >> 2;
    return (u8)(out > 255 ? 255 : out);
}

#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
static inline __attribute__((always_inline))
void c16_gather4_neon(const u32 *table, u32 kr, u32 kg, u32 kb,
                      uint32x4_t *q00, uint32x4_t *q01, uint32x4_t *q10, uint32x4_t *q11,
                      int16x4_t *vwg, int16x4_t *vwb) {
    u32 c00[4], c01[4], c10[4], c11[4];
    u16 wg[4], wb[4];
    for (u32 lane = 0; lane < 4; ++lane) {
        u32 gm = C16_GMAP[(kg >> (8 * lane)) & 255];
        u32 bm = C16_BMAP[(kb >> (8 * lane)) & 255];
        const u32 *e = table + (gm >> 16) + (bm >> 16) + ((kr >> (8 * lane)) & 255);
        c00[lane] = e[0];
        c01[lane] = e[COMPACT_G_STEP];
        c10[lane] = e[COMPACT_B_STEP];
        c11[lane] = e[COMPACT_B_STEP + COMPACT_G_STEP];
        wg[lane] = (u16)gm;
        wb[lane] = (u16)bm;
    }
    *q00 = vld1q_u32(c00); *q01 = vld1q_u32(c01);
    *q10 = vld1q_u32(c10); *q11 = vld1q_u32(c11);
    *vwg = vreinterpret_s16_u16(vld1_u16(wg));
    *vwb = vreinterpret_s16_u16(vld1_u16(wb));
}

static inline __attribute__((always_inline))
void c16_glerp4_neon(uint32x4_t q00, uint32x4_t q01, uint32x4_t q10, uint32x4_t q11,
                     int16x4_t vwg, int16x8_t gl[3]) {
    int16x8_t wg8 = vcombine_s16(vwg, vwg);
    uint16x8_t mask = vdupq_n_u16(1023);
    uint16x8_t los[3] = {
        vandq_u16(vcombine_u16(vmovn_u32(q00), vmovn_u32(q10)), mask),
        vandq_u16(vcombine_u16(vshrn_n_u32(q00, 10), vshrn_n_u32(q10, 10)), mask),
        vshrq_n_u16(vcombine_u16(vshrn_n_u32(q00, 16), vshrn_n_u32(q10, 16)), 4)
    };
    uint16x8_t his[3] = {
        vandq_u16(vcombine_u16(vmovn_u32(q01), vmovn_u32(q11)), mask),
        vandq_u16(vcombine_u16(vshrn_n_u32(q01, 10), vshrn_n_u32(q11, 10)), mask),
        vshrq_n_u16(vcombine_u16(vshrn_n_u32(q01, 16), vshrn_n_u32(q11, 16)), 4)
    };
    for (u32 c = 0; c < 3; ++c) {
        int16x8_t lo = vreinterpretq_s16_u16(los[c]);
        int16x8_t hi = vreinterpretq_s16_u16(his[c]);
        gl[c] = vaddq_s16(lo, vqrdmulhq_s16(vsubq_s16(hi, lo), wg8));
    }
}
#endif

int xreal_compact16_rgb_lut(const u32 *table, u8 *r, u8 *g, u8 *b,
                            u32 rs, u32 gs, u32 bs, u32 width, u32 height) {
    if (!table || !r || !g || !b || !width || !height ||
        width>1920 || height>1080 || rs<width || gs<width || bs<width ||
        rs>4096 || gs>4096 || bs>4096) return -22;
    for (u32 y = 0; y < height; ++y) {
        u8 *rp = r+(u64)y*rs, *gp = g+(u64)y*gs, *bp = b+(u64)y*bs;
        u32 x = 0;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
        u64 last_r = 0, last_g = 0, last_b = 0, out_r = 0, out_g = 0, out_b = 0;
        int have_last = 0;
        for (; x+8 <= width; x += 8) {
            u64 kr, kg, kb;
            __builtin_memcpy(&kr, rp+x, 8);
            __builtin_memcpy(&kg, gp+x, 8);
            __builtin_memcpy(&kb, bp+x, 8);
            if (have_last && kr == last_r && kg == last_g && kb == last_b) {
                __builtin_memcpy(rp+x, &out_r, 8);
                __builtin_memcpy(gp+x, &out_g, 8);
                __builtin_memcpy(bp+x, &out_b, 8);
                continue;
            }
            uint32x4_t a00, a01, a10, a11, b00, b01, b10, b11;
            int16x4_t awg, awb, bwg, bwb;
            c16_gather4_neon(table, (u32)kr, (u32)kg, (u32)kb, &a00, &a01, &a10, &a11, &awg, &awb);
            c16_gather4_neon(table, (u32)(kr >> 32), (u32)(kg >> 32), (u32)(kb >> 32),
                             &b00, &b01, &b10, &b11, &bwg, &bwb);
            int16x8_t gla[3], glb[3];
            c16_glerp4_neon(a00, a01, a10, a11, awg, gla);
            c16_glerp4_neon(b00, b01, b10, b11, bwg, glb);
            int16x8_t wb8 = vcombine_s16(awb, bwb);
            uint8x8_t res[3];
            for (u32 c = 0; c < 3; ++c) {
                int16x8_t g0 = vcombine_s16(vget_low_s16(gla[c]), vget_low_s16(glb[c]));
                int16x8_t g1 = vcombine_s16(vget_high_s16(gla[c]), vget_high_s16(glb[c]));
                int16x8_t v = vaddq_s16(g0, vqrdmulhq_s16(vsubq_s16(g1, g0), wb8));
                uint16x8_t o = vrshrq_n_u16(vreinterpretq_u16_s16(v), 2);
                res[c] = vqmovn_u16(o);
            }
            vst1_u8(rp+x, res[0]);
            vst1_u8(gp+x, res[1]);
            vst1_u8(bp+x, res[2]);
            last_r = kr; last_g = kg; last_b = kb; have_last = 1;
            out_r = vget_lane_u64(vreinterpret_u64_u8(res[0]), 0);
            out_g = vget_lane_u64(vreinterpret_u64_u8(res[1]), 0);
            out_b = vget_lane_u64(vreinterpret_u64_u8(res[2]), 0);
        }
        if (x+4 <= width) {
            u32 kr, kg, kb;
            __builtin_memcpy(&kr, rp+x, 4);
            __builtin_memcpy(&kg, gp+x, 4);
            __builtin_memcpy(&kb, bp+x, 4);
            uint32x4_t q00, q01, q10, q11;
            int16x4_t vwg, vwb;
            c16_gather4_neon(table, kr, kg, kb, &q00, &q01, &q10, &q11, &vwg, &vwb);
            int16x8_t gl[3];
            c16_glerp4_neon(q00, q01, q10, q11, vwg, gl);
            u8 *dst[3] = {rp+x, gp+x, bp+x};
            for (u32 c = 0; c < 3; ++c) {
                int16x4_t g0 = vget_low_s16(gl[c]), g1 = vget_high_s16(gl[c]);
                int16x4_t v = vadd_s16(g0, vqrdmulh_s16(vsub_s16(g1, g0), vwb));
                uint16x4_t o = vrshr_n_u16(vreinterpret_u16_s16(v), 2);
                vst1_lane_u32((uint32_t *)(void *)dst[c], vreinterpret_u32_u8(vqmovn_u16(vcombine_u16(o, o))), 0);
            }
            x += 4;
        }
#else
        u32 last_r = 0, last_g = 0, last_b = 0, out_r = 0, out_g = 0, out_b = 0;
        int have_last = 0;
        for (; x+4 <= width; x += 4) {
            u32 kr, kg, kb;
            __builtin_memcpy(&kr, rp+x, 4);
            __builtin_memcpy(&kg, gp+x, 4);
            __builtin_memcpy(&kb, bp+x, 4);
            if (have_last && kr == last_r && kg == last_g && kb == last_b) {
                __builtin_memcpy(rp+x, &out_r, 4);
                __builtin_memcpy(gp+x, &out_g, 4);
                __builtin_memcpy(bp+x, &out_b, 4);
                continue;
            }
            for (u32 lane = 0; lane < 4; ++lane) {
                u32 gm = C16_GMAP[(kg >> (8 * lane)) & 255];
                u32 bm = C16_BMAP[(kb >> (8 * lane)) & 255];
                const u32 *e = table + (gm >> 16) + (bm >> 16) + ((kr >> (8 * lane)) & 255);
                short wg = (short)(u16)gm, wb = (short)(u16)bm;
                rp[x+lane] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 0, wg, wb);
                gp[x+lane] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 10, wg, wb);
                bp[x+lane] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 20, wg, wb);
            }
            last_r = kr; last_g = kg; last_b = kb; have_last = 1;
            __builtin_memcpy(&out_r, rp+x, 4);
            __builtin_memcpy(&out_g, gp+x, 4);
            __builtin_memcpy(&out_b, bp+x, 4);
        }
#endif
        for (; x < width; ++x) {
            u32 gm = C16_GMAP[gp[x]], bm = C16_BMAP[bp[x]];
            const u32 *e = table + (gm >> 16) + (bm >> 16) + rp[x];
            short wg = (short)(u16)gm, wb = (short)(u16)bm;
            rp[x] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 0, wg, wb);
            gp[x] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 10, wg, wb);
            bp[x] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 20, wg, wb);
        }
    }
    return 0;
}

/* blocks (perf7): compact16 with smooth-block reuse. Frames are walked in
 * 4x2 tiles. When both 2x2 halves are smooth (every channel within
 * BLOCK_FLAT levels), the LUT is evaluated once per 2x2 block at its mean
 * colour and each pixel adds its own small offset from the mean:
 *   out = LUT(mean) + (pixel - mean)
 * The cube's local slope is close to 1 over a 1-2 level span, so this stays
 * within a level or two of the exact result; any other tile takes the exact
 * compact16 path. Video (especially compressed film) is mostly smooth, so
 * most tiles need 2 instead of 8 table lookups. */
#define BLOCK_FLAT 4   /* film-like content: ~95-100% smooth tiles; <= 3 levels on the vendor cube */
/* Hidden counters (PC-relative access, no GOT): read via the accessor. */
static unsigned long long xreal_blocks_smooth_tiles, xreal_blocks_tiles;

void xreal_blocks_take_counts(unsigned long long *smooth, unsigned long long *tiles) {
    *smooth = __atomic_exchange_n(&xreal_blocks_smooth_tiles, 0, __ATOMIC_RELAXED);
    *tiles = __atomic_exchange_n(&xreal_blocks_tiles, 0, __ATOMIC_RELAXED);
}

static inline void c16_group4(const u32 *table, const u32 *gmap, const u32 *bmap,
                              u8 *rp, u8 *gp, u8 *bp) {
    (void)gmap; (void)bmap;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
    u32 kr, kg, kb;
    __builtin_memcpy(&kr, rp, 4);
    __builtin_memcpy(&kg, gp, 4);
    __builtin_memcpy(&kb, bp, 4);
    uint32x4_t q00, q01, q10, q11;
    int16x4_t vwg, vwb;
    c16_gather4_neon(table, kr, kg, kb, &q00, &q01, &q10, &q11, &vwg, &vwb);
    int16x8_t gl[3];
    c16_glerp4_neon(q00, q01, q10, q11, vwg, gl);
    u8 *dst[3] = {rp, gp, bp};
    for (u32 c = 0; c < 3; ++c) {
        int16x4_t g0 = vget_low_s16(gl[c]), g1 = vget_high_s16(gl[c]);
        int16x4_t v = vadd_s16(g0, vqrdmulh_s16(vsub_s16(g1, g0), vwb));
        uint16x4_t o = vrshr_n_u16(vreinterpret_u16_s16(v), 2);
        vst1_lane_u32((uint32_t *)(void *)dst[c], vreinterpret_u32_u8(vqmovn_u16(vcombine_u16(o, o))), 0);
    }
#else
    for (u32 lane = 0; lane < 4; ++lane) {
        u32 gm = C16_GMAP[gp[lane]], bm = C16_BMAP[bp[lane]];
        const u32 *e = table + (gm >> 16) + (bm >> 16) + rp[lane];
        short wg = (short)(u16)gm, wb = (short)(u16)bm;
        rp[lane] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 0, wg, wb);
        gp[lane] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 10, wg, wb);
        bp[lane] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 20, wg, wb);
    }
#endif
}

static inline void c16_pixel(const u32 *table, const u32 *gmap, const u32 *bmap,
                             u32 r, u32 g, u32 b, int out[3]) {
    (void)gmap; (void)bmap;
    u32 gm = C16_GMAP[g], bm = C16_BMAP[b];
    const u32 *e = table + (gm >> 16) + (bm >> 16) + r;
    short wg = (short)(u16)gm, wb = (short)(u16)bm;
    out[0] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 0, wg, wb);
    out[1] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 10, wg, wb);
    out[2] = c16_channel(e[0], e[COMPACT_G_STEP], e[COMPACT_B_STEP], e[COMPACT_B_STEP+COMPACT_G_STEP], 20, wg, wb);
}

static inline int block_smooth(u32 a, u32 b, u32 c, u32 d) {
    u32 lo = a < b ? a : b, hi = a < b ? b : a;
    if (c < lo) lo = c;
    if (c > hi) hi = c;
    if (d < lo) lo = d;
    if (d > hi) hi = d;
    return hi - lo <= BLOCK_FLAT;
}

static inline u8 clamp_u8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* v1 semantics for tiles [x, width) of one row pair; returns smooth/total. */
static void blocks_v1_cols(const u32 *table, const u32 *gmap, const u32 *bmap,
                           u8 *rp[2], u8 *gp[2], u8 *bp[2], u32 x, u32 width,
                           unsigned long long *smooth_out, unsigned long long *tiles_out) {
    unsigned long long smooth = 0, tiles = 0;
        for (; x + 4 <= width; x += 4) {
            ++tiles;
            u8 *pl[3][2] = {{rp[0]+x, rp[1]+x}, {gp[0]+x, gp[1]+x}, {bp[0]+x, bp[1]+x}};
            int ok = 1;
            for (u32 c = 0; ok && c < 3; ++c)
                for (u32 half = 0; ok && half < 4; half += 2)
                    ok = block_smooth(pl[c][0][half], pl[c][0][half+1],
                                      pl[c][1][half], pl[c][1][half+1]);
            if (!ok) {
                c16_group4(table, gmap, bmap, rp[0]+x, gp[0]+x, bp[0]+x);
                c16_group4(table, gmap, bmap, rp[1]+x, gp[1]+x, bp[1]+x);
                continue;
            }
            ++smooth;
            for (u32 half = 0; half < 4; half += 2) {
                int mean[3], out[3];
                for (u32 c = 0; c < 3; ++c)
                    mean[c] = (pl[c][0][half] + pl[c][0][half+1] +
                               pl[c][1][half] + pl[c][1][half+1] + 2) >> 2;
                c16_pixel(table, gmap, bmap, (u32)mean[0], (u32)mean[1], (u32)mean[2], out);
                for (u32 c = 0; c < 3; ++c) {
                    int delta = out[c] - mean[c];
                    for (u32 row = 0; row < 2; ++row)
                        for (u32 k = half; k < half + 2; ++k)
                            pl[c][row][k] = clamp_u8(pl[c][row][k] + delta);
                }
            }
        }
        for (; x < width; ++x)
            for (u32 row = 0; row < 2; ++row) {
                int out[3];
                c16_pixel(table, gmap, bmap, rp[row][x], gp[row][x], bp[row][x], out);
                rp[row][x] = (u8)out[0]; gp[row][x] = (u8)out[1]; bp[row][x] = (u8)out[2];
            }
    *smooth_out += smooth; *tiles_out += tiles;
}

int xreal_blocks_rgb_lut(const u32 *table, u8 *r, u8 *g, u8 *b,
                         u32 rs, u32 gs, u32 bs, u32 width, u32 height) {
    if (!table || !r || !g || !b || !width || !height ||
        width>1920 || height>1080 || rs<width || gs<width || bs<width ||
        rs>4096 || gs>4096 || bs>4096) return -22;
    const u32 *gmap = table + COMPACT_WORDS, *bmap = gmap + 256;
    unsigned long long smooth = 0, tiles = 0;
    u32 y = 0;
    for (; y + 2 <= height; y += 2) {
        u8 *rp[2] = {r+(u64)y*rs, r+(u64)(y+1)*rs};
        u8 *gp[2] = {g+(u64)y*gs, g+(u64)(y+1)*gs};
        u8 *bp[2] = {b+(u64)y*bs, b+(u64)(y+1)*bs};
        u32 x = 0;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
        /* v2: two 4x2 tiles (8x2 pixels) per step, identical results to v1. */
        for (; x + 8 <= width; x += 8) {
            uint8x8_t px[3][2];
            u8 *base[3][2] = {{rp[0]+x, rp[1]+x}, {gp[0]+x, gp[1]+x}, {bp[0]+x, bp[1]+x}};
            uint8x8_t range_ok = vdup_n_u8(0xff);
            uint16x4_t sums[3];
            for (u32 c = 0; c < 3; ++c) {
                px[c][0] = vld1_u8(base[c][0]); px[c][1] = vld1_u8(base[c][1]);
                uint8x8_t mn = vmin_u8(px[c][0], px[c][1]), mx = vmax_u8(px[c][0], px[c][1]);
                uint8x8_t bmin = vpmin_u8(mn, mn), bmax = vpmax_u8(mx, mx);   /* lanes 0..3: blocks */
                range_ok = vand_u8(range_ok, vcle_u8(vsub_u8(bmax, bmin), vdup_n_u8(BLOCK_FLAT)));
                sums[c] = vadd_u16(vpaddl_u8(px[c][0]), vpaddl_u8(px[c][1]));
            }
            u32 ok; vst1_lane_u32(&ok, vreinterpret_u32_u8(range_ok), 0);
            int tile_a = (ok & 0xffffu) == 0xffffu, tile_b = (ok >> 16) == 0xffffu;
            tiles += 2;
            if (!tile_a && !tile_b) {
                for (u32 row = 0; row < 2; ++row) {
                    c16_group4(table, gmap, bmap, rp[row]+x, gp[row]+x, bp[row]+x);
                    c16_group4(table, gmap, bmap, rp[row]+x+4, gp[row]+x+4, bp[row]+x+4);
                }
                continue;
            }
            smooth += (u32)tile_a + (u32)tile_b;
            /* Exact path for a non-smooth tile first: it only touches its own pixels. */
            if (!tile_a)
                for (u32 row = 0; row < 2; ++row) c16_group4(table, gmap, bmap, rp[row]+x, gp[row]+x, bp[row]+x);
            if (!tile_b)
                for (u32 row = 0; row < 2; ++row) c16_group4(table, gmap, bmap, rp[row]+x+4, gp[row]+x+4, bp[row]+x+4);
            /* Four block means -> one compact16 group. */
            u8 mean[3][4], lut[3][4];
            for (u32 c = 0; c < 3; ++c) {
                uint8x8_t m = vmovn_u16(vcombine_u16(vshr_n_u16(vadd_u16(sums[c], vdup_n_u16(2)), 2), vdup_n_u16(0)));
                vst1_lane_u32((uint32_t *)(void *)mean[c], vreinterpret_u32_u8(m), 0);
                __builtin_memcpy(lut[c], mean[c], 4);
            }
            c16_group4(table, gmap, bmap, lut[0], lut[1], lut[2]);
            for (u32 c = 0; c < 3; ++c) {
                u32 lw, mw; __builtin_memcpy(&lw, lut[c], 4); __builtin_memcpy(&mw, mean[c], 4);
                int16x4_t d = vsub_s16(vreinterpret_s16_u16(vget_low_u16(vmovl_u8(vcreate_u8(lw)))),
                                       vreinterpret_s16_u16(vget_low_u16(vmovl_u8(vcreate_u8(mw)))));
                int16x4x2_t z = vzip_s16(d, d);                       /* d0 d0 d1 d1 | d2 d2 d3 d3 */
                int16x8_t dd = vcombine_s16(z.val[0], z.val[1]);
                for (u32 row = 0; row < 2; ++row) {
                    uint8x8_t o = vqmovun_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(px[c][row])), dd));
                    if (tile_a) vst1_lane_u32((uint32_t *)(void *)base[c][row], vreinterpret_u32_u8(o), 0);
                    if (tile_b) vst1_lane_u32((uint32_t *)(void *)(base[c][row]+4), vreinterpret_u32_u8(o), 1);
                }
            }
        }
#endif
        blocks_v1_cols(table, gmap, bmap, rp, gp, bp, x, width, &smooth, &tiles);
    }
    for (; y < height; ++y) {
        u8 *rr = r+(u64)y*rs, *gg = g+(u64)y*gs, *bb = b+(u64)y*bs;
        u32 x = 0;
        for (; x + 4 <= width; x += 4) c16_group4(table, gmap, bmap, rr+x, gg+x, bb+x);
        for (; x < width; ++x) {
            int out[3];
            c16_pixel(table, gmap, bmap, rr[x], gg[x], bb[x], out);
            rr[x] = (u8)out[0]; gg[x] = (u8)out[1]; bb[x] = (u8)out[2];
        }
    }
    __atomic_fetch_add(&xreal_blocks_smooth_tiles, smooth, __ATOMIC_RELAXED);
    __atomic_fetch_add(&xreal_blocks_tiles, tiles, __ATOMIC_RELAXED);
    return 0;
}

/* coef (perf21): the trilinear cube rewritten per cell as
 *   F = A + B r + C g + D b + E rg + F rb + G gb + H rgb   (per channel)
 * with exact integer coefficients from the 8 corner nodes (8-bit values).
 * A cell's 3 x 8 int16 coefficients are padded to one 64-byte cache line, so
 * each pixel reads one contiguous line instead of compact16's four scattered
 * words (suggested by AI-HDR-AUDIT-2026-11-12.md). Terms are Q15 (weights as
 * c16_weight, products rounded); lane 7 pairs A with 32767. Result is
 * (dot + 16384) >> 15, clamped. Within one level of the float reference on
 * the vendor cubes; NEON and portable paths compute identical integers. */
#define COEF_CELLS 4096U
#define COEF_STRIDE 32U          /* int16 per cell: 3 x 8 used + 8 padding */

unsigned long long xreal_coef_table_bytes(void) {
    return (u64)COEF_CELLS * COEF_STRIDE * 2 + 768U * 4;
}

int xreal_build_coef_table(const u16 *cube, short *table) {
    if (!cube || !table) return -22;
    for (u32 b = 0; b < 16; ++b) for (u32 g = 0; g < 16; ++g) for (u32 r = 0; r < 16; ++r) {
        short *cell = table + (u64)((b * 16 + g) * 16 + r) * COEF_STRIDE;
        for (u32 c = 0; c < 3; ++c) {
            int n[2][2][2];
            for (u32 db = 0; db < 2; ++db) for (u32 dg = 0; dg < 2; ++dg) for (u32 dr = 0; dr < 2; ++dr) {
                u32 v = ((u32)cube[(((b + db) * 17 + g + dg) * 17 + r + dr) * 3 + c] + 128) >> 8;
                n[db][dg][dr] = (int)(v > 255 ? 255 : v);
            }
            int A = n[0][0][0];
            int B = n[0][0][1] - A, C = n[0][1][0] - A, D = n[1][0][0] - A;
            int E = n[0][1][1] - n[0][0][1] - n[0][1][0] + A;
            int F = n[1][0][1] - n[0][0][1] - n[1][0][0] + A;
            int G = n[1][1][0] - n[0][1][0] - n[1][0][0] + A;
            int H = n[1][1][1] - n[0][1][1] - n[1][0][1] - n[1][1][0] + n[0][0][1] + n[0][1][0] + n[1][0][0] - A;
            short *k = cell + c * 8;
            k[0] = (short)B; k[1] = (short)C; k[2] = (short)D; k[3] = (short)E;
            k[4] = (short)F; k[5] = (short)G; k[6] = (short)H; k[7] = (short)A;
        }
        for (u32 i = 24; i < COEF_STRIDE; ++i) cell[i] = 0;
    }
    u32 *maps = (u32 *)(void *)(table + (u64)COEF_CELLS * COEF_STRIDE);
    for (u32 v = 0; v < 256; ++v) {
        u32 lo = v * 16 / 255, f = v * 16 - lo * 255;
        if (lo == 16) { lo = 15; f = 255; }
        maps[v] = (lo << 16) | f;
        maps[256 + v] = ((lo * 16) << 16) | f;
        maps[512 + v] = ((lo * 256) << 16) | f;
    }
    return 0;
}

static inline void coef_pixel(const short *table, const u32 *maps, u8 *rp, u8 *gp, u8 *bp) {
    u32 mr = maps[*rp], mg = maps[256 + *gp], mb = maps[512 + *bp];
    const short *k = table + (u64)((mr >> 16) + (mg >> 16) + (mb >> 16)) * COEF_STRIDE;
    int wr = (int)c16_weight(mr & 255), wg = (int)c16_weight(mg & 255), wb = (int)c16_weight(mb & 255);
    int rg = (wr * wg + 16384) >> 15, rb = (wr * wb + 16384) >> 15, gb = (wg * wb + 16384) >> 15;
    int rgb = (rg * wb + 16384) >> 15;
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
    short tv[8] = { (short)wr, (short)wg, (short)wb, (short)rg, (short)rb, (short)gb, (short)rgb, 32767 };
    int16x8_t t = vld1q_s16(tv);
    int out[3];
    for (u32 c = 0; c < 3; ++c) {
        int16x8_t kc = vld1q_s16(k + c * 8);
        int32x4_t acc = vmull_s16(vget_low_s16(kc), vget_low_s16(t));
        acc = vmlal_s16(acc, vget_high_s16(kc), vget_high_s16(t));
        out[c] = (vaddvq_s32(acc) + 16384) >> 15;
    }
#else
    int tv[8] = { wr, wg, wb, rg, rb, gb, rgb, 32767 }, out[3];
    for (u32 c = 0; c < 3; ++c) {
        int s = 0;
        for (u32 i = 0; i < 8; ++i) s += k[c * 8 + i] * tv[i];
        out[c] = (s + 16384) >> 15;
    }
#endif
    *rp = (u8)(out[0] < 0 ? 0 : out[0] > 255 ? 255 : out[0]);
    *gp = (u8)(out[1] < 0 ? 0 : out[1] > 255 ? 255 : out[1]);
    *bp = (u8)(out[2] < 0 ? 0 : out[2] > 255 ? 255 : out[2]);
}

int xreal_coef_rgb_lut(const short *table, u8 *r, u8 *g, u8 *b,
                       u32 rs, u32 gs, u32 bs, u32 width, u32 height) {
    if (!table || !r || !g || !b || !width || !height ||
        width>1920 || height>1080 || rs<width || gs<width || bs<width ||
        rs>4096 || gs>4096 || bs>4096) return -22;
    const u32 *maps = (const u32 *)(const void *)(table + (u64)COEF_CELLS * COEF_STRIDE);
    for (u32 y = 0; y < height; ++y) {
        u8 *rp = r+(u64)y*rs, *gp = g+(u64)y*gs, *bp = b+(u64)y*bs;
        u32 last_r = 0, last_g = 0, last_b = 0, out_r = 0, out_g = 0, out_b = 0;
        int have_last = 0;
        u32 x = 0;
        for (; x + 4 <= width; x += 4) {
            u32 kr, kg, kb;
            __builtin_memcpy(&kr, rp+x, 4);
            __builtin_memcpy(&kg, gp+x, 4);
            __builtin_memcpy(&kb, bp+x, 4);
            if (have_last && kr == last_r && kg == last_g && kb == last_b) {
                __builtin_memcpy(rp+x, &out_r, 4);
                __builtin_memcpy(gp+x, &out_g, 4);
                __builtin_memcpy(bp+x, &out_b, 4);
                continue;
            }
#if defined(__aarch64__) && !defined(XREAL_NO_NEON)
            /* Four pixels together: per-pixel dot products are reduced with
             * pairwise adds into one int32x4, finalized and stored at once.
             * Same integers as coef_pixel. */
            {
                const short *kp[4]; int16x8_t tv[4];
                for (u32 lane = 0; lane < 4; ++lane) {
                    u32 mr = maps[rp[x+lane]], mg = maps[256 + gp[x+lane]], mb = maps[512 + bp[x+lane]];
                    kp[lane] = table + (u64)((mr >> 16) + (mg >> 16) + (mb >> 16)) * COEF_STRIDE;
                    int wr = (int)c16_weight(mr & 255), wg = (int)c16_weight(mg & 255), wb = (int)c16_weight(mb & 255);
                    int rg = (wr * wg + 16384) >> 15, rb = (wr * wb + 16384) >> 15, gb = (wg * wb + 16384) >> 15;
                    int rgb = (rg * wb + 16384) >> 15;
                    int16x4_t lo = vcreate_s16((u64)(u16)wr | (u64)(u16)wg << 16 | (u64)(u16)wb << 32 | (u64)(u16)rg << 48);
                    int16x4_t hi = vcreate_s16((u64)(u16)rb | (u64)(u16)gb << 16 | (u64)(u16)rgb << 32 | (u64)(u16)32767 << 48);
                    tv[lane] = vcombine_s16(lo, hi);
                }
                u8 *dst[3] = { rp + x, gp + x, bp + x };
                for (u32 c = 0; c < 3; ++c) {
                    int32x4_t acc[4];
                    for (u32 lane = 0; lane < 4; ++lane) {
                        int16x8_t kc = vld1q_s16(kp[lane] + c * 8);
                        int32x4_t a = vmull_s16(vget_low_s16(kc), vget_low_s16(tv[lane]));
                        acc[lane] = vmlal_s16(a, vget_high_s16(kc), vget_high_s16(tv[lane]));
                    }
                    int32x4_t sum = vpaddq_s32(vpaddq_s32(acc[0], acc[1]), vpaddq_s32(acc[2], acc[3]));
                    int32x4_t v = vshrq_n_s32(vaddq_s32(sum, vdupq_n_s32(16384)), 15);
                    uint16x4_t n16 = vqmovun_s32(v);
                    uint8x8_t n8 = vqmovn_u16(vcombine_u16(n16, n16));
                    vst1_lane_u32((uint32_t *)(void *)dst[c], vreinterpret_u32_u8(n8), 0);
                }
            }
#else
            for (u32 lane = 0; lane < 4; ++lane)
                coef_pixel(table, maps, rp+x+lane, gp+x+lane, bp+x+lane);
#endif
            last_r = kr; last_g = kg; last_b = kb; have_last = 1;
            __builtin_memcpy(&out_r, rp+x, 4);
            __builtin_memcpy(&out_g, gp+x, 4);
            __builtin_memcpy(&out_b, bp+x, 4);
        }
        for (; x < width; ++x) coef_pixel(table, maps, rp+x, gp+x, bp+x);
    }
    return 0;
}
