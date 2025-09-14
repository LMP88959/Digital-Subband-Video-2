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

#include "dsv_encoder.h"

/* Hierarchical Motion Estimation */

/* 1 = disable sending luma residuals,
 * 2 = disable sending luma+chroma residuals
 */
#define NO_RESIDUALS 0

#define DO_GOOD_ENOUGH 1

#define AVG2(a, b) (((a) + (b) + 1) >> 1)
#define AVG4(a, b, c, d) (((a) + (b) + (c) + (d) + 2) >> 2)
#define UAVG4(a, b, c, d) ((unsigned) ((a) + (b) + (c) + (d) + 2) >> 2)
#define MV_LT(v, t) (abs((v)->u.mv.x) < (t) && abs((v)->u.mv.y) < (t)) /* less than */
#define MV_GT(v, t) (abs((v)->u.mv.x) > (t) && abs((v)->u.mv.y) > (t)) /* greater than */
#define MV_MAG(v) AVG2(abs((v)->u.mv.x), abs((v)->u.mv.y)) /* not actual mag */
#define MV_CMP(v, vx, vy, t) (abs((v)->u.mv.x - (int) (vx)) < (t) && abs((v)->u.mv.y - (int) (vy)) < (t))
#define MK_MV_COMP(fp, hp, qp) ((fp) * 4 + (hp) * 2 + (qp))
#define SQR(x) ((x) * (x))

/* qpel to fpel */
#define QP2FP(fpel, qpel)                             \
    do {                                              \
        (fpel)->u.mv.x = DSV_SAR_R((qpel)->u.mv.x, 2);\
        (fpel)->u.mv.y = DSV_SAR_R((qpel)->u.mv.y, 2);\
    } while (0)

#define ABSDIF(a, b) abs((int) (a) - (int) (b))

#define N_SEARCH_PTS 9
/* READ-ONLY */
static int rectx[N_SEARCH_PTS] = { 0, 1, -1, 0,  0, -1,  1, -1, 1 };
static int recty[N_SEARCH_PTS] = { 0, 0,  0, 1, -1, -1, -1,  1, 1 };

static uint8_t
clamp_u8(int v)
{
    return v > 255 ? 255 : v < 0 ? 0 : v;
}

typedef struct {
    /* green/brown in color, our eyes are more sensitive to greener colors */
    int nature;
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
    c->nature = u < 128 && v < 160;
    c->greyish = abs(u - 128) < 8 && abs(v - 128) < 8;
    c->skinnish = (y > 80) && (y < 230) &&
                      abs(u - 108) < 24 &&
                      abs(v - 148) < 24; /* terrible approximation */
    c->hifreq = (u > 160) && !c->greyish && !c->skinnish;
}

#define SP_SAD_SZ DSV_MIN_BLOCK_SIZE /* sub-pixel SAD size */

#define SP_DIM    (SP_SAD_SZ + 1)
#define HP_DIM    (SP_DIM * 2)

/* conversion from full-pel coords to quarter pel */
#define QP_OFFSET(fpx, fpy) ((4 * (fpx) + (4 * (fpy)) * QP_STRIDE))

#define HP_STRIDE (SP_DIM * 2)
#define QP_STRIDE (SP_DIM * 4)

#define METRIC_NORM 7 /* sum of 2^weights should equal this */
typedef struct {
    int err_weight;
    int tex_weight;
    int avg_weight; /* 2x2 DC similarity is more important at higher quants */
} PSY_COEFS;

#define METRIC_RETURN(a, w, h) (iisqrt(a)*(w)*(h)/AVG2(w,h))

static unsigned
iisqrt(unsigned n)
{
    unsigned pos, res, rem;

    if (n == 0) {
        return 0;
    }
    res = 0;
    pos = 1 << 30;
    rem = n;

    while (pos > rem) {
        pos >>= 2;
    }
    while (pos) {
        unsigned dif = res + pos;
        res >>= 1;
        if (rem >= dif) {
            rem -= dif;
            res += pos;
        }
        pos >>= 2;
    }
    return res;
}

#define METR_CALC(acc) {                                                    \
        int ta, tb, se;/* texture in block A, ~ block B, squared error */   \
        se = UAVG4(abs(a1 - b1), abs(a2 - b2), abs(a3 - b3), abs(a4 - b4)); \
        ta = UAVG4(abs(a1 - a2), abs(a2 - a3), abs(a3 - a4), abs(a4 - a1)); \
        tb = UAVG4(abs(b1 - b2), abs(b2 - b3), abs(b3 - b4), abs(b4 - b1)); \
        (acc) += SQR(se) << psy->err_weight;                                \
        (acc) += SQR(ta - tb) << psy->tex_weight;                           \
        (acc) += SQR(s0 - s1) << psy->avg_weight;                           \
}

#define METR_BODY(w, h)                                        \
        int i, j;                                              \
        unsigned acc = 0;                                      \
        for (j = 0; j < h / 2; j++) {                          \
            int bp = 0;                                        \
            for (i = 0; i < w / 2; i++) {                      \
                int a1, a2, a3, a4, b1, b2, b3, b4, s0, s1;    \
                a1 = a[bp];                                    \
                a2 = a[bp + 1];                                \
                a3 = a[bp + as];                               \
                a4 = a[bp + 1 + as];                           \
                s0 = UAVG4(a1, a2, a3, a4);                    \
                b1 = b[bp];                                    \
                b2 = b[bp + 1];                                \
                b3 = b[bp + bs];                               \
                b4 = b[bp + 1 + bs];                           \
                s1 = UAVG4(b1, b2, b3, b4);                    \
                bp += 2;                                       \
                METR_CALC(acc);                                \
            }                                                  \
            a += 2 * as;                                       \
            b += 2 * bs;                                       \
        }                                                      \

#define MAKE_METR(w)                                           \
static unsigned                                                \
metr_ ##w## xh(uint8_t *a, int as, uint8_t *b, int bs, int h, PSY_COEFS *psy)  \
{                                                              \
    METR_BODY(w, h);                                           \
    return METRIC_RETURN(acc, w, h);                           \
}

#define MAKE_METRWH(w, h)                                      \
static unsigned                                                \
metr_ ##w## x ##h## x (uint8_t *a, int as, uint8_t *b, int bs, PSY_COEFS *psy) \
{                                                              \
    METR_BODY(w, h);                                           \
    return METRIC_RETURN(acc, w, h);                           \
}

MAKE_METR(8)
MAKE_METR(16)
MAKE_METR(32)

MAKE_METRWH(8, 8)
MAKE_METRWH(16, 16)
MAKE_METRWH(32, 32)

static unsigned
metr_wxh(uint8_t *a, int as, uint8_t *b, int bs, int w, int h, PSY_COEFS *psy)
{
    METR_BODY(w, h);
    return METRIC_RETURN(acc, w, h);
}

static unsigned
umetr_wxh(uint8_t *a, int as, uint8_t *b, int bs, int w, int h, PSY_COEFS *psy)
{
    METR_BODY(w, h);
    return acc;
}

#define SSE_BODY(w, h)                                        \
        int i, j;                                             \
        unsigned acc = 0;                                     \
        if (w == 0 || h == 0) {                               \
            return INT_MAX;                                   \
        }                                                     \
        for (j = 0; j < h; j++) {                             \
            for (i = 0; i < w; i++) {                         \
                int dif = (a[i] - b[i]);                      \
                acc += dif * dif;                             \
            }                                                 \
            a += as;                                          \
            b += bs;                                          \
        }                                                     \

#define MAKE_SSE(w)                                           \
static unsigned                                               \
sse_ ##w## xh(uint8_t *a, int as, uint8_t *b, int bs, int h)  \
{                                                             \
    SSE_BODY(w, h);                                           \
    return acc;                                               \
}

#define MAKE_SSEWH(w, h)                                      \
static unsigned                                               \
sse_ ##w## x ##h## x (uint8_t *a, int as, uint8_t *b, int bs) \
{                                                             \
    SSE_BODY(w, h);                                           \
    return acc;                                               \
}

MAKE_SSE(8)
MAKE_SSE(16)
MAKE_SSE(32)

MAKE_SSEWH(8, 8)
MAKE_SSEWH(16, 16)
MAKE_SSEWH(32, 32)

static unsigned
sse_wxh(uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    SSE_BODY(w, h);
    return acc;
}

static unsigned
qpsad(uint8_t *a, int as, uint8_t *b, PSY_COEFS *psy)
{
    int i, j;
    unsigned acc = 0;
    for (j = 0; j < SP_SAD_SZ / 2; j++) {
        int ap = 0;
        for (i = 0; i < SP_SAD_SZ / 2; i++) {
            int a1, a2, a3, a4, b1, b2, b3, b4, s0, s1;
            a1 = a[ap];
            a2 = a[ap + 1];
            a3 = a[ap + as];
            a4 = a[ap + 1 + as];
            s0 = UAVG4(a1, a2, a3, a4);
            b1 = b[QP_OFFSET(i * 2, j * 2)];
            b2 = b[QP_OFFSET(i * 2 + 1, j * 2)];
            b3 = b[QP_OFFSET(i * 2, j * 2 + 1)];
            b4 = b[QP_OFFSET(i * 2 + 1, j * 2 + 1)];
            s1 = UAVG4(b1, b2, b3, b4);
            ap += 2;
            METR_CALC(acc);
        }
        a += 2 * as;
    }
    return METRIC_RETURN(acc, SP_SAD_SZ, SP_SAD_SZ);
}

static unsigned
fastmetr(uint8_t *a, int as, uint8_t *b, int bs, int w, int h, PSY_COEFS *psy)
{
    if (w == 0 || h == 0) {
        return INT_MAX;
    }
    switch (w) {
        case 8:
            switch (h) {
                case 8:
                    return metr_8x8x(a, as, b, bs, psy);
                default:
                    return metr_8xh(a, as, b, bs, h, psy);
            }
            break;
        case 16:
            switch (h) {
                case 16:
                    return metr_16x16x(a, as, b, bs, psy);
                default:
                    return metr_16xh(a, as, b, bs, h, psy);
            }
            break;
        case 32:
            switch (h) {
                case 32:
                    return metr_32x32x(a, as, b, bs, psy);
                default:
                    return metr_32xh(a, as, b, bs, h, psy);
            }
            break;
        default:
            break;
    }
    return metr_wxh(a, as, b, bs, w, h, psy);
}

static unsigned
fastsse(uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    switch (w) {
        case 8:
            switch (h) {
                case 8:
                    return sse_8x8x(a, as, b, bs);
                default:
                    return sse_8xh(a, as, b, bs, h);
            }
            break;
        case 16:
            switch (h) {
                case 16:
                    return sse_16x16x(a, as, b, bs);
                default:
                    return sse_16xh(a, as, b, bs, h);
            }
            break;
        case 32:
            switch (h) {
                case 32:
                    return sse_32x32x(a, as, b, bs);
                default:
                    return sse_32xh(a, as, b, bs, h);
            }
            break;
        default:
            break;
    }
    return sse_wxh(a, as, b, bs, w, h);
}

/* corresponds to switch statement in hier_metr */
#define SQUARED_LEVELS (level > 1)
static unsigned
hier_metr(int level, uint8_t *a, int as, uint8_t *b, int bs, int w, int h, PSY_COEFS *psy)
{
    /* change metric depending on level in hierarchy */
    if (SQUARED_LEVELS) {
        return fastsse(a, as, b, bs, w, h);
    }
    return fastmetr(a, as, b, bs, w, h, psy);
}

static int
mv_cost(DSV_MV *vecs, DSV_PARAMS *p, int i, int j, int mx, int my, int q, int level)
{
    int sqr, cost;

    sqr = SQUARED_LEVELS;
    cost = dsv_mv_cost(vecs, p, i, j, mx, my, q, sqr);
    cost = MIN(cost, 1 << 19);
    if (sqr) {
        return cost * (q * q >> DSV_MAX_QP_BITS) >> (DSV_MAX_QP_BITS - 2);
    }
    return 3 * cost * q >> DSV_MAX_QP_BITS;
}

/* full-pel error of luma and chroma */
static void
yuv_max_subblock_err(unsigned max_err[3], DSV_FRAME *src, DSV_FRAME *ref,
        int bx, int by, int brx, int bry, int bw, int bh,
        int cbx, int cby, int cbrx, int cbry, int cbw, int cbh, PSY_COEFS *psy)
{
    int f, g, z;
    DSV_PLANE *sp, *rp;

    sp = src->planes;
    rp = ref->planes;
    bw /= 2;
    bh /= 2;
    cbw /= 2;
    cbh /= 2;
    for (z = 0; z < 3; z++) {
        unsigned sub[4];
        int subpos = 0;

        memset(sub, 0, sizeof(sub));
        for (g = 0; g <= bh; g += (bh + !bh)) {
            for (f = 0; f <= bw; f += (bw + !bw)) {
                uint8_t *src_d, *ref_d;

                src_d = DSV_GET_XY(&sp[z], bx, by) + (f + g * sp[z].stride);
                ref_d = DSV_GET_XY(&rp[z], brx, bry) + (f + g * rp[z].stride);
                sub[subpos] = umetr_wxh(
                        src_d, sp[z].stride,
                        ref_d, rp[z].stride, bw, bh, psy);
                subpos++;
            }
        }
        /* planes 1,2 are chroma */
        bx = cbx;
        by = cby;
        brx = cbrx;
        bry = cbry;
        bw = cbw;
        bh = cbh;
        max_err[z] = MAX(MAX(sub[0], sub[1]), MAX(sub[2], sub[3]));
    }
}

static int
outofbounds(int i, int j, int nxb, int nyb, int y_w, int y_h, DSV_MV *mv)
{
    int dx = mv->u.mv.x;
    int dy = mv->u.mv.y;
    int px, py, limx, limy;

    limx = ((nxb - 1) * y_w) - 1;
    limy = ((nyb - 1) * y_h) - 1;
    px = i * y_w + DSV_SAR(dx, 2);
    py = j * y_h + DSV_SAR(dy, 2);

    return (px < 0 || py < 0 || px >= limx || py >= limy);
}

static int
invalid_block(DSV_FRAME *f, int bx, int by, int bw, int bh)
{
    int b = f->border * DSV_FRAME_BORDER;
    return bx < -b ||
           by < -b ||
           bx + bw >= (f->width + b) ||
           by + bh >= (f->height + b);
}

static int
block_avg(uint8_t *a, int as, int w, int h)
{
    int i, j;
    int avg = 0;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            avg += a[i];
        }
        a += as;
    }
    return avg / (w * h);
}

/* determine if this block needs EPRM */
static void
calc_EPRM(DSV_PLANE *sp, DSV_PLANE *mvrp, int avg_src, int avg_ref,
          int w, int h, int *eprmi, int *eprmd, int *eprmr)
{
    int i, j;
    int clipi = 0;
    int clipd = 0;
    int clipr = 0;
    uint8_t *mvr = mvrp->data;
    uint8_t *src = sp->data;

    *eprmi = 0;
    *eprmr = 0;
    avg_src -= 128;
    avg_ref -= 128;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            /* see if MV pred or intra pred would clip and require EPRM */
            if (!clipr) {
                clipr = ((src[i] - mvr[i]) + 128) & ~0xff;
            }
            if (!clipi) {
                clipi = (src[i] - avg_ref) & ~0xff;
            }
            if (!clipd) {
                clipd = (src[i] - avg_src) & ~0xff;
            }
            if (clipi && clipd && clipr) {
                goto eprm_done;
            }
        }
        src += sp->stride;
        mvr += mvrp->stride;
    }
eprm_done:
    *eprmi = !!clipi;
    *eprmd = !!clipd;
    *eprmr = !!clipr;
}

static unsigned
block_tex(uint8_t *a, int as, int w, int h)
{
    int i, j;
    int px;
    unsigned sh = 0;
    unsigned sv = 0;
    uint8_t *ptr;
    uint8_t *prevptr;

    ptr = a;
    j = h;
    prevptr = ptr;
    while (j-- > 0) {
        sv += abs(ptr[0] - prevptr[0]);
        for (i = 1; i < w; i++) {
            px = ptr[i];
            sh += abs(px - ptr[i - 1]);
            sv += abs(px - prevptr[i]);
        }
        prevptr = ptr;
        ptr += as;
    }
    return MAX(sh, sv);
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
        for (i = 0; i < w; i++) {
            s += ptr[i];
        }
        ptr += as;
    }
    s /= (w * h);
    *avg = s;
    ptr = a;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            var += abs(ptr[i] - s);
        }
        ptr += as;
    }
    return var;
}

/* combination of var and texture */
static int
block_detail(uint8_t *a, int as, int w, int h, unsigned *avg)
{
    int i, j;
    int s = 0, var = 0, tex;
    uint8_t *ptr;
    int px;
    unsigned sh = 0;
    unsigned sv = 0;
    uint8_t *prevptr;

    ptr = a;
    j = h;
    prevptr = ptr;
    while (j-- > 0) {
        sv += abs(ptr[0] - prevptr[0]);
        s += ptr[0];
        for (i = 1; i < w; i++) {
            px = ptr[i];
            sh += abs(px - ptr[i - 1]);
            sv += abs(px - prevptr[i]);
            s += px;
        }
        prevptr = ptr;
        ptr += as;
    }
    s /= (w * h);
    *avg = s;
    ptr = a;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            var += abs(ptr[i] - s);
        }
        ptr += as;
    }
    var >>= 1;
    tex = MAX(sh, sv) - var;
    return var + MAX(tex, 0);
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
    return iisqrt(MAX(sh, sv)) / AVG2(w, h);
}

/* number of most significant bits of an 8-bit
 * luma sample to include in the histogram */
#define HISTBITS 4
#define NHIST (1 << HISTBITS)

static unsigned
block_peaks(uint8_t *a, int as, int w, int h,
            uint8_t peaks[NHIST], uint16_t hist[NHIST], int bavg)
{
    int x, y;
    int maxv = 0;
    int npeaks = 0;
    int quant16;
    uint8_t *sp = a;
    int avg = bavg;

    memset(hist, 0, sizeof(uint16_t) * NHIST);
    if (bavg < 0) {
        avg = 0;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                avg += a[x + y * as];
            }
        }
        avg /= (w * h);
    }
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
            ds = UAVG4(p1, p2, p3, p4);
            bp += 2;
            hi = ds * quant16 >> 16;
            hist[MIN(hi, (NHIST - 1))]++;
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
block_hist_var(uint8_t *a, int as, int w, int h, uint16_t hist[NHIST])
{
    int x, y;
    unsigned var, quant16, avg;
    uint8_t *sp = a;

    memset(hist, 0, sizeof(uint16_t) * NHIST);
    avg = 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            avg += a[x];
        }
        a += as;
    }
    avg /= (w * h);
    if (avg == 0) {
        avg = 1;
    }
    quant16 = ((1 << (HISTBITS - 1)) << 16) / avg;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int hi = sp[x] * quant16 >> 16;
            hist[CLAMP(hi, 0, (NHIST - 1))]++;
        }
        sp += as;
    }
    avg = 0;
    for (x = 0; x < NHIST; x++) {
        avg += hist[x];
    }
    avg /= NHIST;
    var = 0;
    for (x = 0; x < NHIST; x++) {
        var += (hist[x] - avg) * (hist[x] - avg);
    }

    return (var * 16 * 16) / (NHIST * w * h * w * h);
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
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
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
hpfh(uint8_t *p)
{
    return DSV_HPF_ME(p[-1], p[0], p[1], p[2]);
}

static int
hpfv(uint8_t *p, int s)
{
    return DSV_HPF_ME(p[-s], p[0], p[s], p[2 * s]);
}

static void
hpel(uint8_t *dec, uint8_t *ref, int rs)
{
    static int16_t buf[(DSV_MAX_BLOCK_SIZE + 3) * DSV_MAX_BLOCK_SIZE];
    uint8_t *drow;
    int i, j, c, x;

    for (j = 0; j < SP_DIM + 3; j++) {
        drow = ref + (j - 1) * rs;
        for (i = 0; i < SP_DIM; i++) {
            buf[i + j * SP_DIM] = hpfh(drow + i);
        }
    }
    for (j = 0; j < SP_DIM; j++) {
        drow = dec;
        for (i = 0; i < SP_DIM; i++) {
            x = i + j * SP_DIM;
            drow[HP_STRIDE] = clamp_u8((hpfv(ref + i, rs) + DSV_ME_HP_ADD) >> DSV_ME_HP_SHF);
            *drow++ = ref[i];
            c = DSV_HPF_ME(buf[x + 0 * SP_DIM], buf[x + 1 * SP_DIM], buf[x + 2 * SP_DIM], buf[x + 3 * SP_DIM]);
            drow[HP_STRIDE] = clamp_u8((c + (1 << (DSV_ME_HP_SHF + DSV_ME_HP_SHF - 1))) >> (DSV_ME_HP_SHF + DSV_ME_HP_SHF));
            *drow++ = clamp_u8((hpfh(ref + i) + DSV_ME_HP_ADD) >> DSV_ME_HP_SHF);
        }
        ref += rs;
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
            drow[QP_STRIDE] = AVG2(ref[i], ry[i]);
            *drow++ = ref[i];
            drow[QP_STRIDE] = AVG4(ref[i], rx[i], ry[i], rxy[i]);
            *drow++ = AVG2(ref[i], rx[i]);
        }
        ref += HP_STRIDE;
        rx += HP_STRIDE;
        ry += HP_STRIDE;
        rxy += HP_STRIDE;
        dec += 2 * QP_STRIDE; /* skip a row */
    }
}

static void
err_intra(uint8_t *a, int as, uint8_t *b, int bs,
          int avg_sb, int avg_src, int w, int h,
          unsigned *intra_err, unsigned *intrasrc_err, unsigned *inter_err,
          PSY_COEFS *psy, int ratio)
{
    int i, j;
    unsigned intra_sb = 0, intra_src = 0, inter = 0;
    for (j = 0; j < h / 2; j++) {
        int bp = 0;
        for (i = 0; i < w / 2; i++) {
            int a1, a2, a3, a4, b1, b2, b3, b4, s0, s1;
            int ta, tb, ae;
            a1 = a[bp];
            a2 = a[bp + 1];
            a3 = a[bp + as];
            a4 = a[bp + 1 + as];
            s0 = UAVG4(a1, a2, a3, a4);
            b1 = b[bp];
            b2 = b[bp + 1];
            b3 = b[bp + bs];
            b4 = b[bp + 1 + bs];
            s1 = UAVG4(b1, b2, b3, b4);
            bp += 2;

            ae = UAVG4(abs(a1 - b1), abs(a2 - b2), abs(a3 - b3), abs(a4 - b4));
            ta = UAVG4(abs(a1 - a2), abs(a2 - a3), abs(a3 - a4), abs(a4 - a1));
            tb = UAVG4(abs(b1 - b2), abs(b2 - b3), abs(b3 - b4), abs(b4 - b1));
            inter += SQR(ae) * ratio >> (5 - psy->err_weight);
            inter += SQR(ta - tb) << psy->tex_weight;
            inter += SQR(s0 - s1) << psy->avg_weight;

            ae = UAVG4(abs(a1 - avg_sb), abs(a2 - avg_sb), abs(a3 - avg_sb), abs(a4 - avg_sb));
            ta = UAVG4(abs(a1 - a2), abs(a2 - a3), abs(a3 - a4), abs(a4 - a1));
            intra_sb += SQR(ae) << psy->err_weight;
            intra_sb += SQR(ta) << psy->tex_weight;
            intra_sb += SQR(s0 - avg_sb) << (psy->avg_weight + 1);

            ae = UAVG4(abs(a1 - avg_src), abs(a2 - avg_src), abs(a3 - avg_src), abs(a4 - avg_src));
            ta = UAVG4(abs(a1 - a2), abs(a2 - a3), abs(a3 - a4), abs(a4 - a1));
            intra_src += SQR(ae) << psy->err_weight;
            intra_src += SQR(ta) << psy->tex_weight;
            intra_src += SQR(s0 - avg_src) << (psy->avg_weight + 1);
        }
        a += 2 * as;
        b += 2 * bs;
    }
    *intra_err = intra_sb;
    *intrasrc_err = intra_src;
    *inter_err = inter * ratio >> 5;
}

static void
test_subblock_intra_y(DSV_PARAMS *params, DSV_MV *refmv, DSV_MV *mv,
        DSV_PLANE *srcp, DSV_PLANE *refp,
        int detail_src, int avg_src, int neidif, unsigned ratio,
        int bw, int bh)
{
    int f, g, sbw, sbh, bit_index, nsub = 0, psyscale;
    unsigned avg_tot = 0, err_sub = 0, err_src = 0;
    PSY_COEFS psy;
    uint8_t bits[4] = {
            DSV_MASK_INTRA00,
            DSV_MASK_INTRA01,
            DSV_MASK_INTRA10,
            DSV_MASK_INTRA11,
    };
    if (refmv == NULL) {
        refmv = mv;
    }
    if (mv->u.all && neidif < 3 && MV_CMP(refmv, mv->u.mv.x, mv->u.mv.y, 3)) {
        return;
    }
    sbw = bw / 2;
    sbh = bh / 2;
    if (sbw == 0 || sbh == 0) {
        return;
    }
    psy.avg_weight = 2;
    psy.err_weight = 0;
    psy.tex_weight = 1;
    psyscale = dsv_spatial_psy_factor(params, -1);
    bit_index = 0;
    /* increase detail bias proportionally to how similar the MV was to its neighbors */
    detail_src += detail_src / MAX(neidif, 1);
    for (g = 0; g <= sbh; g += (sbh + !sbh)) {
        for (f = 0; f <= sbw; f += (sbw + !sbw)) {
            uint8_t *src_d, *mvr_d;
            unsigned avg_local, avg_sub, local_detail, dcd;
            unsigned sub_pred_err, src_pred_err, intererr;
            int dc, lo, hi, lerp, sub_better, src_better;

            src_d = srcp->data + (f + g * srcp->stride);
            mvr_d = refp->data + (f + g * refp->stride);
            if (mv->submask & bits[bit_index]) {
                /* already marked as intra, don't bother doing another check */
                goto next;
            }
            avg_sub = block_avg(mvr_d, refp->stride, sbw, sbh);

            local_detail = block_detail(src_d, srcp->stride, sbw, sbh, &avg_local);
            dcd = ABSDIF(avg_local, avg_sub) + 2;
            if (local_detail > (unsigned) (SQR(dcd) * bw * bh * ratio >> 5)) {
                goto next;
            }
            dc = (avg_local + avg_src * 3 + 2) >> 2;
            err_intra(
                    src_d, srcp->stride,
                    mvr_d, refp->stride,
                    avg_sub, dc, sbw, sbh,
                    &sub_pred_err, &src_pred_err, &intererr, &psy, ratio);
            lo = AVG2(detail_src, local_detail);
            hi = detail_src;
            /* scale importance of subblock (local) detail vs whole block detail based on video size */
            lerp = (lo * (32 - psyscale) + hi * psyscale) >> 5; /* interpolate */
            local_detail = MAX(lerp, lo);

            /* add block detail to expression to bias intra decision towards less detailed blocks */
            sub_better = (sub_pred_err + local_detail) < intererr;
            src_better = (src_pred_err + local_detail) < intererr;
            /* compare ref subblock predicted intra to src subblock predicted intra */
            if (sub_better || src_better) {
                mv->submask |= bits[bit_index];
                err_src += src_pred_err;
                err_sub += sub_pred_err;
                if (sub_pred_err < src_pred_err) {
                    avg_tot += avg_sub;
                } else {
                    avg_tot += dc;
                }
                nsub++;
                /* lower the threshold for subsequent subblocks since we're already going to be intra-ing the block anyway */
                detail_src = detail_src * 4 / 5;
            }
next:
            bit_index++;
        }
    }
    if (mv->submask) {
        DSV_MV_SET_INTRA(mv, 1);
        if (err_src < err_sub) {
            mv->dc = (avg_tot / nsub) | DSV_SRC_DC_PRED;
        } else {
            mv->dc = 0;
        }
    }
}

static void
test_subblock_intra_c(
        DSV_PARAMS *params, DSV_MV *mv, DSV_PLANE *sp, DSV_PLANE *rp,
        unsigned mad, unsigned detail_src, unsigned avg_src,
        int cbx, int cby, int cbmx, int cbmy, int cbw, int cbh)
{
    int f, g, sbw, sbh, bit_index, already_intra;
    unsigned avg_ramp, thr;
    uint8_t bits[4] = {
            DSV_MASK_INTRA00,
            DSV_MASK_INTRA01,
            DSV_MASK_INTRA10,
            DSV_MASK_INTRA11,
    };

    if (params->effort < 6) {
        return;
    }

    sbw = cbw / 2;
    sbh = cbh / 2;
    already_intra = DSV_MV_IS_INTRA(mv);
    if (!already_intra) {
        thr = SQR(detail_src);
    } else {
        thr = detail_src;
    }
    if (sbw == 0 || sbh == 0 || mad <= thr || thr > 32 || MV_LT(mv, 4)) {
        return;
    }
    avg_ramp = avg_src * avg_src >> 8;
    bit_index = 0;
    for (g = 0; g <= sbh; g += (sbh + !sbh)) {
        for (f = 0; f <= sbw; f += (sbw + !sbw)) {
            int uavg_src, vavg_src, uavg_mvr, vavg_mvr, erru, errv;
            unsigned dif;

            if (mv->submask & bits[bit_index]) {
                /* already marked as intra, don't bother doing another check */
                goto next;
            }
            c_average(sp, cbx + f, cby + g, sbw, sbh, &uavg_src, &vavg_src);
            c_average(rp, cbmx + f, cbmy + g, sbw, sbh, &uavg_mvr, &vavg_mvr);
            if (!already_intra) {
                erru = abs(uavg_src - uavg_mvr);
                errv = abs(vavg_src - vavg_mvr);
            } else {
                erru = SQR(uavg_src - uavg_mvr);
                errv = SQR(vavg_src - vavg_mvr);
            }
            /* scale error by squared average luma since I believe chroma error
             * is less noticeable in darker areas (just my observation) */
            dif = MAX(erru, errv) * avg_ramp >> 8;

            if (dif > thr) {
                /* set as intra */
                mv->submask |= bits[bit_index];
            }
next:
            bit_index++;
        }
    }
    if (mv->submask) {
        DSV_MV_SET_INTRA(mv, 1);
    }
}

static unsigned
subpixel_ME(
        DSV_PARAMS *params,
        DSV_MV *mvf, /* motion vector field */
        DSV_MV *mv, /* current full-pixel motion vector */
        DSV_FRAME *src, DSV_FRAME *ref,
        int i, int j,
        unsigned best, int quant,
        int bx, int by, int bw, int bh, PSY_COEFS *psy, int *found)
{
    static uint8_t tmph[(2 + HP_STRIDE) * (2 + HP_STRIDE)];
    static uint8_t tmpq[(4 + QP_STRIDE) * (4 + QP_STRIDE)];
    static int dx[4] = { 1, -1, 0,  0 };
    static int dy[4] = { 0,  0, 1, -1 };
    DSV_PLANE srcp, refp, srcsp, refsp;
    int xx, yy, n;
    unsigned score, quad[4];
    uint8_t *imq;
    unsigned yarea, ms1, ms2;
    int pri[2], sec[2], diag[2], bestv[2];
    int *testv[3];
    int area_ratio, iarea_ratio;

    if (best == 0) {
        mv->u.mv.x = MK_MV_COMP(mv->u.mv.x, 0, 0);
        mv->u.mv.y = MK_MV_COMP(mv->u.mv.y, 0, 0);
        *found = 1;
        return best;
    }

    testv[0] = pri;
    testv[1] = sec;
    testv[2] = diag;
    yarea = bw * bh;
    dsv_plane_xy(src, &srcp, 0, bx, by);
    dsv_plane_xy(ref, &refp, 0, bx + mv->u.mv.x, by + mv->u.mv.y);

    for (n = 0; n < 4; n++) {
        quad[n] = sse_wxh(srcp.data, srcp.stride,
                DSV_GET_XY(&refp, dx[n], dy[n]), refp.stride, bw, bh);
    }

    /* scale down to match area */
    area_ratio = 8 * (SP_SAD_SZ * SP_SAD_SZ) / yarea;
    iarea_ratio = 8 * yarea / (SP_SAD_SZ * SP_SAD_SZ);

    best = best * area_ratio >> 3;
    xx = bx + ((bw >> 1) - ((SP_SAD_SZ + 1) / 2));
    yy = by + ((bh >> 1) - ((SP_SAD_SZ + 1) / 2));
    dsv_plane_xy(src, &srcsp, 0, xx, yy);
    dsv_plane_xy(ref, &refsp, 0,
            xx + mv->u.mv.x - 1, /* offset by 1 in x and y so */
            yy + mv->u.mv.y - 1);/* we can do negative hpel */
    /* interpolate into half-pel then into quarter-pel */
    hpel(tmph, refsp.data, refsp.stride);
    qpel(tmpq, tmph);
#define SETVXY(v, x, y) v[0] = x; v[1] = y
#define SETVV(v, n) v[0] = n[0]; v[1] = n[1]

    SETVXY(bestv, 0, 0);
    SETVXY(pri,  0, -1);
    SETVXY(sec, -1,  0);

    ms1 = quad[1];
    ms2 = quad[3];

    if (quad[3] >= quad[2]) {
        SETVXY(pri, 0, 1);
        ms2 = quad[2];
    }
    if (quad[1] >= quad[0]) {
        SETVXY(sec, 1, 0);
        ms1 = quad[0];
    }
    if (ms2 > ms1) {
        int tv[2]; /* swap */
        SETVV(tv, sec);
        SETVV(sec, pri);
        SETVV(pri, tv);
    }

    SETVXY(diag, pri[0] + sec[0], pri[1] + sec[1]);

    imq = tmpq + QP_OFFSET(1, 1);

    for (n = 0; n <= 6; n++) {
        int evx, evy, cost, t[2];
        if (n == 6) {
            t[0] = pri[0] + diag[0];
            t[1] = pri[1] + diag[1];
        } else {
            int hpel = !(n & 1);
            t[0] = testv[n >> 1][0] * (1 << hpel);
            t[1] = testv[n >> 1][1] * (1 << hpel);
        }
        if (((t[0] | t[1]) & 1) && params->effort < 8) { /* skip qpel at low effort */
            continue;
        }
        score = qpsad(srcsp.data, srcsp.stride, imq + t[0] + t[1] * QP_STRIDE, psy);
        evx = MK_MV_COMP(mv->u.mv.x, 0, t[0]);
        evy = MK_MV_COMP(mv->u.mv.y, 0, t[1]);
        cost = mv_cost(mvf, params, i, j, evx, evy, quant, 0);
        if (best > score + cost) {
            best = score;
            SETVV(bestv, t);
            *found = 1;
        }
    }
    mv->u.mv.x = MK_MV_COMP(mv->u.mv.x, 0, bestv[0]);
    mv->u.mv.y = MK_MV_COMP(mv->u.mv.y, 0, bestv[1]);
    return best * iarea_ratio >> 3;
}

static int
remove_dupes(DSV_MV **list, int n)
{
    int i, j, newn;
    for (j = 1, newn = 1; j < n; j++) {
        DSV_MV *mv = list[j];
        for (i = 0; i < newn; i++) {
            if (mv->u.all == list[i]->u.all) {
                break;
            }
        }
        if (i == newn) {
            list[newn++] = mv;
        }
    }
    return newn;
}

/* NOTE: assumes 'vx, vy' are fpel */
#define ADD_MV_XY(list, vx, vy)               \
do {                                          \
    DSV_MV tmv;                               \
    tmv.u.mv.x = vx * 4;                      \
    tmv.u.mv.y = vy * 4;                      \
    n = add_mv_to_list(hme, list, n, &tmv);   \
} while(0)

/* NOTE: assumes 'mv' is qpel */
static int
add_mv_to_list(DSV_HME *hme, DSV_MV **list, int n, DSV_MV *mv)
{
    list[n] = &hme->mv_bank[hme->n_mv_bank_used++];
    QP2FP(list[n], mv);
    return n + 1;
}

static int
add_spatial_predictions(int level, int i, int j, int n, DSV_MV **list,
        DSV_PARAMS *p, DSV_MV *mvf, DSV_HME *hme)
{
    int step;

    step = 1 << level;
    if (level == 0) {
        DSV_MV tmv;
        int px, py;

        dsv_movec_pred(mvf, p, i, j, &px, &py);
        tmv.u.mv.x = px;
        tmv.u.mv.y = py;
        n = add_mv_to_list(hme, list, n, &tmv);
    }
    if (i > 0) { /* left */
        n = add_mv_to_list(hme, list, n, mvf + (i - step) + j * p->nblocks_h);
    }
    if (j > 0) { /* top */
        n = add_mv_to_list(hme, list, n, mvf + i + (j - step) * p->nblocks_h);
    }
    if (i > 0 && j > 0) { /* top-left */
        n = add_mv_to_list(hme, list, n, mvf + (i - step) + (j - step) * p->nblocks_h);
    }
    return n;
}

/* add spatially and temporally predicted candidates */
static int
add_temporal_predictions(int level, int i, int j, int n, DSV_MV **list,
        DSV_PARAMS *p, DSV_HME *hme)
{
    DSV_MV *ref_mvf;
    int step;

    step = 1 << level;
    ref_mvf = hme->ref_mvf;
    if (ref_mvf != NULL) { /* if we have access to the previous frame's MVs */
        int rx, ry, k;
        /* add all temporal neighbors */
        for (k = 0; k < N_SEARCH_PTS; k++) {
            rx = i + rectx[k] * step;
            ry = j + recty[k] * step;

            if ((rx < 0) ||
                (ry < 0) ||
                (rx >= p->nblocks_h) ||
                (ry >= p->nblocks_v)) {
                continue;
            }
            n = add_mv_to_list(hme, list, n, ref_mvf + rx + ry * p->nblocks_h);
        }
    }
    return n;
}

/* prune vectors that don't conform with the rest and take the new list's average */
static int
find_inliers(DSV_MV **list, DSV_MV **newl, int n, int *ax, int *ay)
{
    int i;
    int dist[16];
    int avgx, avgy;
    int avgd = 0, ssd = 0, thresh, nin = 0;

    if (n == 0) {
        return 0;
    }
    avgx = *ax;
    avgy = *ay;
    for (i = 0; i < n; i++) {
        dist[i] = (SQR(list[i]->u.mv.x - avgx) + SQR(list[i]->u.mv.y - avgy));
        avgd += dist[i];
    }
    avgd /= n;
    for (i = 0; i < n; i++) {
        ssd += SQR(dist[i] - avgd);
    }
    thresh = avgd + iisqrt(ssd / n); /* average dist + stddev = thresh */
    avgx = 0;
    avgy = 0;
    for (i = 0; i < n; i++) {
        if (dist[i] <= thresh) {
          avgx += list[i]->u.mv.x;
          avgy += list[i]->u.mv.y;
          newl[nin] = list[i];
          nin++;
      }
    }
    if (nin == 0) {
        return 0;
    }
    *ax = avgx / nin;
    *ay = avgy / nin;
    return nin;
}

static int
refine_level(DSV_HME *hme, int level, int *scene_change_blocks, int *avg_err, int gx, int gy)
{
    DSV_FRAME *src, *ref, *ogr;
    DSV_MV *mv;
    DSV_MV *mvf, *parent = NULL;
    DSV_PARAMS *params = hme->params;
    DSV_MV zero;
    int i, j, y_w, y_h, nxb, nyb, step, hs, vs;
    unsigned parent_mask, total_err = 0;
    int nintra = 0; /* number of intra blocks */
    DSV_PLANE *sp, *rp;
    int ndiff = 0, num_eligible_blocks = 0;
    unsigned quant_rd = hme->quant * hme->quant;

    y_w = params->blk_w;
    y_h = params->blk_h;

    nxb = params->nblocks_h;
    nyb = params->nblocks_v;

    src = hme->src[level];
    ref = hme->ref[level];
    ogr = hme->ogr[level];

    sp = src->planes + 0;
    rp = ref->planes + 0;

    hme->mvf[level] = dsv_alloc(sizeof(DSV_MV) * nxb * nyb);

    mvf = hme->mvf[level];

    hs = DSV_FORMAT_H_SHIFT(params->vidmeta->subsamp);
    vs = DSV_FORMAT_V_SHIFT(params->vidmeta->subsamp);

    if (level < hme->enc->pyramid_levels) {
        parent = hme->mvf[level + 1];
    }

    memset(&zero, 0, sizeof(zero));

    step = 1 << level;
    parent_mask = ~((step << 1) - 1);

    for (j = 0; j < nyb; j += step) {
        for (i = 0; i < nxb; i += step) {
            DSV_PLANE srcp;
            int dx, dy;
            int bx, by, bw, bh;
            int k, m, n = 0;
            DSV_MV *cands[128];
            unsigned metr[4], best, score_zero, score, best_score;
            unsigned qthresh, good_enough = 0;
            int lax = 0, lay = 0;
            unsigned var_src = 0, avg_src = 0;
            PSY_COEFS psy;
            /* defaults */
            psy.err_weight = 2;
            psy.tex_weight = 1;
            psy.avg_weight = 0;

            bx = (i * y_w) >> level;
            by = (j * y_h) >> level;
            /* bounds check for safety */
            if ((bx >= src->width) || (by >= src->height)) {
                DSV_MV zmv = { 0 }; /* inter with no other flag */
                mvf[i + j * nxb] = zmv;
                continue;
            }
            memset(&hme->mv_bank, 0, sizeof(hme->mv_bank));
            hme->n_mv_bank_used = 0;
            dsv_plane_xy(src, &srcp, 0, bx, by);
            bw = MIN(srcp.w, y_w);
            bh = MIN(srcp.h, y_h);

            ADD_MV_XY(cands, 0, 0);
            if (!SQUARED_LEVELS) {
                /* TODO this logic could use much more testing */
                int sigmot;
                sigmot = (abs(gx) >= 3) || (abs(gy) >= 3);
                var_src = block_detail(srcp.data, srcp.stride, bw, bh, &avg_src);

                if (var_src <= (unsigned) (8 * bw * bh * hme->quant >> 9)) {
                    psy.err_weight = 2;
                    psy.tex_weight = 1;
                    psy.avg_weight = 0;
                } else {
                    psy.err_weight = 1;
                    psy.tex_weight = 2;
                    psy.avg_weight = 0;
                }

                if (sigmot) {
                    sigmot = psy.err_weight;
                    psy.err_weight = psy.tex_weight;
                    psy.tex_weight = sigmot;
                }
            }
            if (parent != NULL) {
#define N_POINTS (1 + 8)
                static int pt[N_POINTS * 2] = { 0, 0,
                        -2,  0,   2, 0,   0, -2,    0, 2,
                        -2, -2,   2, 2,   2, -2,   -2, 2 };
                int x, y, pi, pj;
                int sumx = 0, sumy = 0, npar = 0;
                DSV_MV *lcand[16];

                pi = i & parent_mask;
                pj = j & parent_mask;
                for (m = 0; m < N_POINTS; m++) {
                    x = pi + pt[(m << 1) + 0] * step;
                    y = pj + pt[(m << 1) + 1] * step;
                    if (x >= 0 && x < nxb && y >= 0 && y < nyb) {
                        DSV_MV *pmv = parent + x + y * nxb;
                        sumx += pmv->u.mv.x;
                        sumy += pmv->u.mv.y;
                        lcand[npar] = pmv;
                        npar++;
                    }
                }
                if (npar) {
                    int nl;
                    DSV_MV *newl[16];
                    lax = sumx / npar;
                    lay = sumy / npar;
                    nl = find_inliers(lcand, newl, npar, &lax, &lay);
                    ADD_MV_XY(cands, lax, lay);

                    n = add_spatial_predictions(level, i, j, n, cands, params, mvf, hme);
                    n = add_temporal_predictions(level, i, j, n, cands, params, hme);

                    ADD_MV_XY(cands, gx, gy);

                    for (m = 0; m < nl; m++) {
                        ADD_MV_XY(cands, newl[m]->u.mv.x, newl[m]->u.mv.y);
                    }
                }
            }

            /* we only care about unique non-zero vectors */
            n = remove_dupes(cands, n);

            best = 0;
            best_score = score_zero = UINT_MAX;
            for (k = 0; k < n; k++) {
                DSV_PLANE refp;
                int evx, evy, cost;

                dx = DSV_SAR(cands[k]->u.mv.x, level);
                dy = DSV_SAR(cands[k]->u.mv.y, level);

                if (invalid_block(ref, bx + dx, by + dy, bw, bh)) {
                    continue;
                }

                dsv_plane_xy(ref, &refp, 0, bx + dx, by + dy);
                score = hier_metr(level, srcp.data, srcp.stride, refp.data, refp.stride, bw, bh, &psy);
                evx = MK_MV_COMP(dx * step, 0, 0);
                evy = MK_MV_COMP(dy * step, 0, 0);
                cost = mv_cost(mvf, params, i, j, evx, evy, hme->quant, level);
                if (best_score > score + cost) {
                    best_score = score;
                    best = k;
                }
                if (dx == 0 && dy == 0) {
                    score_zero = score;
                }
            }

            dx = DSV_SAR(cands[best]->u.mv.x, level);
            dy = DSV_SAR(cands[best]->u.mv.y, level);
            dx = CLAMP(dx, (-bw - bx), (ref->width - bx));
            dy = CLAMP(dy, (-bh - by), (ref->height - by));

            mv = &mvf[i + j * nxb];
            memset(mv, 0, sizeof(*mv));

            /* full-pel search around best candidate vector */
            best = best_score;
            m = 0;

            qthresh = (unsigned) (hme->quant * bw * bh >> 10);
            if (DO_GOOD_ENOUGH && ((abs(dx) <= 1 && abs(dy) <= 1))) {
                /* compare to source reference frame */
                unsigned zoscore = fastmetr(srcp.data, sp->stride,
                                    DSV_GET_XY(&ogr->planes[0], bx, by),
                                    ogr->planes[0].stride, bw, bh, &psy);
                if (zoscore < qthresh) {
                    /* bias towards zero vector */
                    best = (level == 0) ? score_zero : 0;
                    dx = 0;
                    dy = 0;
                    good_enough = 1;
                }
            }
#define FAST_DIAG 1
            if (!good_enough) {
                n = (level != 0 || (params->effort >= 2 && !FAST_DIAG)) ? N_SEARCH_PTS : (N_SEARCH_PTS / 2 + 1);
                for (k = 0; k < n; k++) {
                    int evx, evy, cost;
                    int tvx, tvy;

                    tvx = dx + rectx[k];
                    tvy = dy + recty[k];
                    score = hier_metr(level, srcp.data, sp->stride,
                            DSV_GET_XY(rp, bx + tvx, by + tvy),
                            rp->stride, bw, bh, &psy);

                    if (FAST_DIAG && k >= 1 && k <= 4) {
                        metr[k - 1] = score;
                    }

                    evx = MK_MV_COMP(tvx * step, 0, 0);
                    evy = MK_MV_COMP(tvy * step, 0, 0);
                    cost = mv_cost(mvf, params, i, j, evx, evy, hme->quant, level);
                    if (DO_GOOD_ENOUGH && level == 0) {
                        if (score <= qthresh) {
                            best = score;
                            m = k;
                            good_enough = 1;
                            break;
                        }
                    }
                    if (best > score + cost) {
                        best = score;
                        m = k;
                    }
                }
            }
#if FAST_DIAG
            if (!good_enough) {
                int tv[2], pri, sec;
                int evx, evy, cost;

                pri = metr[3] >= metr[2] ? 3 : 4;
                sec = metr[1] >= metr[0] ? 1 : 2;

                tv[0] = rectx[pri] + rectx[sec] + rectx[m];
                tv[1] = recty[pri] + recty[sec] + recty[m];
                score = hier_metr(level, srcp.data, sp->stride,
                        DSV_GET_XY(rp, bx + tv[0], by + tv[1]),
                        rp->stride, bw, bh, &psy);
                evx = MK_MV_COMP(tv[0] * step, 0, 0);
                evy = MK_MV_COMP(tv[1] * step, 0, 0);
                cost = mv_cost(mvf, params, i, j, evx, evy, hme->quant, level);
                if (best > score + cost) {
                    best = score;
                    dx = tv[0];
                    dy = tv[1];
                    if (DO_GOOD_ENOUGH && score <= qthresh) {
                        good_enough = 1;
                    }
                } else {
                    dx += rectx[m];
                    dy += recty[m];
                }
            }
#else
            dx += rectx[m];
            dy += recty[m];
#endif

            mv->u.mv.x = dx * step;
            mv->u.mv.y = dy * step;

            /* subpel refine at base level */
            if (level == 0) {
                int fpelx, fpely; /* full-pel MV coords */
                unsigned yarea = bw * bh;
                unsigned best_fp = best;
                int found = 0;

                fpelx = mv->u.mv.x;
                fpely = mv->u.mv.y;

                if (params->effort >= 4) {
                    if (!invalid_block(ref, bx + lax, by + lay, bw, bh)) {
                        DSV_MV tmpv;
                        /* first search local average from parents */
                        tmpv.u.mv.x = lax;
                        tmpv.u.mv.y = lay;
                        best = subpixel_ME(params, mvf, &tmpv, src, ref, i, j, best_fp, hme->quant, bx, by, bw, bh, &psy, &found);
                        if (found) {
                            mv->u.all = tmpv.u.all;
                            fpelx = lax;
                            fpely = lay;
                        }
                    }

                    if (!found && !good_enough) {
                        /* if nothing so far, search final MV from HME */
                        best = subpixel_ME(params, mvf, mv, src, ref, i, j, best_fp, hme->quant, bx, by, bw, bh, &psy, &found);
                    }
                }
                if (!found) {
                    mv->u.mv.x = MK_MV_COMP(fpelx, 0, 0);
                    mv->u.mv.y = MK_MV_COMP(fpely, 0, 0);
                }
                /* mode decision + block metric gathering
                 * src = source block
                 * ogr = original ref frame block at full-pel motion (x, y)
                 * ref = reconstructed ref frame block at full-pel motion (x, y)
                 */ {
                    DSV_PLANE refp, ogrp;
                    unsigned var_ref, avg_ref;
                    int uavg_src, vavg_src, uavg_ref, vavg_ref;
                    int cbx, cby, cbw, cbh, cbmx, cbmy;
                    unsigned mad, ogrerr, ogrmad;
                    unsigned avg_y_dif, avg_c_dif;
                    int eprmi, eprmd, eprmr;
                    int neidif, oob_vector; /* out of bounds */
                    unsigned skipt = (quant_rd >> 19);
                    unsigned ratio = 1 << 5; /* ratio of subpel_min_err / fullpel_min_err */
                    unsigned chroma_ratio = 1 << 4; /* ratio of chroma block size to luma block size */
                    int ipolvar, dv;
                    CHROMA_PSY cpsy;
                    DSV_MV *refmv = NULL;
                    if (hme->ref_mvf != NULL) {
                        refmv = &hme->ref_mvf[i + j * nxb];
                    }
                    if (DSV_IS_SUBPEL(mv)) {
                        ratio = (best << 5) / (best_fp + !best_fp);
                    }
                    dsv_plane_xy(ogr, &ogrp, 0, bx + fpelx, by + fpely);
                    dsv_plane_xy(ref, &refp, 0, bx + fpelx, by + fpely);
                    ogrerr = fastmetr(srcp.data, srcp.stride, ogrp.data, ogrp.stride, bw, bh, &psy);
                    ogrmad = DSV_UDIV_ROUND(ogrerr, yarea);
                    ogrmad = (ogrmad * ratio >> 5);
                    mad = DSV_UDIV_ROUND(best, yarea);

                    var_src = block_detail(srcp.data, srcp.stride, bw, bh, &avg_src);
                    var_ref = block_detail(refp.data, refp.stride, bw, bh, &avg_ref);
                    /* weigh variance between src and ref by how much better the subpixel ME was */
                    dv = MIN(ratio, 32);
                    ipolvar = (var_src * dv + var_ref * (32 - dv)) >> 5;
                    dv = ABSDIF(var_src, ipolvar);

                    DSV_MV_SET_MAINTAIN(mv, (var_src > 16 * yarea) && (var_src < 32 * yarea));

                    cbx = i * (y_w >> hs);
                    cby = j * (y_h >> vs);
                    cbmx = cbx + DSV_SAR(fpelx, hs);
                    cbmy = cby + DSV_SAR(fpely, vs);
                    cbw = bw >> hs;
                    cbh = bh >> vs;
                    chroma_ratio = ((cbw * cbh) << 4) / yarea;

                    c_average(sp, cbx, cby, cbw, cbh, &uavg_src, &vavg_src);
                    c_average(rp, cbmx, cbmy, cbw, cbh, &uavg_ref, &vavg_ref);

                    chroma_analysis(&cpsy, avg_src, uavg_src, vavg_src);

                    avg_y_dif = ABSDIF(avg_src, avg_ref);
                    avg_c_dif = AVG2(abs(uavg_src - uavg_ref), abs(vavg_src - vavg_ref));

                    calc_EPRM(&srcp, &refp, avg_src, avg_ref, bw, bh, &eprmi, &eprmd, &eprmr);
                    DSV_MV_SET_SIMCMPLX(mv, 0);

                    oob_vector = outofbounds(i, j, nxb, nyb, y_w, y_h, mv);
                    neidif = dsv_neighbordif(mvf, params, i, j);

                    /* test skip mode */
                    if (mv->u.all == 0 && hme->enc->skip_block_thresh >= 0 && !params->lossless) {
                        unsigned cth, sth = skipt * yarea;
                        unsigned zsub[3], dcd;

                        sth += 4 * var_src;

                        sth += yarea * hme->enc->skip_block_thresh;
                        if (hme->quant < (1 << (DSV_MAX_QP_BITS - 2))) {
                            sth = sth * hme->quant >> (DSV_MAX_QP_BITS - 2);
                        }
                        if (avg_y_dif <= 2) {
                            sth = MAX(sth, 3 * (yarea + var_src));
                        }
                        sth = MAX(sth, yarea);
                        if (good_enough) {
                            sth *= 2;
                        }
                        yuv_max_subblock_err(zsub, src, ref,
                                bx, by, bx, by, bw, bh,
                                cbx, cby, cbx, cby, cbw, cbh, &psy);
                        cth = (chroma_ratio * sth * MAX(skipt, 1) >> (4 + 1));
                        dcd = ABSDIF(avg_src, avg_ref);
                        zsub[0] += SQR(dcd) * yarea;
                        if (zsub[0] <= sth && zsub[1] <= cth && zsub[2] <= cth) {
                            DSV_MV_SET_SKIP(mv, 1);
                            mv->err = 0;
                            /* don't bother with anything else if we're skipping the block anyway */
                            goto skip;
                        }
                    }
                    /* see if we can afford to zero out the residuals */
                    if (!oob_vector && !params->lossless) {
                        int y_prereq, c_prereq;
                        int utex, vtex, carea = 4 * cbw * cbh;

                        utex = block_tex(sp[1].data + cbx + cby * sp[1].stride, sp[1].stride, cbw, cbh);
                        vtex = block_tex(sp[2].data + cbx + cby * sp[2].stride, sp[2].stride, cbw, cbh);

                        y_prereq = (avg_y_dif <= 2);
                        c_prereq = (utex > carea || vtex > carea) &&
                                !cpsy.greyish && (avg_c_dif <= 2);
                        if (y_prereq || c_prereq) {
                            unsigned bsub[3];
                            unsigned xth = skipt * yarea;
                            /* don't transmit residuals if the differences are minimal */
                            yuv_max_subblock_err(bsub, src, ref,
                                    bx, by, bx + fpelx, by + fpely, bw, bh,
                                    cbx, cby, cbmx, cbmy, cbw, cbh, &psy);

                            xth += ipolvar;
                            xth = MAX((int) xth - ((int) yarea * neidif * 2), 0);

                            xth = xth * hme->quant >> DSV_MAX_QP_BITS;
                            xth = CLAMP(xth, 32, yarea * 4);
                            bsub[0] = (bsub[0] * ratio >> 5);
                            bsub[1] = (bsub[1] * ratio >> 5);
                            bsub[2] = (bsub[2] * ratio >> 5);

                            if (y_prereq && bsub[0] < 4 * xth) {
                                DSV_MV_SET_NOXMITY(mv, 1);
                            }
                            xth = chroma_ratio * xth >> 4;
                            if (c_prereq && bsub[1] < xth && bsub[2] < xth) {
                                DSV_MV_SET_NOXMITC(mv, 1);
                            }
                        }
#if NO_RESIDUALS
                        DSV_MV_SET_NOXMITY(mv, 1);
#if NO_RESIDUALS == 2
                        DSV_MV_SET_NOXMITC(mv, 1);
#endif
#endif
                        if ((unsigned) dv < (var_src / 4)) {
                            /* similar enough complexity to enable P-frame visual masking */
                            DSV_MV_SET_SIMCMPLX(mv, 1);
                        }
                    }

#if 1 /* have intra blocks */
                    test_subblock_intra_y(params, refmv, mv,
                                          &srcp, &refp,
                                          ipolvar, avg_src, neidif, ratio,
                                          bw, bh);
                    test_subblock_intra_c(params, mv, sp, rp,
                                          mad, ipolvar / (bw * bh), avg_src,
                                          cbx, cby, cbmx, cbmy,
                                          cbw, cbh);
#endif
                    /* end-of-block stats */
                    if (!DSV_MV_IS_NOXMITY(mv)) {
                        mv->err = mad;
                        total_err += mad;
                    }
                    /* more difference, more likely to need a scene change */
                    ndiff += (ogrmad > 11) + (avg_c_dif >= 32);
skip:
                    if (best > 0) {
                        num_eligible_blocks++;
                    }
                    if (DSV_MV_IS_INTRA(mv)) {
                        int merged = (mv->dc & DSV_SRC_DC_PRED) ? eprmd : eprmi;
                        if (mv->submask != DSV_MASK_ALL_INTRA) {
                            merged |= eprmr;
                        }
                        DSV_MV_SET_EPRM(mv, !!merged);
                        nintra++;
                        /* intra does not have subpel precision */
                        mv->u.mv.x = MK_MV_COMP(fpelx, 0, 0);
                        mv->u.mv.y = MK_MV_COMP(fpely, 0, 0);
                    } else {
                        int merged = eprmr;
                        if (mv->submask) {
                            merged |= eprmi;
                        }
                        DSV_MV_SET_EPRM(mv, !!merged);
                    }
                    if (DSV_MV_IS_INTRA(mv) || DSV_MV_IS_EPRM(mv)) {
                        DSV_MV_SET_SIMCMPLX(mv, 0); /* don't do this for intra/eprm to retain precision */
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
        *avg_err = total_err / (nxb * nyb);
    }
    return nintra;
}

extern DSV_MV *
dsv_intra_analysis(DSV_FRAME *src, DSV_PARAMS *params)
{
    int i, j, y_w, y_h, nxb, nyb, scale;
    DSV_MV *ba;
    uint16_t hist[NHIST];
    uint8_t peaks[NHIST];

    y_w = params->blk_w;
    y_h = params->blk_h;

    nxb = params->nblocks_h;
    nyb = params->nblocks_v;

    ba = dsv_alloc(nxb * nyb * sizeof(DSV_MV));

    scale = 2 * dsv_spatial_psy_factor(params, -1);
    for (j = 0; j < nyb; j++) {
        for (i = 0; i < nxb; i++) {
            DSV_PLANE srcp;
            int bx, by, bw, bh;
            DSV_MV *mv;
            unsigned luma_detail, luma_avg, var_t;
            int cbx, cby, cbw, cbh, subsamp;
            CHROMA_PSY cpsy;
            int maintain = 1;
            int keep_hf = 1;
            int npeaks = 0;
            int foliage = 0;
            int is_text = 0;

            bx = (i * y_w);
            by = (j * y_h);

            mv = &ba[i + j * nxb];

            mv->flags &= ~(1 << DSV_MV_BIT_SKIP);
            mv->flags &= ~(1 << DSV_MV_BIT_MAINTAIN);
            mv->flags &= ~(1 << DSV_MV_BIT_RINGING);

            if ((bx >= src->width) || (by >= src->height)) {
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

            luma_detail = block_detail(srcp.data, srcp.stride, bw, bh, &luma_avg);

            if (params->do_psy & (DSV_PSY_ADAPTIVE_RINGING | DSV_PSY_CONTENT_ANALYSIS)) {
                int skip_tones;
                int uavg, vavg;
                int tf = 0;
                int tf2 = 0;
                int qtex, hvar;
                int luma_var, luma_tex;

                hvar = block_hist_var(srcp.data, srcp.stride, bw, bh, hist);
                qtex = quant_tex(srcp.data, srcp.stride, bw, bh);

                luma_var = block_var(srcp.data, srcp.stride, bw, bh, &luma_avg);
                luma_var /= (bw * bh);
                luma_tex = block_tex(srcp.data, srcp.stride, bw, bh);
                luma_tex /= (bw * bh);

                npeaks = block_peaks(srcp.data, srcp.stride, bw, bh, peaks, hist, luma_avg);
                /* empirically determined: */
                is_text = (abs(npeaks - 2) <= 1);
                if (qtex == 1 || qtex == 2) {
                    tf2 = hvar <= 3 && (luma_tex >= 10 && luma_var >= luma_tex);
                }
                if (qtex == 2 || qtex == 3) {
                    tf = luma_tex >= 8 && luma_var >= (2 * luma_tex);
                    tf &= (abs(hvar - 5) <= 3);
                }
                is_text &= (tf || tf2);

                c_average(src->planes, cbx, cby, cbw, cbh, &uavg, &vavg);
                chroma_analysis(&cpsy, luma_avg, uavg, vavg);
                /* guess at what foliage is */
                foliage = (cpsy.nature && luma_avg < 160);
                foliage &= luma_detail > (unsigned) ((36 * bw * bh) / MAX(scale, 1));
                if (foliage) {
                    is_text = 0;
                }
                /* avoid giving more bits to high freq colors since we are
                 *    less sensitive to the spatial frequencies of them anyway.
                 */
                skip_tones = cpsy.hifreq;
                if ((params->do_psy & DSV_PSY_ADAPTIVE_RINGING) && !skip_tones &&
                        (foliage ||
                        (hvar <= (MIN(qtex - 3, 2) * 16) && qtex > 1))) {
                    DSV_MV_SET_RINGING(mv, 1);
                }

                var_t = 8;
                if (cpsy.nature || cpsy.greyish || cpsy.skinnish) {
                    var_t += 12;
                } else if (!cpsy.hifreq) {
                    var_t += 8;
                }
            } else {
                var_t = 16;
            }

            if (params->do_psy & (DSV_PSY_CONTENT_ANALYSIS | DSV_PSY_ADAPTIVE_QUANT)) {
                luma_detail /= (bw * bh);
                keep_hf &= luma_detail < 48;
                maintain = (luma_detail < var_t * 4);
            }
            if (params->do_psy & DSV_PSY_CONTENT_ANALYSIS) {
                if (foliage) {
                    keep_hf = 0;
                    maintain = 1;
                } else if (is_text) {
                    keep_hf = 1;
                    maintain = 0;
                }
            }
            if (params->do_psy & DSV_PSY_ADAPTIVE_RINGING) {
                if (luma_avg < 24) {
                    DSV_MV_SET_RINGING(mv, 1); /* improve dark areas */
                }
            }
            DSV_MV_SET_MAINTAIN(mv, !!maintain);
            DSV_MV_SET_SKIP(mv, !!keep_hf); /* reusing the skip flag */
        }
    }
    return ba;
}
static void
global_motion(DSV_MV *vecs, DSV_PARAMS *p, int level, int *gx, int *gy)
{
    int i, j;
    int avgx = 0, avgy = 0;
    int nblk = 0;
    DSV_MV *mv;
    int step;
    step = 1 << level;

    for (j = 0; j < p->nblocks_v; j += step) {
        for (i = 0; i < p->nblocks_h; i += step) {
            mv = &vecs[i + j * p->nblocks_h];

            avgx += mv->u.mv.x;
            avgy += mv->u.mv.y;
            nblk++;
        }
    }
    if (nblk) {
        *gx = avgx * 2 / nblk; /* scale up for next highest level */
        *gy = avgy * 2 / nblk;
    } else {
        *gx = 0;
        *gy = 0;
    }
}

extern int
dsv_hme(DSV_HME *hme, int *scene_change_blocks, int *avg_err)
{
    int i = hme->enc->pyramid_levels;
    int nintra = 0;
    int globalx = 0, globaly = 0;

    while (i >= 0) {
        nintra = refine_level(hme, i, scene_change_blocks, avg_err, globalx, globaly);
        if (i != 0) {
            global_motion(hme->mvf[i], hme->params, i, &globalx, &globaly);
        }
        i--;
    }
    return (nintra * 100) / (hme->params->nblocks_h * hme->params->nblocks_v);
}

