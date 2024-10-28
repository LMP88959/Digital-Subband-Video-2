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

#include "dsv_encoder.h"

/* Hierarchical Motion Estimation */

#define QPEL_CHECK_THRESH 3

 /* luma value under which chroma errors are given less or no importance */
#define LUMA_CHROMA_CUTOFF 64

static uint8_t
clamp_u8(int v)
{
    return v > 255 ? 255 : v < 0 ? 0 : v;
}

typedef struct {
    /* green in color, our eyes are more sensitive to greener colors */
    int greenish;
    /* high frequency colors (blue, violet), our eyes are less sensitive to these */
    int hifreq;
    /* almost achromatic, (I think) our eyes can notice chroma error more in greyish regions */
    int greyish;
    /* similar in hue to human skin, important to keep these better quality */
    int skinnish;
} CHROMA_PSY;

static void
chroma_analysis(CHROMA_PSY *c, int y, int u, int v)
{
    c->greenish = u < 128 && v < 128;
    c->greyish = abs(u - 128) < 8 && abs(v - 128) < 8;
    c->skinnish = (y > 80) && (y < 230) &&
                      abs(u - 108) < 24 &&
                      abs(v - 148) < 24; /* terrible approximation */
    c->hifreq = (u > 160) && !c->greyish && !c->skinnish;
}

/* SP_SAD_SZ + 1 should be a power of two for performance reasons */
#define SP_SAD_SZ 15 /* sub-pixel SAD size */

#define SP_DIM    (SP_SAD_SZ + 1)
#define HP_DIM    (SP_DIM * 2)

/* conversion from full-pel coords to half/quarter pel */
#define FP_OFFSET(fpx, fpy) ((1 * (fpx) + (1 * (fpy)) * FP_STRIDE))
#define HP_OFFSET(fpx, fpy) ((2 * (fpx) + (2 * (fpy)) * HP_STRIDE))
#define QP_OFFSET(fpx, fpy) ((4 * (fpx) + (4 * (fpy)) * QP_STRIDE))

#define FP_STRIDE (SP_DIM * 1)
#define HP_STRIDE (SP_DIM * 2)
#define QP_STRIDE (SP_DIM * 4)

#define SIMPLE_SAD 0
#if SIMPLE_SAD
#define SADCMP(d) abs(d)
#define SADRET(a, w, h) (a)
#else
/* RMSE */
#define SADCMP(d) ((d) * (d))
#define SADRET(a, w, h) iisqrt((a))*(w)*(h)/(((w)+(h)+1)>>1)
#endif

static int
iisqrt(unsigned int n)
{
    unsigned int pos, res, rem;

    res = 0;
    pos = 1 << 30;
    rem = n;

    while (pos > rem) {
        pos >>= 2;
    }
    while (pos) {
        unsigned int dif = res + pos;
        res >>= 1;
        if (rem >= dif) {
            rem -= dif;
            res += pos;
        }
        pos >>= 2;
    }
    return ((int) res);
}

/* NOTE: if SIMPLE_SAD is not set, these are RMSE functions, not SAD */
#define MAKE_SAD(w)                                                           \
static int                                                                    \
sad_ ##w## xh(uint8_t *a, int as, uint8_t *b, int bs, int h)                  \
{                                                                             \
    int i, j, acc = 0;                                                        \
    for (j = 0; j < h; j++) {                                                 \
        for (i = 0; i < w; i++) {                                             \
            int dif = (a[i] - b[i]);                                          \
            acc += SADCMP(dif);                                               \
        }                                                                     \
        a += as;                                                              \
        b += bs;                                                              \
    }                                                                         \
    return SADRET(acc, w, h);                                                 \
}

MAKE_SAD(8)
MAKE_SAD(16)
MAKE_SAD(32)

static int
sad_wxh(uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    int i, j;
    unsigned acc = 0;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            int dif = (a[i] - b[i]);
            acc += SADCMP(dif);
        }
        a += as;
        b += bs;
    }
    return SADRET(acc, w, h);
}

static int
fastsad(uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    switch (w) {
        case 8:
            return sad_8xh(a, as, b, bs, h);
        case 16:
            return sad_16xh(a, as, b, bs, h);
        case 32:
            return sad_32xh(a, as, b, bs, h);
        default:
            return sad_wxh(a, as, b, bs, w, h);
    }
}

/* this function is intended to 'prove' to the intra decision
 * that the ref block with (0,0) motion does more good than evil */
static int
inter_zero_good(uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    int i, j;
    int ngood = 0;
    int prevA, prevB;
    uint8_t *prevptrA = a;
    uint8_t *prevptrB = b;

    for (j = 0; j < h; j++) {
        prevA = a[0];
        prevB = b[0];
        for (i = 0; i < w; i++) {
            unsigned pixtex;
            int pa = a[i];
            int pb = b[i];
            int dif = abs(pa - pb);
            /* high texture = beneficial to 'good' decision (inter)
             * because intra blocks don't keep high frequency details */
            pixtex = abs(pa - prevA);
            pixtex += abs(pa - prevptrA[i]);
            pixtex += abs(pb - prevB);
            pixtex += abs(pb - prevptrB[i]);
            if (dif < 8) {
                ngood += (7 - dif) * pixtex;
            } else {
                ngood -= dif * pixtex;
            }
            prevA = pa;
            prevB = pb;
        }
        prevptrA = a;
        prevptrB = b;
        a += as;
        b += bs;
    }
    return ngood > 0;
}

static int
invalid_block(DSV_FRAME *f, int x, int y, int sx, int sy)
{
    int b = f->border * DSV_FRAME_BORDER;
    return x < -b || y < -b || x + sx > f->width + b || y + sy > f->height + b;
}

/*
 * determine if this block needs EPRM
 * returns average luma of MV(0,0) of reconstructed block
 */
static unsigned
calc_EPRM(
        DSV_PLANE *sp, DSV_PLANE *rp, DSV_PLANE *mvrp,
        int w, int h, int *eprmi, int *eprmr)
{
    int i, j;
    int ravg, prd;
    int clipi = 0;
    int clipr = 0;
    uint8_t *mvr = mvrp->data;
    uint8_t *rec = rp->data;
    uint8_t *src = sp->data;

    ravg = 0;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            ravg += rec[i];
        }
        rec += rp->stride;
    }
    ravg /= (w * h);
    rec = rp->data;

    *eprmi = 0;
    *eprmr = 0;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            /* see if MV pred or intra pred would clip and require EPRM */
            if (!clipr) {
                prd = (src[i] - mvr[i]) + 128;
                clipr |= (prd < 0 || prd > 255);
            }
            if (!clipi) {
                prd = (src[i] - ravg) + 128;
                clipi |= (prd < 0 || prd > 255);
            }
            if (clipi && clipr) {
                goto eprm_done;
            }
        }
        src += sp->stride;
        mvr += mvrp->stride;
    }
eprm_done:
    *eprmi = clipi;
    *eprmr = clipr;
    return ravg;
}

/*
 * if the MV is full-pel, determine if the inter
 * prediction is better than the intra prediction
 *
 * returns 1 if inter is better
 */
static unsigned
is_inter_better(
        DSV_PLANE *sp, DSV_PLANE *mvrp,
        int ravg,
        int w, int h,
        DSV_MV *mv,
        int eprmi, int eprmr)
{
    int i, j;
    uint8_t *mvr = mvrp->data;
    uint8_t *src = sp->data;
    unsigned sser = 0;
    unsigned ssei = 0;
    int is_subpel = (mv->u.mv.x & 3) || (mv->u.mv.y & 3);

    /* full-pel is easy to check since there's no interpolation involved */
    if (is_subpel) {
        return 0;
    }

    mvr = mvrp->data;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            int spx, residual, recon, dif;

            spx = src[i];
            if (eprmi) {
                residual = clamp_u8((spx - ravg) / 2 + 128);
                recon = clamp_u8(ravg + (residual - 128) * 2);
            } else {
                residual = clamp_u8((spx - ravg) + 128);
                recon = clamp_u8((ravg + residual) - 128);
            }
            dif = (recon - spx);
            ssei += dif * dif;

            if (eprmr) {
                residual = clamp_u8((spx - mvr[i]) / 2 + 128);
                recon = clamp_u8(mvr[i] + (residual - 128) * 2);
            } else {
                residual = clamp_u8((spx - mvr[i]) + 128);
                recon = clamp_u8((mvr[i] + residual) - 128);
            }
            dif = (recon - spx);
            sser += dif * dif;
        }
        src += sp->stride;
        mvr += mvrp->stride;
    }
    if (sser < ssei) {
        /* inter full-pel pred was better than intra */
        return 1;
    }

    return 0;
}

static unsigned
block_tex(uint8_t *a, int as, int w, int h)
{
    int i, j;
    int prev;
    int px;
    unsigned sh = 0;
    unsigned sv = 0;
    uint8_t *ptr;
    uint8_t *prevptr;

    ptr = a;
    j = h;
    prevptr = ptr;
    while (j-- > 0) {
        i = w;
        prev = ptr[i - 1];
        while (i-- > 0) {
            px = ptr[i];
            sh += abs(px - prev);
            sv += abs(px - prevptr[i]);
            prev = px;
        }
        prevptr = ptr;
        ptr += as;
    }
    return MAX(sh, sv) / (w * h);
}

static int
block_var(uint8_t *a, int as, int w, int h, unsigned *avg)
{
    int i, j;
    int s = 0, var = 0;
    uint8_t *ptr;

    ptr = a;
    j = h;
    while (j-- > 0) {
        i = w;
        while (i-- > 0) {
            s += ptr[i];
        }
        ptr += as;
    }
    s /= (w * h);
    *avg = s;
    ptr = a;
    j = h;
    while (j-- > 0) {
        i = w;
        while (i-- > 0) {
            var += abs(ptr[i] - s);
        }
        ptr += as;
    }
    return var / (w * h);
}

static int
block_span(uint8_t *a, int as, int w, int h)
{
    int i, j;
    int minv = 255, maxv = 0;
    uint8_t *aptr;

    aptr = a;
    j = h;
    while (j-- > 0) {
        i = w;
        while (i-- > 0) {
            minv = MIN(minv, aptr[i]);
            maxv = MAX(maxv, aptr[i]);
        }
        aptr += as;
    }
    return abs(minv - maxv);
}

static int
quant_tex(uint8_t *a, int as, int w, int h)
{
    int i, j;
    int prev;
    int px;
    unsigned sh = 0;
    unsigned sv = 0;
    uint8_t *ptr;
    uint8_t *prevptr;

    ptr = a;
    j = h;
    prevptr = ptr;
    while (j-- > 0) {
        i = w;
        prev = ptr[i - 1] >> 4;
        while (i-- > 0) {
            int d;
            /* squared tex */
            px = ptr[i] >> 4;
            d = (px - prev);
            sh += d * d;
            d = (px - (prevptr[i] >> 4));
            sv += d * d;
            prev = px;
        }
        prevptr = ptr;
        ptr += as;
    }
    return iisqrt(MAX(sh, sv)) / ((w + h + 1) >> 1);
}

/* number of most significant bits of an 8-bit
 * luma sample to include in the histogram */
#define HISTBITS 4
#define NHIST (1 << HISTBITS)

static unsigned
hist_dif(uint16_t histA[NHIST], uint16_t histB[NHIST])
{
    int i;
    unsigned d = 0;

    for (i = 0; i < NHIST; i++) {
        int dif = (int) histA[i] - (int) histB[i];
        d += dif * dif;
    }
    return iisqrt(d / NHIST);
}

static unsigned
block_peaks(uint8_t *a, int as, int w, int h,
        uint8_t peaks[NHIST], uint16_t hist[NHIST])
{
    int x, y;
    int maxv = 0;
    int npeaks = 0;
    int quant16;
    uint8_t *sp = a;
    int avg = 0;

    memset(hist, 0, sizeof(uint16_t) * NHIST);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            avg += a[x + y * as];
        }
    }
    avg /= (w * h);
    if (avg == 0) {
        avg = 1;
    }
    quant16 = ((1 << (HISTBITS - 1)) << 16) / avg;
    /* 2x downsample */
    w /= 2;
    h /= 2;
    for (y = 0; y < h; y++) {
        int bp = 0;
        for (x = 0; x < w; x++) {
            int p1, p2, p3, p4, ds, hi;
            p1 = sp[bp];
            p2 = sp[bp + 1];
            p3 = sp[bp + as];
            p4 = sp[bp + 1 + as];
            ds = ((p1 + p2 + p3 + p4 + 2) >> 2);
            bp += 2;
            hi = ds * quant16 >> 16;
            hist[CLAMP(hi, 0, (NHIST - 1))]++;
        }
        sp += 2 * as;
    }
    avg = 0;
    for (x = 0; x < NHIST; x++) {
        maxv = MAX(maxv, hist[x]);
        avg += hist[x];
    }
    avg /= NHIST;
    maxv >>= 2;
    for (x = 0; x < NHIST; x++) {
        int c, is_peak;

        c = hist[x];
        is_peak = 1;
        if (x > 0) {
            is_peak &= (c > (hist[x - 1]));
        }
        if (x < (NHIST - 1)) {
            is_peak &= (c > (hist[x + 1]));
        }
        is_peak &= (c > maxv) || (c > avg);
        if (is_peak) {
            peaks[npeaks++] = x;
        }
    }

    return npeaks;
}

/* my algorithm for determining if a block contains 'visual edge(s)' as a way
 * to roughly model complexity masking and what features we notice easily.
 *
 * A visual edge is an edge that is substantial and clean.
 * A visual edge is not necessarily the same as an edge obtained from classical
 * edge detection algorithms like the Canny and Sobel methods.
 *
 * Algorithm description:
 *   1. Create histogram of quantized samples in block.
 *   2. Find the most frequent quantized sample.
 *   3. Determine the number of peaks in the histogram, P.
 *   4. Compute texture of quantized samples, T.
 *
 *   If there are one or few P, the block has a relatively small
 *   number of significantly different shades, so it is 'substantial'.
 *   If T is small, then the block is not very noisy, so it is 'clean'.
 */
static unsigned
block_has_vis_edge(uint8_t *a, int as, int w, int h)
{
    uint16_t hist[NHIST];
    uint8_t peaks[NHIST];
    int npeaks;

    npeaks = block_peaks(a, as, w, h, peaks, hist);
    return (npeaks > 1 && npeaks < 4) && quant_tex(a, as, w, h) <= 2;
}

static void
c_span(DSV_PLANE *p, int x, int y, int w, int h, int *uspan, int *vspan)
{
    DSV_PLANE *u = &p[1];
    DSV_PLANE *v = &p[2];
    int i, j;
    int uminv = 255, umaxv = 0;
    int vminv = 255, vmaxv = 0;
    uint8_t *ptrU, *ptrV;

    ptrU = u->data + x + y * u->stride;
    ptrV = v->data + x + y * v->stride;
    j = h;
    while (j-- > 0) {
        i = w;
        while (i-- > 0) {
            uminv = MIN(uminv, ptrU[i]);
            umaxv = MAX(umaxv, ptrU[i]);
            vminv = MIN(vminv, ptrV[i]);
            vmaxv = MAX(vmaxv, ptrV[i]);
        }
        ptrU += u->stride;
        ptrV += v->stride;
    }
    *uspan = abs(uminv - umaxv);
    *vspan = abs(vminv - vmaxv);
}

static void
c_average(DSV_PLANE *p, int x, int y, int w, int h, int *uavg, int *vavg)
{
    DSV_PLANE *u = &p[1];
    DSV_PLANE *v = &p[2];
    int i, j;
    int su = 0;
    int sv = 0;
    uint8_t *ptrU, *ptrV;

    ptrU = u->data + x + y * u->stride;
    ptrV = v->data + x + y * v->stride;
    j = h;
    while (j-- > 0) {
        i = w;
        while (i-- > 0) {
            su += ptrU[i];
            sv += ptrV[i];
        }
        ptrU += u->stride;
        ptrV += v->stride;
    }
    *uavg = su / (w * h);
    *vavg = sv / (w * h);
}

static int
hpsad(uint8_t *a, int as, uint8_t *b)
{
    int i, j;
    unsigned acc = 0;

    for (j = 0; j < SP_SAD_SZ; j++) {
        for (i = 0; i < SP_SAD_SZ; i++) {
            int d = (a[i] - b[HP_OFFSET(i, j)]);
            acc += SADCMP(d);
        }
        a += as;
    }
    return SADRET(acc, SP_SAD_SZ, SP_SAD_SZ);
}

static int
qpsad(uint8_t *a, int as, uint8_t *b)
{
    int i, j;
    unsigned acc = 0;

    for (j = 0; j < SP_SAD_SZ; j++) {
        for (i = 0; i < SP_SAD_SZ; i++) {
            int d = (a[i] - b[QP_OFFSET(i, j)]);
            acc += SADCMP(d);
        }
        a += as;
    }
    return SADRET(acc, SP_SAD_SZ, SP_SAD_SZ);
}

/* D.1 Luma Half-Pixel Filter */
static int
hpfh(uint8_t *p)
{
    return DSV_HPF(p[-1], p[0], p[1], p[2]);
}

static int
hpfv(uint8_t *p, int s)
{
    return DSV_HPF(p[-s], p[0], p[s], p[2 * s]);
}

static void
hpel(uint8_t *dec, uint8_t *ref, int rw)
{
    static int16_t buf[(DSV_MAX_BLOCK_SIZE + 3) * DSV_MAX_BLOCK_SIZE];
    uint8_t *drow;
    int i, j, c, x;

    for (j = 0; j < SP_DIM + 3; j++) {
        for (i = 0; i < SP_DIM; i++) {
            buf[i + j * SP_DIM] = hpfh(ref + (j - 1) * rw + i);
        }
    }
    for (j = 0; j < SP_DIM; j++) {
        drow = dec;
        for (i = 0; i < SP_DIM; i++) {
            x = i + j * SP_DIM;
            drow[HP_STRIDE] = clamp_u8((hpfv(ref + i, rw) + DSV_HP_ADD) >> DSV_HP_SHF);
            *drow++ = ref[i];
            c = DSV_HPF(buf[x + 0 * SP_DIM], buf[x + 1 * SP_DIM], buf[x + 2 * SP_DIM], buf[x + 3 * SP_DIM]);
            drow[HP_STRIDE] = clamp_u8((c + (1 << (DSV_HP_SHF + DSV_HP_SHF - 1))) >> (DSV_HP_SHF + DSV_HP_SHF));
            *drow++ = clamp_u8((hpfh(ref + i) + DSV_HP_ADD) >> DSV_HP_SHF);
        }
        ref += rw;
        dec += 2 * HP_STRIDE; /* skip a row */
    }
}

static void
qpel(uint8_t *dec, uint8_t *ref)
{
    uint8_t *drow, *rx, *ry, *rxy;
    int i, j;
    rx = ref + 1;
    ry = ref + HP_STRIDE;
    rxy = ry + 1;
    for (j = 0; j < HP_DIM; j++) {
        drow = dec;
        for (i = 0; i < HP_DIM; i++) {
            drow[QP_STRIDE] = (ref[i] + ry[i] + 1) >> 1;
            *drow++ = ref[i];
            drow[QP_STRIDE] = (ref[i] + rx[i] + ry[i] + rxy[i] + 2) >> 2;
            *drow++ = (ref[i] + rx[i] + 1) >> 1;
        }
        ref += HP_STRIDE;
        rx += HP_STRIDE;
        ry += HP_STRIDE;
        rxy += HP_STRIDE;
        dec += 2 * QP_STRIDE; /* skip a row */
    }
}

static int
seg_bits(int v)
{
    int n_bits, len = 0;
    unsigned x;

    if (v < 0) {
        v = -v;
    }
    v++;
    x = v;
    for (n_bits = -1; x; n_bits++) {
        x >>= 1;
    }
    len = n_bits * 2 + 1;

    if (v) {
        return len + 1;
    }
    return len;
}

static int
mv_cost(DSV_MV *vecs, DSV_PARAMS *p, int i, int j, int mx, int my)
{
    int px, py;
    dsv_movec_pred(vecs, p, i, j, &px, &py);
    return seg_bits(mx - px) + seg_bits(my - py);
}

static int
detail_subblocks(
        DSV_PARAMS *params, DSV_MV *mv,
        DSV_PLANE *srcp, DSV_PLANE *zrecp,
        int bw, int bh, int tex_src)
{
    int f, g, sbw, sbh, mask_index;
    uint8_t masks[4] = {
            ~DSV_MASK_INTRA00,
            ~DSV_MASK_INTRA01,
            ~DSV_MASK_INTRA10,
            ~DSV_MASK_INTRA11,
    };

    /* do extra checks for 4 quadrants */
    mv->submask = DSV_MASK_ALL_INTRA;
    /* don't give low texture intra blocks the opportunity to cause trouble */
    if (params->effort < 5 || tex_src < 2) {
        goto skip;
    }
    sbw = bw / 2;
    sbh = bh / 2;
    mask_index = 0;
    for (g = 0; g <= sbh; g += sbh) {
        for (f = 0; f <= sbw; f += sbw) {
            if (inter_zero_good(srcp->data + (f + g * srcp->stride), srcp->stride,
                    zrecp->data + (f + g * zrecp->stride), zrecp->stride, sbw, sbh)) {
                /* mark as inter with zero vector */
                mv->submask &= masks[mask_index];
            }
            mask_index++;
        }
    }

skip:
    if (mv->submask) {
        mv->mode = DSV_MODE_INTRA;
        return 1;
    }
    return 0;
}

static int
cleanup_subblocks(
        DSV_PARAMS *params, DSV_MV *mv,
        int mad, int var_d,
        int var_src, int tex_src, int avg_src,
        DSV_PLANE *sp, DSV_PLANE *rcp,
        DSV_PLANE *srcp, DSV_PLANE *zrecp,
        int bw, int bh,
        int cbx, int cby,
        int cbw, int cbh)
{
    int f, g, sbw, sbh, bit_index, nlsb = 0, ncsb = 0;
    int luma_vars[4];
    int luma_vardif[4];
    int luma_avgs[4];
    int skip_luma = 0;
    uint8_t bits[4] = {
            DSV_MASK_INTRA00,
            DSV_MASK_INTRA01,
            DSV_MASK_INTRA10,
            DSV_MASK_INTRA11,
    };
    mv->submask = 0;

    if (params->effort < 6) {
        return 0;
    }

    if (mad < MAX(tex_src, 4) || (mad < 16 && var_src > 0)) {
        skip_luma = 1;
    }
    sbw = bw / 2;
    sbh = bh / 2;
    bit_index = 0;

    for (g = 0; g <= sbh; g += sbh) {
        for (f = 0; f <= sbw; f += sbw) {
            uint8_t *src_d, *rec_d;
            unsigned vs, avs;
            unsigned vr, avr;
            uint16_t histS[NHIST];
            uint16_t histR[NHIST];

            src_d = srcp->data + (f + g * srcp->stride);
            rec_d = zrecp->data + (f + g * zrecp->stride);

            vs = block_var(src_d, srcp->stride, sbw, sbh, &avs);
            vr = block_var(rec_d, zrecp->stride, sbw, sbh, &avr);
            luma_vars[bit_index] = vs;
            luma_vardif[bit_index] = (int) vs - (int) vr;
            luma_avgs[bit_index] = avs;
            if (!skip_luma) {
                int npeaksS, npeaksR;
                int ts, tr;
                int histdif;
                int significant;
                uint8_t peaksS[NHIST], peaksR[NHIST];

                ts = block_tex(src_d, srcp->stride, sbw, sbh);
                tr = block_tex(rec_d, zrecp->stride, sbw, sbh);
                npeaksS = block_peaks(src_d, srcp->stride, sbw, sbh, peaksS, histS);
                npeaksR = block_peaks(rec_d, zrecp->stride, sbw, sbh, peaksR, histR);
                histdif = hist_dif(histS, histR);
                significant = (tr - ts) >= 8
                        || (npeaksS < 4 && npeaksR > 3 * npeaksS / 2);
                if (significant && (histdif > 2)) {
                    int lo, hi, do_intra = 1;
                    int is_subpel = (mv->u.mv.x & 3) || (mv->u.mv.y & 3);
                    if (npeaksS && npeaksR) {
                        /* low/high peak differences between blocks */
                        lo = abs((int) peaksS[0] - (int) peaksR[0]);
                        hi = abs((int) peaksS[MAX(npeaksS - 1, 0)] - (int) peaksR[MAX(npeaksR - 1, 0)]);
                        do_intra = is_subpel ? (hi > 1 && lo > 1) : hi > 0 && lo > 0;
                    } else if (npeaksS == 0 && npeaksR == 0) {
                        do_intra = 0;
                    }
                    if (do_intra) {
                        mv->submask |= bits[bit_index];
                        nlsb++;
                    }
                }
            }
            bit_index++;
        }
    }
    if (avg_src <= LUMA_CHROMA_CUTOFF || (var_src > var_d)) {
       goto skip_chroma;
    }
    sbw = cbw / 2;
    sbh = cbh / 2;
    bit_index = 0;

    for (g = 0; g <= sbh; g += sbh) {
        for (f = 0; f <= sbw; f += sbw) {
            unsigned ud, vd;
            int usa, vsa, ura, vra; /* chroma src/rec averages */
            int downshift;
            CHROMA_PSY spsy;

            if (mv->submask & bits[bit_index]) {
                continue; /* skip subblocks that are already intra */
            }
            if (luma_vars[bit_index] > 3 || luma_vardif[bit_index] <= 1) {
                /* likely not worth messing up the luma if the chroma is bad in these cases */
                continue;
            }
            c_average(sp, cbx + f, cby + g, sbw, sbh, &usa, &vsa);
            c_average(rcp, cbx + f, cby + g, sbw, sbh, &ura, &vra);
            chroma_analysis(&spsy, luma_avgs[bit_index], usa, vsa);
            if (spsy.skinnish) {
                downshift = 3; /* less lenient with skin tones */
            } else if (spsy.hifreq) {
                downshift = 5; /* more lenient with high frequency colors */
            } else {
                downshift = 4;
            }
            ud = abs(usa - ura) >> downshift;
            vd = abs(vsa - vra) >> downshift;
            if (ud || vd) { /* if chroma averages were significantly different */
                mv->submask |= bits[bit_index];
                ncsb += ud + vd;
            }
            bit_index++;
        }
    }
skip_chroma:
    /* combine luma and chroma test scores */
    if ((nlsb + (ncsb / (skip_luma ? 1 : 2))) > 0) {
        mv->mode = DSV_MODE_INTRA;
        return 1;
    }
    mv->submask = 0;
    return 0;
}

static int
subpixel_ME(
        DSV_PARAMS *params,
        DSV_MV *mf, /* motion field */
        DSV_MV *mv, /* current full-pixel motion vector */
        DSV_FRAME *src, DSV_FRAME *ref,
        int i, int j,
        int best,
        int bx, int by, int bw, int bh)
{
    /* for subpixel motion vector rate/distortion decisions */
#define HMV_IMPORTANCE 3 /* half pel */
#define QMV_IMPORTANCE 2 /* quarter pel */
#define SPEL_NSEARCH 8 /* search points for sub-pel search */
    static int xh[SPEL_NSEARCH] = { 1, -1, 0,  0, -1,  1, -1, 1 };
    static int yh[SPEL_NSEARCH] = { 0,  0, 1, -1, -1, -1,  1, 1 };
    static uint8_t tmp[(2 + HP_STRIDE) * (2 + HP_STRIDE)];
    DSV_PLANE srcp_h, refp_h;
    int xx, yy, m, k, score;
    int best_hp;
    uint8_t *tmphp;
    unsigned yarea = bw * bh;

    /* scale down to match area */
    best_hp = best * (SP_SAD_SZ * SP_SAD_SZ) / yarea;
    xx = bx + ((bw >> 1) - (SP_SAD_SZ / 2));
    yy = by + ((bh >> 1) - (SP_SAD_SZ / 2));
    dsv_plane_xy(src, &srcp_h, 0, xx, yy);
    dsv_plane_xy(ref, &refp_h, 0,
            xx + mv->u.mv.x - 1, /* offset by 1 in x and y so */
            yy + mv->u.mv.y - 1);/* we can do negative hpel */
    m = -1;
    hpel(tmp, refp_h.data, refp_h.stride);
    /* start at fullpel (1, 1) */
    tmphp = tmp + HP_OFFSET(1, 1);
    for (k = 0; k < SPEL_NSEARCH; k++) {
        int evx, evy;
        score = hpsad(srcp_h.data, srcp_h.stride,
                tmphp + xh[k] + yh[k] * HP_STRIDE);

        evx = ((mv->u.mv.x * 2) + xh[k]) * 2;
        evy = ((mv->u.mv.y * 2) + yh[k]) * 2;
        score += mv_cost(mf, params, i, j, evx, evy) << HMV_IMPORTANCE;
        if (best_hp > score) {
            best_hp = score;
            m = k;
        }
    }
    mv->u.mv.x *= 2;
    mv->u.mv.y *= 2;
    if (m != -1) {
        mv->u.mv.x += xh[m];
        mv->u.mv.y += yh[m];
        best = best_hp * yarea / (SP_SAD_SZ * SP_SAD_SZ);
    }
    /* try qpel */
    if (params->effort >= 9 ||
            (params->effort == 8 &&
                    abs(mv->u.mv.x) <= QPEL_CHECK_THRESH &&
                    abs(mv->u.mv.y) <= QPEL_CHECK_THRESH)) {
        static uint8_t tmpq[(4 + QP_STRIDE) * (QP_STRIDE + 4)];
        uint8_t *tmpqp;
        int best_qp;
        int qpx, qpy;
        /* start at (1, 1) + potential hpel best */
        qpx = 4;
        qpy = 4;
        if (m != -1) {
            qpx += xh[m] * 2;
            qpy += yh[m] * 2;
            best_qp = best_hp;
        } else {
            best_qp = best * (SP_SAD_SZ * SP_SAD_SZ) / yarea;
        }
        m = -1;

        qpel(tmpq, tmp);
        tmpqp = tmpq + qpx + qpy * QP_STRIDE;
        for (k = 0; k < SPEL_NSEARCH; k++) {
            int evx, evy;
            score = qpsad(srcp_h.data, srcp_h.stride,
                    tmpqp + xh[k] + yh[k] * QP_STRIDE);
            evx = (mv->u.mv.x * 2) + xh[k];
            evy = (mv->u.mv.y * 2) + yh[k];
            score += mv_cost(mf, params, i, j, evx, evy) << QMV_IMPORTANCE;
            if (best_qp > score) {
                best_qp = score;
                m = k;
            }
        }
        mv->u.mv.x *= 2;
        mv->u.mv.y *= 2;
        if (m != -1) {
            mv->u.mv.x += xh[m];
            mv->u.mv.y += yh[m];
            best = best_qp * yarea / (SP_SAD_SZ * SP_SAD_SZ);
        }
    } else {
        mv->u.mv.x *= 2;
        mv->u.mv.y *= 2;
    }
    return best;
}

static int
refine_level(DSV_HME *hme, int level, int *scene_change_blocks)
{
    DSV_FRAME *src, *ref;
    DSV_MV *mv;
    DSV_MV *mf, *parent = NULL;
    DSV_PARAMS *params = hme->params;
    DSV_MV zero;
    int i, j, y_w, y_h, nxb, nyb, step;
    unsigned parent_mask;
    int nintra = 0; /* number of intra blocks */
    DSV_PLANE *sp, *rp, *rcp;
    int ndiff = 0, num_eligible_blocks = 0;

    y_w = params->blk_w;
    y_h = params->blk_h;

    nxb = params->nblocks_h;
    nyb = params->nblocks_v;

    src = hme->src[level];
    ref = hme->ref[level];

    sp = src->planes + 0;
    rp = ref->planes + 0;
    rcp = hme->recon->planes + 0;

    hme->mvf[level] = dsv_alloc(sizeof(DSV_MV) * nxb * nyb);

    mf = hme->mvf[level];

    if (level < hme->enc->pyramid_levels) {
        parent = hme->mvf[level + 1];
    }

    memset(&zero, 0, sizeof(zero));

    step = 1 << level;
    parent_mask = ~((step << 1) - 1);

    for (j = 0; j < nyb; j += step) {
        for (i = 0; i < nxb; i += step) {
#define FPEL_NSEARCH 9 /* search points for full-pel search */
            static int xf[FPEL_NSEARCH] = { 0,  1, -1, 0,  0, -1,  1, -1, 1 };
            static int yf[FPEL_NSEARCH] = { 0,  0,  0, 1, -1, -1, -1,  1, 1 };
            DSV_PLANE srcp;
            int dx, dy, bestdx, bestdy;
            int best, score;
            int bx, by, bw, bh;
            int k, xx, yy, m, n = 0;
            DSV_MV *inherited[8];

            bx = (i * y_w) >> level;
            by = (j * y_h) >> level;
            /* bounds check for safety */
            if ((bx >= src->width) || (by >= src->height)) {
                DSV_MV zmv = { 0 };
                zmv.mode = DSV_MODE_INTER;
                mf[i + j * nxb] = zmv;
                continue;
            }

            dsv_plane_xy(src, &srcp, 0, bx, by);
            bw = MIN(srcp.w, y_w);
            bh = MIN(srcp.h, y_h);

            inherited[n++] = &zero;
            if (parent != NULL) {
                static int pt[5 * 2] = { 0, 0,  -2, 0,  2, 0,  0, -2,  0, 2 };
                int x, y, pi, pj;

                pi = i & parent_mask;
                pj = j & parent_mask;
                for (m = 0; m < 5; m++) {
                    x = pi + pt[(m << 1) + 0] * step;
                    y = pj + pt[(m << 1) + 1] * step;
                    if (x >= 0 && x < nxb &&
                        y >= 0 && y < nyb) {
                        mv = parent + x + y * nxb;
                        /* we only care about unique non-zero parents */
                        if (mv->u.all) {
                            int exists = 0;
                            for (k = 0; k < n; k++) {
                                if (inherited[k]->u.all == mv->u.all) {
                                    exists = 1;
                                    break;
                                }
                            }
                            if (!exists) {
                                inherited[n++] = mv;
                            }
                        }
                    }
                }
            }
            /* find best inherited vector */
            best = n - 1;
            bestdx = inherited[best]->u.mv.x;
            bestdy = inherited[best]->u.mv.y;
            if (n > 1) {
                int best_score = INT_MAX;
                for (k = 0; k < n; k++) {
                    DSV_PLANE refp;

                    if (invalid_block(src, bx, by, bw, bh)) {
                        continue;
                    }

                    dx = DSV_SAR(inherited[k]->u.mv.x, level);
                    dy = DSV_SAR(inherited[k]->u.mv.y, level);

                    if (invalid_block(ref, bx + dx, by + dy, bw, bh)) {
                        continue;
                    }

                    dsv_plane_xy(ref, &refp, 0, bx + dx, by + dy);
                    score = fastsad(srcp.data, srcp.stride, refp.data, refp.stride, bw, bh);
                    if (best_score > score) {
                        best_score = score;
                        best = k;
                    }
                }
                bestdx = inherited[best]->u.mv.x;
                bestdy = inherited[best]->u.mv.y;
            }

            dx = DSV_SAR(bestdx, level);
            dy = DSV_SAR(bestdy, level);

            mv = &mf[i + j * nxb];
            mv->mode = DSV_MODE_INTER;
            mv->submask = 0;

            /* full-pel search around inherited best vector */
            {
                int l, r, u, d;

                l = -bw - bx;
                u = -bh - by;
                r = ref->width - bx;
                d = ref->height - by;

                dx = CLAMP(dx, l, r);
                dy = CLAMP(dy, u, d);
            }

            best = INT_MAX;
            xx = bx + dx;
            yy = by + dy;

            m = 0;
            n = (params->effort >= 2) ? FPEL_NSEARCH : (FPEL_NSEARCH / 2 + 1);
            for (k = 0; k < n; k++) {
                int evx, evy;

                score = fastsad(srcp.data, sp->stride,
                        DSV_GET_XY(rp, xx + xf[k], yy + yf[k]),
                        rp->stride, bw, bh);

                evx = (dx + xf[k]) * (1 << (2 + level));
                evy = (dy + yf[k]) * (1 << (2 + level));
                score += mv_cost(mf, params, i, j, evx, evy) << 4;
                if (best > score) {
                    best = score;
                    m = k;
                }
            }

            dx += xf[m];
            dy += yf[m];

            mv->u.mv.x = dx * (1 << level);
            mv->u.mv.y = dy * (1 << level);
            /* subpel refine at base level */
            if (level == 0) {
                int skip_subpel = 0;

                if (params->effort < 10) {
                    DSV_PLANE skip_refp;
                    dsv_plane_xy(ref, &skip_refp, 0, bx + mv->u.mv.x, by + mv->u.mv.y);
                    skip_subpel = fastsad(srcp.data, srcp.stride, skip_refp.data, skip_refp.stride, bw, bh) <= (bw * bh);
                }
                if (params->effort >= 4 && !skip_subpel) {
                    best = subpixel_ME(params, mf, mv, src, ref, i, j, best, bx, by, bw, bh);
                } else {
                    mv->u.mv.x *= 4;
                    mv->u.mv.y *= 4;
                }
                /* intra decision + block metric gathering */ {
                    DSV_PLANE srcp_l, zrefp, zrecp, mvrefp, mvrecp;
                    unsigned tex_src, tex_mvrec;
                    unsigned var_src, var_zref, var_mvrec;
                    unsigned avg_src, avg_zref, avg_mvrec, avg_zrec;
                    int uavg_src, vavg_src, uavg_zref, vavg_zref;
                    int cbx, cby, cbw, cbh, subsamp;
                    CHROMA_PSY cpsy;
                    unsigned avgdif, vardif;
                    unsigned ubest, mad, avg_c_dif;
                    unsigned var_t = 16, var_d;
                    int eprmi, eprmr;

                    xx = bx + ((bw >> 1) - (SP_SAD_SZ / 2));
                    yy = by + ((bh >> 1) - (SP_SAD_SZ / 2));
                    dsv_plane_xy(src, &srcp_l, 0, xx, yy);
                    dsv_plane_xy(ref, &zrefp, 0, bx, by);
                    dsv_plane_xy(ref, &mvrefp, 0, bx + DSV_SAR(mv->u.mv.x, 2), by + DSV_SAR(mv->u.mv.y, 2));
                    dsv_plane_xy(hme->recon, &zrecp, 0, bx, by);
                    dsv_plane_xy(hme->recon, &mvrecp, 0, bx + DSV_SAR(mv->u.mv.x, 2), by + DSV_SAR(mv->u.mv.y, 2));

                    ubest = best;
                    mad = DSV_UDIV_ROUND(ubest, (bw * bh));
                    /* gather metrics about the blocks.
                     * _src = source block
                     * _mvrec = reconstructed ref frame block at full-pel motion (x,y)
                     * _zref = ref frame at (0, 0) relative to cur block
                     */
                    tex_src = block_tex(srcp.data, srcp.stride, bw, bh);
                    tex_mvrec = block_tex(mvrecp.data, mvrecp.stride, bw, bh);
                    var_src = block_var(srcp.data, srcp.stride, bw, bh, &avg_src);
                    var_zref = block_var(zrefp.data, zrefp.stride, bw, bh, &avg_zref);
                    var_mvrec = block_var(mvrecp.data, mvrecp.stride, bw, bh, &avg_mvrec);

                    avgdif = abs((int) avg_src - (int) avg_zref);
                    vardif = abs((int) var_src - (int) var_zref);

                    subsamp = params->vidmeta->subsamp;
                    cbx = i * (y_w >> DSV_FORMAT_H_SHIFT(subsamp));
                    cby = j * (y_h >> DSV_FORMAT_V_SHIFT(subsamp));
                    cbw = bw >> DSV_FORMAT_H_SHIFT(subsamp);
                    cbh = bh >> DSV_FORMAT_V_SHIFT(subsamp);
                    c_average(sp, cbx, cby, cbw, cbh, &uavg_src, &vavg_src);
                    c_average(rp, cbx, cby, cbw, cbh, &uavg_zref, &vavg_zref);

                    avg_c_dif = (abs(uavg_src - uavg_zref) + abs(vavg_src - vavg_zref) + 1) / 2;
                    if (ubest > 0) {
                        num_eligible_blocks++;
                    }
                    chroma_analysis(&cpsy, avg_src, uavg_src, vavg_src);

                    if (cpsy.greenish || (!cpsy.hifreq) || cpsy.greyish || cpsy.skinnish) {
                        var_t += 16;
                    }
                    var_d = var_t / 4;

                    /* more difference, more likely to need a scene change */
                    ndiff += ((mad > 11) || (vardif > 10)) + (avg_c_dif / 32);

                    mv->eprm = 0;
                    mv->skip = 0;

                    avg_zrec = calc_EPRM(&srcp, &zrecp, &mvrecp, bw, bh, &eprmi, &eprmr);

                    if (hme->enc->skip_block_thresh >= 0) {
                        unsigned skipt = hme->enc->skip_block_thresh;
                        int mvfac = abs(mv->u.mv.x) + abs(mv->u.mv.y);
                        if ((mvfac == 0 && mad <= (skipt + 2)) || (mvfac == 1 && mad == 0)) {
                            mv->skip = MAX(avgdif, avg_c_dif) <= skipt && vardif <= skipt;

                            if (mv->skip) {
                                /* don't bother with anything else if we're skipping the block anyway */
                                goto force_inter;
                            }
                        }
                    }

                    if (is_inter_better(&srcp, &mvrecp, avg_zrec, bw, bh, mv, eprmi, eprmr)) {
                        if (mv->u.all == 0) {
                            goto inter;
                        }
                        if (MAX(var_src, tex_src) > 0) {
                            goto force_inter; /* intra looks better in flat regions so bias against it */
                        }
                    }
                    /* using gotos to make it a bit easier to read (for myself) */
#if 1 /* have intra blocks */
                    /* only bother checking low texture/variance blocks */
                    if (mv->u.all != 0 && (tex_src <= var_d || var_src <= var_d)) {
                        int vts, vtr, tol, spandif;
                        int span_src, span_mvrec;
                        int is_subpel = (mv->u.mv.x & 3) || (mv->u.mv.y & 3);

                        vts = MAX(var_src, tex_src);
                        vtr = (var_mvrec + tex_mvrec + 1) / 2; /* average rather than MAX seems to give better results */
                        tol = vts * vts;

                        if (vtr > tol || (vts > 0 && (vtr > 3 * vts / 2))) {
                            goto intra;
                        }
                        span_src = block_span(srcp.data, srcp.stride, bw, bh);
                        span_mvrec = block_span(mvrecp.data, mvrecp.stride, bw, bh);
                        spandif = (span_mvrec - span_src);
                        if (span_mvrec > (span_src * span_src)) {
                            goto intra;
                        }
                        tol = 4;
                        if (span_src / tol > 0 && (span_mvrec / tol > 3 * span_src / (tol * 2))) {
                            goto intra;
                        }
#if 1 /* chroma check */
                        /* harder to see color error in dark areas */
                        if (avg_src > LUMA_CHROMA_CUTOFF && spandif > 8 && !is_subpel) {
                            int greyish_rec, uavg_rec, vavg_rec;
                            int mvcbx, mvcby, ud, vd;
                            int uspan_src, vspan_src, uspan_rec, vspan_rec;
                            int sdu, sdv;

                            mvcbx = cbx + DSV_SAR(mv->u.mv.x, 2 + DSV_FORMAT_H_SHIFT(subsamp));
                            mvcby = cby + DSV_SAR(mv->u.mv.y, 2 + DSV_FORMAT_V_SHIFT(subsamp));

                            c_average(rcp, mvcbx, mvcby, cbw, cbh, &uavg_rec, &vavg_rec);
                            greyish_rec = abs(uavg_rec - 128) < 8 && abs(vavg_rec - 128) < 8;
                            if (cpsy.greyish != greyish_rec) {
                                goto intra;
                            }
                            ud = abs(uavg_src - uavg_rec);
                            vd = abs(vavg_src - vavg_rec);
                            if (MAX(ud, vd) > 6) {
                                goto intra;
                            }
                            c_span(sp, mvcbx, mvcby, cbw, cbh, &uspan_src, &vspan_src);
                            c_span(rcp, mvcbx, mvcby, cbw, cbh, &uspan_rec, &vspan_rec);
                            sdu = (uspan_rec - uspan_src);
                            sdv = (vspan_rec - vspan_src);
                            if (sdu > 64 || sdv > 64 || sdu > spandif || sdv > spandif) {
                                goto intra;
                            }
                        }
#endif
                    }
                    goto inter;
intra:
                    if (detail_subblocks(params, mv, &srcp, &zrecp, bw, bh, tex_src)) {
                        nintra++;
                    }
inter:
                    if (mv->mode != DSV_MODE_INTRA &&
                        cleanup_subblocks(params, mv,
                            mad, var_d,
                            var_src, tex_src, avg_src,
                            sp, rcp,
                            &srcp, &zrecp,
                            bw, bh,
                            cbx, cby,
                            cbw, cbh)) {
                        nintra++;
                    }
#endif
force_inter:
                    ;
                    if (mv->mode == DSV_MODE_INTRA) {
                        mv->eprm = eprmi;
                        if (mv->submask != DSV_MASK_ALL_INTRA) {
                            mv->eprm |= eprmr;
                        }
                    } else {
                        mv->eprm = eprmr;
                    }
                }
            }
        }
    }
    if (level == 0) {
        if (num_eligible_blocks == 0) {
            num_eligible_blocks = 1;
        }
        *scene_change_blocks = ndiff * 100 / num_eligible_blocks;
    }
    return nintra;
}

extern DSV_MV *
dsv_intra_analysis(DSV_FRAME *src, DSV_FRAME *small_frame, int scale, DSV_PARAMS *params)
{
    int i, j, y_w, y_h, nxb, nyb;
    DSV_MV *ba;

    y_w = params->blk_w;
    y_h = params->blk_h;

    nxb = params->nblocks_h;
    nyb = params->nblocks_v;

    ba = dsv_alloc(nxb * nyb * sizeof(DSV_MV));

    for (j = 0; j < nyb; j++) {
        for (i = 0; i < nxb; i++) {
            DSV_PLANE srcp;
            int bx, by, bw, bh;
            DSV_MV *mv;
            int luma_var, luma_tex;
            unsigned luma_avg;
            int cbx, cby, cbw, cbh, subsamp;
            CHROMA_PSY cpsy;
            int frame_lum;
            int smx, smy;
            int rel_luma;
            int maintain = 1;
            int keep_hf = 1;
            int var_t;

            bx = (i * y_w);
            by = (j * y_h);

            mv = &ba[i + j * nxb];

            if ((bx >= src->width) || (by >= src->height)) {
                mv->skip = 0;
                mv->maintain = 0;
                mv->use_ringing = 0;
                continue;
            }
            dsv_plane_xy(src, &srcp, 0, bx, by);

            bw = MIN(srcp.w, y_w);
            bh = MIN(srcp.h, y_h);

            subsamp = params->vidmeta->subsamp;
            cbx = i * (y_w >> DSV_FORMAT_H_SHIFT(subsamp));
            cby = j * (y_h >> DSV_FORMAT_V_SHIFT(subsamp));
            cbw = bw >> DSV_FORMAT_H_SHIFT(subsamp);
            cbh = bh >> DSV_FORMAT_V_SHIFT(subsamp);

            luma_var = block_var(srcp.data, srcp.stride, bw, bh, &luma_avg);
            luma_tex = block_tex(srcp.data, srcp.stride, bw, bh);

            if (params->do_psy) {
                int foliage, skip_tones, noisy;
                int uavg, vavg;

                c_average(src->planes, cbx, cby, cbw, cbh, &uavg, &vavg);
                chroma_analysis(&cpsy, luma_avg, uavg, vavg);
                /* guess at what foliage is */
                foliage = (cpsy.greenish && luma_avg < 160) &&
                        abs(luma_var - luma_tex) <= (luma_var / 4);
                /* 1. avoid giving more bits to high freq colors since we are
                 *    less sensitive to the spatial frequencies of them anyway.
                 * 2. avoid introducing ringing artifacts on skin tones since
                 *    we will likey notice the distortion more.
                 */
                skip_tones = cpsy.hifreq || cpsy.skinnish;
                /* not foliage but sufficiently noisy with a noticeable magnitude */
                noisy = (luma_var > 4 && luma_var == luma_tex);
                mv->use_ringing = !skip_tones && (foliage || (!foliage && noisy));
            } else {
                mv->use_ringing = 0;
            }
            /* attenuate by relative brightness */
            smx = DSV_ROUND_SHIFT(bx, scale);
            smy = DSV_ROUND_SHIFT(by, scale);
            smx = CLAMP(smx, 0, small_frame->planes[0].w - 1);
            smy = CLAMP(smy, 0, small_frame->planes[0].h - 1);
            frame_lum = *DSV_GET_XY(&small_frame->planes[0], smx, smy);

            rel_luma = (abs((int) luma_avg - frame_lum) / 8);

            if (params->do_psy) {
                var_t = 8;
                if (cpsy.greenish || cpsy.greyish || cpsy.skinnish) {
                    var_t += 12;
                } else if (!cpsy.hifreq) {
                    var_t += 6;
                }
                var_t -= rel_luma;
            } else {
                var_t = 16;
            }
            maintain = (luma_var < var_t) && (luma_tex < var_t);
            if (params->do_psy) {
                if (!cpsy.hifreq) {
                    keep_hf = 0;
                    maintain |= block_has_vis_edge(srcp.data, srcp.stride, bw, bh);
                    /* med/low texture dominant blocks are visually important */
                    if (luma_tex > luma_var && luma_tex < 32) {
                        keep_hf = 1;
                    }
                    if (!keep_hf) {
                        int luma_span;

                        luma_span = block_span(srcp.data, srcp.stride, bw, bh);
                        /* large span + med/low texture = likely important detail */
                        if (luma_span >= 92 && luma_tex < 24) {
                            keep_hf = 1;
                        }
                    }
                } else {
                    maintain = 0;
                    keep_hf = 0;
                }
            }
            mv->maintain = maintain;
            mv->skip = keep_hf; /* reusing the skip flag */
        }
    }

    return ba;
}

extern int
dsv_hme(DSV_HME *hme, int *scene_change_blocks)
{
    int i = hme->enc->pyramid_levels;
    int nintra = 0;

    while (i >= 0) {
        nintra = refine_level(hme, i, scene_change_blocks);
        i--;
    }
    return (nintra * 100) / (hme->params->nblocks_h * hme->params->nblocks_v);
}

