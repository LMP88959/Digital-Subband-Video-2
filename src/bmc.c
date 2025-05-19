/*****************************************************************************/
/*
 * Digital Subband Video 2
 *   DSV-2
 *
 *     -
 *    =--  2024-2025 EMMIR
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

/* copy directly from reference block (full-pel) */
static void
cpyblk(uint8_t *dec, uint8_t *ref, int dw, int rw, int w, int h)
{
    while (h-- > 0) {
        memcpy(dec, ref, w);
        ref += rw;
        dec += dw;
    }
}

/* D.5.2 Filtering */

#define ITEST4x4(t) (abs(e0 - avg) < (t) && \
                     abs(i0 - avg) < (t) && \
                     abs(e1 - avg) < (t) && \
                     abs(i1 - avg) < (t) && \
                     abs(e2 - avg) < (t) && \
                     abs(i2 - avg) < (t))

#define FILTER_DIM 4 /* do not touch, filters are hardcoded as 4x4 operations */


#define LPF ((5 * (e0 + i0) + 3 * (e1 + i1) + 8) >> 4)

#define FC_E1 ((3 * (avg + e1) + 2 * e2 + 4) >> 3)
#define FC_E0 ((avg + 2 * e1 + e2 + 4) >> 3)  /* assumes avg was already *= 5 */
#define FC_I0 (avg)
#define FC_I1 ((avg + 2 * i1 + i2 + 4) >> 3)  /* assumes avg was already *= 5 */

static void
ihfilter4x4(DSV_PLANE *dp, int x, int y, int edge, int threshE, int threshM)
{
    int line, top, bot;
    uint8_t *b = dp->data;
    int w = dp->w;
    int h = dp->h;
    int s = dp->stride;
    int in_edge;

    if ((x < FILTER_DIM) || (x > w - FILTER_DIM) ||
        (edge && threshE <= 0) || (threshM <= 0)) {
        return;
    }
    top = x + CLAMP(y, 0, (h - 1)) * s;
    bot = x + CLAMP(y + FILTER_DIM, 0, (h - 1)) * s;
    in_edge = x < (w - FILTER_DIM - FILTER_DIM);
    if (!edge) {
        threshE = threshM;
    }
    for (line = top; line < bot; line += s) {
        int i2, i1, i0, e0, e1, e2, avg;

        e2 = b[line - 3];
        e1 = b[line - 2];
        e0 = b[line - 1];
        i0 = b[line + 0];
        i1 = b[line + 1];
        i2 = b[line + 2];

        avg = LPF;
        if (ITEST4x4(threshE)) {
            b[line - 2] = FC_E1;
            b[line + 0] = FC_I0;
            avg *= 5;
            b[line - 1] = FC_E0;
            b[line + 1] = FC_I1;
        }

        if (in_edge) {
            int k = line + FILTER_DIM;
            i2 = b[k - 2];
            i1 = b[k - 1];
            i0 = b[k + 0];
            e0 = b[k + 1];
            e1 = b[k + 2];
            e2 = b[k + 3];

            avg = LPF;
            if (ITEST4x4(threshM)) {
                b[k + 0] = FC_I0;
                b[k + 2] = FC_E1;
                avg *= 5;
                b[k - 1] = FC_I1;
                b[k + 1] = FC_E0;
            }
        }
    }
}

static void
ivfilter4x4(DSV_PLANE *dp, int x, int y, int edge, int threshE, int threshM)
{
    int beg, end;
    int i, s2, s3;
    int w = dp->w;
    int h = dp->h;
    int s = dp->stride;
    uint8_t *b = dp->data;
    uint8_t *bk = b + FILTER_DIM * s;
    int in_edge;

    if ((y < FILTER_DIM) || (y > h - FILTER_DIM) ||
        (edge && threshE <= 0) || (threshM <= 0)) {
        return;
    }
    beg = CLAMP(x, 0, (w - 1)) + y * s;
    end = CLAMP(x + FILTER_DIM, 0, (w - 1)) + y * s;
    s2 = s * 2;
    s3 = s * 3;
    in_edge = y < (h - FILTER_DIM - FILTER_DIM);
    if (!edge) {
        threshE = threshM;
    }
    for (i = beg; i < end; i++) {
        int i2, i1, i0, e0, e1, e2, avg;

        e2 = b[i - s3];
        e1 = b[i - s2];
        e0 = b[i - s];
        i0 = b[i + 0];
        i1 = b[i + s];
        i2 = b[i + s2];

        avg = LPF;
        if (ITEST4x4(threshE)) {
            b[i - s2] = FC_E1;
            b[i + 0] = FC_I0;
            avg *= 5;
            b[i - s] = FC_E0;
            b[i + s] = FC_I1;
        }

        if (in_edge) {
            i2 = bk[i - s2];
            i1 = bk[i - s];
            i0 = bk[i + 0];
            e0 = bk[i + s];
            e1 = bk[i + s2];
            e2 = bk[i + s3];

            avg = LPF;
            if (ITEST4x4(threshM)) {
                bk[i + 0] = FC_I0;
                bk[i + s2] = FC_E1;
                avg *= 5;
                bk[i - s] = FC_I1;
                bk[i + s] = FC_E0;
            }
        }
    }
}

/* downsampled filter factor for a 4x4 block */
static unsigned
dsff4x4(uint8_t *a, int as)
{
    unsigned sh, sv;
    int dsp0, dsp1, dsp2, dsp3;

    /* create downsampled pixels */
    dsp0 = ((a[0] + a[1] + a[as + 0] + a[as + 1] + 2) >> 2);
    dsp1 = ((a[2] + a[3] + a[as + 2] + a[as + 3] + 2) >> 2);
    a += 2 * as;
    dsp2 = ((a[0] + a[1] + a[as + 0] + a[as + 1] + 2) >> 2);
    dsp3 = ((a[2] + a[3] + a[as + 2] + a[as + 3] + 2) >> 2);

    /* determine if the detail should be kept */
    sh = abs((dsp0 + dsp1) - (dsp3 + dsp2));
    sv = abs((dsp2 + dsp1) - (dsp3 + dsp0));
    if (MAX(sh, sv) < 8) {
        return 0;
    }
    /* find how much it should be smoothed, derived from 'texture' metric */
    dsp2 = 255 - dsp2;
    dsp3 = 255 - dsp3;
    sh = abs(dsp0 - dsp1) + abs(dsp3 - dsp2);
    sv = (abs(dsp2 - dsp1) + abs(dsp3 - dsp0) + 2) >> 2;
    if (sh > sv) {
        return (3 * sh + sv + 2) >> 2;
    }
    return (3 * sv + sh + 2) >> 2;
}

/* texture of a 4x4 block */
static void
texf4x4(uint8_t *ptr, int as, int *psh, int *psv)
{
    int j, prev, px;
    unsigned sh = 0, sv = 0;
    uint8_t *prevptr;

    j = 4;
    prevptr = ptr;
    while (j-- > 0) {
        px = ptr[3];
        sv += abs(px - prevptr[3]);
        prev = px;
        px = ptr[2];
        sh += abs(px - prev);
        sv += abs(px - prevptr[2]);
        prev = px;
        px = ptr[1];
        sh += abs(px - prev);
        sv += abs(px - prevptr[1]);
        prev = px;
        px = ptr[0];
        sh += abs(px - prev);
        sv += abs(px - prevptr[0]);
        prev = px;

        prevptr = ptr;
        ptr += as;
    }
    *psh = sh;
    *psv = sv;
}

#define HISTBITS 4
#define NHIST (1 << HISTBITS)

/* de-gradient filter */
static void
degrad4x4(uint8_t *a, int as)
{
    uint8_t hist[NHIST];
    uint16_t avgs[NHIST];
    int x, y, lo, hi, alo, ahi, flo, fhi, t;
    uint8_t *sp = a;

    memset(hist, 0, sizeof(hist));
    memset(avgs, 0, sizeof(avgs));
    /* generate histogram of quantized luma samples + averages of each bucket */
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            t = sp[x] >> (8 - HISTBITS);
            hist[t]++;
            avgs[t] += sp[x];
        }
        sp += as;
    }
    lo = -1;
    hi = -1;
    /* find darkest and brightest buckets of the histogram */
    for (x = 0; x < NHIST; x++) {
        if (hist[x]) {
            if (lo == -1) {
                lo = x;
            }
            hi = x;
        }
    }
    /* ignore mostly flat blocks */
    if (lo >= hi) {
        return;
    }
    alo = avgs[lo] / hist[lo];
    ahi = avgs[hi] / hist[hi];

    if (alo == 0) {
        alo = 1;
    }
    if (ahi == 0) {
        ahi = 1;
    }
    flo = hist[lo];
    fhi = hist[hi];
    /* midpoint brightness of the block and the epsilon around the midpoint */
    t = (alo + ahi + 1) >> 1;

    sp = a;
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            int os = sp[x];
            /* blend outliers with the low and high bucket averages */
            if (os < t) {
                sp[x] = os + ((flo * (alo - os)) / 16);
            } else if (os > t) {
                sp[x] = os + ((fhi * (ahi - os)) / 16);
            }
        }
        sp += as;
    }
}

extern void
dsv_post_process(DSV_PLANE *dp)
{
    int i, j, x, y;
    int nsbx, nsby;

    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;
    for (j = 0; j < nsby; j++) {
        y = j * FILTER_DIM;
        if ((y + FILTER_DIM) >= dp->h) {
            continue;
        }
        for (i = 0; i < nsbx; i++) {
            x = i * FILTER_DIM;
            if ((x + FILTER_DIM) >= dp->w) {
                continue;
            }
            degrad4x4(DSV_GET_XY(dp, x, y), dp->stride);
        }
    }
}

static int
curve_tex(int tt)
{
    if (tt < 8) {
        return (8 - tt) * 8;
    }
    if (tt > 192) {
        return 91 + (tt >> 1);
    }
    return (tt - (8 - 1));
}

static int
compute_filter_q(DSV_PARAMS *p, int q)
{
    int psyf = dsv_spatial_psy_factor(p, -1);
    if (q > 1536) {
        q = 1536;
    }
    q += q * psyf >> (7 + 3);
    if (q < 1024) {
        q = 512 + q / 2;
    }
    return q;
}

extern void
dsv_intra_filter(int q, DSV_PARAMS *p, DSV_FMETA *fm, int c, DSV_PLANE *dp, int do_filter)
{
    int i, j, x, y;
    int nsbx, nsby;
    int fthresh;
    if (p->lossless) {
        return;
    }
    if (c != 0) {
        return;
    }
    if (!do_filter) {
        return;
    }
    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;
    q = compute_filter_q(p, q);
    fthresh = 32 * (14 - dsv_lb2(q));
    for (j = 0; j < nsby; j++) {
        int fy = (j * p->nblocks_v / nsby);
        y = j * FILTER_DIM;
        if ((y + FILTER_DIM) >= dp->h) {
            continue;
        }
        for (i = 0; i < nsbx; i++) {
            int fx = (i * p->nblocks_h / nsbx);
            int flags = fm->blockdata[fx + fy * p->nblocks_h];
            int tt = 32;

            x = i * FILTER_DIM;
            if ((x + FILTER_DIM) >= dp->w) {
                continue;
            }
            if (do_filter && !(flags & DSV_IS_RINGING)) {
                int sh, sv;

                texf4x4(DSV_GET_XY(dp, x, y), dp->stride, &sh, &sv);
                /* only filter blocks with significant texture */
                if (MAX(sh, sv) > 8) {
                    if (flags & (DSV_IS_MAINTAIN | DSV_IS_STABLE)) {
                        tt = dsff4x4(DSV_GET_XY(dp, x, y), dp->stride);
                        if (flags & DSV_IS_STABLE) {
                            tt = tt * 5 >> 2;
                        }
                    } else {
                        tt >>= 2;
                    }
                    tt = (tt * 2 / 3);
                    tt = (CLAMP(tt, 0, fthresh) * q) >> DSV_MAX_QP_BITS;
                    ihfilter4x4(dp, x, y, 0, tt, tt);
                    ivfilter4x4(dp, x, y, 0, tt, tt);
                    if (sh > sv) {
                        tt = (3 * sh + sv);
                    } else {
                        tt = (3 * sv + sh);
                    }
                    tt = curve_tex(tt);
                    tt = 16 + ((tt + 8) >> 4);
                    tt = (CLAMP(tt, 0, fthresh) * q) >> DSV_MAX_QP_BITS;
                    ihfilter4x4(dp, x, y, 0, tt, tt);
                    ivfilter4x4(dp, x, y, 0, tt, tt);
                }
            }
        }
    }
}

static void
luma_filter(DSV_MV *vecs, int q, DSV_PARAMS *p, DSV_PLANE *dp, int do_filter)
{
    int i, j, x, y;
    int nsbx, nsby;
    int sharpen;
#define NDCACHE_INVALID -1
    /* x, y, neighbordif */
    int cached[3] = { NDCACHE_INVALID, NDCACHE_INVALID, NDCACHE_INVALID };
    int fthresh;

    if (p->vidmeta->inter_sharpen) {
        sharpen = p->temporal_mc; /* sharpen when the smoother half-pel filter was used */
    } else {
        sharpen = 0;
    }
    if (p->lossless) {
        return;
    }
    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;
    q = compute_filter_q(p, q);
    fthresh = 32 * (14 - dsv_lb2(q));
    for (j = 0; j < nsby; j++) {
        int edgev, fy;

        fy = (j * p->nblocks_v / nsby);
        edgev = ((j * FILTER_DIM) % p->blk_h) == 0;
        y = j * FILTER_DIM;
        if ((y + FILTER_DIM) >= dp->h) {
            continue;
        }
        for (i = 0; i < nsbx; i++) {
            int edgeh, fx, nd;
            DSV_MV *mv;
            uint8_t *dxy;

            fx = (i * p->nblocks_h / nsbx);
            edgeh = ((i * FILTER_DIM) % p->blk_w) == 0;
            mv = &vecs[fx + fy * p->nblocks_h];

            x = i * FILTER_DIM;

            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            if ((x + FILTER_DIM) >= dp->w) {
                continue;
            }
            if (DSV_MV_IS_INTRA(mv)) {
                int intra_thresh = ((32 * q) >> DSV_MAX_QP_BITS);
                intra_thresh = CLAMP(intra_thresh, 2, 32);

                ihfilter4x4(dp, x, y, edgeh, intra_thresh * 2, intra_thresh);
                ivfilter4x4(dp, x, y, edgev, intra_thresh * 2, intra_thresh);
                continue;
            }
            /* if current x,y is different than what was cached, update cache */
            if (do_filter && (fx != cached[0] || fy != cached[1] || cached[2] == NDCACHE_INVALID)) {
                nd = dsv_neighbordif(vecs, p, fx, fy);
                cached[0] = fx;
                cached[1] = fy;
                cached[2] = nd;
            } else {
                /* use cached value */
                nd = cached[2];
            }

            dxy = DSV_GET_XY(dp, x, y);

            if (do_filter && nd) {
                int tt, sh, sv;
                texf4x4(dxy, dp->stride, &sh, &sv);
                /* non-linearize texture */
                tt = curve_tex((sh + sv + 1) >> 1);

                /* filter strength scaled proportionally
                 * with the motion block vector's neighbor difference
                 */
                if (edgeh || edgev) {
                    /* 4x4 block edges that coincide with motion block edges
                     * should have the 4x4 block's texture influence the strength
                     * because the texture is likely to have come from poor
                     * motion compensation
                     */
                    tt = nd * tt >> 2;
                } else {
                    tt = nd * 4;
                }
                tt = (CLAMP(tt, 0, fthresh) * q) >> DSV_MAX_QP_BITS;
                if (sh > 2 * sv) {
                    ivfilter4x4(dp, x, y, edgev, tt * 2, tt);
                } else if (sv > 2 * sh) {
                    ihfilter4x4(dp, x, y, edgeh, tt * 2, tt);
                } else {
                    ihfilter4x4(dp, x, y, 0, tt, tt);
                    ivfilter4x4(dp, x, y, 0, tt, tt);
                }
            }

            if (sharpen && DSV_IS_DIAG(mv) && DSV_IS_QPEL(mv) && abs(mv->u.mv.x) < 8 && abs(mv->u.mv.y) < 8) {
                degrad4x4(dxy, dp->stride);
            }
        }
    }
}

static void
chroma_filter(DSV_MV *vecs, int q, DSV_PARAMS *p, DSV_PLANE *dp)
{
    int i, j, x, y, bw, bh, sh, sv;
    int intra_thresh;
    DSV_MV *mv;

    sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
    sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);

    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;
    if (p->lossless) {
        return;
    }
    intra_thresh = ((64 * q) >> DSV_MAX_QP_BITS);
    intra_thresh = CLAMP(intra_thresh, 2, 32);

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            if (!DSV_MV_IS_SKIP(mv)) {
                int z, t = intra_thresh;
                if (!DSV_MV_IS_INTRA(mv)) {
                    t = (dsv_neighbordif(vecs, p, i, j) * q) >> DSV_MAX_QP_BITS;
                    t = 4 + (t + 1) * (t + 2);
                    t = MIN(64, t);
                }
                /* only filtering top and left sides */
                for (z = 0; z < bh; z += FILTER_DIM) {
                    if ((y + z + FILTER_DIM) < dp->h) {
                        ihfilter4x4(dp, x, y + z, 0, t, t);
                    }
                }
                for (z = 0; z < bw; z += FILTER_DIM) {
                    if ((x + z + FILTER_DIM) < dp->w) {
                        ivfilter4x4(dp, x + z, y, 0, t, t);
                    }
                }
            }
        }
    }
}

static void
luma_qp(uint8_t *dec, int ds,
        uint8_t *ref, int rs,
        int bw, int bh,
        int dx, int dy, int tmc)
{
    static int16_t tbuf[(DSV_MAX_BLOCK_SIZE + 3) * DSV_MAX_BLOCK_SIZE];
    int16_t *tmp;
    int x, y, a, b, c, d, f, large_mv, dqtx, dqty;
#define BF_SHIFT  (DSV_HP_SHF + 1)
#define BF_MULADD (1 << DSV_HP_SHF)
    tmp = tbuf;

    large_mv = abs(dx) >= 8 || abs(dy) >= 8;
    dx &= 3;
    dy &= 3;
    /* large motion OR half-pel OR (temporal MC flag % 2) gets the smoother filter */
    dqtx = large_mv || !(dx & 1) || (tmc & 1);
    dqty = large_mv || !(dy & 1) || (tmc & 1);

    /* subpixel filtering is done with two different filters for each direction:
     * A - less sharp
     * B - more sharp
     *
     * temporal MC allows alternating between sightly sharper and softer filters
     * every frame which generally averages out to a better approximation over
     * longer subpel motion sequences.
     */
    for (y = 0; y < bh + 3; y++) {
        for (x = 0; x < bw; x++) {
            a = ref[x + 0];
            b = ref[x + 1];
            c = ref[x + 2];
            d = ref[x + 3];
            if (dqtx) {
                f = DSV_HPF_A(a, b, c, d);
            } else {
                f = DSV_HPF_B(a, b, c, d);
            }
#if 1
            /* linear blend */
            switch (dx) {
                case 0:
                    tmp[x] = (BF_MULADD * 2 * b + BF_MULADD) >> BF_SHIFT;
                    break;
                case 1:
                    tmp[x] = (f + BF_MULADD * b + BF_MULADD) >> BF_SHIFT;
                    break;
                case 2:
                    tmp[x] = (f * 2 + BF_MULADD) >> BF_SHIFT;
                    break;
                case 3:
                    tmp[x] = (f + BF_MULADD * c + BF_MULADD) >> BF_SHIFT;
                    break;
            }
#else
            /* equivalent to above, it's slower but code is more concise */
            if (dx & 2) {
                tmp[x] = (f * (4 - dx) + BF_MULADD * c * (dx - 2) + BF_MULADD) >> BF_SHIFT;
            } else {
                tmp[x] = (f * dx + BF_MULADD * b * (2 - dx) + BF_MULADD) >> BF_SHIFT;
            }
#endif
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
            if (dqty) {
                f = DSV_HPF_A(a, b, c, d);
            } else {
                f = DSV_HPF_B(a, b, c, d);
            }
#if 1
            /* linear blend */
            switch (dy) {
                case 0:
                    dec[x] = clamp_u8((BF_MULADD * 2 * b + BF_MULADD) >> BF_SHIFT);
                    break;
                case 1:
                    dec[x] = clamp_u8((f + BF_MULADD * b + BF_MULADD) >> BF_SHIFT);
                    break;
                case 2:
                    dec[x] = clamp_u8((f * 2 + BF_MULADD) >> BF_SHIFT);
                    break;
                case 3:
                    dec[x] = clamp_u8((f + BF_MULADD * c + BF_MULADD) >> BF_SHIFT);
                    break;
            }
#else
            /* equivalent to above, it's slower but code is more concise */
            if (dy & 2) {
                dec[x] = clamp_u8((f * (4 - dy) + BF_MULADD * c * (dy - 2) + BF_MULADD) >> BF_SHIFT);
            } else {
                dec[x] = clamp_u8((f * dy + BF_MULADD * b * (2 - dy) + BF_MULADD) >> BF_SHIFT);
            }
#endif
        }
        dec += ds;
        tmp += DSV_MAX_BLOCK_SIZE;
    }
}

/* D.1 Chroma Sub-Pixel Filter */
static void
bilinear_sp(
        uint8_t *dec, int ds,
        uint8_t *ref, int rs,
        int w, int h, int dx, int dy, int sh, int sv)
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
        cpyblk(dec, ref, ds, rs, w, h);
    }
}

static void
predict(DSV_MV *vecs, DSV_PARAMS *p, int c, DSV_FRAME *ref, DSV_PLANE *dp)
{
    int i, j, r, x, y, bw, bh, sh, sv, limx, limy;
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
            int dx, dy, px, py;
            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            dx = DSV_SAR(mv->u.mv.x, sh);
            dy = DSV_SAR(mv->u.mv.y, sv);

            px = x + DSV_SAR(dx, 2);
            py = y + DSV_SAR(dy, 2);

            if (DSV_MV_IS_INTRA(mv)) {
                /* D.2 Compensating Intra Blocks */
                uint8_t *dec;
                int avgc;

                px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                py = CLAMP(py, -DSV_FRAME_BORDER, limy);

                if (mv->submask == DSV_MASK_ALL_INTRA) {
                    if (c == 0 && mv->dc) { /* DC is only for luma */
                        avgc = mv->dc;
                    } else {
                        avgc = avgval(DSV_GET_XY(rp, px, py), rp->stride, bw, bh);
                    }
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
                                if (c == 0 && mv->dc) { /* DC is only for luma */
                                    avgc = mv->dc;
                                } else {
                                    avgc = avgval(DSV_GET_XY(rp, px + f, py + g), rp->stride, sbw, sbh);
                                }

                                dec = DSV_GET_XY(dp, sbx, sby);
                                for (r = 0; r < sbh; r++) {
                                    memset(dec, avgc, sbw);
                                    dec += dp->stride;
                                }
                            } else {
                                cpyblk(DSV_GET_XY(dp, sbx, sby),
                                       DSV_GET_XY(rp, px + f, py + g),
                                       dp->stride, rp->stride, sbw, sbh);
                            }
                            mask_index++;
                        }
                    }
                }
            } else { /* inter */
                /* D.1 Compensating Inter Blocks */
                if (c == 0) {
                    if (!DSV_IS_SUBPEL(mv)) {
                        px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                        py = CLAMP(py, -DSV_FRAME_BORDER, limy);
                        cpyblk(DSV_GET_XY(dp, x, y), DSV_GET_XY(rp, px, py), dp->stride, rp->stride, bw, bh);
                    } else {
                        px = CLAMP(px - 1, -DSV_FRAME_BORDER, limx);
                        py = CLAMP(py - 1, -DSV_FRAME_BORDER, limy);
                        luma_qp(DSV_GET_XY(dp, x, y), dp->stride,
                                DSV_GET_XY(rp, px, py), rp->stride,
                                bw, bh, mv->u.mv.x, mv->u.mv.y, p->temporal_mc);
                    }
                } else {
                    px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                    py = CLAMP(py, -DSV_FRAME_BORDER, limy);
                    bilinear_sp(DSV_GET_XY(dp, x, y), dp->stride, DSV_GET_XY(rp, px, py), rp->stride, bw, bh, mv->u.mv.x, mv->u.mv.y, sh, sv);
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
            if (p->lossless) {
                for (n = 0; n < bh; n++) {
                    for (m = 0; m < bw; m++) {
                        out[m] = (pred[m] + res[m] - 128);
                    }
                    pred += predp->stride;
                    res += resp->stride;
                    out += outp->stride;
                }
            } else {
                /* D.4 Reconstruction */
                if (!DSV_MV_IS_EPRM(mv) || (!DSV_MV_IS_INTRA(mv) && DSV_MV_IS_SKIP(mv))) {
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
            if (p->lossless) {
                uint8_t *pred = DSV_GET_XY(predp, x, y);
                for (n = 0; n < bh; n++) {
                    for (m = 0; m < bw; m++) {
                        res[m] = (res[m] - pred[m] + 128);
                    }
                    res += resp->stride;
                    pred += predp->stride;
                }
            } else {
                if (!DSV_MV_IS_INTRA(mv) && (DSV_MV_IS_SKIP(mv) ||
                        ((c == 0 && DSV_MV_IS_NOXMITY(mv)) ||
                         (c != 0 && DSV_MV_IS_NOXMITC(mv))))) {
                    for (n = 0; n < bh; n++) {
                        memset(res, 128, bw);
                        res += resp->stride;
                    }
                } else {
                    uint8_t *pred = DSV_GET_XY(predp, x, y);
                    if (DSV_MV_IS_EPRM(mv)) {
                        for (n = 0; n < bh; n++) {
                            for (m = 0; m < bw; m++) {
                                res[m] = clamp_u8((res[m] - pred[m] + 256) >> 1);
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
/* called by encoder */
extern void
dsv_add_res(DSV_MV *mv, DSV_FMETA *fm, int q, DSV_FRAME *resd, DSV_FRAME *pred, int do_filter)
{
    DSV_PLANE *pp, *rp;
    int c;

    for (c = 0; c < 3; c++) {
        pp = pred->planes + c;
        rp = resd->planes + c;

        reconstruct(mv, fm->params, c, rp, pp, rp);
        if (c == 0) {
            luma_filter(mv, q, fm->params, rp, do_filter);
        } else {
            chroma_filter(mv, q, fm->params, rp);
        }
    }
}


/* called by decoder */
extern void
dsv_add_pred(DSV_MV *mv, DSV_FMETA *fm, int q, DSV_FRAME *resd, DSV_FRAME *out, DSV_FRAME *ref, int do_filter)
{
    DSV_PLANE *rp, *op;
    int c;

    for (c = 0; c < 3; c++) {
        rp = resd->planes + c;
        op = out->planes + c;

        predict(mv, fm->params, c, ref, op); /* make prediction onto temp frame (out) */
        reconstruct(mv, fm->params, c, rp, op, op);
        if (c == 0) {
            luma_filter(mv, q, fm->params, op, do_filter);
        } else {
            chroma_filter(mv, q, fm->params, op);
        }
    }
}
