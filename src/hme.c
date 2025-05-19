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

#define AVG2(a, b) (((a) + (b) + 1) >> 1)
#define AVG4(a, b, c, d) (((a) + (b) + (c) + (d) + 2) >> 2)
#define UAVG4(a, b, c, d) ((unsigned) ((a) + (b) + (c) + (d) + 2) >> 2)
#define MV_LT(v, t) (abs((v)->u.mv.x) < (t) && abs((v)->u.mv.y) < (t)) /* less than */
#define MV_MAG(v) ((abs((v)->u.mv.x) + abs((v)->u.mv.y) + 1) >> 1) /* not actual mag */
#define MV_CMP(v, vx, vy, t) (abs((v)->u.mv.x - (int) (vx)) < (t) && abs((v)->u.mv.y - (int) (vy)) < (t))
#define MK_MV_COMP(fp, hp, qp) ((fp) * 4 + (hp) * 2 + (qp))

#define ABSDIF(a, b) abs((int) (a) - (int) (b))

static uint8_t
clamp_u8(int v)
{
    return v > 255 ? 255 : v < 0 ? 0 : v;
}

static int
similar_complexity(int varD, int varS, int mul)
{
    unsigned dif, f;

    dif = ABSDIF(varD, varS);
    f = varS * mul >> 2;

    return (dif < MAX(1, f));
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

#define METRIC_ERR(d) ((d) * (d))
#define METRIC_RETURN(a, w, h) (iisqrt(a)*(w)*(h)/(((w)+(h)+1)>>1))

static unsigned
iisqrt(unsigned n)
{
    unsigned pos, res, rem;

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
                acc += METRIC_ERR(a1 - b1);                    \
                acc += METRIC_ERR(a2 - b2);                    \
                acc += METRIC_ERR(a3 - b3);                    \
                acc += METRIC_ERR(a4 - b4);                    \
                acc += 2 * METRIC_ERR(s0 - s1);                \
            }                                                  \
            a += 2 * as;                                       \
            b += 2 * bs;                                       \
        }                                                      \
        return METRIC_RETURN(acc, w, h)

#define MAKE_METR(w)                                           \
static unsigned                                                \
metr_ ##w## xh(uint8_t *a, int as, uint8_t *b, int bs, int h)  \
{                                                              \
    METR_BODY(w, h);                                           \
}

#define MAKE_METRWH(w, h)                                      \
static unsigned                                                \
metr_ ##w## x ##h## x (uint8_t *a, int as, uint8_t *b, int bs) \
{                                                              \
    METR_BODY(w, h);                                           \
}

MAKE_METR(8)
MAKE_METR(16)
MAKE_METR(32)

MAKE_METRWH(8, 8)
MAKE_METRWH(16, 16)
MAKE_METRWH(32, 32)

static unsigned
metr_wxh(uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    METR_BODY(w, h);
}

#define SSE_BODY(w, h)                                        \
        int i, j;                                             \
        unsigned acc = 0;                                     \
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
qpsad(uint8_t *a, int as, uint8_t *b)
{
    int i, j;
    unsigned acc = 0;
    for (j = 0; j < SP_SAD_SZ; j++) {
        for (i = 0; i < SP_SAD_SZ; i++) {
            unsigned d = METRIC_ERR((int) a[i] - (int) b[QP_OFFSET(i, j)]);
            acc += d;
        }
        a += as;
    }
    return METRIC_RETURN(acc, SP_SAD_SZ, SP_SAD_SZ);
}

static void
sse_intra(uint8_t *a, int as, uint8_t *b, int bs,
          int avg_sb, int avg_src, int w, int h,
          unsigned *intra_err, unsigned *intrasrc_err, unsigned *inter_err)
{
    int i, j, dif;
    unsigned intra_sb = 0, intra_src = 0, inter = 0;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            dif = (a[i] - avg_sb);
            intra_sb += dif * dif;
            dif = (a[i] - avg_src);
            intra_src += dif * dif;
            dif = (a[i] - b[i]);
            inter += dif * dif;
        }
        a += as;
        b += bs;
    }
    *intra_err = intra_sb;
    *intrasrc_err = intra_src;
    *inter_err = inter;
}

static unsigned
msse_wxh(uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    SSE_BODY(w, h);
    return METRIC_RETURN(acc, w, h);
}

static unsigned
fastmetr(uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    switch (w) {
        case 8:
            switch (h) {
                case 8:
                    return metr_8x8x(a, as, b, bs);
                default:
                    return metr_8xh(a, as, b, bs, h);
            }
            break;
        case 16:
            switch (h) {
                case 16:
                    return metr_16x16x(a, as, b, bs);
                default:
                    return metr_16xh(a, as, b, bs, h);
            }
            break;
        case 32:
            switch (h) {
                case 32:
                    return metr_32x32x(a, as, b, bs);
                default:
                    return metr_32xh(a, as, b, bs, h);
            }
            break;
        default:
            break;
    }
    return metr_wxh(a, as, b, bs, w, h);
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
hier_metr(int level, uint8_t *a, int as, uint8_t *b, int bs, int w, int h)
{
    /* change metric depending on level in hierarchy */
    switch (level) {
        case 0:
            return fastmetr(a, as, b, bs, w, h);
        default:
            return fastsse(a, as, b, bs, w, h);
    }
}

/* full-pel MSSE of luma and chroma */
static void
yuv_err(unsigned res[3], unsigned sub[3][4], DSV_FRAME *src, DSV_FRAME *ref,
        int bx, int by, int bw, int bh,
        int cbx, int cby, int cbw, int cbh)
{
    int f, g, z;
    DSV_PLANE *sp, *rp;

    memset(res, 0, 3 * sizeof(unsigned));
    memset(sub, 0, 3 * 4 * sizeof(unsigned));
    sp = src->planes;
    rp = ref->planes;
    bw /= 2;
    bh /= 2;
    cbw /= 2;
    cbh /= 2;
    for (z = 0; z < 3; z++) {
        int subpos = 0;
        for (g = 0; g <= bh; g += bh) {
            for (f = 0; f <= bw; f += bw) {
                uint8_t *src_d, *ref_d;

                src_d = DSV_GET_XY(&sp[z], bx, by) + (f + g * sp[z].stride);
                ref_d = DSV_GET_XY(&rp[z], bx, by) + (f + g * rp[z].stride);
                sub[z][subpos] = msse_wxh(
                        src_d, sp[z].stride,
                        ref_d, rp[z].stride, bw, bh);
                res[z] += sub[z][subpos];
                subpos++;
            }
        }
        /* planes 1,2 are chroma */
        bx = cbx;
        by = cby;
        bw = cbw;
        bh = cbh;
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
invalid_block(DSV_FRAME *f, int x, int y, int sx, int sy)
{
    int b = f->border * DSV_FRAME_BORDER;
    return x < -b || y < -b || x + sx > f->width + b || y + sy > f->height + b;
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

/*
 * determine if this block needs EPRM
 */
static void
calc_EPRM(DSV_PLANE *sp, DSV_PLANE *mvrp, int avg_src,
          int w, int h, int *eprmi, int *eprmd, int *eprmr)
{
    int i, j;
    int ravg, prd;
    int clipi = 0;
    int clipd = 0;
    int clipr = 0;
    uint8_t *mvr = mvrp->data;
    uint8_t *src = sp->data;

    ravg = block_avg(mvrp->data, mvrp->stride, w, h) - 128;
    *eprmi = 0;
    *eprmr = 0;
    avg_src -= 128;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            /* see if MV pred or intra pred would clip and require EPRM */
            if (!clipr) {
                prd = (src[i] - mvr[i]) + 128;
                clipr = prd & ~0xff;
            }
            if (!clipi) {
                prd = (src[i] - ravg); /* ravg is precomputed with 128 offset */
                clipi = prd & ~0xff;
            }
            if (!clipd) {
                prd = (src[i] - avg_src);
                clipd = prd & ~0xff;
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
test_subblock_intra_y(DSV_PARAMS *params, DSV_MV *mv,
        DSV_PLANE *srcp, DSV_PLANE *refp,
        int detail_src, int avg_src, int neidif, unsigned ratio,
        int bw, int bh)
{
    int f, g, sbw, sbh, bit_index, nsub = 0, psyscale;
    unsigned avgs[4], avg_tot = 0, err_sub = 0, err_src = 0;
    uint8_t bits[4] = {
            DSV_MASK_INTRA00,
            DSV_MASK_INTRA01,
            DSV_MASK_INTRA10,
            DSV_MASK_INTRA11,
    };
    sbw = bw / 2;
    sbh = bh / 2;
    if (sbw == 0 || sbh == 0 || mv->u.all == 0) {
        return;
    }
    psyscale = dsv_spatial_psy_factor(params, -1);
    bit_index = 0;
    /* increase detail bias proportionally to how similar the MV was to its neighbors */
    detail_src += detail_src / MAX(neidif, 1);
    for (g = 0; g <= sbh; g += sbh) {
        for (f = 0; f <= sbw; f += sbw) {
            uint8_t *src_d, *mvr_d;
            unsigned avg_sub, local_detail;
            unsigned sub_pred_err, src_pred_err, intererr;
            int lo, hi, lerp, sub_better, src_better;

            src_d = srcp->data + (f + g * srcp->stride);
            mvr_d = refp->data + (f + g * refp->stride);
            if (mv->submask & bits[bit_index]) {
                /* already marked as intra, don't bother doing another check */
                goto next;
            }
            avg_sub = block_avg(mvr_d, refp->stride, sbw, sbh);
            sse_intra(src_d, srcp->stride, mvr_d, refp->stride, avg_sub, avg_src, sbw, sbh, &sub_pred_err, &src_pred_err, &intererr);
            intererr = intererr * ratio >> 5;
            local_detail = block_detail(src_d, srcp->stride, sbw, sbh, &avgs[bit_index]);
            if (local_detail > (unsigned) (3 * bw * bh)) {
                goto next;
            }
            lo = (detail_src + local_detail + 1) >> 1;
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
                nsub++;
                err_src += src_pred_err;
                err_sub += sub_pred_err;
                avg_tot += avgs[bit_index];
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
test_subblock_intra_c(int i, int j,
        DSV_PARAMS *params, DSV_MV *mv,
        unsigned mad, unsigned detail_src, unsigned avg_src,
        int fpelx, int fpely,
        DSV_PLANE *sp, DSV_PLANE *rp,
        int y_w, int y_h,
        int bw, int bh)
{
    int f, g, sbw, sbh, bit_index, hs, vs;
    int mvx, mvy, cbx, cby;
    unsigned avg_ramp;
    uint8_t bits[4] = {
            DSV_MASK_INTRA00,
            DSV_MASK_INTRA01,
            DSV_MASK_INTRA10,
            DSV_MASK_INTRA11,
    };

    if (params->effort < 6) {
        return;
    }

    hs = DSV_FORMAT_H_SHIFT(params->vidmeta->subsamp);
    vs = DSV_FORMAT_V_SHIFT(params->vidmeta->subsamp);
    cbx = i * (y_w >> hs);
    cby = j * (y_h >> vs);
    mvx = cbx + DSV_SAR(fpelx, hs);
    mvy = cby + DSV_SAR(fpely, vs);
    sbw = (bw >> hs) / 2;
    sbh = (bh >> vs) / 2;
    detail_src /= (bw * bh);
    if (sbw == 0 || sbh == 0 || mad <= detail_src) {
        return;
    }
    avg_ramp = avg_src * avg_src >> 8;
    bit_index = 0;
    for (g = 0; g <= sbh; g += sbh) {
        for (f = 0; f <= sbw; f += sbw) {
            int uavg_src, vavg_src, uavg_mvr, vavg_mvr, erru, errv;
            unsigned dif, thr;

            if (mv->submask & bits[bit_index]) {
                /* already marked as intra, don't bother doing another check */
                goto next;
            }
            c_average(sp, cbx + f, cby + g, sbw, sbh, &uavg_src, &vavg_src);
            c_average(rp, mvx + f, mvy + g, sbw, sbh, &uavg_mvr, &vavg_mvr);
            erru = abs(uavg_src - uavg_mvr);
            errv = abs(vavg_src - vavg_mvr);
            /* scale error by squared average luma since I believe chroma error
             * is less noticeable in darker areas (just my observation) */
            dif = MAX(erru, errv) * avg_ramp >> 8;
            thr = MIN(detail_src, 16);

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
        DSV_MV *mf, /* motion vector field */
        DSV_MV *mv, /* current full-pixel motion vector */
        DSV_FRAME *src, DSV_FRAME *ref,
        int i, int j,
        unsigned best, int quant,
        int bx, int by, int bw, int bh)
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

    best = MAX((int) best - (int) (yarea / 2), 0);
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
            if (!hpel && params->effort < 8) { /* skip qpel at low effort */
                continue;
            }
            t[0] = testv[n >> 1][0] << hpel;
            t[1] = testv[n >> 1][1] << hpel;
        }
        score = qpsad(srcsp.data, srcsp.stride, imq + t[0] + t[1] * QP_STRIDE);
        evx = MK_MV_COMP(mv->u.mv.x, 0, t[0]);
        evy = MK_MV_COMP(mv->u.mv.y, 0, t[1]);
        cost = dsv_mv_cost(mf, params, i, j, evx, evy, quant, 0);
        if (best > score + cost) {
            best = score;
            SETVV(bestv, t);
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

static int
refine_level(DSV_HME *hme, int level, int *scene_change_blocks, int *avg_err)
{
    DSV_FRAME *src, *ref, *ogr;
    DSV_MV *mv;
    DSV_MV *mf, *parent = NULL;
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

    mf = hme->mvf[level];

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
#define FPEL_NSEARCH 9 /* search points for full-pel search */
            static int xf[FPEL_NSEARCH] = { 0, 1, -1, 0,  0, -1,  1, -1, 1 };
            static int yf[FPEL_NSEARCH] = { 0, 0,  0, 1, -1, -1, -1,  1, 1 };
            DSV_PLANE srcp;
            int dx, dy, bestdx, bestdy;
            int bx, by, bw, bh;
            int k, m, n = 0;
            DSV_MV *inherited[32];
            DSV_MV spatialP, spatialL, spatialT, temporal;
            unsigned metr[4], best, score, best_score, good_enough = 0;
            unsigned qthresh;

            bx = (i * y_w) >> level;
            by = (j * y_h) >> level;
            /* bounds check for safety */
            if ((bx >= src->width) || (by >= src->height)) {
                DSV_MV zmv = { 0 }; /* inter with no other flag */
                mf[i + j * nxb] = zmv;
                continue;
            }

            dsv_plane_xy(src, &srcp, 0, bx, by);
            bw = MIN(srcp.w, y_w);
            bh = MIN(srcp.h, y_h);

            inherited[n++] = &zero;
            if (level == 0) {
                /* add spatially and temporally predicted candidates */
                int px, py;
                dsv_movec_pred(mf, params, i, j, &px, &py);
                spatialP.u.mv.x = DSV_SAR(px + 2, 2);
                spatialP.u.mv.y = DSV_SAR(py + 2, 2);
                inherited[n++] = &spatialP;
                if (i > 0) { /* left */
                    DSV_MV *pmv = (mf + j * params->nblocks_h + (i - 1));
                    spatialL.u.mv.x = DSV_SAR(pmv->u.mv.x + 2, 2);
                    spatialL.u.mv.y = DSV_SAR(pmv->u.mv.y + 2, 2);
                    inherited[n++] = &spatialL;
                }
                if (j > 0) { /* top */
                    DSV_MV *pmv = (mf + (j - 1) * params->nblocks_h + i);
                    spatialT.u.mv.x = DSV_SAR(pmv->u.mv.x + 2, 2);
                    spatialT.u.mv.y = DSV_SAR(pmv->u.mv.y + 2, 2);
                    inherited[n++] = &spatialT;
                }
                if (hme->ref_mvf != NULL) {
                    DSV_MV *tprev = &hme->ref_mvf[i + j * nxb];
                    temporal.u.mv.x = DSV_SAR(tprev->u.mv.x + 2, 2);
                    temporal.u.mv.y = DSV_SAR(tprev->u.mv.y + 2, 2);
                    inherited[n++] = &temporal;
                }
            } else {
                spatialP.u.all = 0;
            }

            if (parent != NULL) {
#define N_POINTS (1 + 4)
                static int pt[N_POINTS * 2] = { 0, 0,  -2, 0,   2, 0,   0, -2,   0, 2 };
                int x, y, pi, pj;

                pi = i & parent_mask;
                pj = j & parent_mask;
                for (m = 0; m < N_POINTS; m++) {
                    x = pi + pt[(m << 1) + 0] * step;
                    y = pj + pt[(m << 1) + 1] * step;
                    if (x >= 0 && x < nxb && y >= 0 && y < nyb) {
                        inherited[n++] = parent + x + y * nxb;
                    }
                }
            }
            /* we only care about unique non-zero vectors */
            n = remove_dupes(inherited, n);
            /* find best vector */
            best = 0;
            bestdx = inherited[best]->u.mv.x;
            bestdy = inherited[best]->u.mv.y;
            best_score = UINT_MAX;
            if (n > 1) {
                for (k = 0; k < n; k++) {
                    DSV_PLANE refp;
                    int evx, evy, cost;

                    if (invalid_block(src, bx, by, bw, bh)) {
                        continue;
                    }

                    dx = DSV_SAR(inherited[k]->u.mv.x, level);
                    dy = DSV_SAR(inherited[k]->u.mv.y, level);

                    if (invalid_block(ref, bx + dx, by + dy, bw, bh)) {
                        continue;
                    }

                    dsv_plane_xy(ref, &refp, 0, bx + dx, by + dy);
                    score = hier_metr(level, srcp.data, srcp.stride, refp.data, refp.stride, bw, bh);
                    evx = MK_MV_COMP(dx, 0, 0) * step;
                    evy = MK_MV_COMP(dy, 0, 0) * step;
                    cost = dsv_mv_cost(mf, params, i, j, evx, evy, hme->quant, (level != 0));
                    if (best_score > score + cost) {
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
            mv->u.all = 0;
            mv->flags = 0;
            mv->submask = 0;
            mv->dc = 0;
            mv->err = 0;

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
            best = best_score;
            m = 0;

            qthresh = (unsigned) (hme->quant * bw * bh >> 10);
            if (abs(dx) <= 1 && abs(dy) <= 1) {
                unsigned zoscore = sse_wxh(srcp.data, sp->stride,
                                    DSV_GET_XY(&ogr->planes[0], bx, by),
                                    ogr->planes[0].stride, bw, bh);
                if (zoscore < qthresh) {
                    /* bias towards zero vector */
                    if (level == 0) {
                        best = hier_metr(level, srcp.data, sp->stride,
                                DSV_GET_XY(rp, bx, by),
                                rp->stride, bw, bh);
                    } else {
                        best = 0;
                    }
                    dx = 0;
                    dy = 0;
                    good_enough = 1;
                }
            }
#define FAST_DIAG 1
            if (!good_enough) {
                n = (level != 0 || (params->effort >= 2 && !FAST_DIAG)) ? FPEL_NSEARCH : (FPEL_NSEARCH / 2 + 1);
                for (k = 0; k < n; k++) {
                    int evx, evy, cost;
                    int tvx, tvy;

                    tvx = dx + xf[k];
                    tvy = dy + yf[k];
                    score = hier_metr(level, srcp.data, sp->stride,
                            DSV_GET_XY(rp, bx + tvx, by + tvy),
                            rp->stride, bw, bh);

                    if (FAST_DIAG && k >= 1 && k <= 4) {
                        metr[k - 1] = score;
                    }

                    evx = MK_MV_COMP(dx + xf[k], 0, 0) * step;
                    evy = MK_MV_COMP(dy + yf[k], 0, 0) * step;
                    cost = dsv_mv_cost(mf, params, i, j, evx, evy, hme->quant, (level != 0));
                    if (level == 0) {
                        if ((score + cost) <= qthresh) {
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

                tv[0] = xf[pri] + xf[sec] + xf[m];
                tv[1] = yf[pri] + yf[sec] + yf[m];
                score = hier_metr(level, srcp.data, sp->stride,
                        DSV_GET_XY(rp, bx + tv[0], by + tv[1]),
                        rp->stride, bw, bh);
                evx = MK_MV_COMP(tv[0], 0, 0) * step;
                evy = MK_MV_COMP(tv[1], 0, 0) * step;
                cost = dsv_mv_cost(mf, params, i, j, evx, evy, hme->quant, (level != 0));
                if (best > score + cost) {
                    best = score;
                    dx = tv[0];
                    dy = tv[1];
                    if ((score + cost) <= qthresh) {
                        good_enough = 1;
                    }
                } else {
                    dx += xf[m];
                    dy += yf[m];
                }
            }
#else
            dx += xf[m];
            dy += yf[m];
#endif

            mv->u.mv.x = dx * step;
            mv->u.mv.y = dy * step;

            /* subpel refine at base level */
            if (level == 0) {
                int fpelx, fpely; /* full-pel MV coords */
                unsigned yarea = bw * bh;
                unsigned best_fp = best;

                fpelx = mv->u.mv.x;
                fpely = mv->u.mv.y;

                if (params->effort >= 4 && !good_enough) {
                    best = subpixel_ME(params, mf, mv, src, ref, i, j, best, hme->quant, bx, by, bw, bh);
                } else {
                    mv->u.mv.x *= 4;
                    mv->u.mv.y *= 4;
                }

                /* mode decision + block metric gathering
                 * _src = source block
                 * _ogr = original ref frame block at full-pel motion (x, y)
                 * _ref = reconstructed ref frame block at full-pel motion (x, y)
                 */ {
                    DSV_PLANE refp, ogrp;
                    unsigned var_src, var_ref, avg_src, avg_ref;
                    int uavg_src, vavg_src, uavg_ref, vavg_ref;
                    int cbx, cby, cbw, cbh;
                    unsigned mad, ogrerr, ogrmad;
                    unsigned avg_y_dif, avg_c_dif;
                    int eprmi, eprmd, eprmr;
                    int neidif, oob_vector; /* out of bounds */
                    unsigned skipt = (quant_rd >> 19);
                    unsigned ratio = 1 << 5; /* ratio of subpel_min_err / fullpel_min_err */

                    if (DSV_IS_SUBPEL(mv)) {
                        ratio = (best << 5) / (best_fp + !best_fp);
                    }

                    dsv_plane_xy(ogr, &ogrp, 0, bx + fpelx, by + fpely);
                    dsv_plane_xy(ref, &refp, 0, bx + fpelx, by + fpely);
                    ogrerr = fastmetr(srcp.data, srcp.stride, ogrp.data, ogrp.stride, bw, bh);
                    ogrmad = DSV_UDIV_ROUND(ogrerr, yarea);
                    ogrmad = (ogrmad * ratio >> 5);
                    mad = DSV_UDIV_ROUND(best, yarea);

                    var_src = block_detail(srcp.data, srcp.stride, bw, bh, &avg_src);
                    var_ref = block_detail(refp.data, refp.stride, bw, bh, &avg_ref);

                    avg_y_dif = ABSDIF(avg_src, avg_ref);
                    cbx = i * (y_w >> hs);
                    cby = j * (y_h >> vs);
                    cbw = bw >> hs;
                    cbh = bh >> vs;
                    c_average(sp, cbx, cby, cbw, cbh, &uavg_src, &vavg_src);
                    c_average(rp,
                            cbx + DSV_SAR(fpelx, hs),
                            cby + DSV_SAR(fpely, vs),
                            cbw, cbh, &uavg_ref, &vavg_ref);
                    avg_c_dif = AVG2(abs(uavg_src - uavg_ref), abs(vavg_src - vavg_ref));

                    calc_EPRM(&srcp, &refp, avg_src, bw, bh, &eprmi, &eprmd, &eprmr);
                    DSV_MV_SET_SIMCMPLX(mv, 0);

                    oob_vector = outofbounds(i, j, nxb, nyb, y_w, y_h, mv);

                    if (hme->enc->skip_block_thresh >= 0 && !params->lossless) {
                        unsigned sth = skipt * yarea;
                        unsigned zerr[3], zsub[3][4];
                        unsigned mag, ysub;

                        mag = MV_MAG(mv);
                        if (mv->u.all == 0) {
                            sth += 4 * var_src;
                        }
                        sth += var_src / MAX(mag, 1);
                        sth += yarea * hme->enc->skip_block_thresh;
                        yuv_err(zerr, zsub, src, ref, bx, by, bw, bh, cbx, cby, cbw, cbh);
                        ysub = MAX(MAX(zsub[0][0], zsub[0][1]), MAX(zsub[0][2], zsub[0][3]));
                        if (((ysub * 4)) <= sth / 2 && zerr[1] <= sth / 8 && zerr[2] <= sth / 8) {
                            DSV_MV_SET_SKIP(mv, 1);
                            mv->err = 0;
                            /* don't bother with anything else if we're skipping the block anyway */
                            goto skip;
                        }
                    }
                    neidif = dsv_neighbordif(mf, params, i, j);

                    if (!oob_vector && !params->lossless) {
                        int y_prereq, c_prereq;
                        y_prereq = (avg_y_dif <= 2);
                        c_prereq = (avg_c_dif <= 2);
                        if (y_prereq || c_prereq) {
                            unsigned mag, ysub, usub, vsub;
                            unsigned berr[3], bsub[3][4];
                            unsigned xth = skipt * yarea;
                            /* don't transmit residuals if the differences are minimal */
                            yuv_err(berr, bsub, src, ref,
                                    bx + fpelx, by + fpely, bw, bh,
                                    cbx + DSV_SAR(fpelx, hs), cby + DSV_SAR(fpely, vs), cbw, cbh);
                            mag = MV_MAG(mv);
                            xth += var_src / (mag + 4);
                            xth = MAX((int) xth - ((int) yarea * (neidif * 2)), 0);

                            ysub = MAX(MAX(bsub[0][0], bsub[0][1]), MAX(bsub[0][2], bsub[0][3]));
                            usub = MAX(MAX(bsub[1][0], bsub[1][1]), MAX(bsub[1][2], bsub[1][3]));
                            vsub = MAX(MAX(bsub[2][0], bsub[2][1]), MAX(bsub[2][2], bsub[2][3]));
                            ysub = (ysub * ratio >> 5);
                            usub = (usub * ratio >> 5);
                            vsub = (vsub * ratio >> 5);

                            if (y_prereq && ysub <= xth) {
                                DSV_MV_SET_NOXMITY(mv, 1);
                            }
                            xth = ((cbw * cbh) * xth / yarea) / 2;
                            if (c_prereq && usub <= xth && vsub <= xth) {
                                DSV_MV_SET_NOXMITC(mv, 1);
                            }
                        }

#if NO_RESIDUALS
                        DSV_MV_SET_NOXMITY(mv, 1);
#if NO_RESIDUALS == 2
                        DSV_MV_SET_NOXMITC(mv, 1);
#endif
#endif
                        if (similar_complexity(var_ref, var_src, (1 << 1))) {
                            /* similar enough complexity to enable P-frame visual masking */
                            DSV_MV_SET_SIMCMPLX(mv, 1);
                        }
                    }

#if 1 /* have intra blocks */
                    test_subblock_intra_y(params, mv,
                                          &srcp, &refp,
                                          var_src, avg_src, neidif, ratio,
                                          bw, bh);
                    test_subblock_intra_c(i, j, params, mv,
                                          mad, var_src, avg_src,
                                          fpelx, fpely,
                                          sp, rp, y_w, y_h, bw, bh);
#endif
                    /* end-of-block stats */
                    mv->err = mad;
                    total_err += mad;
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

#define SPSCALE(x) ((x) * scale >> 7)
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
            int hvar, qtex;
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

            hvar = block_hist_var(srcp.data, srcp.stride, bw, bh, hist);
            qtex = quant_tex(srcp.data, srcp.stride, bw, bh);

            if (params->do_psy & (DSV_PSY_ADAPTIVE_RINGING | DSV_PSY_CONTENT_ANALYSIS)) {
                int skip_tones;
                int uavg, vavg;
                int tf = 0;
                int tf2 = 0;
                int luma_var, luma_tex;

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
                /* hifreq chroma, don't care about it */
                if (cpsy.hifreq) {
                    maintain = 0;
                    keep_hf = 0;
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

extern int
dsv_hme(DSV_HME *hme, int *scene_change_blocks, int *avg_err)
{
    int i = hme->enc->pyramid_levels;
    int nintra = 0;

    while (i >= 0) {
        nintra = refine_level(hme, i, scene_change_blocks, avg_err);
        i--;
    }
    return (nintra * 100) / (hme->params->nblocks_h * hme->params->nblocks_v);
}

