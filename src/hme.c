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

#include "dsv_encoder.h"

/* Hierarchical Motion Estimation */

/* 1 = disable sending luma residuals,
 * 2 = disable sending luma+chroma residuals
 */
#define NO_RESIDUALS 0

#define METRIC_MODE_SAD 1
#define METRIC_MODE_SSE 2
#define METRIC_MODE_PSY 3
#define METRIC_MODE METRIC_MODE_PSY

#define DO_NOXMIT 1
#define DO_GOOD_ENOUGH 1

#define MAX_CANDS 128

#define AVG2(a, b) (((a) + (b)) / 2)
#define UAVG2(a, b) ((unsigned) ((a) + (b) + 1) >> 1)
#define AVG4(a, b, c, d) (((a) + (b) + (c) + (d)) / 4)
#define MV_LT(v, t) (abs((v)->u.mv.x) < (t) && abs((v)->u.mv.y) < (t)) /* less than */
#define MV_GT(v, t) (abs((v)->u.mv.x) > (t) && abs((v)->u.mv.y) > (t)) /* greater than */
#define MV_MAG(v) UAVG2(abs((v)->u.mv.x), abs((v)->u.mv.y)) /* not actual mag */
#define MV_CMP(v, vx, vy, t) (abs((v)->u.mv.x - (int) (vx)) < (t) && abs((v)->u.mv.y - (int) (vy)) < (t))
#define MV_DIFLT(a, b, t) (abs((int) (a)->u.mv.x - (int) (b)->u.mv.x) < (t) && abs((int) (a)->u.mv.y - (int) (b)->u.mv.y) < (t))
#define MK_MV_COMP(fp, hp, qp) ((fp) * 4 + (hp) * 2 + (qp))
#define SQR(x) ((x) * (x))
#define MIN4(a, b, c, d) MIN(MIN(a, b), MIN(c, d))
#define MAX4(a, b, c, d) MAX(MAX(a, b), MAX(c, d))
#define USUB(usn, sub) MAX(((int) (usn) - (int) (sub)), 0)

/* for blending luma / chroma motion estimation error scores */
#define BLEND_SCORES(lscore, cscore) ((lscore) = (lscore) + (cscore))

#define READONLY /* used for code readability */

/* qpel to fpel */
#define QP2FP(fpel, qpel)                             \
    do {                                              \
        (fpel)->u.mv.x = DSV_SAR_R((qpel)->u.mv.x, 2);\
        (fpel)->u.mv.y = DSV_SAR_R((qpel)->u.mv.y, 2);\
    } while (0)

#define ABSDIF(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))
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

typedef struct {
    int err_weight; /* weight of per pixel SSD */
    int tex_weight; /* weight of delta differences in 2x2 area */
    int avg_weight; /* weight of 2x2 average SSD */
} PSY_COEFS;

#define N_SEARCH_PTS 9

static READONLY int rectx[N_SEARCH_PTS] = { 0, 1, -1, 0,  0, -1,  1, -1, 1 };
static READONLY int recty[N_SEARCH_PTS] = { 0, 0,  0, 1, -1, -1, -1,  1, 1 };
static READONLY PSY_COEFS chroma_me_psy = { 2, 0, 0 };

static void
chroma_analysis(DSV_META *meta, CHROMA_PSY *c, int y, int u, int v)
{
    if (meta->colorspace == DSV_COLORSPACE_BC2) {
        /* for BC2, y=br, u=cs, v=ci */
        c->nature = /*abs(y - 80) < 80 &&*/u > 90 && v < 150;
        c->greyish = abs(u - 128) < 8 && abs(v - 128) < 8;
        c->skinnish = (y < 220) &&
                abs(u - 96) <= 24 &&
                abs(v - 120) <= 16; /* terrible approximation */
        c->hifreq = (v > 128) && !c->greyish && !c->skinnish;
    } else {
        c->nature = u < 128 && v < 160;
        c->greyish = abs(u - 128) < 8 && abs(v - 128) < 8;
        c->skinnish = (y < 230) &&
                          abs(u - 108) < 24 &&
                          abs(v - 148) < 24; /* another terrible approximation */
        c->hifreq = (u > 160) && !c->greyish && !c->skinnish;
    }
}

static uint8_t
clamp_u8(int v)
{
    return v > 255 ? 255 : v < 0 ? 0 : v;
}

#define SP_SAD_SZ DSV_MIN_BLOCK_SIZE /* sub-pixel SAD size */

#define SP_DIM    (SP_SAD_SZ + 1)
#define HP_DIM    (SP_DIM * 2)

/* conversion from full-pel coords to quarter pel */
#define QP_OFFSET(fpx, fpy) ((4 * (fpx) + (4 * (fpy)) * QP_STRIDE))

#define HP_STRIDE (SP_DIM * 2)
#define QP_STRIDE (SP_DIM * 4)

#define METRIC_RETURN_PSY(a, w, h) (iisqrt((a))*(w)*(h)/UAVG2(w,h))

#define METRIC_RETURN_SSE(a, w, h) (iisqrt(a)*(w)*(h)/UAVG2(w,h))
#define METRIC_RETURN_SAD(a, w, h) (a)

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

#define METR_CALC(acc) {                                                        \
        int ta, tb, se;/* texture in block A, ~ block B, squared error */       \
        se = DSV_UAVG4(SQR(a1 - b1), SQR(a2 - b2), SQR(a3 - b3), SQR(a4 - b4)); \
        ta = AVG4((a1 - a2), (a3 - a4), (a3 - a1), (a4 - a2));                  \
        tb = AVG4((b1 - b2), (b3 - b4), (b3 - b1), (b4 - b2));                  \
        (acc) += se << psy->err_weight;                                         \
        (acc) += SQR(ta - tb) << psy->tex_weight;                               \
        (acc) += SQR(s0 - s1) << psy->avg_weight;                               \
}

#define METR_BODY(w, h)                                        \
        int i, j;                                              \
        unsigned acc = 0;                                      \
        for (j = 0; j < h / 2; j++) {                          \
            uint8_t *acur = a;                                 \
            uint8_t *anxt = a + as;                            \
            uint8_t *bcur = b;                                 \
            uint8_t *bnxt = b + bs;                            \
            for (i = 0; i < w / 2; i++) {                      \
                int a1, a2, a3, a4, b1, b2, b3, b4, s0, s1;    \
                a1 = *acur++;                                  \
                a2 = *acur++;                                  \
                a3 = *anxt++;                                  \
                a4 = *anxt++;                                  \
                s0 = DSV_UAVG4(a1, a2, a3, a4);                \
                b1 = *bcur++;                                  \
                b2 = *bcur++;                                  \
                b3 = *bnxt++;                                  \
                b4 = *bnxt++;                                  \
                s1 = DSV_UAVG4(b1, b2, b3, b4);                \
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
    return METRIC_RETURN_PSY(acc, w, h);                           \
}

#define MAKE_METRWH(w, h)                                      \
static unsigned                                                \
metr_ ##w## x ##h## x (uint8_t *a, int as, uint8_t *b, int bs, PSY_COEFS *psy) \
{                                                              \
    METR_BODY(w, h);                                           \
    return METRIC_RETURN_PSY(acc, w, h);                           \
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
    return METRIC_RETURN_PSY(acc, w, h);
}
static unsigned
intra_metr_wxh(uint8_t *a, int as, int dc, int w, int h, PSY_COEFS *psy)
{
#if METRIC_MODE == METRIC_MODE_SAD
    {
        int i, j;
        unsigned acc = 0;
        for (j = 0; j < h ; j++) {
            for (i = 0; i < w; i++) {
                int dif = (a[i] - dc);
                acc += abs(dif);
            }
            a += as;
        }
        (void) psy;
        return METRIC_RETURN_SAD(acc, w, h);
    }
#elif METRIC_MODE == METRIC_MODE_SSE
    {
        int i, j;
        unsigned acc = 0;
        for (j = 0; j < h ; j++) {
            for (i = 0; i < w; i++) {
                int dif = (a[i] - dc);
                acc += dif * dif;
            }
            a += as;
        }
        (void) psy;
        return METRIC_RETURN_SSE(acc, w, h);
    }
#elif METRIC_MODE == METRIC_MODE_PSY
    int i, j;
    unsigned acc = 0;
    for (j = 0; j < h / 2; j++) {
        uint8_t *acur = a;
        uint8_t *anxt = a + as;
        for (i = 0; i < w / 2; i++) {
            int a1, a2, a3, a4, s0, s1;
            a1 = *acur++;
            a2 = *acur++;
            a3 = *anxt++;
            a4 = *anxt++;
            s0 = ((unsigned) ((a1) + (a2) + (a3) + (a4) + 2) >> 2);
            s1 = dc;
            {
                int ta, se;/* texture in block A, ~ block B, squared error */
                se = ((unsigned) ((((a1 - dc) * (a1 - dc))) + ((a2 - dc) * (a2 - dc)) + ((a3 - dc) * (a3 - dc)) + ((a4 - dc) * (a4 - dc)) + 2) >> 2);
                ta = (((a1 - a2) + (a3 - a4) + (a3 - a1) + (a4 - a2)) / 4);
                acc += se << psy->err_weight;
                acc += (ta * ta) << psy->tex_weight;
                acc += ((s0 - s1) * (s0 - s1)) << psy->avg_weight;
            };
        }
        a += 2 * as;
    }
    return METRIC_RETURN_PSY(acc, w, h);
#endif
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
#if METRIC_MODE == METRIC_MODE_SAD
    int i, j;
    unsigned acc = 0;
    for (j = 0; j < SP_SAD_SZ ; j++) {
        for (i = 0; i < SP_SAD_SZ; i++) {
            int dif = (a[i] - b[QP_OFFSET(i, j)]);
            acc += abs(dif);
        }
        a += as;
    }
    return METRIC_RETURN_SAD(acc, SP_SAD_SZ, SP_SAD_SZ);
#elif METRIC_MODE == METRIC_MODE_SSE
    int i, j;
    unsigned acc = 0;
    for (j = 0; j < SP_SAD_SZ ; j++) {
        for (i = 0; i < SP_SAD_SZ; i++) {
            int dif = (a[i] - b[QP_OFFSET(i, j)]);
            acc += dif * dif;
        }
        a += as;
    }
    return METRIC_RETURN_SSE(acc, SP_SAD_SZ, SP_SAD_SZ);
#elif METRIC_MODE == METRIC_MODE_PSY
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
            s0 = DSV_UAVG4(a1, a2, a3, a4);
            b1 = b[QP_OFFSET(i * 2, j * 2)];
            b2 = b[QP_OFFSET(i * 2 + 1, j * 2)];
            b3 = b[QP_OFFSET(i * 2, j * 2 + 1)];
            b4 = b[QP_OFFSET(i * 2 + 1, j * 2 + 1)];
            s1 = DSV_UAVG4(b1, b2, b3, b4);
            ap += 2;
            METR_CALC(acc);
        }
        a += 2 * as;
    }
    return METRIC_RETURN_PSY(acc, SP_SAD_SZ, SP_SAD_SZ);
#endif
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

static unsigned
fastmetr(uint8_t *a, int as, uint8_t *b, int bs, int w, int h, PSY_COEFS *psy)
{
    if (w == 0 || h == 0) {
        return INT_MAX;
    }
#if METRIC_MODE == METRIC_MODE_SAD
    {
        int i, j;
        unsigned acc = 0;
        for (j = 0; j < h ; j++) {
            for (i = 0; i < w; i++) {
                int dif = (a[i] - b[i]);
                acc += abs(dif);
            }
            a += as;
            b += bs;
        }
        (void) psy;
        return METRIC_RETURN_SAD(acc, w, h);
    }
#elif METRIC_MODE == METRIC_MODE_SSE
    (void) psy;
    bs = fastsse(a, as, b, bs, w, h);
    return METRIC_RETURN_SSE(bs, w, h);
#elif METRIC_MODE == METRIC_MODE_PSY
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
#endif
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

static int
block_avg(uint8_t *a, int as, int w, int h)
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
invalid_block(DSV_FRAME *f, int bx, int by, int bw, int bh, int pad)
{
    int b = f->border * DSV_FRAME_BORDER;
    return (bx - pad) < -b ||
           (by - pad) < -b ||
           (bx + bw + pad) >= (f->width + b) ||
           (by + bh + pad) >= (f->height + b);
}

/* determine if this block needs EPRM */
static void
calc_EPRM(DSV_PLANE *src, DSV_PLANE *mvr, int avg_src, int avg_ref,
          int w, int h, int *eprmi, int *eprmd, int *eprmr)
{
    int i, j, clipi = 0, clipd = 0, clipr = 0;
    uint8_t *srcp = src->data;
    uint8_t *mvrp = mvr->data;

    avg_src -= 128;
    avg_ref -= 128;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            int s = srcp[i];
            /* see if MV pred or intra pred would clip and require EPRM */
            if (!clipr) {
                clipr = ((s - mvrp[i]) + 128) & ~0xff;
            }
            if (!clipi) {
                clipi = (s - avg_ref) & ~0xff;
            }
            if (!clipd) {
                clipd = (s - avg_src) & ~0xff;
            }
            if (clipi && clipd && clipr) {
                *eprmi = 1;
                *eprmd = 1;
                *eprmr = 1;
                return;
            }
        }
        srcp += src->stride;
        mvrp += mvr->stride;
    }
    *eprmi = !!clipi;
    *eprmd = !!clipd;
    *eprmr = !!clipr;
}

static int
block_var(uint8_t *a, int as, int w, int h, unsigned *avg)
{
    int i, j;
    int s = 0, var = 0;
    uint8_t *ptr;
    if (w == 0 || h == 0) {
        return 0;
    }
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

/* psychovisual metric */
static int
block_detail(uint8_t *a, int as, int w, int h, unsigned *avg)
{
    PSY_COEFS psy;

    if (w == 0 || h == 0) {
        return 0;
    }

    *avg = block_avg(a, as, w, h);
    psy.err_weight = 0;
    psy.tex_weight = 0;
    psy.avg_weight = 0;
    return intra_metr_wxh(a, as, *avg, w, h, &psy);
}

/* downsample block */
static void
dsb(uint8_t *a, int as, int bw, int bh, uint8_t *out, int os)
{
    int i, j;
    uint8_t *abp0, *abp1;

    bw /= 2;
    bh /= 2;
    for (j = 0; j < bh; j++) {
        abp0 = a;
        abp1 = a + as;
        for (i = 0; i < bw; i++) {
            out[i] = DSV_UAVG4(abp0[0], abp0[1], abp1[0], abp1[1]);
            abp0 += 2;
            abp1 += 2;
        }
        out += os;
        a += 2 * as;
    }
}

static void
c_average(DSV_PLANE *p, int x, int y, int w, int h, int *uavg, int *vavg)
{
    DSV_PLANE *u = &p[1];
    DSV_PLANE *v = &p[2];
    int i, j, su = 0, sv = 0;
    uint8_t *ptrU, *ptrV;

    ptrU = DSV_GET_XY(u, x, y);
    ptrV = DSV_GET_XY(v, x, y);
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

#define BUF_DIM (SP_DIM + 1)
/* upsample reference block 4x (to qpel resolution) */
static void
subpel4x(uint8_t *dec, uint8_t *ref, int rs)
{
    int16_t buf[(BUF_DIM + 3) * BUF_DIM];
    uint8_t halfpel[HP_DIM + 2][HP_DIM + 2];
    int i, j;

    for (j = 0; j < (BUF_DIM + 3); j++) {
        uint8_t *r = ref + (j - 1) * rs;
        for (i = 0; i < BUF_DIM; i++) {
            buf[i + j * BUF_DIM] = DSV_HPF_ME(r[i - 1], r[i], r[i + 1], r[i + 2]);
        }
    }
    /* hpel via 4-tap filter */
    for (j = 0; j < BUF_DIM; j++) {
        uint8_t *r = ref;
        for (i = 0; i < BUF_DIM; i++) {
            int i2, j2;
            int h, v, hv;
            int x = i + j * BUF_DIM;

            h  = DSV_HPF_ME(r[i - 1], r[i], r[i + 1], r[i + 2]);
            v  = DSV_HPF_ME(r[i - rs], r[i], r[i + rs], r[i + 2 * rs]);
            hv = DSV_HPF_ME(buf[x + 0 * BUF_DIM], buf[x + BUF_DIM], buf[x + 2 * BUF_DIM], buf[x + 3 * BUF_DIM]);

            i2 = i * 2;
            j2 = j * 2;

            halfpel[j2 + 0][i2 + 0] = r[i];
            halfpel[j2 + 0][i2 + 1] = clamp_u8((h + DSV_ME_HP_ADD) >> DSV_ME_HP_SHF);
            halfpel[j2 + 1][i2 + 0] = clamp_u8((v + DSV_ME_HP_ADD) >> DSV_ME_HP_SHF);
            halfpel[j2 + 1][i2 + 1] = clamp_u8((hv + (1 << (DSV_ME_HP_SHF + DSV_ME_HP_SHF - 1))) >> (DSV_ME_HP_SHF + DSV_ME_HP_SHF));
        }
        ref += rs;
    }
    /* qpel via bilinear */
    for (j = 0; j < HP_DIM; j++) {
        uint8_t *d = dec;
        for (i = 0; i < HP_DIM; i++) {
            int p, rx, ry, rxy;

            p   = halfpel[j][i];
            rx  = halfpel[j][i + 1];
            ry  = halfpel[j + 1][i];
            rxy = halfpel[j + 1][i + 1];

            d[0] = p;
            d[1] = UAVG2(p, rx);
            d[QP_STRIDE + 0] = UAVG2(p, ry);
            d[QP_STRIDE + 1] = DSV_UAVG4(p, rx, ry, rxy);

            d += 2;
        }
        dec += 2 * QP_STRIDE; /* skip a row */
    }
}

static void
haar(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl)
{
    DSV_SBC *os, *od, *dpLL, *dpLH, *dpHL, *dpHH;
    int x, y, woff, hoff, ws, hs, idx;

    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    os = src;
    od = dst;

    dpLL = dst;
    dpLH = dst + woff;
    dpHL = dst + hoff * width;
    dpHH = dst + woff + hoff * width;
    for (y = 0; y < hs; y += 2) {
        DSV_SBC *spA, *spB;

        spA = src + y * width;
        spB = spA + width;
        for (x = 0, idx = 0; x < ws; x += 2, idx++) {
            int x0, x1, x2, x3, s0, s1, d0, d1;

            x0 = spA[x + 0];
            x1 = spA[x + 1];
            x2 = spB[x + 0];
            x3 = spB[x + 1];

            s0 = x0 + x1;
            s1 = x2 + x3;
            d0 = x0 - x1;
            d1 = x2 - x3;

            dpLL[idx] = s0 + s1; /* LL */
            dpLH[idx] = d0 + d1; /* LH */
            dpHL[idx] = s0 - s1; /* HL */
            dpHH[idx] = d0 - d1; /* HH */
        }
        dpLL += width;
        dpLH += width;
        dpHL += width;
        dpHH += width;
    }
    ws *= sizeof(DSV_SBC);
    while (hs-- > 0) {
        memcpy(os, od, ws);
        od += width;
        os += width;
    }
}

typedef struct {
    unsigned total_energy, total_energy_dc, total_energy_ac; /* src + rec combined */
    unsigned energy_src, energy_rec; /* DC + AC combined */
} HAAR_REPORT;

static void
haar_energy(DSV_HME *hme, uint8_t *a, int as, uint8_t *b, int bs, int w, int h,
        int intra, unsigned ratio, HAAR_REPORT *report)
{
    int lvls, l, s, x, y, oddw, oddh;
    DSV_SBC temp_src[SQR(DSV_MAX_BLOCK_SIZE)];
    DSV_SBC temp_dst[SQR(DSV_MAX_BLOCK_SIZE)];
    unsigned mx, en_src = 0, en_rec = 0;
    unsigned q = hme->avg_quant;
    DSV_SBC *d;
    DSV_FMETA fm;

    memset(&fm, 0, sizeof(fm));
    if (w == 0 || h == 0) {
        memset(report, 0, sizeof(*report));
        return;
    }

    fm.params = hme->params;
    fm.isP = 1;
    fm.cur_plane = 0;

    oddw = w & 1;
    oddh = h & 1;
    /* make even */
    w += oddw;
    h += oddh;
    d = temp_src;
    /* technically we should be checking EPRM versions here if EPRM... TODO */
    if (intra >= 0) {
        for (y = 0; y < h - oddh; y++) {
            for (x = 0; x < w - oddw; x++) {
                d[x] = (a[x] - intra);
            }
            if (oddw) {
                d[x] = 0;
            }
            a += as;
            d += w;
        }
    } else {
        for (y = 0; y < h - oddh; y++) {
            for (x = 0; x < w - oddw; x++) {
                d[x] = (a[x] - b[x]);
            }
            if (oddw) {
                d[x] = 0;
            }
            a += as;
            b += bs;
            d += w;
        }
    }
    if (oddh) {
        memset(d, 0, sizeof(DSV_SBC) * w);
    }
    mx = MAX(w, h);
    lvls = dsv_lb2(mx);
    if (mx > (1 << lvls)) {
        lvls++;
    }
    for (l = 1; l <= lvls; l++) {
        haar(temp_src, temp_dst, w, h, l);
    }
    report->total_energy_dc = 0;
    report->total_energy_ac = 0;
    for (l = 0; l < lvls; l++) {
        /* approximately match P quant (hzcc.c) */
        unsigned lq = q;
        if (l >= 4) {
            lq = dsv_lfquant(&fm, q);
        } else {
            lq = dsv_hfquant(&fm, q, -1, 3 - l);
        }
        lq = (1 << 15) / (lq * 2);
        /* go thru LL,LH,HL,HH subbands, skip LL for all but the lowest freq */
        for (s = (l ? 1 : 0); s < 4; s++) {
            DSV_SBC *sp;
            int ws, hs, offset = 0;
            unsigned sb_energy_src = 0;
            unsigned sb_energy_rec = 0;
            unsigned sq = (s == 0) ? (1 << 15) : lq;

            ws = DSV_ROUND_SHIFT(w, lvls - l);
            hs = DSV_ROUND_SHIFT(h, lvls - l);
            if (s & 1) { /* L */
                offset += ws;
            }
            if (s & 2) { /* H */
                offset += hs * w;
            }
            sp = temp_src + offset;
            for (y = 0; y < hs; y++) {
                for (x = 0; x < ws; x++) {
                    unsigned tmp = abs(sp[x]);
                    sb_energy_src += tmp;
                    sb_energy_rec += tmp * sq >> 15;
                }
                sp += w;
            }
            if (s == 0) {
                report->total_energy_dc = sb_energy_src + sb_energy_rec;
            } else {
                report->total_energy_ac += sb_energy_src + sb_energy_rec;
            }
            if (intra < 0 && (s != 0)) {
                /* inter, so factor the subpel ratio into the energy computation */
                sb_energy_src = (sb_energy_src * ratio) >> 5;
                sb_energy_rec = (sb_energy_rec * ratio) >> 5;
            }

            en_src += sb_energy_src;
            en_rec += sb_energy_rec;
        }
    }
    report->energy_src = en_src;
    report->energy_rec = en_rec;
    report->total_energy = en_src + en_rec;
}

#define INTRA_SB_FAC(var) (255 - clamp_u8(128 - abs((dsv_flb2(var) >> 3) - 128)))
#define INTER_SB_FAC(var, err) clamp_u8((dsv_flb2(((var) + 1) * 16) * 2) / (1 + (err)))

/* compute per-subblock factors used for AQ in hzcc.c */
static void
calc_sb_facs(uint8_t *facs, int nxb, unsigned vars[4], unsigned luma_resid[4], unsigned yarea)
{
    int sb00, sb10, sb01, sb11;
    sb00 = 0;
    sb10 = 1;
    sb01 = nxb * 2;
    sb11 = 1 + nxb * 2;
    if (luma_resid == NULL) {
        /* intra */
        facs[sb00] = INTRA_SB_FAC(vars[0] * 4 / yarea);
        facs[sb10] = INTRA_SB_FAC(vars[1] * 4 / yarea);
        facs[sb01] = INTRA_SB_FAC(vars[2] * 4 / yarea);
        facs[sb11] = INTRA_SB_FAC(vars[3] * 4 / yarea);
        return;
    }
    /* inter */
    facs[sb00] = INTER_SB_FAC(vars[0], luma_resid[0]);
    facs[sb10] = INTER_SB_FAC(vars[1], luma_resid[1]);
    facs[sb01] = INTER_SB_FAC(vars[2], luma_resid[2]);
    facs[sb11] = INTER_SB_FAC(vars[3], luma_resid[3]);
}

/* full-pel error of luma and chroma */
static void
yuv_residual_energy(DSV_HME *hme, unsigned err_src[3], unsigned err_rec[3], unsigned luma_sub[4], DSV_FRAME *src, DSV_FRAME *ref,
        int bx, int by, int brx, int bry, int bw, int bh,
        int cbx, int cby, int cbrx, int cbry, int cbw, int cbh, unsigned ratio)
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
        unsigned sub_src[4], sub_rec[4];
        int subpos = 0;

        memset(sub_src, 0, sizeof(sub_src));
        memset(sub_rec, 0, sizeof(sub_rec));
        for (g = 0; g <= bh; g += (bh + !bh)) {
            for (f = 0; f <= bw; f += (bw + !bw)) {
                uint8_t *src_d, *ref_d;
                HAAR_REPORT report;

                src_d = DSV_GET_XY(&sp[z], bx, by) + (f + g * sp[z].stride);
                ref_d = DSV_GET_XY(&rp[z], brx, bry) + (f + g * rp[z].stride);

                haar_energy(hme,
                        src_d, sp[z].stride,
                        ref_d, rp[z].stride, bw, bh, -1, ratio, &report);
                sub_src[subpos] = report.energy_src;
                sub_rec[subpos] = report.energy_rec;
                if (z == 0) {
                    luma_sub[subpos] = sub_src[subpos];
                }
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
        err_src[z] = MAX4(sub_src[0], sub_src[1], sub_src[2], sub_src[3]);
        err_rec[z] = MAX4(sub_rec[0], sub_rec[1], sub_rec[2], sub_rec[3]);
    }
}

#define N_SUBVARS (4 + 1 + 1)
static void
subblock_details(unsigned vars[N_SUBVARS], DSV_FRAME *src,
        int bx, int by, int bw, int bh,
        int cbx, int cby, int cbw, int cbh)
{
    int f, g, z;
    DSV_PLANE *sp;

    sp = src->planes;
    bw /= 2;
    bh /= 2;

    memset(vars, 0, N_SUBVARS * sizeof(unsigned));

    for (z = 0; z < 3; z++) {
        uint8_t *src_d;
        unsigned avg;

        if (z == 0) {
            int subpos = 0;

            for (g = 0; g <= bh; g += (bh + !bh)) {
                for (f = 0; f <= bw; f += (bw + !bw)) {
                    src_d = DSV_GET_XY(&sp[z], bx, by) + (f + g * sp[z].stride);
                    vars[subpos] = block_detail(src_d, sp[z].stride, bw, bh, &avg);
                    subpos++;
                }
            }
        } else {
            src_d = DSV_GET_XY(&sp[z], cbx, cby);
            vars[3 + z] = block_var(src_d, sp[z].stride, cbw, cbh, &avg);
        }
    }
}

static int
test_skip_mode(unsigned skip_block_thresh, DSV_MV *refmv, DSV_MV *mv,
        unsigned var_src, int nochange, int good_enough,
        unsigned ydif, unsigned udif, unsigned vdif, unsigned yarea, unsigned carea,
        unsigned residual_energy_src[3], unsigned quant_rd)
{
    unsigned cth, sth = 0;
    unsigned qs = (quant_rd >> 9);
    unsigned lowt;
    int temp_stab, nomotion, tshift = 0;

    sth = 64;
    if (good_enough) {
        sth += 16;
    }
    nomotion = (refmv && MV_LT(refmv, 4)) || !refmv;
    temp_stab = nomotion && MV_LT(mv, 4) && (good_enough || (refmv && DSV_MV_IS_INTRA(refmv)));
    sth *= iisqrt(MAX(var_src, 1) * qs);
    sth += yarea * skip_block_thresh;
    cth = (sth * carea / yarea);
    ydif *= yarea;
    udif *= carea;
    vdif *= carea;
    lowt = (32 - MIN(var_src / yarea, 32)) + 32;
    qs = CLAMP(qs, lowt, 64);
    sth = sth * qs;
    cth = cth * qs;
    tshift = 12 - (!!temp_stab + !!nochange) * 2;
    sth >>= tshift;
    cth >>= tshift;
    if ((residual_energy_src[0] + ydif) <= sth &&
        (residual_energy_src[1] + udif) <= cth &&
        (residual_energy_src[2] + vdif) <= cth
        ) {
        DSV_MV_SET_SKIP(mv, 1);
        mv->u.all = 0;
        mv->err[0] = 0;
        return 1;
    }
    return 0;
}

static void
test_subblock_intra_y(DSV_HME *hme, DSV_MV *mv,
        DSV_PLANE *srcp, DSV_PLANE *refp,
        unsigned detail_src, int neidif, unsigned ratio, int nochange,
        int bw, int bh, unsigned vars[N_SUBVARS], int is_fade)
{
    int f, g, sbw, sbh, sub_id;
    unsigned err_sub = 0, err_src = 0, err_inter = 0;
    unsigned mask_src = 0, mask_sub = 0;
    unsigned dc_w[4], dc_v[4];
    unsigned qsf; /* quant scale factor */
    unsigned static_factor;
    unsigned avg_quant;
    uint8_t bits[4] = {
            DSV_MASK_INTRA00,
            DSV_MASK_INTRA01,
            DSV_MASK_INTRA10,
            DSV_MASK_INTRA11,
    };
    avg_quant = hme->avg_quant;
    sbw = bw / 2;
    sbh = bh / 2;
    if (sbw == 0 || sbh == 0) {
        return;
    }

    /* increase detail bias proportionally to how similar the MV was to its neighbors */
    detail_src += detail_src / (neidif + 1);

    static_factor = ((nochange ? 4 : 1) * bw * bh * 8) / ((SQR(avg_quant) >> DSV_MAX_QP_BITS) + 1);

    sub_id = 0;
    for (g = 0; g <= sbh; g += (sbh + !sbh)) {
        for (f = 0; f <= sbw; f += (sbw + !sbw)) {
            uint8_t *src_d, *mvr_d;
            unsigned avg_src, avg_sub, local_detail;
            HAAR_REPORT inter_report, src_report, sub_report;
            unsigned src_bias; /* bias against DC intra */
            int sub_better, src_better;

            src_d = srcp->data + (f + g * srcp->stride);
            mvr_d = refp->data + (f + g * refp->stride);

            avg_src = block_avg(src_d, srcp->stride, sbw, sbh);
            avg_sub = block_avg(mvr_d, refp->stride, sbw, sbh);

            haar_energy(hme, src_d, srcp->stride, mvr_d, refp->stride, /* inter test */
                          sbw, sbh, -1, ratio, &inter_report);
            err_inter += inter_report.total_energy;

            local_detail = vars[sub_id];

            qsf = (SQR(MIN(avg_quant, 1024)) + local_detail) / (4 * bw * bh);

            src_bias = (SQR(MIN(local_detail + 64, 65535)) / MAX(SQR(avg_quant) >> DSV_MAX_QP_BITS, 1));
            haar_energy(hme, src_d, srcp->stride, NULL, 0, /* intra test */
                         sbw, sbh, avg_src, -1, &src_report);
            src_report.total_energy += src_bias;
            haar_energy(hme, src_d, srcp->stride, NULL, 0, /* intra test */
                         sbw, sbh, avg_sub, -1, &sub_report);

            /* add block detail to expression to bias intra decision towards less detailed blocks */
            src_better = (src_report.total_energy + qsf + local_detail + static_factor) < inter_report.total_energy;
            sub_better = (sub_report.total_energy + qsf + local_detail + static_factor) < inter_report.total_energy;
            if (src_better) {
                mask_src |= bits[sub_id];
            }
            if (sub_better) {
                mask_sub |= bits[sub_id];
            }
            err_src += src_report.total_energy;
            err_sub += sub_report.total_energy;
            dc_w[sub_id] = src_report.total_energy;
            dc_v[sub_id] = avg_src;
            sub_id++;
        }
    }

    if (mask_src || mask_sub) {
        int sub_better, src_better;
        unsigned minvar = MIN4(vars[0], vars[1], vars[2], vars[3]);
        sub_better = (err_sub + minvar + static_factor) < 2 * err_inter;
        src_better = (err_src + minvar + static_factor) < 2 * err_inter;

        if (sub_better || src_better) {
            if (src_better && (err_src < 2 * err_sub)) {
                unsigned best_dc;
                unsigned i, w, dc_tot = 0;
                best_dc = 0;
                for (i = 0; i < 4; i++) {
                    w = MAX(dc_w[i], 1);
                    w = ((1 << 19) / w);  /* inverse weights */
                    dc_tot += w;
                    best_dc += w * dc_v[i];
                }
                best_dc /= dc_tot;

                DSV_ASSERT(best_dc >= 0 && best_dc <= 255);
                /* if fading, then do all intra so that a fade is actually performed
                 * instead of having one or two subblocks a different color
                 */
                mv->submask = is_fade ? DSV_MASK_ALL_INTRA : mask_src;
                mv->dc = best_dc | DSV_SRC_DC_PRED;
            } else if (sub_better) {
                mv->submask = is_fade ? DSV_MASK_ALL_INTRA : mask_sub;
                mv->dc = 0;
            }
            if (mv->submask) {
                DSV_MV_SET_INTRA(mv, 1);
            }
        }
    }
}

static void
test_subblock_intra_c(
        DSV_PARAMS *params, DSV_MV *refmv, DSV_MV *mv, unsigned residual_energy[3],
        int bw, int bh, unsigned luma_err, unsigned detail_src, int nochange, unsigned quant)
{
    unsigned add, cerr;
    if (params->effort < 6) {
        return;
    }
    if (refmv && MV_LT(mv, 4) && MV_LT(refmv, 4) && DSV_MV_IS_INTRA(refmv)) {
        return;
    }
    if (residual_energy[0] < ((quant >> (DSV_MAX_QP_BITS - 3)) * detail_src / 32)) {
        return;
    }

    add = (MV_MAG(mv) + 1) * luma_err;
    if (nochange) {
        add *= 4;
    }
    add /= bw * bh;
    cerr = SQR((residual_energy[1] + residual_energy[2])) * 32 / (MAX(quant, SQR(256)) + luma_err);
    if (cerr > MAX(add, 32)) {
        mv->submask = DSV_MASK_ALL_INTRA;
        DSV_MV_SET_INTRA(mv, 1);
    }
}

#define NNEIGH 9
static void
get_neighbors(DSV_MV *vecs, DSV_PARAMS *p, int x, int y, int step, DSV_MV *n, int isref)
{
    int i;

    for (i = 0; i < NNEIGH; i++) {
        n[i].u.all = 0;
    }
    if (isref) {
        n[0] = vecs[y * p->nblocks_h + x]; /* zero */
        if (x < (p->nblocks_h - step)) { /* right */
            n[1] = vecs[y * p->nblocks_h + (x + step)];
        }
        if (x >= step && y < (p->nblocks_v - step)) { /* bottom-left */
            n[2] = vecs[(y + step) * p->nblocks_h + (x - step)];
        }
        if (y < (p->nblocks_v - step)) { /* bottom */
            n[3] = vecs[(y + step) * p->nblocks_h + x];
        }
        if (x < (p->nblocks_h - step) && y < (p->nblocks_v - step)) { /* bottom-right */
            n[4] = vecs[(y + step) * p->nblocks_h + (x + step)];
        }
    } else {
        if (x >= (2 * step)) { /* left-1 */
            n[0] = vecs[y * p->nblocks_h + (x - (2 * step))];
        }
        if (y >= (2 * step)) { /* top-1 */
            n[1] = vecs[(y - (2 * step)) * p->nblocks_h + x];
        }
        if (x >= (2 * step) && y >= step) { /* top-left-1 */
            n[2] = vecs[(y - step) * p->nblocks_h + (x - (2 * step))];
        }
        if (x < (p->nblocks_h - step) && y >= (2 * step)) { /* top-right -1 */
            n[3] = vecs[(y - (2 * step)) * p->nblocks_h + (x + step)];
        }
        if (x < (p->nblocks_h - (2 * step)) && y >= step) { /* top-right +1 */
            n[4] = vecs[(y - step) * p->nblocks_h + (x + (2 * step))];
        }
    }
    if (x >= step) { /* left */
        n[5] = vecs[y * p->nblocks_h + (x - step)];
    }
    if (y >= step) { /* top */
        n[6] = vecs[(y - step) * p->nblocks_h + x];
    }
    if (x >= step && y >= step) { /* top-left */
        n[7] = vecs[(y - step) * p->nblocks_h + (x - step)];
    }
    if (x < (p->nblocks_h - step) && y >= step) { /* top-right */
        n[8] = vecs[(y - step) * p->nblocks_h + (x + step)];
    }
}

static int
calc_zero_bias(DSV_MV *mvf, DSV_MV *refmv, int i, int j, DSV_PARAMS *params, int qthresh)
{
    /* correlate local motion for jitter suppression */
    DSV_MV nei[NNEIGH];
    DSV_MV neiref[NNEIGH];
    int ne, nz = 0;
    int zero_bias = 0;

    if (qthresh == 0) {
        return 0;
    }
    get_neighbors(mvf, params, i, j, 1, nei, 0);
    if (refmv) {
        get_neighbors(refmv, params, i, j, 1, neiref, 1);
    }
    for (ne = 0; ne < NNEIGH; ne++) {
        if (nei[ne].u.all == 0) {
            nz += 1;
        }
        if (refmv && neiref[ne].u.all == 0) {
            nz += ((ne == 0) ? 2 : 1);
        }
    }
    zero_bias = nz * qthresh / (2 * (NNEIGH - 1));
    zero_bias = SQR(zero_bias) / qthresh;
    return MAX(zero_bias, 0);
}

static unsigned
subpixel_ME(
        DSV_HME *hme,
        DSV_MV *mvf, /* motion vector field */
        DSV_MV *mv, /* OUTPUT: will return just the subpel components */
        int fpelx, int fpely,
        DSV_FRAME *src, DSV_FRAME *ref,
        int i, int j,
        unsigned best, unsigned var_src, unsigned qthresh,
        int bx, int by, int bw, int bh, PSY_COEFS *psy)
{
    uint8_t tmpq[(4 + QP_STRIDE) * (4 + QP_STRIDE)];
    static READONLY int dx[4] = { 1, -1, 0,  0 };
    static READONLY int dy[4] = { 0,  0, 1, -1 };
    DSV_PLANE srcp, refp, srcsp, refsp;
    int xx, yy, n;
    unsigned score, quad[4];
    uint8_t *imq;
    unsigned yarea, ms1, ms2;
    int pri[2], sec[2], diag[2], bestv[2];
    int *testv[3];
    int area_ratio, iarea_ratio;
    DSV_PARAMS *params = hme->params;
    int zero_bias = 0;

    mv->u.mv.x = 0; /* set default result as full pel */
    mv->u.mv.y = 0;

    if (best == 0) {
        return best;
    }

    if (fpelx == 0 && fpely == 0) {
        zero_bias = calc_zero_bias(mvf, hme->ref_mvf, i, j, params, qthresh);
        if ((unsigned) zero_bias >= best) {
            return best;
        }
        best -= zero_bias;
    }
    testv[0] = pri;
    testv[1] = sec;
    testv[2] = diag;
    yarea = bw * bh;
    dsv_plane_xy(src, &srcp, 0, bx, by);
    dsv_plane_xy(ref, &refp, 0, bx + fpelx, by + fpely);

    for (n = 0; n < 4; n++) {
        quad[n] = fastmetr(srcp.data, srcp.stride,
                DSV_GET_XY(&refp, dx[n], dy[n]), refp.stride, bw, bh, psy);
    }

    /* scale down to match area */
    area_ratio = 8 * (SP_SAD_SZ * SP_SAD_SZ) / yarea;
    iarea_ratio = 8 * yarea / (SP_SAD_SZ * SP_SAD_SZ);
    best = best * area_ratio >> 3;

    xx = bx + ((bw >> 1) - ((SP_SAD_SZ + 1) / 2));
    yy = by + ((bh >> 1) - ((SP_SAD_SZ + 1) / 2));
    dsv_plane_xy(src, &srcsp, 0, xx, yy);
    dsv_plane_xy(ref, &refsp, 0,
            xx + fpelx - 1, /* offset by 1 in x and y so */
            yy + fpely - 1);/* we can do negative hpel */
    /* interpolate into half-pel then into quarter-pel */
    subpel4x(tmpq, refsp.data, refsp.stride);
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
        int evx, evy, t[2];
        unsigned qpbias = 0;
        int qpel;

        if (n == 6) {
            t[0] = pri[0] + diag[0];
            t[1] = pri[1] + diag[1];
        } else {
            int hpel = !(n & 1);
            t[0] = testv[n >> 1][0] * (1 << hpel);
            t[1] = testv[n >> 1][1] * (1 << hpel);
        }
        qpel = (t[0] | t[1]) & 1;
        if (qpel && params->effort < 8) { /* skip qpel at low effort */
            continue;
        }
        score = qpsad(srcsp.data, srcsp.stride, imq + t[0] + t[1] * QP_STRIDE, psy);

        evx = MK_MV_COMP(fpelx, 0, t[0]);
        evy = MK_MV_COMP(fpely, 0, t[1]);
        score += mv_cost(mvf, params, i, j, evx, evy, hme->avg_quant, 0);
        qpbias = (var_src * ((t[0] & 1) + (t[1] & 1) + 1) / 128); /* bias against qpel depending on source detail */

        if (best > score + qpbias) {
            best = score;
            SETVV(bestv, t);
        }
    }
    mv->u.mv.x = bestv[0];
    mv->u.mv.y = bestv[1];
    best = best * iarea_ratio >> 3;

    /* add bias back so it doesn't influence error analysis */
    return best + zero_bias;
}

static int
remove_dupes(DSV_MV **list, int n)
{
    int i, j, newn = 0;

    for (j = 0; j < n; j++) {
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
    int ne, step;
    DSV_MV nei[NNEIGH];

    step = 1 << level;
    if (level == 0) {
        DSV_MV tmv;
        int px, py;

        dsv_movec_pred(mvf, p, i, j, &px, &py);
        tmv.u.mv.x = px;
        tmv.u.mv.y = py;
        n = add_mv_to_list(hme, list, n, &tmv);
    }

    get_neighbors(mvf, p, i, j, step, nei, 0);
    for (ne = 0; ne < NNEIGH; ne++) {
        n = add_mv_to_list(hme, list, n, &nei[ne]);
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
        DSV_MV nei[NNEIGH];
        int ne;

        get_neighbors(ref_mvf, p, i, j, step, nei, 1);
        for (ne = 0; ne < NNEIGH; ne++) {
            n = add_mv_to_list(hme, list, n, &nei[ne]);
        }
    }
    return n;
}

/* prune vectors that don't conform with the rest and take the new list's average */
static int
find_inliers(DSV_MV **list, DSV_MV **newl, int n, int *ax, int *ay)
{
    int i;
    unsigned dist[16], avgd = 0, sad = 0, thresh;
    int avgx, avgy;
    int nin = 0;

    if (n == 0) {
        return 0;
    }
    if (n >= 16) {
        DSV_ASSERT(0);
        return 0;
    }
    avgx = *ax;
    avgy = *ay;
    for (i = 0; i < n; i++) {
        dist[i] = (ABSDIF(list[i]->u.mv.x, avgx) + ABSDIF(list[i]->u.mv.y, avgy));
        avgd += dist[i];
    }
    avgd /= n;
    for (i = 0; i < n; i++) {
        sad += ABSDIF(dist[i], avgd);
    }
    thresh = avgd + sad / n; /* average dist + average absolute dist = thresh */
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
refine_best_fpel_cand(DSV_HME *hme, int level, int i, int j,
       /* both input and output: */ int *bestx, int *besty, unsigned *best,
        unsigned good_enough_thresh,
        DSV_PLANE *src_block,
        int bx, int by, int bw, int bh, PSY_COEFS *psy,
        int cbx, int cby, int cbw, int cbh)
{
    int k, tvx, tvy, step;
    unsigned score, metr[4];
    DSV_FRAME *ref;
    DSV_PLANE *rp;

    step = 1 << level;
    ref = hme->ref[level];
    rp = ref->planes + 0;

    metr[0] = metr[1] = metr[2] = metr[3] = UINT_MAX;

retry:
    for (k = 0; k < (N_SEARCH_PTS / 2 + 1); k++) {
        tvx = *bestx + rectx[k];
        tvy = *besty + recty[k];

        if (invalid_block(ref, bx + tvx, by + tvy, bw, bh, 0)) {
            continue;
        }

        score = hier_metr(level, src_block->data, src_block->stride,
                DSV_GET_XY(rp, bx + tvx, by + tvy), rp->stride, bw, bh, psy);
        if (hme->enc->do_chroma_me && level != 0) {
            int cpl, hs, vs;
            int cbmx, cbmy;
            unsigned cscore = 0;
            hs = DSV_FORMAT_H_SHIFT(hme->params->vidmeta->subsamp);
            vs = DSV_FORMAT_V_SHIFT(hme->params->vidmeta->subsamp);
            cbmx = cbx + DSV_SAR(tvx, hs);
            cbmy = cby + DSV_SAR(tvy, vs);
            for (cpl = 1; cpl <= 2; cpl++) {
                DSV_PLANE srccr;
                dsv_plane_xy(hme->src[level], &srccr, cpl, cbx, cby);
                cscore += hier_metr(level, srccr.data, srccr.stride,
                        DSV_GET_XY(ref->planes + cpl, cbmx, cbmy),
                        (ref->planes + cpl)->stride, cbw, cbh, &chroma_me_psy);
            }
            BLEND_SCORES(score, cscore);
        }

        if (k >= 1 && k <= 4) {
            metr[k - 1] = score;
        }
        if (DO_GOOD_ENOUGH && level == 0 && (!tvx && !tvy) && score <= good_enough_thresh) {
            *bestx = tvx;
            *besty = tvy;
            *best = score;
            return 1;
        }
        score += mv_cost(hme->mvf[level], hme->params, i, j,
                MK_MV_COMP(tvx * step, 0, 0),
                MK_MV_COMP(tvy * step, 0, 0), hme->avg_quant, level);
        if (*best > score) {
            *best = score;
            *bestx = tvx;
            *besty = tvy;
            goto retry;
        }
    }

    /* diagonal check */
    tvx = *bestx + rectx[(metr[0] <= metr[1]) ? 1 : 2];
    tvy = *besty + recty[(metr[2] <= metr[3]) ? 3 : 4];

    if (invalid_block(ref, bx + tvx, by + tvy, bw, bh, 0)) {
        return 0;
    }
    score = hier_metr(level, src_block->data, src_block->stride,
            DSV_GET_XY(rp, bx + tvx, by + tvy), rp->stride, bw, bh, psy);
    if (hme->enc->do_chroma_me && level != 0) {
        int cpl, hs, vs;
        int cbmx, cbmy;
        unsigned cscore = 0;
        hs = DSV_FORMAT_H_SHIFT(hme->params->vidmeta->subsamp);
        vs = DSV_FORMAT_V_SHIFT(hme->params->vidmeta->subsamp);
        cbmx = cbx + DSV_SAR(tvx, hs);
        cbmy = cby + DSV_SAR(tvy, vs);
        for (cpl = 1; cpl <= 2; cpl++) {
            DSV_PLANE srccr;
            dsv_plane_xy(hme->src[level], &srccr, cpl, cbx, cby);
            cscore += hier_metr(level, srccr.data, srccr.stride,
                    DSV_GET_XY(ref->planes + cpl, cbmx, cbmy),
                    (ref->planes + cpl)->stride, cbw, cbh, &chroma_me_psy);
        }
        BLEND_SCORES(score, cscore);
    }
    score += mv_cost(hme->mvf[level], hme->params, i, j,
            MK_MV_COMP(tvx * step, 0, 0),
            MK_MV_COMP(tvy * step, 0, 0), hme->avg_quant, level);
    if (*best > score) {
        unsigned obest = *best;
        *best = score;
        *bestx = tvx;
        *besty = tvy;
        if ((obest - (obest >> 2)) > score){
            goto retry;
        }
    }
    return 0;
}

static int
refine_level(DSV_HME *hme, int level, int gx, int gy)
{
    DSV_FRAME *src, *ref, *ogr;
    DSV_MV *mv;
    DSV_MV *mvf, *parent = NULL;
    DSV_PARAMS *params = hme->params;
    int i, j, y_w, y_h, nxb, nyb, step, hs, vs;
    unsigned parent_mask, total_err = 0, n_in_avg = 0;
    int nintra = 0; /* number of intra blocks */
    DSV_PLANE *sp, *rp;
    int ndiff = 0, num_eligible_blocks = 0;
    unsigned quant_rd = SQR(hme->avg_quant);
    int total_ivar = 0, total_var = 0;

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

    step = 1 << level;
    parent_mask = ~((step << 1) - 1);

    for (j = 0; j < nyb; j += step) {
        for (i = 0; i < nxb; i += step) {
            DSV_PLANE srcp;
            int dx, dy;
            int bx, by, bw, bh;
            int k, m, n = 0;
            DSV_MV *cands[MAX_CANDS];
            unsigned best, score_zero, score_la, score, best_score;
            unsigned qthresh, good_enough = 0;
            int lax = 0, lay = 0, motion_bias;
            int cbx, cby, cbw, cbh; /* chroma block */
            PSY_COEFS psy;
            unsigned var_src = 0, avg_src = 0;
            unsigned vars[N_SUBVARS];
            unsigned yarea;
            HAAR_REPORT zero_report;

            /* defaults */
            psy.err_weight = 3;
            psy.tex_weight = 0;
            psy.avg_weight = 0;

            bx = (i * y_w) >> level;
            by = (j * y_h) >> level;
            cbx = (i * y_w) >> (level + hs);
            cby = (j * y_h) >> (level + vs);
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
            yarea = bw * bh;
            ADD_MV_XY(cands, 0, 0);
            motion_bias = y_w * y_h;

            cbw = bw >> hs;
            cbh = bh >> vs;

            if (!SQUARED_LEVELS) {
                unsigned minvar, maxvar;
                int tvar;

                subblock_details(vars, src,
                        bx, by, bw, bh,
                        cbx, cby, cbw, cbh);
                minvar = MIN4(vars[0], vars[1], vars[2], vars[3]);
                maxvar = MAX4(vars[0], vars[1], vars[2], vars[3]);
                var_src = minvar + maxvar;
                avg_src = block_avg(srcp.data, srcp.stride, bw, bh);

                tvar = var_src + SQR(var_src >> 10);
                tvar = ((tvar * hme->avg_quant) / (bw * bh << 6));
                motion_bias = tvar / (2 + (abs(gx) + abs(gy)));
                if (var_src <= (unsigned) MIN(bw * bh * hme->avg_quant >> 8, 8 * bw * bh)) {
                    psy.err_weight = 2;
                    psy.tex_weight = 2;
                    psy.avg_weight = 1;
                } else if (var_src <= (unsigned) (bw * bh * hme->avg_quant >> 6)) {
                    psy.err_weight = 2;
                    psy.tex_weight = 2;
                    psy.avg_weight = 1;
                    motion_bias = 0; /* zero it to keep smooth motion smooth */
                } else {
                    psy.err_weight = 2;
                    psy.tex_weight = 1;
                    psy.avg_weight = 2;
                }
            }
            if (params->lossless) {
                psy.err_weight = 3;
                psy.tex_weight = 0;
                psy.avg_weight = 0;
            }
            if (parent != NULL) {
#define N_POINTS (1 + 8)
                static READONLY int pt[N_POINTS * 2] = { 0, 0,
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
                    lax = DSV_SAR(lax, level);
                    lay = DSV_SAR(lay, level);

                    n = add_spatial_predictions(level, i, j, n, cands, params, mvf, hme);
                    n = add_temporal_predictions(level, i, j, n, cands, params, hme);

                    ADD_MV_XY(cands, gx, gy);

                    for (m = 0; m < nl; m++) {
                        ADD_MV_XY(cands, newl[m]->u.mv.x, newl[m]->u.mv.y);
                    }
                }
            }
            /* scale candidates down to the resolution of the current pyramid level */
            for (k = 0; k < n; k++) {
                cands[k]->u.mv.x = DSV_SAR(cands[k]->u.mv.x, level);
                cands[k]->u.mv.y = DSV_SAR(cands[k]->u.mv.y, level);
            }
            DSV_ASSERT(n <= MAX_CANDS);
            /* we only care about unique non-zero vectors */
            n = remove_dupes(cands, n);

            best = 0;
            best_score = score_zero = score_la = UINT_MAX;
            /* find best candidate */
            for (k = 0; k < n; k++) {
                dx = cands[k]->u.mv.x;
                dy = cands[k]->u.mv.y;

                if (invalid_block(ref, bx + dx, by + dy, bw, bh, 0)) {
                    continue;
                }

                score = hier_metr(level, srcp.data, srcp.stride,
                        DSV_GET_XY(rp, bx + dx, by + dy),
                        rp->stride, bw, bh, &psy);
                if (hme->enc->do_chroma_me && level != 0) {
                    int cbmx, cbmy;
                    int cpl;
                    unsigned cscore = 0;

                    cbmx = cbx + DSV_SAR(dx, hs);
                    cbmy = cby + DSV_SAR(dy, vs);
                    for (cpl = 1; cpl <= 2; cpl++) {
                        DSV_PLANE srccr;
                        dsv_plane_xy(src, &srccr, cpl, cbx, cby);
                        cscore += hier_metr(level, srccr.data, srccr.stride,
                                DSV_GET_XY(ref->planes + cpl, cbmx, cbmy),
                                (ref->planes + cpl)->stride, cbw, cbh, &chroma_me_psy);
                    }
                    BLEND_SCORES(score, cscore);
                }

                if (dx == 0 && dy == 0) {
                    score_zero = score;
                }
                if (dx == lax && dy == lay) {
                    score_la = score;
                }
                score += mv_cost(mvf, params, i, j,
                        MK_MV_COMP(dx * step, 0, 0),
                        MK_MV_COMP(dy * step, 0, 0), hme->avg_quant, level);
                if (dx == lax && dy == lay) {
                    score = USUB(score, (motion_bias >> level));
                }
                if (dx == gx && dy == gy) {
                    score += 16 * yarea / MAX(k, 1); /* k can't be zero here but why not be extra safe? */
                }
                if (best_score > score) {
                    best_score = score;
                    best = k;
                }
            }

            dx = cands[best]->u.mv.x;
            dy = cands[best]->u.mv.y;
#define IN_THRESH(bestsc, refsc) refsc <= bestsc || ((unsigned) ABSDIF(bestsc, refsc) <= (unsigned) (var_src))
            if ((best_score < (avg_src < 64 ? (8 * yarea) : (4 * yarea))) && (score_la < (1 << 20))) {
                if (IN_THRESH(best_score, score_la)) {
                    dx = lax;
                    dy = lay;
                    best_score = score_la;
                }
            }
            mv = &mvf[i + j * nxb];
            memset(mv, 0, sizeof(*mv));

            best = best_score;
            m = 0;

            qthresh = (unsigned) (hme->avg_quant * bw * bh >> 11);

            if (DO_GOOD_ENOUGH) {
                unsigned minthresh, nzeros = NNEIGH;
                DSV_MV nei[NNEIGH];
                int ne, logfactor;
                int is_zero_fade = 0;

                /* compare to source reference frame */
                unsigned zoscore = fastmetr(srcp.data, srcp.stride,
                                    DSV_GET_XY(&ogr->planes[0], bx, by),
                                    ogr->planes[0].stride, bw, bh, &psy);
                if (hme->enc->do_chroma_me) {
                    int cpl;
                    unsigned cscore = 0;
                    for (cpl = 1; cpl <= 2; cpl++) {
                        DSV_PLANE srccr;
                        dsv_plane_xy(src, &srccr, cpl, cbx, cby);
                        cscore += fastmetr(srccr.data, srccr.stride,
                                DSV_GET_XY(ogr->planes + cpl, cbx, cby),
                                (ogr->planes + cpl)->stride, cbw, cbh, &chroma_me_psy);
                    }
                    BLEND_SCORES(zoscore, cscore);
                }
                get_neighbors(mvf, params, i, j, step, nei, 0);

                haar_energy(hme, srcp.data, srcp.stride, DSV_GET_XY(&ogr->planes[0], bx, by), ogr->planes[0].stride,
                              bw, bh, -1, 32, &zero_report);

                is_zero_fade = zero_report.total_energy_dc > 3 * zero_report.total_energy_ac;

                /* increase threshold depending on the motion of neighboring blocks */
                for (ne = 0; ne < NNEIGH; ne++) {
                    nzeros -= (nei[ne].u.all != 0);
                }
                logfactor = dsv_flb2(hme->avg_quant / 16);
                logfactor = SQR(logfactor) >> 16;
                if (hme->avg_quant < 768) {
                    logfactor = logfactor - (768 - i) / 8;
                }
                logfactor = MAX(1, (logfactor >> level));
                minthresh = ((is_zero_fade ? logfactor : 1) * (level + 1) * (bw * bh * nzeros) + 4) >> 3;
                qthresh = MAX(qthresh, minthresh);
                if (zoscore <= qthresh) {
                    /* bias towards zero vector */
                    best = (level == 0) ? score_zero : 0;
                    dx = 0;
                    dy = 0;
                    good_enough = 1;
                }
            }

            if (!good_enough) {
                /* try to improve upon the best candidate vector by
                 * searching in a rectangular fashion around it */
                good_enough = refine_best_fpel_cand(hme, level, i, j,
                        &dx, &dy, &best, qthresh,
                        &srcp,
                        bx, by, bw, bh, &psy, cbx, cby, cbw, cbh);
            }
            /* scale vector back to full-resolution */
            mv->u.mv.x = dx * step;
            mv->u.mv.y = dy * step;

            /* subpel refine at base level */
            if (level == 0) {
                int fpelx, fpely; /* full-pel MV coords */
                unsigned best_fp;

                fpelx = mv->u.mv.x;
                fpely = mv->u.mv.y;
                if (fpelx == lax && fpely == lay) {
                    best += motion_bias;
                }
                best_fp = best;
                mv->u.all = 0;
                if (params->effort >= 4) {

                    /* first search local average from parents */
                    if (!invalid_block(ref, bx + lax, by + lay, bw, bh, 4)) {
                        best = subpixel_ME(hme, mvf, mv, lax, lay, src, ref, i, j,
                                best_fp,
                                var_src, qthresh, bx, by, bw, bh, &psy);
                        if (mv->u.all) { /* found a subpel */
                            fpelx = lax;
                            fpely = lay;
                        }
                    }

                    if (!mv->u.all && !good_enough && !invalid_block(ref, bx + fpelx, by + fpely, bw, bh, 4)) {
                        /* if nothing so far, search final MV from HME */
                        best = subpixel_ME(hme, mvf, mv, fpelx, fpely, src, ref, i, j,
                                best_fp,
                                var_src, qthresh, bx, by, bw, bh, &psy);
                    }
                }

                mv->u.mv.x = MK_MV_COMP(fpelx, 0, mv->u.mv.x);
                mv->u.mv.y = MK_MV_COMP(fpely, 0, mv->u.mv.y);

                /* mode decision + block metric gathering
                 * src = source block
                 * ogr = original ref frame block at full-pel motion (x, y)
                 * ref = reconstructed ref frame block at full-pel motion (x, y)
                 */ {
                    DSV_PLANE refp, ogrp;
                    unsigned var_ref, avg_ref;
                    unsigned carea;
                    int cbmx, cbmy;
                    int eprmi, eprmd, eprmr;
                    int neidif, oob_vector; /* out of bounds */
                    unsigned ratio = 1 << 5; /* ratio of subpel_min_err / fullpel_min_err */
                    DSV_MV *refmv = NULL;
                    unsigned residual_energy_src[3];
                    unsigned residual_energy_rec[3];
                    unsigned luma_sub[4];
                    int nochange, doskip;
                    uint8_t *sb_facs = hme->enc->sb_facs + (i * 2) + (j * 2) * nxb * 2;
                    unsigned doskipthresh = ((1 << 30) / (SQR(hme->avg_quant) + 1));

                    if (hme->ref_mvf != NULL) {
                        refmv = &hme->ref_mvf[i + j * nxb];
                    }
                    if (DSV_IS_SUBPEL(mv)) {
                        ratio = (best << 5) / (best_fp + !best_fp);
                    }
                    dsv_plane_xy(ogr, &ogrp, 0, bx + fpelx, by + fpely);
                    dsv_plane_xy(ref, &refp, 0, bx + fpelx, by + fpely);

                    var_ref = block_detail(refp.data, refp.stride, bw, bh, &avg_ref);

                    cbmx = cbx + DSV_SAR(fpelx, hs);
                    cbmy = cby + DSV_SAR(fpely, vs);
                    carea = cbw * cbh;

                    DSV_MV_SET_SIMCMPLX(mv, 0);

                    oob_vector = outofbounds(i, j, nxb, nyb, y_w, y_h, mv);
                    neidif = dsv_neighbordif(mvf, params, i, j);
                    yuv_residual_energy(hme, residual_energy_src, residual_energy_rec, luma_sub, src, ref,
                            bx, by, bx + fpelx, by + fpely, bw, bh,
                            cbx, cby, cbmx, cbmy, cbw, cbh, ratio);

                    calc_sb_facs(sb_facs, nxb, vars, luma_sub, yarea);

                    /* test skip mode */
                    nochange = !hme->enc->changemap[i + j * nxb];

                    doskipthresh = CLAMP(doskipthresh, 0, 256);
                    doskip = (var_src >= doskipthresh || var_ref >= doskipthresh);
                    if (doskip && (good_enough || mv->u.all == 0) && hme->enc->skip_block_thresh >= 0 && !params->lossless) {
                        int uavg_src, vavg_src, uavg_ref, vavg_ref;

                        c_average(sp, cbx, cby, cbw, cbh, &uavg_src, &vavg_src);
                        c_average(rp, cbmx, cbmy, cbw, cbh, &uavg_ref, &vavg_ref);

                        if (test_skip_mode(
                                hme->enc->skip_block_thresh, refmv, mv,
                                var_src, nochange, good_enough,
                                SQR(ABSDIF(avg_src, avg_ref)),
                                SQR(ABSDIF(uavg_src, uavg_ref)),
                                SQR(ABSDIF(vavg_src, vavg_ref)),
                                yarea, carea,
                                residual_energy_src,  quant_rd
                                )) {
                            /* don't bother with anything else if we're skipping the block anyway */
                            goto skip;
                        }
                    }

                    /* see if we can afford to zero out the residuals */
                    if (DO_NOXMIT && var_src > (8 * yarea) && (!good_enough || mv->u.all) && !oob_vector && !params->lossless) {
                        unsigned xth = (quant_rd >> 19) * yarea;

                        xth += var_ref;
                        xth = USUB(xth, (yarea * neidif * 2));
                        xth = xth * hme->avg_quant >> (DSV_MAX_QP_BITS + 3);

                        if (avg_src < 64) {
                            xth = (xth * avg_src) >> 6;
                        }
                        if (residual_energy_rec[0] < xth) {
                            DSV_MV_SET_NOXMITY(mv, 1);
                        }

                        xth = xth * carea / (8 * yarea);
                        if (residual_energy_rec[1] < xth && residual_energy_rec[2] < xth) {
                            DSV_MV_SET_NOXMITC(mv, 1);
                        }

#if NO_RESIDUALS
                        DSV_MV_SET_NOXMITY(mv, 1);
#if NO_RESIDUALS == 2
                        DSV_MV_SET_NOXMITC(mv, 1);
#endif
#endif
                    }
                    if (!DSV_MV_IS_INTRA(mv)) {
                        unsigned intra_metr = intra_metr_wxh(srcp.data, srcp.stride, avg_src, bw, bh, &psy);
                        if (best < intra_metr) {
                            DSV_MV_SET_SIMCMPLX(mv, 1);
                        }
                    }
#if 1 /* have intra blocks */
                    if (!DSV_MV_IS_NOXMITY(mv)) {
                        int is_fade;
                        HAAR_REPORT mv_report;
                        haar_energy(hme, srcp.data, srcp.stride, ogrp.data, ogrp.stride, /* inter test with source reference */
                                    bw, bh, -1, ratio, &mv_report);

                        is_fade = mv_report.total_energy_dc > 3 * mv_report.total_energy_ac;

                        test_subblock_intra_y(hme, mv,
                                &srcp, &refp,
                                var_ref, neidif, ratio, nochange,
                                bw, bh, vars, is_fade);
                    }

                    if (!DSV_MV_IS_NOXMITC(mv)) {
                        test_subblock_intra_c(params, refmv, mv, residual_energy_src, bw, bh,
                                best, var_ref, nochange, quant_rd);
                    }
#endif
                    calc_EPRM(&srcp, &refp, mv->dc & 0xff, avg_ref, bw, bh, &eprmi, &eprmd, &eprmr);

                    /* end-of-block stats */
                    {
                        unsigned merr = DSV_UDIV_ROUND(best, yarea);
                        mv->err[0] = merr;
                        total_err += merr;
                        if (merr) {
                            n_in_avg++;
                        }
                    }
                    /* more difference, more likely to need a scene change */
                    ndiff += (zero_report.total_energy * hme->avg_quant / ((DSV_MAX_QP + 1) * yarea) > 11);
skip:
                    if (best > 0) {
                        num_eligible_blocks++;
                    }
                    total_var += USUB(var_src, var_ref) * 256 / yarea;
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
                        total_ivar += var_src * 256 / yarea;
                    } else {
                        if (mv->submask) {
                            DSV_ASSERT(0); /* should never happen because inter blocks don't have submasks */
                        }
                        DSV_MV_SET_EPRM(mv, !!eprmr);
                    }
                }
            }
        }
    }
    if (level == 0) {
        unsigned err_var = 0;
        if (num_eligible_blocks == 0) {
            num_eligible_blocks = 1;
        }
        hme->mes->scene_change_blocks = ndiff * 100 / num_eligible_blocks;
        hme->mes->avg_err = total_err / MAX(n_in_avg, 1);
        for (j = 0; j < nyb; j++) {
            for (i = 0; i < nxb; i++) {
                mv = &mvf[i + j * nxb];
                err_var += ABSDIF(mv->err[0], hme->mes->avg_err);
            }
        }
        hme->mes->var_err = err_var / MAX(n_in_avg, 1);
        hme->mes->tot_var = total_var / (nxb * nyb);
        hme->mes->tot_ivar = total_ivar * nintra / (nxb * nyb);
    }
    return 0;
}

extern DSV_MV *
dsv_intra_analysis(DSV_FRAME *src, DSV_PARAMS *params, uint8_t *sb_facs)
{
    int i, j, y_w, y_h, nxb, nyb, scale;
    DSV_MV *ba;
    unsigned bound_scale;
    y_w = params->blk_w;
    y_h = params->blk_h;

    nxb = params->nblocks_h;
    nyb = params->nblocks_v;

    ba = dsv_alloc(nxb * nyb * sizeof(DSV_MV));
    scale = dsv_spatial_psy_factor(params, -1);
    bound_scale = CLAMP(scale, 256, 512);
    for (j = 0; j < nyb; j++) {
        for (i = 0; i < nxb; i++) {
            DSV_PLANE srcp;
            int bx, by, bw, bh;
            DSV_MV *mv;
            unsigned var_t;
            int cbx, cby, cbw, cbh, subsamp;
            CHROMA_PSY cpsy;
            int maintain = 1;
            int keep_hf = 1;
            int ringing = 0;
            unsigned yarea;
            unsigned avgvar, maxvar = 0;
            uint8_t ds2[SQR(DSV_MAX_BLOCK_SIZE / 2 + 1)];
            uint8_t ds4[SQR(DSV_MAX_BLOCK_SIZE / 4 + 1)];
            unsigned vars[N_SUBVARS];

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
            yarea = bw * bh;

            subsamp = params->vidmeta->subsamp;
            cbx = i * (y_w >> DSV_FORMAT_H_SHIFT(subsamp));
            cby = j * (y_h >> DSV_FORMAT_V_SHIFT(subsamp));
            cbw = bw >> DSV_FORMAT_H_SHIFT(subsamp);
            cbh = bh >> DSV_FORMAT_V_SHIFT(subsamp);

            subblock_details(vars, src,
                    bx, by, bw, bh,
                    cbx, cby, cbw, cbh);
            avgvar = DSV_UAVG4(vars[0], vars[1], vars[2], vars[3]);
            maxvar = MAX4(vars[0], vars[1], vars[2], vars[3]);

            calc_sb_facs(sb_facs + (i * 2) + (j * 2) * nxb * 2, nxb, vars, NULL, yarea);

            avgvar = USUB(vars[0] + vars[1] + vars[2] + vars[3] + vars[4] + vars[5], avgvar);

            dsb(srcp.data, srcp.stride, bw, bh, ds2, DSV_MAX_BLOCK_SIZE / 2);
            dsb(ds2, DSV_MAX_BLOCK_SIZE / 2, bw / 2, bh / 2, ds4, DSV_MAX_BLOCK_SIZE / 4);
            if (params->do_psy & (DSV_PSY_ADAPTIVE_RINGING | DSV_PSY_CONTENT_ANALYSIS)) {
                int skip_tones;
                int uavg, vavg;
                unsigned dum0, dum1;
                unsigned var2, var4;
                unsigned luma_avg;

                var2 = block_var(ds2, DSV_MAX_BLOCK_SIZE / 2, bw / 2, bh / 2, &dum0);
                var4 = block_var(ds4, DSV_MAX_BLOCK_SIZE / 4, bw / 4, bh / 4, &dum1);

                luma_avg = block_avg(srcp.data, srcp.stride, bw, bh);

                c_average(src->planes, cbx, cby, cbw, cbh, &uavg, &vavg);
                chroma_analysis(params->vidmeta, &cpsy, luma_avg, uavg, vavg);

                ringing = 0;
                if (cpsy.nature) {
                    ringing |= var2 > yarea;
                    ringing &= var4 < yarea * 3 / 4;
                } else {
                    ringing |= var2 > yarea;
                    ringing &= var4 < yarea / 4;
                }

                if (MIN4(vars[0], vars[1], vars[2], vars[3]) <= maxvar * 16 / MAX(scale, 1)) {
                    ringing = 0;
                }

                /* avoid giving more bits to high freq colors since we are
                 *    less sensitive to the spatial frequencies of them anyway.
                 */
                skip_tones = cpsy.hifreq;
                if ((params->do_psy & DSV_PSY_ADAPTIVE_RINGING) &&
                        !skip_tones && ringing) {
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
                var_t = var_t * 256 / bound_scale;
                keep_hf &= (maxvar <= yarea * (128 * 256) / bound_scale);
                maintain = (avgvar < var_t * yarea);
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

extern void
dsv_hme(DSV_HME *hme)
{
    int i = hme->enc->pyramid_levels;
    int globalx = 0, globaly = 0;

    while (i >= 0) {
        refine_level(hme, i, globalx, globaly);
        if (i != 0) {
            global_motion(hme->mvf[i], hme->params, i, &globalx, &globaly);
        }
        i--;
    }
}

