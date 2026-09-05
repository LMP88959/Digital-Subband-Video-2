/*****************************************************************************/
/*
 * Digital Subband Video 2
 *   DSV-2
 *
 *     -
 *    =--  2024-2026 EMMIR
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
avgval(uint8_t *a, int as, int w, int h)
{
    int i, j;
    int avg = 0;
    if (w == 0 || h == 0) {
        return 0;
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            avg += a[i];
        }
        a += as;
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

#define FILTER_DIM 4 /* do not touch, filters are hardcoded as 4x4 operations */

#define FILTER_EDGE(ptr, stride, tE, tF)                                   \
    do {                                                                   \
        int i2, i1, i0, e0, e1, e2, avg;                                   \
        int d0e, d0i, d1e, d1i, d2e, d2i;                                  \
        uint8_t *curp = ptr;                                               \
                                                                           \
        e2 = curp[-3 * (stride)];                                          \
        e1 = curp[-2 * (stride)];                                          \
        e0 = curp[-1 * (stride)];                                          \
        i0 = curp[ 0 * (stride)];                                          \
        i1 = curp[ 1 * (stride)];                                          \
        i2 = curp[ 2 * (stride)];                                          \
                                                                           \
        avg = (8 * (i0 + e0) + 6 * (e1 + i1) + 2 * (e2 + i2) + 16) >> 5;   \
                                                                           \
        d0e = e0 - avg;                                                    \
        d0i = i0 - avg;                                                    \
        d1e = e1 - avg;                                                    \
        d1i = i1 - avg;                                                    \
        d2e = e2 - avg;                                                    \
        d2i = i2 - avg;                                                    \
        if (d0e < (tE) && -d0e < (tE) &&                                   \
            d0i < (tE) && -d0i < (tE) &&                                   \
            d1e < (tF) && -d1e < (tF) &&                                   \
            d1i < (tF) && -d1i < (tF) &&                                   \
            d2e < (tF) && -d2e < (tF) &&                                   \
            d2i < (tF) && -d2i < (tF)) {                                   \
            curp[-2 * (stride)] = (4 * e1 + 2 * e2 + i0 + e0 + 4) >> 3;    \
            curp[-1 * (stride)] = (2 * (i0 + e0 + e1) + i1 + e2 + 4) >> 3; \
            curp[ 0 * (stride)] = (2 * (i1 + i2 + e1) + e0 + i0 + 4) >> 3; \
        }                                                                  \
    } while (0)


static void
ihfilter4x4(DSV_PLANE *dp, int x, int y, int threshE, int threshF)
{
    uint8_t *p, *data;
    int s, f;

    if (threshE <= 0 || threshF <= 0) {
        return;
    }

    data = dp->data;
    s = dp->stride;
    p = data + x + y * s;

    for (f = 0; f < FILTER_DIM; f++) {
        FILTER_EDGE(p, 1, threshE, threshF);
        FILTER_EDGE(p + FILTER_DIM, -1, threshE, threshF);
        p += s;
    }
}


static void
ivfilter4x4(DSV_PLANE *dp, int x, int y, int threshE, int threshF)
{
    uint8_t *p, *data;
    int s, f;

    if (threshE <= 0 || threshF <= 0) {
        return;
    }

    data = dp->data;
    s = dp->stride;
    p = data + x + y * s;

    for (f = 0; f < FILTER_DIM; f++) {
        FILTER_EDGE(p, s, threshE, threshF);
        FILTER_EDGE(p + FILTER_DIM * s, -s, threshE, threshF);
        p++;
    }
}

static void
haar4x4(uint8_t *src, int as, int *psh, int *psv)
{
    uint8_t *spA, *spB;
    int x, y;
    int sh = 0, sv = 0;

    spA = src;
    spB = src + as;
    as *= 2;
    for (y = 0; y < 4; y += 2) {
        for (x = 0; x < 4; x += 2) {
            int s0, s1, d0, d1, HH;
            int x0, x1, x2, x3;

            x0 = spA[x + 0];
            x1 = spA[x + 1];
            x2 = spB[x + 0];
            x3 = spB[x + 1];

            s0 = x0 + x1;
            s1 = x2 + x3;
            d0 = x0 - x1;
            d1 = x2 - x3;

            HH = (d0 - d1) / 2;

            sh += d0 + d1 + HH; /* LH + HH */
            sv += s0 - s1 + HH; /* HL + HH  */
        }
        spA += as;
        spB += as;
    }
    *psh = sh;
    *psv = sv;
}

#define MIN4(a, b, c, d) MIN(MIN(a, b), MIN(c, d))
#define MAX4(a, b, c, d) MAX(MAX(a, b), MAX(c, d))

static int
ds4x4to2x2(uint8_t *a0, int as, int dsp[4])
{
    uint8_t *a1 = a0 + as;
    /* create downsampled pixels */
    dsp[0] = DSV_UAVG4(a0[0], a0[1], a1[0], a1[1]);
    dsp[1] = DSV_UAVG4(a0[2], a0[3], a1[2], a1[3]);
    a0 += 2 * as;
    a1 += 2 * as;
    dsp[2] = DSV_UAVG4(a0[0], a0[1], a1[0], a1[1]);
    dsp[3] = DSV_UAVG4(a0[2], a0[3], a1[2], a1[3]);

    return 2 * (MAX4(dsp[0], dsp[1], dsp[2], dsp[3]) - MIN4(dsp[0], dsp[1], dsp[2], dsp[3]));
}

static void
haar2L(uint8_t *a, int as, int *psh, int *psv, int *pslh, int *pslv, int *span)
{
    int dsp[4], HH;

    haar4x4(a, as, psh, psv);
    *span = ds4x4to2x2(a, as, dsp);
    HH = (dsp[0] - dsp[1] - dsp[2] + dsp[3]) / 2;
    *psh = abs(*psh);
    *psv = abs(*psv);
    *pslh = abs((dsp[0] - dsp[1] + dsp[2] - dsp[3]) + HH);
    *pslv = abs((dsp[0] + dsp[1] - dsp[2] - dsp[3]) + HH);
}

extern void
dsv_intra_filter(int q, DSV_PARAMS *p, DSV_FMETA *fm, int c, DSV_PLANE *dp, int do_filter)
{
    int i, j, x, y;
    int nsbx, nsby;
    int fthresh, lowt;
    uint8_t *tcache = NULL;

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

    tcache = dsv_alloc(nsbx * sizeof(*tcache));
    if (tcache == NULL) {
        DSV_ERROR(("out of memory"));
    }
    fthresh = q / 64;
    if (q < 128) {
        fthresh -= (128 - q) / 8;
    }
    fthresh = CLAMP(fthresh, 0, 24);
    lowt = MIN((q >> 8), 16);
    for (j = 0; j < nsby; j++) {
        int fy = j / FILTER_DIM;
        y = j * FILTER_DIM;

        memset(tcache, 0, nsbx * sizeof(*tcache));
        for (i = 0; i < nsbx; i++) {
            int fx = i / FILTER_DIM;
            int flags = fm->blockdata[fx + fy * p->nblocks_h];

            x = i * FILTER_DIM;

            if (!(flags & DSV_IS_RINGING)) {
                int sh, sv, shl, svl, span;
                int tt = 0, avg_energy;
                haar2L(DSV_GET_XY(dp, x, y), dp->stride, &sh, &sv, &shl, &svl, &span);
                avg_energy = DSV_UAVG4(sh, shl, sv, svl);

                if ((span >> 1) < MAX(lowt, avg_energy)) {
                    int tl = (shl + svl);
                    int th = (sh + sv);

                    int ad;
                    if (th > (tl * 5 / 2) || ((tl <= (avg_energy + 1)) || (th <= (avg_energy + 1)))) {
                        ad = 0;
                    } else if (tl > (th * 5 / 2)) {
                        ad = abs(tl - avg_energy);
                    } else {
                        ad = MAX(lowt, avg_energy);
                    }
                    tt = MIN(ad, fthresh);
                    tcache[i] = clamp_u8(tt);
                    ihfilter4x4(dp, x, y, tcache[i], tcache[i]);
                }
            }
        }
        for (i = 0; i < nsbx; i++) {
            int fx = i / FILTER_DIM;
            int flags = fm->blockdata[fx + fy * p->nblocks_h];

            x = i * FILTER_DIM;

            if (!(flags & DSV_IS_RINGING) && tcache[i]) {
                ivfilter4x4(dp, x, y, tcache[i], tcache[i]);
            }
        }
    }
    dsv_free(tcache);
}

/* fast median of 9 values using network of 25 comparisons */
static int
median9(uint8_t *src, int x, int y, int stride)
{
    uint8_t v[9];

    memcpy(v + 0, src + (x - 1) + (y - 1) * stride, 3 * sizeof(uint8_t));
    memcpy(v + 3, src + (x - 1) + (y - 0) * stride, 3 * sizeof(uint8_t));
    memcpy(v + 6, src + (x - 1) + (y + 1) * stride, 3 * sizeof(uint8_t));

#define SORT2(a, b) do {\
        if ((b) < (a)) {\
            uint8_t t;  \
            t = (a);    \
            (a) = (b);  \
            (b) = t;    \
        }} while (0)

    SORT2(v[0], v[3]);
    SORT2(v[1], v[7]);
    SORT2(v[2], v[5]);
    SORT2(v[4], v[8]);
    SORT2(v[0], v[7]);

    SORT2(v[2], v[4]);
    SORT2(v[3], v[8]);
    SORT2(v[5], v[6]);
    SORT2(v[0], v[2]);
    SORT2(v[1], v[3]);

    SORT2(v[4], v[5]);
    SORT2(v[7], v[8]);
    SORT2(v[1], v[4]);
    SORT2(v[3], v[6]);
    SORT2(v[5], v[7]);

    SORT2(v[0], v[1]);
    SORT2(v[2], v[4]);
    SORT2(v[3], v[5]);
    SORT2(v[6], v[8]);
    SORT2(v[2], v[3]);

    SORT2(v[4], v[5]);
    SORT2(v[6], v[7]);
    SORT2(v[1], v[2]);
    SORT2(v[3], v[4]);
    SORT2(v[5], v[6]);

    return v[4];
}

static void
filter_mc_err(uint8_t *src, int stride, int w, int h, int thresh)
{
    uint8_t tmp[(DSV_MAX_BLOCK_SIZE + 2) * (DSV_MAX_BLOCK_SIZE + 2)];
    uint8_t *tmpp;
    int src_w, src_h;
    int x, y;

    /* shouldn't happen but who knows... */
    if (w <= 0 || h <= 0) {
        return;
    }
    thresh = (48 - CLAMP(thresh, 0, 48));
    if (thresh == 0) {
        return;
    }

    src_w = w + 2;
    src_h = h + 2;
    tmpp = tmp + src_w + 1;

    for (y = 0; y < src_h; y++) {
        memcpy(tmp + y * src_w, src + (y - 1) * stride - 1, src_w);
    }
    /* this bad boy gets auto-vectorized (at least in clang) */
    for (y = 0; y < h; y++) {
        int top, cur, bot;

        top = (y - 1) * src_w;
        cur = (y + 0) * src_w;
        bot = (y + 1) * src_w;

        for (x = 0; x < w; x++) {
            int c, l, r, u, d;
            int lo, hi;
            int str, med;

            c = tmpp[cur + x];
            l = tmpp[cur + x - 1];
            r = tmpp[cur + x + 1];
            u = tmpp[top + x];
            d = tmpp[bot + x];

            lo = MIN(l, r);
            hi = MAX(l, r);

            str = thresh;

            if (c > lo && c < hi) {
                lo = MIN(u, d);
                hi = MAX(u, d);

                if (c > lo && c < hi) {
                    str >>= 1;
                }
            }
            med = median9(tmpp, x, y, src_w);
            src[y * stride + x] = clamp_u8(c + ((med - c) * str / 64));
        }
    }
}

#define EDGE_4x4        3
#define EDGE_SUB        2
#define EDGE_BLOCK      1
#define EDGE_HALF_BIT   (1 << 2) /* 0 = left/top half, 1 = right/bottom half */

static int
classify_edge(int coord, int block_dim)
{
    if ((coord & (block_dim - 1)) == 0) {
        /* on block boundary */
        return EDGE_BLOCK;
    }
    if ((coord & ((block_dim / 2) - 1)) == 0) {
        /* on subblock boundary AND NOT on block boundary */
        return EDGE_SUB;
    }
    /* on 4x4 boundary AND NOT on block OR subblock boundary */
    if ((coord & (block_dim - 1)) < (block_dim / 2)) {
        return EDGE_4x4; /* left/top */
    }
    return EDGE_4x4 | EDGE_HALF_BIT; /* right/bottom */
}

static unsigned
locate_subblock(int edgehh, int edgevv)
{
    switch (edgehh) {
        case EDGE_BLOCK:
        case EDGE_4x4:
            return ((edgevv == EDGE_SUB || (edgevv & EDGE_HALF_BIT)) ? DSV_MASK_INTRA10 : DSV_MASK_INTRA00);
        case EDGE_SUB:
        case EDGE_4x4 | EDGE_HALF_BIT:
            return ((edgevv == EDGE_SUB || (edgevv & EDGE_HALF_BIT)) ? DSV_MASK_INTRA11 : DSV_MASK_INTRA01);
        default:
            break;
    }
    DSV_ASSERT(0);
    return 0;
}

#define F_BLOCK_UNALIGNED_W 1
#define F_BLOCK_UNALIGNED_H 2
#define F_BLOCK_BOTTOMMOST  4
#define F_BLOCK_RIGHTMOST   8

#define F_AUX_ND_MASK 0x1f /* mask to get neighbordif out of mv->aux */
#define F_AUX_STR_H_SHIFT 10
#define F_AUX_STR_V_SHIFT 8

static void
luma_filter(DSV_MV *vecs, int q, DSV_PARAMS *p, DSV_PLANE *dp)
{
    int i, j, x, y;
    int nsbx, nsby;
    int fthreshE, fthreshF;
    uint8_t *tcache = NULL;
    int align_flags, pos_flags;
    int psyf;
    unsigned tq = (1 + DSV_MAX_QP) - q;
    int logq = dsv_lb2(q);
    int blkshift_h, blkshift_v;
    if (p->lossless) {
        return;
    }
    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;
    blkshift_h = dsv_lb2(p->blk_w / FILTER_DIM);
    blkshift_v = dsv_lb2(p->blk_h / FILTER_DIM);
    align_flags = 0;
    if (dp->w % p->blk_w) {
        align_flags |= F_BLOCK_UNALIGNED_W;
    }
    if (dp->h % p->blk_h) {
        align_flags |= F_BLOCK_UNALIGNED_H;
    }
    tcache = dsv_alloc(nsbx * sizeof(*tcache));
    if (tcache == NULL) {
        DSV_ERROR(("out of memory"));
    }
    psyf = dsv_spatial_psy_factor(p, -1) >= 32;
    fthreshE = (tq << 19) / (3 * tq * tq);
    fthreshF = 32 - (dsv_flb2(tq / 256) >> 3) / 6;
    if (q < 128) {
        fthreshE -= (128 - q) / 3;
        fthreshF -= (128 - q) / 9;
    }
    fthreshE = CLAMP(fthreshE, 0, 0x7f);
    fthreshF = CLAMP(fthreshF, 0, 0x7f);
    for (j = 0; j < p->nblocks_v; j++) {
        int errmvy = 0;

        pos_flags = align_flags;
        if (j == (p->nblocks_v - 1)) {
            pos_flags |= F_BLOCK_BOTTOMMOST;
        }
        if ((pos_flags & (F_BLOCK_UNALIGNED_H | F_BLOCK_BOTTOMMOST)) == (F_BLOCK_UNALIGNED_H | F_BLOCK_BOTTOMMOST)) {
            errmvy = -p->nblocks_h;
        }
        for (i = 0; i < p->nblocks_h; i++) {
            DSV_MV *mv = &vecs[i + j * p->nblocks_h];
            int nd, intra;
            int errmvx = 0;
            mv->aux = 0;
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            intra = DSV_MV_IS_INTRA(mv);
            nd = dsv_neighbordif(vecs, p, i, j) >> psyf;
            nd = dsv_lb2(CLAMP(nd, 0, 65535)) + p->vidmeta->filter_strength;
            mv->aux = MAX(nd, 0);
            if (i > 0) { /* left */
                DSV_MV *leftmv = (mv - 1);
                int str = 0;
                if (intra || (leftmv->flags & ((1 << DSV_MV_BIT_INTRA) | (1 << DSV_MV_BIT_SKIP)))) {
                    str++;
                }
                if (DSV_IS_SUBPEL(leftmv)) {
                    str++;
                }
                mv->aux |= str << F_AUX_STR_H_SHIFT;
            }
            if (j > 0) { /* top */
                DSV_MV *topmv = (mv - p->nblocks_h);
                int str = 0;
                if (intra || (topmv->flags & ((1 << DSV_MV_BIT_INTRA) | (1 << DSV_MV_BIT_SKIP)))) {
                    str++;
                }
                if (DSV_IS_SUBPEL(topmv)) {
                    str++;
                }
                mv->aux |= str << F_AUX_STR_V_SHIFT;
            }
            if (i == (p->nblocks_h - 1)) {
                pos_flags |= F_BLOCK_RIGHTMOST;
            }
            /* mv error is unreliable on bottom/right edge blocks in frames whose width/height is not an exact multiple of the block's dimensions */
            if ((pos_flags & (F_BLOCK_UNALIGNED_W | F_BLOCK_RIGHTMOST)) == (F_BLOCK_UNALIGNED_W | F_BLOCK_RIGHTMOST)) {
                errmvx = -1;
            }
            mv->err[0] = clamp_u8(((mv + errmvx + errmvy)->err[0] << logq) >> 9);

            x = i * p->blk_w;
            y = j * p->blk_h;
            if (mv->err[0]) {
                filter_mc_err(DSV_GET_XY(dp, x, y), dp->stride, p->blk_w, p->blk_h, q / mv->err[0]);
            }
        }
    }
    /* two horizontal passes to avoid step-like filtering artifacts */
    for (j = 0; j < nsby; j++) {
        DSV_MV *mvrow;
        int edgevtype, fy;
        fy = j >> blkshift_v;
        y = j * FILTER_DIM;

        edgevtype = classify_edge(y, p->blk_h);

        mvrow = vecs + fy * p->nblocks_h;
        for (i = 0; i < nsbx; i++) {
            int edgehtype, fx, nd;
            DSV_MV *mv;
            int tt, ht = 0, vt = 0;

            fx = i >> blkshift_h;
            x = i * FILTER_DIM;
            mv = mvrow + fx;
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            edgehtype = classify_edge(x, p->blk_w);
            nd = mv->aux & F_AUX_ND_MASK;

            tt = mv->err[0];
            if (DSV_MV_IS_INTRA(mv) && (mv->submask & locate_subblock(edgehtype, edgevtype))) {
                tt += fthreshF;
                tt = MIN(tt, fthreshE);
                ht = tt;
                vt = tt;
            } else {
                int edgeh, edgev;

                edgeh = (edgehtype & ~EDGE_HALF_BIT) == EDGE_BLOCK;
                edgev = (edgevtype & ~EDGE_HALF_BIT) == EDGE_BLOCK;

                tt = (tt << nd) >> 4;
                tt = MIN(tt, fthreshF);
                ht = tt;
                vt = tt;

                if (edgeh || edgev) {
                    int sh, sv, shl, svl, span;
                    int strh, strv;
                    strh = ((mv->aux >> F_AUX_STR_H_SHIFT) & 0x3);
                    strv = ((mv->aux >> F_AUX_STR_V_SHIFT) & 0x3);

                    haar2L(DSV_GET_XY(dp, x - 2, y - 2), dp->stride, &sh, &sv, &shl, &svl, &span);

                    span *= 2;
                    if (edgeh) {
                        ht = ((abs(span - sh) + abs(span - shl) * 2) << (nd + strh)) >> 6;
                        ht = MIN(ht, fthreshE);
                    }
                    if (edgev) {
                        vt = ((abs(span - sv) + abs(span - svl) * 2) << (nd + strv)) >> 6;
                        vt = MIN(vt, fthreshE);
                    }
                    if (sh > 2 * sv) {
                        ht >>= 1;
                    }
                    if (sv > 2 * sh) {
                        vt >>= 1;
                    }
                }
            }
            tcache[i] = vt;
            ihfilter4x4(dp, x, y, ht, MIN(ht, fthreshF));
        }
        for (i = 0; i < nsbx; i++) {
            DSV_MV *mv;
            x = i * FILTER_DIM;
            mv = mvrow + (i >> blkshift_h);
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            ivfilter4x4(dp, x, y, tcache[i], MIN(tcache[i], fthreshF));
        }
    }
    dsv_free(tcache);
}

static void
chroma_filter(DSV_MV *vecs, int q, DSV_PARAMS *p, DSV_PLANE *dp)
{
    int i, j, x, y, sh, sv;
    int nsbx, nsby;
    int align_flags, pos_flags;
    int fthresh;
    unsigned tq = (1 + DSV_MAX_QP) - q;
    int logq = dsv_lb2(q);
    int blkshift_h, blkshift_v;

    if (p->lossless) {
        return;
    }
    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;

    sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
    sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    blkshift_h = dsv_lb2((p->blk_w >> sh) / FILTER_DIM);
    blkshift_v = dsv_lb2((p->blk_h >> sv) / FILTER_DIM);
    align_flags = 0;
    if (dp->w % p->blk_w) {
        align_flags |= F_BLOCK_UNALIGNED_W;
    }
    if (dp->h % p->blk_h) {
        align_flags |= F_BLOCK_UNALIGNED_H;
    }
    fthresh = (tq << 19) / (3 * tq * tq);
    if (q < 128) {
        fthresh -= (128 - q) / 3;
    }
    fthresh = CLAMP(fthresh * 2, 0, 0x7f);
    for (j = 0; j < p->nblocks_v; j++) {
        int errmvy = 0;

        pos_flags = align_flags;
        if (j == (p->nblocks_v - 1)) {
            pos_flags |= F_BLOCK_BOTTOMMOST;
        }
        if ((pos_flags & (F_BLOCK_UNALIGNED_H | F_BLOCK_BOTTOMMOST)) == (F_BLOCK_UNALIGNED_H | F_BLOCK_BOTTOMMOST)) {
            errmvy = -p->nblocks_h;
        }
        for (i = 0; i < p->nblocks_h; i++) {
            DSV_MV *mv, *emv;
            int errmvx = 0;
            int thr, nd;
            mv = &vecs[i + j * p->nblocks_h];
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            if (i == (p->nblocks_h - 1)) {
                pos_flags |= F_BLOCK_RIGHTMOST;
            }
            /* mv error is unreliable on bottom/right edge blocks in frames whose width/height is not an exact multiple of the block's dimensions */
            if ((pos_flags & (F_BLOCK_UNALIGNED_W | F_BLOCK_RIGHTMOST)) == (F_BLOCK_UNALIGNED_W | F_BLOCK_RIGHTMOST)) {
                errmvx = -1;
            }
            emv = mv + errmvx + errmvy;
            nd = mv->aux & F_AUX_ND_MASK;
            thr = (((emv->err[1] << logq) >> 9) + 1) << nd;
            mv->err[1] = MIN(thr, fthresh);
            thr = (((emv->err[2] << logq) >> 9) + 1) << nd;
            mv->err[2] = MIN(thr, fthresh);
        }
    }
    /* two horizontal passes to avoid step-like filtering artifacts */
    for (j = 0; j < nsby; j++) {
        DSV_MV *mvrow;
        int fy;
        fy = j >> blkshift_v;
        y = j * FILTER_DIM;

        mvrow = vecs + fy * p->nblocks_h;
        for (i = 0; i < nsbx; i++) {
            DSV_MV *mv;
            mv = mvrow + (i >> blkshift_h);
            x = i * FILTER_DIM;
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            ihfilter4x4(dp + 0, x, y, mv->err[1], mv->err[1] >> 2);
            ihfilter4x4(dp + 1, x, y, mv->err[2], mv->err[2] >> 2);
        }
        for (i = 0; i < nsbx; i++) {
            DSV_MV *mv;
            mv = mvrow + (i >> blkshift_h);
            x = i * FILTER_DIM;
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            ivfilter4x4(dp + 0, x, y, mv->err[1], mv->err[1] >> 2);
            ivfilter4x4(dp + 1, x, y, mv->err[2], mv->err[2] >> 2);
        }
    }
}

static void
luma_qp(uint8_t *dec, int ds,
        uint8_t *ref, int rs,
        int bw, int bh,
        int dx, int dy, int tmc)
{
    int16_t tbuf[(DSV_MAX_BLOCK_SIZE + 3) * DSV_MAX_BLOCK_SIZE];
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
            int px, py;
            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            px = x + DSV_SAR(mv->u.mv.x, 2 + sh);
            py = y + DSV_SAR(mv->u.mv.y, 2 + sv);

            if (DSV_MV_IS_INTRA(mv)) {
                /* D.2 Compensating Intra Blocks */
                uint8_t *dec;
                int avgc;

                px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                py = CLAMP(py, -DSV_FRAME_BORDER, limy);

                if (mv->submask == DSV_MASK_ALL_INTRA) {
                    if (c == 0 && (mv->dc & DSV_SRC_DC_PRED)) { /* DC is only for luma */
                        avgc = mv->dc & 0xff;
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

                    for (g = 0; g <= sbh; g += (sbh + !sbh)) {
                        for (f = 0; f <= sbw; f += (sbw + !sbw)) {
                            sbx = x + f;
                            sby = y + g;
                            if (mv->submask & masks[mask_index]) {
                                if (c == 0 && (mv->dc & DSV_SRC_DC_PRED)) { /* DC is only for luma */
                                    avgc = mv->dc & 0xff;
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
    int i, j, x, y, bw, bh, areashift, sh, sv;
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
    areashift = dsv_lb2(bw * bh);

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
                    unsigned lerr = 0;
                    for (n = 0; n < bh; n++) {
                        for (m = 0; m < bw; m++) {
                            int true_resid = res[m] - 128;
                            lerr += abs(true_resid);
                            /* source = (prediction + residual) */
                            out[m] = clamp_u8(pred[m] + true_resid);
                        }
                        pred += predp->stride;
                        res += resp->stride;
                        out += outp->stride;
                    }
                    mv->err[c] = lerr >> areashift;
                } else {
                    unsigned lerr = 0;
                    for (n = 0; n < bh; n++) {
                        for (m = 0; m < bw; m++) {
                            int true_resid = 2 * (res[m] - 128);
                            lerr += abs(true_resid);
                            out[m] = clamp_u8(pred[m] + true_resid);
                        }
                        pred += predp->stride;
                        res += resp->stride;
                        out += outp->stride;
                    }
                    mv->err[c] = lerr >> areashift;
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
    }
    if (do_filter) {
        dsv_extend_frame(resd);
        luma_filter(mv, q, fm->params, resd->planes + 0);
        chroma_filter(mv, q, fm->params, resd->planes + 1);
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
    }
    if (do_filter) {
        dsv_extend_frame(out);
        luma_filter(mv, q, fm->params, out->planes + 0);
        chroma_filter(mv, q, fm->params, out->planes + 1);
    }
}
