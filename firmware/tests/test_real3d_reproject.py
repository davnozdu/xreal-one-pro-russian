"""perf23 Real3D: independent per-pixel oracle for the production warp with
depth reprojection (displacement read at x + D(x), per cell), for luma
(vertical + horizontal smoothing) and the joint U/V pass (vertical only)."""
import ctypes as C
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_real3d_smooth import depth_shift, sample_q5


def oracle(src, depth, width, height, stride, dw, dh, bits, begin, end, fill, horizontal):
    ratio, rows = width // dw, height // dh
    out = bytearray([fill]) * (stride*height)
    den = 2*rows
    for y in range(begin*rows, end*rows):
        zy, ry = divmod(y, rows)
        num = 2*ry + 1 - rows
        other = max(zy-1, 0) if num < 0 else min(zy+1, dh-1)
        w = abs(num)
        vb = []
        for zx in range(dw):
            a = depth_shift(depth[zy*dw+zx], ratio, bits)
            b = depth_shift(depth[other*dw+zx], ratio, bits)
            vb.append((a*(den-w) + b*w + rows) >> (den.bit_length()-1))
        mv = [vb[min(zx + (vb[zx] + 16*ratio)//(32*ratio), dw-1)] for zx in range(dw)]
        row = src[y*stride:y*stride+width]
        for x in range(width):
            zx, k = divmod(x, ratio)
            shift = mv[zx]
            if horizontal:
                left, right = mv[max(zx-1, 0)], mv[min(zx+1, dw-1)]
                if abs(left-shift) > 4 or abs(right-shift) > 4:
                    hnum = 2*k + 1 - ratio
                    side = left if hnum < 0 else right
                    hden, hw = 2*ratio, abs(hnum)
                    shift = (shift*(hden-hw) + side*hw + ratio) >> (hden.bit_length()-1)
            out[y*stride+x] = sample_q5(row, x*32 + shift, width)
    return bytes(out)


def main(path):
    lib = C.CDLL(path)
    luma = lib.real3d_warp_luma_reproject_range_for_test
    luma.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p] + [C.c_uint]*9
    pair = lib.real3d_warp_chroma_pair_range_for_test
    pair.argtypes = [C.c_char_p]*5 + [C.c_uint]*8
    rng = random.Random(23)
    dw, dh = 60, 6
    cases = 0
    for bits in (2, 3, 4):
        for kind in ('random', 'step', 'ramp', 'flat', 'object'):
            if kind == 'random':
                depth = bytes(rng.randrange(256) for _ in range(dw*dh))
            elif kind == 'step':
                depth = bytes(200 if zx > 20 + 3*zy else 10 for zy in range(dh) for zx in range(dw))
            elif kind == 'ramp':
                depth = bytes((zx*rng.randrange(4) + zy) % 256 for zy in range(dh) for zx in range(dw))
            elif kind == 'object':
                depth = bytes(180 if 20 <= zx < 35 else 20 for zy in range(dh) for zx in range(dw))
            else:
                depth = bytes([rng.randrange(256)])*(dw*dh)
            width, height, stride = dw*4, dh*4, dw*4 + 24
            src = bytes(rng.randrange(256) for _ in range(stride*height))
            for begin, end in ((0, dh), (0, 1), (2, 5), (5, 6)):
                dst = C.create_string_buffer(bytes([238])*(stride*height), stride*height)
                assert luma(src, dst, depth, width, height, stride, stride, dw, dh, begin, end, bits) == 0
                assert dst.raw == oracle(src, depth, width, height, stride, dw, dh, bits, begin, end, 238, True), \
                    ('luma', bits, kind, begin, end)
                cases += 1
            cw, ch, cs = dw*2, dh*2, dw*2 + 16
            u = bytes(rng.randrange(256) for _ in range(cs*ch))
            v = bytes(rng.randrange(256) for _ in range(cs*ch))
            for begin, end in ((0, dh), (1, 4)):
                du = C.create_string_buffer(bytes([7])*(cs*ch), cs*ch)
                dv = C.create_string_buffer(bytes([7])*(cs*ch), cs*ch)
                assert pair(u, du, v, dv, depth, cw, ch, cs, dw, dh, begin, end, bits) == 0
                assert du.raw == oracle(u, depth, cw, ch, cs, dw, dh, bits, begin, end, 7, False), ('U', bits, kind)
                assert dv.raw == oracle(v, depth, cw, ch, cs, dw, dh, bits, begin, end, 7, False), ('V', bits, kind)
                cases += 1
    zbuf = lib.real3d_zbuf_project_for_test
    zbuf.argtypes = [C.c_char_p, C.c_char_p, C.c_uint, C.c_uint, C.c_uint, C.c_uint]
    for v in (0, 17, 64, 200, 255):
        out = C.create_string_buffer(480)
        assert zbuf(bytes([v])*480, out, 480, 1, 3, 256) == 0
        assert out.raw == bytes([v])*480, ('zbuf-const', v)
    # Foreground object width preservation and zero ghosting under x1.35 gain
    z_obj = bytearray([16]*480)
    z_obj[200] = 80; z_obj[201] = 140
    for c in range(202, 238): z_obj[c] = 160
    z_obj[238] = 140; z_obj[239] = 80
    out_obj = C.create_string_buffer(480)
    assert zbuf(bytes(z_obj), out_obj, 480, 1, 3, 346) == 0
    bilin = lib.real3d_warp_cells_bilinear_range_for_test
    bilin.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p] + [C.c_uint]*9
    src_obj = bytes(220 if 800 <= x < 960 else 40 for x in range(1920))
    dst_obj = C.create_string_buffer(1920*4)
    assert bilin(src_obj*4, dst_obj, out_obj.raw, 1920, 4, 1920, 1920, 480, 1, 0, 1, 3) == 0
    row0 = list(dst_obj.raw[:1920])
    fg = [x for x, val in enumerate(row0) if val > 130]
    assert 156 <= len(fg) <= 164 and fg[-1] - fg[0] + 1 == len(fg), ('zbuf-obj', len(fg), fg[0], fg[-1])
    # perf29: adaptive Q5 Z-buffer + cliff-guarded warp on thin 8px wire + 160px object at +28px shift
    q5w = lib.real3d_zbuf_q5_warp_for_test
    q5w.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p] + [C.c_uint]*8
    z_wire = bytearray([20]*480)
    z_wire[100] = 90; z_wire[101] = 90
    for c in range(200, 240): z_wire[c] = 90
    src_wire = bytes(220 if (400 <= x < 408 or 800 <= x < 960) else 40 for x in range(1920))
    dst_wire = C.create_string_buffer(1920*4)
    assert q5w(src_wire*4, dst_wire, bytes(z_wire), 1920, 4, 1920, 480, 1, 3, 20, 820) == 0
    rw = list(dst_wire.raw[:1920])
    wire = [x for x in range(300, 500) if rw[x] > 130]
    obj = [x for x in range(700, 1000) if rw[x] > 130]
    assert len(wire) == 8 and wire[-1] - wire[0] + 1 == 8, ('q5-wire', wire)
    assert len(obj) == 160 and obj[-1] - obj[0] + 1 == 160, ('q5-obj', len(obj), obj[0], obj[-1])

    # perf35: non-multiple-of-4 shift (26 px): 960-bin Z-buffer keeps exact 8 px wire (no +/-2 px bleed)
    dst_26 = C.create_string_buffer(1920*4)
    assert q5w(src_wire*4, dst_26, bytes(z_wire), 1920, 4, 1920, 480, 1, 3, 20, 761) == 0
    rw26 = list(dst_26.raw[:1920])
    wire26 = [x for x in range(300, 500) if rw26[x] > 130]
    assert len(wire26) == 8 and wire26[-1] - wire26[0] + 1 == 8, ('q5-wire-26px', wire26)

    # perf35: vertical cliff guard (zero vertical halo on horizontal top/bottom silhouettes)
    dh_v = 4
    z_vert = bytearray([20]*(480*dh_v))
    for zy in (2, 3):
        for c in range(200, 240):
            z_vert[zy*480 + c] = 90
    src_vert = bytearray([40]*(1920*dh_v*4))
    for y in range(8, 16):
        for x in range(800, 960):
            src_vert[y*1920 + x] = 220
    # Put a bright decoy stripe in the background where an unguarded vertical blend (14..24 px) would land
    for y in range(0, 8):
        for x in range(780, 800):
            src_vert[y*1920 + x] = 180
    dst_vert = C.create_string_buffer(1920*dh_v*4)
    assert q5w(bytes(src_vert), dst_vert, bytes(z_vert), 1920, dh_v*4, 1920, 480, dh_v, 3, 20, 820) == 0
    # Top of foreground (y=8..15) must stay contiguous (159..160 px; 1-px chamfer at 90-deg corner on y=8, zero ghost in disocclusion)
    for y in range(8, 16):
        row = list(dst_vert.raw[y*1920:(y+1)*1920])
        fg = [x for x in range(700, 1000) if row[x] > 130]
        assert (159 if y == 8 else 160) <= len(fg) <= 160 and fg[-1] - fg[0] + 1 == len(fg), ('vcliff-fg', y, len(fg), fg[0], fg[-1])
    # Background row y=7 directly above the shifted object (x=772..779) must NOT drag the decoy stripe at x=780..799
    row7 = list(dst_vert.raw[7*1920:8*1920])
    assert all(row7[x] == 40 for x in range(760, 780)), ('vcliff-bg-halo', row7[760:780])

    # perf35: diagonal silhouette must NOT have a 4-row staircase and must NOT blend fg/bg
    dh_d = 6
    z_diag = bytearray([20]*(480*dh_d))
    src_diag = bytearray([40]*(1920*dh_d*4))
    for zy in range(dh_d):
        edge_c = 200 + zy
        for c in range(edge_c, 260):
            z_diag[zy*480 + c] = 90
        for ry in range(4):
            y = zy*4 + ry
            for x in range(edge_c*4 + ry, 260*4):
                src_diag[y*1920 + x] = 220
    dst_diag = C.create_string_buffer(1920*dh_d*4)
    assert q5w(bytes(src_diag), dst_diag, bytes(z_diag), 1920, dh_d*4, 1920, 480, dh_d, 3, 20, 820) == 0
    edges = []
    for y in range(4, 20):
        row = list(dst_diag.raw[y*1920:(y+1)*1920])
        fg = [x for x in range(700, 1100) if row[x] > 130]
        assert fg, ('diag-empty', y)
        edges.append(fg[0])
    # Must advance monotonically without staying frozen for 4 rows in a row
    assert edges[-1] > edges[0] + 8, ('diag-slope', edges)
    assert any(edges[i] != edges[i+1] for i in range(0, 4)), ('diag-staircase', edges[:5])

    # perf35: chroma vertical cliff guard & 1:1 alignment with luma (no colour fringe)
    q5c = lib.real3d_zbuf_q5_chroma_warp_for_test
    q5c.argtypes = [C.c_char_p]*5 + [C.c_uint]*8
    u_src = bytearray([80]*(960*dh_v*2))
    v_src = bytearray([160]*(960*dh_v*2))
    for y in range(4, 8):
        for x in range(400, 480):
            u_src[y*960 + x] = 210
            v_src[y*960 + x] = 50
    u_dst = C.create_string_buffer(960*dh_v*2)
    v_dst = C.create_string_buffer(960*dh_v*2)
    assert q5c(bytes(u_src), u_dst, bytes(v_src), v_dst, bytes(z_vert),
               960, dh_v*2, 960, 480, dh_v, 3, 20, 820) == 0
    # Background row y=3 right above foreground must remain pure background (no vertical chroma bleed)
    assert all(b == 80 for b in u_dst.raw[3*960:4*960]), 'chroma-vcliff-u-bg'
    assert all(b == 160 for b in v_dst.raw[3*960:4*960]), 'chroma-vcliff-v-bg'
    # Foreground rows y=4..7 must keep exact 80 chroma px width (= 160 luma px / 2)
    for y in range(4, 8):
        ur = list(u_dst.raw[y*960:(y+1)*960])
        fg_u = [x for x in range(350, 500) if ur[x] > 145]
        assert len(fg_u) == 80 and fg_u[-1] - fg_u[0] + 1 == 80, ('chroma-fg', y, len(fg_u))

    # perf35: disocclusion fill skips 1 edge bin to avoid stretching NPU foreground bleed
    src_bleed = bytearray([40]*1920)
    for x in range(800, 962):  # 2 px bleed past depth mask at x=960..961
        src_bleed[x] = 220
    dst_bleed = C.create_string_buffer(1920*4)
    assert q5w(bytes(src_bleed)*4, dst_bleed, bytes(z_wire), 1920, 4, 1920, 480, 1, 3, 20, 820) == 0
    rb = list(dst_bleed.raw[:1920])
    # Disocclusion zone (932..959) must be clean background (40), NOT stretched 220 halo
    assert all(rb[x] < 100 for x in range(936, 960)), ('disocc-bleed-halo', rb[936:960])

    # perf36: continuous 3x3 smooth + monotonic non-folding depth pre-pass (zero duplicate-edge halo)
    sm_pre = lib.real3d_depth_smooth_prepass_for_test
    sm_pre.argtypes = [C.c_char_p, C.c_char_p, C.c_uint, C.c_uint, C.c_uint]
    z_step = bytearray([10]*(480*4))
    for y in range(4):
        for c in range(200, 240):
            z_step[y*480 + c] = 160
    z_sm = C.create_string_buffer(480*4)
    assert sm_pre(bytes(z_step), z_sm, 480, 4, 3) == 0
    row_sm = list(z_sm.raw[480:960])
    # Source sampling position 4*c + d(c)/8 must be strictly monotonically increasing across every cell
    src_pos = [4.0*c + row_sm[c]/8.0 for c in range(480)]
    assert all(src_pos[c+1] > src_pos[c] for c in range(479)), 'non-monotonic-foldover'

    print(f'Real3D perf23/28/29/35/36 reprojection & smooth non-folding oracle passed: {cases} cases')


if __name__ == '__main__':
    main(sys.argv[1])
