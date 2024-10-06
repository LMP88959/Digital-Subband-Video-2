/*****************************************************************************/
/*
 * Digital Subband Video 2
 *   DSV-2
 *
 *     -
 *    =--     2024 EMMIR
 *   ==---  Envel Graphics
 *  ===----
 *
 *   GitHub : https://github.com/LMP88959
 *   YouTube: https://www.youtube.com/@EMMIR_KC/videos
 *   Discord: https://discord.com/invite/hdYctSmyQJ
 */
/*****************************************************************************/

#include "dsv_internal.h"

static uint8_t
clamp_u8(int v)
{
    return v > 255 ? 255 : v < 0 ? 0 : v;
}

static int
avgval(uint8_t *dec, int dw, int w, int h)
{
    int i, j;
    int avg = 0;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            avg += dec[i];
        }
        dec += dw;
    }
    return avg / (w * h);
}

/* copy directly from previous reference block */
static void
cpyzero(uint8_t *dec, uint8_t *ref, int dw, int rw, int w, int h)
{
    while (h-- > 0) {
        memcpy(dec, ref, w);
        ref += rw;
        dec += dw;
    }
}

/* D.5.2 Filtering */
#define TESTINTRA_1 intra && (abs(e1 - i1) < thresh)
#define TESTINTRA_2 (abs(e1 - avg) < thresh && \
                     abs(e2 - avg) < thresh && \
                     abs(i1 - avg) < thresh && \
                     abs(i2 - avg) < thresh)

#define TESTINTER_1 !intra && (abs(i0 - e0) < ntt)
#define TESTINTER_2 (abs(i0 - i1) << 2) < ntt && (abs(e0 - e1) << 2) < ntt
#define TESTINTER_3 abs(i0 - i1) >= ntt || abs(e0 - e1) >= ntt

static int
compute_ntt(int t)
{
    t = MIN(t, 5);
    return t * t;
}

static void
hfilter(DSV_PLANE *dp, int x, int y, int bw, int bh, int thresh, int intra)
{
    int line, top, bot, ntt;
    uint8_t *b = dp->data;
    int w = dp->w;
    int h = dp->h;
    int s = dp->stride;

    ntt = compute_ntt(thresh);
    top = x + CLAMP(y, 0, (h - 1)) * s;
    bot = x + CLAMP(y + bh, 0, (h - 1)) * s;

    if ((x < 4) || (x > w - 4) || thresh <= 0) {
        return;
    }
    for (line = top; line < bot; line += s) {
        int i3, i2, i1, i0, e0, e1, e2;

        e2 = b[line - 3];
        e1 = b[line - 2];
        e0 = b[line - 1];
        i0 = b[line + 0];
        i1 = b[line + 1];
        i2 = b[line + 2];

        if (TESTINTRA_1) {
            int avg = (e2 + e1 + e1 + e0 + i0 + i1 + i1 + i2 + 4) >> 3;
            if (!TESTINTRA_2) {
                continue;
            }
            b[line - 2] = (avg + e1 + 1) >> 1;
            b[line - 1] = avg;
            b[line + 0] = avg;
            b[line + 1] = (avg + i1 + 1) >> 1;
        } else if (TESTINTER_1) {
            if (TESTINTER_2) {
                i3 = b[line + 3];
                i0 = (e0 + i0 + e1 + i1 + 2) >> 2;
                b[line + 0] = ((i2 + (i1 << 1) + (i0 << 2) + e1 + 4) >> 3);
                b[line + 1] = ((i2 + i1 + (i0 << 1) + 2) >> 2);
                b[line + 2] = (((i3 << 1) + (i2 * 3) + i1 + (i0 << 1) + 4) >> 3);
            } else {
                if (TESTINTER_3) {
                    continue;
                }
                b[line - 2] = ((e2 + e1 + e0 + i0 + 2) >> 2);
                b[line - 1] = ((e2 + (e1 << 1) + (e0 << 1) + (i0 << 1) + i1 + 4) >> 3);
                b[line + 0] = ((i2 + (i1 << 1) + (i0 << 1) + (e0 << 1) + e1 + 4) >> 3);
                b[line + 1] = ((i2 + i1 + i0 + e0 + 2) >> 2);
            }
        }

        if (x < w - bw - bw) {
            int k = line + bw;
            i2 = b[k - 2];
            i1 = b[k - 1];
            i0 = b[k + 0];
            e0 = b[k + 1];
            e1 = b[k + 2];
            e2 = b[k + 3];

            if (TESTINTRA_1) {
                int avg = (e2 + e1 + e1 + e0 + i0 + i1 + i1 + i2 + 4) >> 3;
                if (!TESTINTRA_2) {
                    continue;
                }
                b[k - 1] = (avg + i1 + 1) >> 1;
                b[k + 0] = avg;
                b[k + 1] = avg;
                b[k + 2] = (avg + e1 + 1) >> 1;
            } else if (TESTINTER_1) {
                if (TESTINTER_2) {
                    i3 = b[k - 3];
                    i0 = (e0 + i0 + e1 + i1 + 2) >> 2;
                    b[k - 2] = (((i3 << 1) + (i2 * 3) + i1 + (i0 << 1) + 4) >> 3);
                    b[k - 1] = ((i2 + i1 + (i0 << 1) + 2) >> 2);
                    b[k + 0] = ((i2 + (i1 << 1) + (i0 << 2) + e1 + 4) >> 3);
                } else {
                    if (TESTINTER_3) {
                        continue;
                    }
                    b[k - 1] = ((i2 + i1 + i0 + e0 + 2) >> 2);
                    b[k + 2] = ((e2 + e1 + e0 + i0 + 2) >> 2);
                    b[k + 0] = ((i2 + (i1 << 1) + (i0 << 1) + (e0 << 1) + e1 + 4) >> 3);
                    b[k + 1] = ((e2 + (e1 << 1) + (e0 << 1) + (i0 << 1) + i1 + 4) >> 3);
                }
            }
        }
    }
}

static void
vfilter(DSV_PLANE *dp, int x, int y, int bw, int bh, int thresh, int intra)
{
    int beg, end, ntt;
    int i, s2, s3;
    int w = dp->w;
    int h = dp->h;
    int s = dp->stride;
    uint8_t *b = dp->data;
    uint8_t *bk = b + bh * s;

    beg = CLAMP(x, 0, (w - 1)) + y * s;
    end = CLAMP(x + bw, 0, (w - 1)) + y * s;
    s2 = s * 2;
    s3 = s * 3;
    ntt = compute_ntt(thresh);

    if ((y < 4) || (y > h - 4) || thresh <= 0) {
        return;
    }
    for (i = beg; i < end; i++) {
        int i3, i2, i1, i0, e0, e1, e2;

        e2 = b[i - s3];
        e1 = b[i - s2];
        e0 = b[i - s];
        i0 = b[i + 0];
        i1 = b[i + s];
        i2 = b[i + s2];
        if (TESTINTRA_1) {
            int avg = (e2 + e1 + e1 + e0 + i0 + i1 + i1 + i2 + 4) >> 3;
            if (!TESTINTRA_2) {
                continue;
            }
            b[i - s2] = (avg + e1 + 1) >> 1;
            b[i - s] = avg;
            b[i + 0] = avg;
            b[i + s] = (avg + i1 + 1) >> 1;
        } else if (TESTINTER_1) {
            if (TESTINTER_2) {
                i3 = b[i + s3];
                i0 = (e0 + i0 + e1 + i1 + 2) >> 2;
                b[i + 0] = ((i2 + (i1 << 1) + (i0 << 2) + e1 + 4) >> 3);
                b[i + s] = ((i2 + i1 + (i0 << 1) + 2) >> 2);
                b[i + s2] = (((i3 << 1) + (i2 * 3) + i1 + (i0 << 1) + 4) >> 3);
            } else {
                if (TESTINTER_3) {
                    continue;
                }
                b[i - s2] = ((e2 + e1 + e0 + i0 + 2) >> 2);
                b[i + s] = ((i2 + i1 + i0 + e0 + 2) >> 2);
                b[i - s] = ((e2 + (e1 << 1) + (e0 << 1) + (i0 << 1) + i1 + 4) >> 3);
                b[i + 0] = ((i2 + (i1 << 1) + (i0 << 1) + (e0 << 1) + e1 + 4) >> 3);
            }
        }

        if (y < h - bh - bh) {
            i2 = bk[i - s2];
            i1 = bk[i - s];
            i0 = bk[i + 0];
            e0 = bk[i + s];
            e1 = bk[i + s2];
            e2 = bk[i + s3];
            if (TESTINTRA_1) {
                int avg = (e2 + e1 + e1 + e0 + i0 + i1 + i1 + i2 + 4) >> 3;
                if (!TESTINTRA_2) {
                    continue;
                }
                bk[i - s] = (avg + i2 + 1) >> 1;
                bk[i + 0] = avg;
                bk[i + s] = avg;
                bk[i + s2] = (avg + e2 + 1) >> 1;
            } else if (TESTINTER_1) {
                if (TESTINTER_2) {
                    i3 = bk[i - s3];
                    i0 = (e0 + i0 + e1 + i1 + 2) >> 2;
                    bk[i - s2] = (((i3 << 1) + (i2 * 3) + i1 + (i0 << 1) + 4) >> 3);
                    bk[i - s] = ((i2 + i1 + (i0 << 1) + 2) >> 2);
                    bk[i + 0] = ((i2 + (i1 << 1) + (i0 << 2) + e1 + 4) >> 3);
                } else {
                    if (TESTINTER_3) {
                        continue;
                    }
                    bk[i - s] = ((i2 + i1 + i0 + e0 + 2) >> 2);
                    bk[i + s2] = ((e2 + e1 + e0 + i0 + 2) >> 2);
                    bk[i + 0] = ((i2 + (i1 << 1) + (i0 << 1) + (e0 << 1) + e1 + 4) >> 3);
                    bk[i + s] = ((e2 + (e1 << 1) + (e0 << 1) + (i0 << 1) + i1 + 4) >> 3);
                }
            }
        }
    }
}

/* D.5.1 Filter Thresholds */
static int
est_mag2(int ix, int iy)
{
    ix = abs(ix);
    iy = abs(iy);
    if (ix > iy) {
        return (3 * ix + iy) >> 2;
    }
    return (3 * iy + ix) >> 2;
}

static int
difsq2(int v0x, int v0y, int v1x, int v1y)
{
    if ((v0x == 0 && v0y == 0) || (v1x == 0 && v1y == 0)) {
        return 0;
    }
    return est_mag2(v0x - v1x, v0y - v1y);
}

static int
neighbordif(DSV_MV *vecs, DSV_PARAMS *p, int x, int y, int sh, int sv)
{
    DSV_MV *mv;
    DSV_MV *cmv;
    int d0, d1, mind, maxd, cmx, cmy;
    int vx[2];
    int vy[2];

    cmv = &vecs[x + y * p->nblocks_h];

    cmx = DSV_SAR(cmv->u.mv.x, (sh + 1));
    cmy = DSV_SAR(cmv->u.mv.y, (sv + 1));

    vx[0] = cmx;
    vx[1] = cmx;

    vy[0] = cmy;
    vy[1] = cmy;

    if (x > 0) { /* left */
        mv = (vecs + y * p->nblocks_h + (x - 1));
        if (mv->mode == DSV_MODE_INTER) {
            vx[0] = DSV_SAR(mv->u.mv.x, (sh + 1));
            vy[0] = DSV_SAR(mv->u.mv.y, (sv + 1));
        }
    }
    if (y > 0) { /* top */
        mv = (vecs + (y - 1) * p->nblocks_h + x);
        if (mv->mode == DSV_MODE_INTER) {
            vx[1] = DSV_SAR(mv->u.mv.x, (sh + 1));
            vy[1] = DSV_SAR(mv->u.mv.y, (sv + 1));
        }
    }

    /* difference squared between current motion vector and its left neighbor */
    d0 = difsq2(vx[0], vy[0], cmx, cmy);
    /* difference squared between current motion vector and its top neighbor */
    d1 = difsq2(vx[1], vy[1], cmx, cmy);

    mind = MIN(d0, d1);
    maxd = MAX(d0, d1);

    if (mind > 0) {
        /* if no vector was extremely similar, we scale based off the maximum difference */
        return 16 * maxd;
    }
    /* else we scale based off of how different the most and least similar vectors are */
    return 4 + 8 * (maxd - mind);
}

/* D.5 In-Loop Filtering */
static void
filter(DSV_MV *vecs, uint8_t *blockdata, int q, DSV_PARAMS *p, int c, DSV_PLANE *dp)
{
    int i, j, x, y, bw, bh, sh, sv;
    int intra_thresh;
    DSV_MV *mv;

    if (c == 0) {
        sh = 0;
        sv = 0;
    } else {
        sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
        sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    }
    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;

    /* D.5.1 Filter Thresholds */
    intra_thresh = ((64 * q) >> DSV_MAX_QP_BITS);
    intra_thresh = CLAMP(intra_thresh, 2, 24);

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];
            if (mv->mode == DSV_MODE_INTER) {
                if (!(blockdata[i + j * p->nblocks_h] & DSV_IS_SKIP)) {
                    int t;

                    t = (neighbordif(vecs, p, i, j, sh, sv) * q) >> DSV_MAX_QP_BITS;
                    hfilter(dp, x, y, bw, bh, t, 0);
                    vfilter(dp, x, y, bw, bh, t, 0);
                }
            } else {
                int f, g, sbx, sby, sbw, sbh, mask_index, is_intra;
                uint8_t masks[4] = {
                    DSV_MASK_INTRA00,
                    DSV_MASK_INTRA01,
                    DSV_MASK_INTRA10,
                    DSV_MASK_INTRA11,
                };

                if (mv->submask == DSV_MASK_ALL_INTRA) {
                    hfilter(dp, x, y, bw, bh, intra_thresh, 1);
                    vfilter(dp, x, y, bw, bh, intra_thresh, 1);
                } else {
                    sbw = bw / 2;
                    sbh = bh / 2;
                    mask_index = 0;
                    for (g = 0; g <= sbh; g += sbh) {
                        for (f = 0; f <= sbw; f += sbw) {
                            sbx = x + f;
                            sby = y + g;
                            is_intra = (mv->submask & masks[mask_index]);

                            hfilter(dp, sbx, sby, sbw, sbh, intra_thresh, is_intra);
                            vfilter(dp, sbx, sby, sbw, sbh, intra_thresh, is_intra);
                            mask_index++;
                        }
                    }
                }
            }
        }
    }
}

/* D.1 Luma Half-Pixel Filter */
static void
luma_qp(uint8_t *dec, int ds,
        uint8_t *ref, int rs,
        int bw, int bh,
        int dx, int dy)
{
    static int16_t tbuf[(DSV_MAX_BLOCK_SIZE + 3) * DSV_MAX_BLOCK_SIZE];
    int16_t *tmp;
    int x, y, a, b, c, d, f;

    tmp = tbuf;
    for (y = 0; y < bh + 3; y++) {
        for (x = 0; x < bw; x++) {
            a = ref[x + 0];
            b = ref[x + 1];
            c = ref[x + 2];
            d = ref[x + 3];
            f = DSV_HPF(a, b, c, d);

            if (dx & 2) {
                tmp[x] = (f * (4 - dx) + 32 * c * (dx - 2) + 32) >> 6;
            } else {
                tmp[x] = (f * dx + 32 * b * (2 - dx) + 32) >> 6;
            }
        }
        tmp += DSV_MAX_BLOCK_SIZE;
        ref += rs;
    }
    tmp -= (bh + 3) * DSV_MAX_BLOCK_SIZE;

    for (y = 0; y < bh; y++) {
        for (x = 0; x < bw; x++) {
            a = tmp[x + 0 * DSV_MAX_BLOCK_SIZE];
            b = tmp[x + 1 * DSV_MAX_BLOCK_SIZE];
            c = tmp[x + 2 * DSV_MAX_BLOCK_SIZE];
            d = tmp[x + 3 * DSV_MAX_BLOCK_SIZE];
            f = DSV_HPF(a, b, c, d);

            if (dy & 2) {
                dec[x] = clamp_u8((f * (4 - dy) + 32 * c * (dy - 2) + 32) >> 6);
            } else {
                dec[x] = clamp_u8((f * dy + 32 * b * (2 - dy) + 32) >> 6);
            }
        }
        dec += ds;
        tmp += DSV_MAX_BLOCK_SIZE;
    }
}

/* D.1 Chroma Sub-Pixel Filter */
static void
bilinear_sp(uint8_t *ref, int rs, uint8_t *dec, int ds, int w, int h, int dx, int dy, int sh, int sv)
{
    int hf, vf, hbits, vbits;
    /* 1/4-pel for no subsamp,
     * 1/8-pel for half resolution,
     * 1/16-pel for quarter resolution */
    hbits = (2 + sh);
    vbits = (2 + sv);
    hf = 1 << hbits;
    vf = 1 << vbits;
    dx &= hf - 1;
    dy &= vf - 1;

    if (dx | dy) {
        int x, y, f0, f1, f2, f3, af, sf;

        f0 = (hf - dx) * (vf - dy);
        f1 = dx * (vf - dy);
        f2 = (hf - dx) * dy;
        f3 = dx * dy;

        sf = (hbits + vbits);
        af = 1 << (sf - 1);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                dec[x] = (f0 * ref[x] +
                          f1 * ref[x + 1] +
                          f2 * ref[rs + x] +
                          f3 * ref[rs + x + 1] + af) >> sf;
            }
            dec += ds;
            ref += rs;
        }
    } else {
        cpyzero(dec, ref, ds, rs, w, h);
    }
}

static void
predict(DSV_MV *vecs, DSV_PARAMS *p, int c, DSV_FRAME *ref, DSV_PLANE *dp)
{
    int i, j, r, x, y, dx, dy, px, py, bw, bh, sh, sv, limx, limy;
    DSV_PLANE *rp;
    DSV_MV *mv;

    if (c == 0) {
        sh = 0;
        sv = 0;
    } else {
        sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
        sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    }
    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;

    limx = (dp->w - bw) + DSV_FRAME_BORDER - 1;
    limy = (dp->h - bh) + DSV_FRAME_BORDER - 1;
    rp = ref->planes + c;

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            if (mv->mode == DSV_MODE_INTER) {
                /* D.1 Compensating Inter Blocks */
                dx = DSV_SAR(mv->u.mv.x, sh);
                dy = DSV_SAR(mv->u.mv.y, sv);

                px = x + DSV_SAR(dx, 2);
                py = y + DSV_SAR(dy, 2);
                px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                py = CLAMP(py, -DSV_FRAME_BORDER, limy);

                if (c == 0) {
                    luma_qp(DSV_GET_XY(dp, x, y), dp->stride,
                            DSV_GET_XY(rp, px - 1, py - 1), rp->stride,
                            bw, bh, mv->u.mv.x & 3, mv->u.mv.y & 3);
                } else {
                    bilinear_sp(DSV_GET_XY(rp, px, py), rp->stride, DSV_GET_XY(dp, x, y), dp->stride, bw, bh, mv->u.mv.x, mv->u.mv.y, sh, sv);
                }
            } else { /* intra */
                /* D.2 Compensating Intra Blocks */
                uint8_t *dec;
                int avgc;

                if (mv->submask == DSV_MASK_ALL_INTRA) {
                    avgc = avgval(DSV_GET_XY(rp, x, y), rp->stride, bw, bh);
                    dec = DSV_GET_XY(dp, x, y);
                    for (r = 0; r < bh; r++) {
                        memset(dec, avgc, bw);
                        dec += dp->stride;
                    }
                } else {
                    int f, g, sbx, sby, sbw, sbh, mask_index;
                    uint8_t masks[4] = {
                        DSV_MASK_INTRA00,
                        DSV_MASK_INTRA01,
                        DSV_MASK_INTRA10,
                        DSV_MASK_INTRA11,
                    };
                    sbw = bw / 2;
                    sbh = bh / 2;
                    mask_index = 0;
                    for (g = 0; g <= sbh; g += sbh) {
                        for (f = 0; f <= sbw; f += sbw) {
                            sbx = x + f;
                            sby = y + g;
                            if (mv->submask & masks[mask_index]) {
                                avgc = avgval(DSV_GET_XY(rp, sbx, sby), rp->stride, sbw, sbh);
                                dec = DSV_GET_XY(dp, sbx, sby);
                                for (r = 0; r < sbh; r++) {
                                    memset(dec, avgc, sbw);
                                    dec += dp->stride;
                                }
                            } else {
                                cpyzero(DSV_GET_XY(dp, sbx, sby),
                                        DSV_GET_XY(rp, sbx, sby),
                                        dp->stride, rp->stride, sbw, sbh);
                            }
                            mask_index++;
                        }
                    }
                }
            }
        }
    }
}

static void
reconstruct(DSV_MV *vecs, DSV_PARAMS *p, int c, DSV_PLANE *resp, DSV_PLANE *predp, DSV_PLANE *outp)
{
    int i, j, x, y, bw, bh, sh, sv;
    DSV_MV *mv;

    if (c == 0) {
        sh = 0;
        sv = 0;
    } else {
        sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
        sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    }
    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            int m, n;
            uint8_t *res, *pred, *out;

            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            res = DSV_GET_XY(resp, x, y);
            pred = DSV_GET_XY(predp, x, y);
            out = DSV_GET_XY(outp, x, y);

            /* D.4 Reconstruction */
            if (!mv->eprm || (mv->mode == DSV_MODE_INTER && mv->skip)) {
                for (n = 0; n < bh; n++) {
                    for (m = 0; m < bw; m++) {
                        /* source = (prediction + residual) */
                        out[m] = clamp_u8(pred[m] + res[m] - 128);
                    }
                    pred += predp->stride;
                    res += resp->stride;
                    out += outp->stride;
                }
            } else {
                for (n = 0; n < bh; n++) {
                    for (m = 0; m < bw; m++) {
                        out[m] = clamp_u8(pred[m] + (res[m] - 128) * 2);
                    }
                    pred += predp->stride;
                    res += resp->stride;
                    out += outp->stride;
                }
            }
        }
    }
}

static void
subtract(DSV_MV *vecs, DSV_PARAMS *p, int c, DSV_PLANE *resp, DSV_PLANE *predp)
{
    int i, j, x, y, bw, bh, sh, sv;
    DSV_MV *mv;

    if (c == 0) {
        sh = 0;
        sv = 0;
    } else {
        sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
        sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    }
    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            uint8_t *res;
            int m, n;

            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            res = DSV_GET_XY(resp, x, y);
            if (mv->mode == DSV_MODE_INTER && mv->skip) {
                for (n = 0; n < bh; n++) {
                    memset(res, 128, bw);
                    res += resp->stride;
                }
            } else {
                uint8_t *pred = DSV_GET_XY(predp, x, y);
                if (mv->eprm) {
                    for (n = 0; n < bh; n++) {
                        for (m = 0; m < bw; m++) {
                            res[m] = clamp_u8((res[m] - pred[m]) / 2 + 128);
                        }
                        res += resp->stride;
                        pred += predp->stride;
                    }
                } else {
                    for (n = 0; n < bh; n++) {
                        for (m = 0; m < bw; m++) {
                            res[m] = clamp_u8(res[m] - pred[m] + 128);
                        }
                        res += resp->stride;
                        pred += predp->stride;
                    }
                }
            }
        }
    }
}

extern void
dsv_sub_pred(DSV_MV *mv, DSV_PARAMS *p, DSV_FRAME *pred, DSV_FRAME *resd, DSV_FRAME *ref)
{
    DSV_PLANE *pp, *rp;
    int c;

    for (c = 0; c < 3; c++) {
        pp = pred->planes + c;
        rp = resd->planes + c;

        predict(mv, p, c, ref, pp);
        subtract(mv, p, c, rp, pp);
    }
}

extern void
dsv_add_res(DSV_MV *mv, DSV_FMETA *fm, int q, DSV_FRAME *resd, DSV_FRAME *pred)
{
    DSV_PLANE *pp, *rp;
    int c;

    for (c = 0; c < 3; c++) {
        pp = pred->planes + c;
        rp = resd->planes + c;

        reconstruct(mv, fm->params, c, rp, pp, rp);
        filter(mv, fm->blockdata, q, fm->params, c, rp);
    }
}

extern void
dsv_add_pred(DSV_MV *mv, DSV_FMETA *fm, int q, DSV_FRAME *resd, DSV_FRAME *out, DSV_FRAME *ref)
{
    DSV_PLANE *rp, *op;
    int c;

    for (c = 0; c < 3; c++) {
        rp = resd->planes + c;
        op = out->planes + c;

        predict(mv, fm->params, c, ref, op); /* make prediction onto temp frame (out) */
        reconstruct(mv, fm->params, c, rp, op, op);
        filter(mv, fm->blockdata, q, fm->params, c, op);
    }
}
