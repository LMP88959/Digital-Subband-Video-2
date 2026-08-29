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

#define IS_P    (fm->isP)
#define IS_LUMA (fm->cur_plane == 0)

#define LLI_CONDITION  (IS_LUMA  && !IS_P && (l == 4))
#define L2A_CONDITION  (IS_LUMA  && !IS_P && (l == 2))
#define CC_CONDITION   (!IS_LUMA && !IS_P && (l >= 1 && l <= (lvls - 2)))
#define L1_CONDITION   (IS_LUMA  && !IS_P && (l == 1))

/* overflow safety */
#define OVF_SAFETY_CONDITION (l >= 11 && l >= (lvls - 3) && !fm->params->lossless)

/* C.3 Subband Transforms
 *
 * P frames are exclusively Haar
 * I frames have some non-Haar filters.
 *    - the highest frequency level of the luma plane uses the
 *        Asymmetric Subband Filter.
 *    - the second highest frequency level of the luma plane has
 *        support for adaptive filtering.
 *    - the fourth highest frequency level of the luma plane does not use Haar.
 *    - chroma is mostly non-Haar.
 *
 * The second level is very important psychovisually. It is the 'bridge' between
 * blurry and sharp and should be preserved and emphasized as much as possible.
 */

static void
cpysub(DSV_SBC *dst, DSV_SBC *src, unsigned w, unsigned h, unsigned stride)
{
    w *= sizeof(DSV_SBC);
    while (h-- > 0) {
        memcpy(dst, src, w);
        src += stride;
        dst += stride;
    }
}

/* C.3 Rounding Division */
static int
round8(int v)
{
    return (v + (v < 0 ? -4 : 4)) / 8;
}

/* pos/neg reflect */
#define RP(i, n, s) (((i) >= (n) ? (2 * (n) - (i) - 2) : (i)) * (s))
#define RN(i, s) (((i) < 0 ? -(i) : (i)) * (s))

/* filter defines, 'x' means any character
 *
 * filters for L2 and greater are of the form:
 * [ 1, xx0, CENTER_SAMPLE, xx0, 1 ]
 *
 * xx0 = main filter coefficient
 * xxS = filter normalization shift
 * xxA = filter rounding addition
 */

/* a poor filter with heavy ringing actually gives us benefits
 * with regard to perceptual quality for natural images by
 * giving the image noise around high frequency details --
 * preserving them better while also creating a general illusion of detail */

/* L2 ringing filter */
#define R20 3
#define R2S 3
#define R2A (1 << (R2S - 1))

/* L2 standard filter */
#define S20 9
#define S2S 5
#define S2A (1 << (S2S - 1))

/* scaling + reordering coefficients from LHLHLHLH to LLLLHHHH */
#define SCALE_PACK(in, out, scaleL, scaleH, n, si, so)      \
  for (i = 0; i < even_n; i += 2) {                         \
      out[(i +    0) / 2 * so] = in[(i + 0) * si] * scaleL; \
      out[(i + half) / 2 * so] = in[(i + 1) * si] * scaleH; \
  }                                                         \
  if (n & 1) {                                              \
      out[(n - 1) / 2 * so] = in[(n - 1) * si] * scaleL;    \
  }

#define UNSCALE_UNPACK(in, out, scaleL, scaleH, n, si, so)  \
  for (i = 0; i < even_n; i += 2) {                         \
      out[(i + 0) * so] = in[(i +    0) / 2 * si] / scaleL; \
      out[(i + 1) * so] = in[(i + half) / 2 * si] / scaleH; \
  }                                                         \
  if (n & 1) {                                              \
      out[(n - 1) * so] = in[(n - 1) / 2 * si] / scaleL;    \
  }

/* simple 3 tap low/high pass filters */
#define DO_SIMPLE_HI(v, op, s)                                \
  for (i = 1; i < n - 1; i += 2) {                            \
      v[i * s] op (v[(i - 1) * s] + v[(i + 1) * s] + 1) >> 1; \
  }                                                           \
  if (!(n & 1)) {                                             \
      v[(n - 1) * s] op v[(n - 2) * s];                       \
  }

#define DO_SIMPLE_LO(v, op, s)                                 \
  v[0] op v[s] >> 1;                                           \
  for (i = 2; i < even_n; i += 2) {                            \
      v[i * s] op (v[(i - 1) * s] + v[(i + 1) * s] + 2) >> 2;  \
  }


/* 5 tap low/high pass filters with/without adaptive ringing */
#define MAKE_5_TAP(v, C0, CA, CS, op, s)            \
  v[i * s] op (-v[RN(i - 3, s)] +                   \
          C0 * (v[(i - 1) * s] + v[(i + 1) * s]) -  \
                v[RP(i + 3, n, s)] + CA) >> CS

#define DO_5_TAP_LO_A(v, C0, CA, CS, R0, RA, RS, op, s)             \
  delta *= 2;                                                       \
  v[0] op v[s] >> 1;                                                \
  for (i = 2; i < even_n; i += 2) {                                 \
      int bv = sb[(sbp >> DSV_BLOCK_INTERP_P) * sbs];               \
      if (bv & DSV_IS_RINGING) {                                    \
          MAKE_5_TAP(v, R0, RA, RS, op, s);                         \
      } else {                                                      \
          MAKE_5_TAP(v, C0, CA, CS, op, s);                         \
      }                                                             \
      sbp += delta;                                                 \
  }

/* Filter coefficients for this encoder's ASF analysis implementation.
 * These coefficients and forward filtering methods can be unique to each
 * encoder since the decoder simply does a 3-tap synthesis. */
#define LPFA 98
#define LPFB 37
#define LPFC 17
#define LPFD 5

#define LPFAR 98
#define LPFBR 38
#define LPFCR 21
#define LPFDR 6
#define LPFER 4

#define HPFA 64
#define HPFB 32

#define ASFNORM 7

#define ASF7R9H3_LO(i, vs, s) \
                  (LPFA *  vs[RN(i + 0, s)] \
                 + LPFB * (vs[RN(i - 1, s)] + vs[RP(i + 1, n, s)]) \
                 - LPFC * (vs[RN(i - 2, s)] + vs[RP(i + 2, n, s)]) \
                 - LPFD * (vs[RN(i - 3, s)] + vs[RP(i + 3, n, s)]))

#define ASF7R9H3_LO_R(i, vs, s) \
                  (LPFAR *  vs[RN(i + 0, s)] \
                 + LPFBR * (vs[RN(i - 1, s)] + vs[RP(i + 1, n, s)]) \
                 - LPFCR * (vs[RN(i - 2, s)] + vs[RP(i + 2, n, s)]) \
                 - LPFDR * (vs[RN(i - 3, s)] + vs[RP(i + 3, n, s)]) \
                 + LPFER * (vs[RN(i - 4, s)] + vs[RP(i + 4, n, s)]))

#define ASF7R9H3_HI(i, vs, s) \
                  (HPFA *  vs[RN(i + 0, s)] \
                 - HPFB * (vs[RN(i - 1, s)] + vs[RP(i + 1, n, s)]))

static void
dwt_forward(DSV_SBC *io, int n)
{
    int i, even_n = n & ~1;
    DO_SIMPLE_HI(io, -=, 1);
    DO_SIMPLE_LO(io, +=, 1);
}

static void
dwt_inverse(DSV_SBC *io, int n)
{
    int i, even_n = n & ~1;
    DO_SIMPLE_LO(io, -=, 1);
    DO_SIMPLE_HI(io, +=, 1);
}

static void
filterL2_a(DSV_SBC *out, DSV_SBC *in, int n, int s, uint8_t *sb, int delta, int sbs)
{
    int i, sbp = 0, even_n = n & ~1, half = n + (n & 1);
    DO_SIMPLE_HI(in, -=, s);
    DO_5_TAP_LO_A(in, S20, S2A, S2S, R20, R2A, R2S, +=, s);
    SCALE_PACK(in, out, 2, 2, n, s, s);
}

static void
ifilterL2_a(DSV_SBC *out, DSV_SBC *in, int n, int s, uint8_t *sb, int delta, int sbs)
{
    int i, sbp = 0, even_n = n & ~1, half = n + (n & 1);
    UNSCALE_UNPACK(in, out, 2, 2, n, s, s);
    DO_5_TAP_LO_A(out, S20, S2A, S2S, R20, R2A, R2S, -=, s);
    DO_SIMPLE_HI(out, +=, s);
}

/* ASF7R9H3 asymmetric subband filter.
 * ASF7R9H3
 *   7 is the number of standard lowpass filter taps
 *   9 is the number of ringing mode lowpass filter taps
 *   3 is the number of standard highpass filter taps
 *
 * 'n' is guaranteed to be even here because this is the highest freq subband
 * (full frame size) and is only applied to the luma plane (which the spec
 * requires to have even dimensions) */
static void
filterL1(DSV_SBC *out, DSV_SBC *in, int n, int s, uint8_t *sb, int delta, int sbs)
{
    int i, L, H, sbp = 0;
    delta *= 2;
    for (i = 1; i < n - 2; i += 2) {
        int bv = sb[(sbp >> DSV_BLOCK_INTERP_P) * sbs];
        if (bv & DSV_IS_RINGING) {
            L = ASF7R9H3_LO_R((i - 1), in, s);
        } else {
            L = ASF7R9H3_LO((i - 1), in, s);
        }
        H = ASF7R9H3_HI((i - 0), in, s);

        out[(i + 0) / 2 * s] = (L + (1 << (ASFNORM - 2))) >> (ASFNORM - 1);
        out[(i + n) / 2 * s] = (H + (1 << (ASFNORM - 4))) >> (ASFNORM - 3);
        sbp += delta;
    }
    /* deal with edges */
    in[1 * s] -= (in[0 * s] + in[2 * s] + 1) >> 1;
    in[(n - 3) * s] -= (in[(n - 4) * s] + in[(n - 2) * s] + 1) >> 1;
    if (!(n & 1)) {
        in[(n - 1) * s] -= in[(n - 2) * s];
    }
    in[0 * s] += in[1 * s] >> 1;
    in[2 * s] += (in[1 * s] + in[3 * s] + 2) >> 2;
    in[(n - 2) * s] += (in[(n - 3) * s] + in[(n - 1) * s] + 2) >> 2;

    out[0 / 2 * s] = in[0 * s] * 2;
    out[n / 2 * s] = in[1 * s] * 4;

    out[((n - 2) + 0) / 2 * s] = in[((n - 2) + 0) * s] * 2;
    out[((n - 2) + n) / 2 * s] = in[((n - 2) + 1) * s] * 4;
}

/* for performance reasons, vertical/column transforms first copy the column into
 * a linear buffer, transform the linear buffer, and then copy back to the column
 */
#define fwd_2d(tmpbuf, img, width, height, stride, filter, scaleL, scaleH) \
{ \
    int i, x, y; \
    DSV_SBC *tmp = tmpbuf; \
    for (y = 0; y < height; y++) { \
        DSV_SBC *row = img + y * stride; \
        int even_n = width & ~1, half = width + (width & 1); \
        memcpy(tmp, row, width * sizeof(*tmp)); \
        filter(tmp, width); \
        SCALE_PACK(tmp, row, scaleL, scaleH, width, 1, 1); \
    } \
    for (x = 0; x < width; x++) { \
        DSV_SBC *col = img + x; \
        int even_n = height & ~1, half = height + (height & 1); \
        for (y = 0; y < height; y++) { \
            tmp[y] = img[y * stride + x]; \
        } \
        filter(tmp, height); \
        SCALE_PACK(tmp, col, scaleL, scaleH, height, 1, stride); \
    } \
}

#define inv_2d(tmpbuf, img, width, height, stride, ifilter, scaleL, scaleH) \
{ \
    int i, x, y; \
    DSV_SBC *tmp = tmpbuf; \
    for (x = 0; x < width; x++) { \
        DSV_SBC *col = img + x; \
        int even_n = height & ~1, half = height + (height & 1); \
        UNSCALE_UNPACK(col, tmp, scaleL, scaleH, height, stride, 1); \
        ifilter(tmp, height); \
        for (y = 0; y < height; y++) { \
            img[y * stride + x] = tmp[y]; \
        } \
    } \
    for (y = 0; y < height; y++) { \
        int even_n = width & ~1, half = width + (width & 1); \
        DSV_SBC *row = img + y * stride; \
        UNSCALE_UNPACK(row, tmp, scaleL, scaleH, width, 1, 1); \
        ifilter(tmp, width); \
        memcpy(row, tmp, width * sizeof(*tmp)); \
    } \
}

static void
fwd_L1a_2d(DSV_SBC *tmp, DSV_SBC *in, int sW, int sH, int lvl, DSV_FMETA *fm)
{
    int i, j, bx = 0, by = 0, dbx, dby, w, h;
    uint8_t *line;

    w = DSV_ROUND_SHIFT(sW, lvl - 1);
    h = DSV_ROUND_SHIFT(sH, lvl - 1);

    /* stretch blockdata to fit sub-image */
    dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / w;
    dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / h;
    for (j = 0; j < h; j++) {
        line = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
        filterL1(tmp + sW * j, in + sW * j, w, 1, line, dbx, 1);
        by += dby;
    }
    for (i = 0; i < w; i++) {
        line = fm->blockdata + (bx >> DSV_BLOCK_INTERP_P);
        filterL1(in + i, tmp + i, h, sW, line, dby, fm->params->nblocks_h);
        bx += dbx;
    }
}

/* adaptive */
static void
fwd_L2a_2d(DSV_SBC *tmp, DSV_SBC *in, int sW, int sH, int lvl, DSV_FMETA *fm)
{
    int i, j, bx = 0, by = 0, dbx, dby, w, h;
    uint8_t *line;

    w = DSV_ROUND_SHIFT(sW, lvl - 1);
    h = DSV_ROUND_SHIFT(sH, lvl - 1);

    /* stretch blockdata to fit sub-image */
    dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / w;
    dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / h;
    for (j = 0; j < h; j++) {
        line = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
        filterL2_a(tmp + sW * j, in + sW * j, w, 1, line, dbx, 1);
        by += dby;
    }
    for (i = 0; i < w; i++) {
        line = fm->blockdata + (bx >> DSV_BLOCK_INTERP_P);
        filterL2_a(in + i, tmp + i, h, sW, line, dby, fm->params->nblocks_h);
        bx += dbx;
    }
}

static void
inv_L2a_2d(DSV_SBC *tmp, DSV_SBC *in, int sW, int sH, int lvl, DSV_FMETA *fm)
{
    int i, j, bx = 0, by = 0, dbx, dby, w, h;
    uint8_t *line;

    w = DSV_ROUND_SHIFT(sW, lvl - 1);
    h = DSV_ROUND_SHIFT(sH, lvl - 1);

    dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / w;
    dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / h;
    for (i = 0; i < w; i++) {
        line = fm->blockdata + (bx >> DSV_BLOCK_INTERP_P);
        ifilterL2_a(tmp + i, in + i, h, sW, line, dby, fm->params->nblocks_h);
        bx += dbx;
    }
    for (j = 0; j < h; j++) {
        line = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
        ifilterL2_a(in + sW * j, tmp + sW * j, w, 1, line, dbx, 1);
        by += dby;
    }
}

static void
fwd(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl, int ovf_safety)
{
    DSV_SBC *os, *od, *dpLL, *dpLH, *dpHL, *dpHH;
    int x, y, woff, hoff, ws, hs, oddw, oddh, idx;

    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    oddw = ws & 1;
    oddh = hs & 1;
    os = src;
    od = dst;

    dpLL = dst;
    dpLH = dst + woff;
    dpHL = dst + hoff * width;
    dpHH = dst + woff + hoff * width;
    for (y = 0; y < hs - oddh; y += 2) {
        DSV_SBC *spA, *spB;

        spA = src + y * width;
        spB = spA + width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            int x0, x1, x2, x3, s0, s1, d0, d1;

            x0 = spA[x + 0];
            x1 = spA[x + 1];
            x2 = spB[x + 0];
            x3 = spB[x + 1];

            s0 = x0 + x1;
            s1 = x2 + x3;
            d0 = x0 - x1;
            d1 = x2 - x3;

            dpLL[idx] = DSV_SAR(s0 + s1, ovf_safety); /* LL */
            dpLH[idx] = d0 + d1; /* LH */
            dpHL[idx] = s0 - s1; /* HL */
            dpHH[idx] = d0 - d1; /* HH */
        }
        if (oddw) {
            int x0, x2, s, d;

            x0 = spA[ws - 1];
            x2 = spB[ws - 1];
            s = x0 + x2;
            d = x0 - x2;

            dpLL[idx] = DSV_SAR(s * 2, ovf_safety); /* LL */
            dpHL[idx] = d * 2; /* HL */
        }
        dpLL += width;
        dpLH += width;
        dpHL += width;
        dpHH += width;
    }
    if (oddh) {
        DSV_SBC *spA = src + (hs - 1) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            int x0, x1, s, d;

            x0 = spA[x + 0];
            x1 = spA[x + 1];
            s = x0 + x1;
            d = x0 - x1;

            dpLL[idx] = DSV_SAR(s * 2, ovf_safety); /* LL */
            dpLH[idx] = d * 2; /* LH */
        }
        if (oddw) {
            int x0 = spA[ws - 1];
            dpLL[idx] = DSV_SAR(x0 * 4, ovf_safety); /* LL */
        }
    }
    cpysub(os, od, ws, hs, width);
}

static int
is_monotonic(int a, int b, int c, int hqp)
{
    int da, dc;
    da = b - a;
    dc = c - b;
    return da != 0 && dc != 0 && /* ensure gradient exists (not partially or totally flat) */
           (DSV_SIGNOF(da) == DSV_SIGNOF(dc)) && /* ensure gradient is monotonous */
           abs(da) < hqp && abs(dc) < hqp; /* ensure start and end values in gradient are not wildly different (for better edge preservation) */
}

/* C.3.1.2 Haar Filtered Inverse Transform */
static void
inv(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl, int hqpLH, int hqpHL, int ovf_safety)
{
    int x, y, woff, hoff, ws, hs, oddw, oddh;
    int LL, LH, HL, HH;
    int idx, mhqpLH, mhqpHL;

    DSV_SBC *os, *od, *spLL, *spLH, *spHL, *spHH;
    mhqpLH = 8 * hqpLH;
    mhqpHL = 8 * hqpHL;
    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    oddw = ws & 1;
    oddh = hs & 1;
    os = src;
    od = dst;

    spLL = src;
    spLH = src + woff;
    spHL = src + hoff * width;
    spHH = src + woff + hoff * width;
    for (y = 0; y < hs - oddh; y += 2) {
        DSV_SBC *dpA, *dpB;
        int inY = hqpHL && (y > 0 && y < (hs - oddh - 1));

        dpA = dst + y * width;
        dpB = dpA + width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            int nudge, lp, ln;
            int s0, s1, d0, d1;
            int inX = hqpLH && (x > 0 && x < (ws - oddw - 1));

            LL = spLL[idx] * (1 << ovf_safety);
            LH = spLH[idx];
            HL = spHL[idx];
            HH = spHH[idx];

            if (inX) {
                lp = spLL[idx - 1] * (1 << ovf_safety); /* prev */
                ln = spLL[idx + 1] * (1 << ovf_safety); /* next */
                if (is_monotonic(lp, LL, ln, mhqpLH)) {
                    nudge = round8(lp - ln) - LH;
                    LH += CLAMP(nudge, -hqpLH, hqpLH); /* nudge LH to smooth it */
                }
            }
            if (inY) { /* do the same as above but in the Y direction */
                lp = spLL[idx - width] * (1 << ovf_safety);
                ln = spLL[idx + width] * (1 << ovf_safety);
                if (is_monotonic(lp, LL, ln, mhqpHL)) {
                    nudge = round8(lp - ln) - HL;
                    HL += CLAMP(nudge, -hqpHL, hqpHL); /* nudge HL to smooth it */
                }
            }

            s0 = LL + HL;
            s1 = LL - HL;
            d0 = LH + HH;
            d1 = LH - HH;

            dpA[x + 0] = (s0 + d0) / 4;
            dpA[x + 1] = (s0 - d0) / 4;
            dpB[x + 0] = (s1 + d1) / 4;
            dpB[x + 1] = (s1 - d1) / 4;
        }

        if (oddw) {
            LL = spLL[idx] * (1 << ovf_safety);
            HL = spHL[idx];

            dpA[ws - 1] = (LL + HL) / 4;
            dpB[ws - 1] = (LL - HL) / 4;
        }
        spLL += width;
        spLH += width;
        spHL += width;
        spHH += width;
    }
    if (oddh) {
        DSV_SBC *dpA = dst + (hs - 1) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            LL = spLL[idx] * (1 << ovf_safety);
            LH = spLH[idx];

            dpA[x + 0] = (LL + LH) / 4;
            dpA[x + 1] = (LL - LH) / 4;
        }
        if (oddw) {
            LL = spLL[idx] * (1 << ovf_safety);
            dpA[ws - 1] = LL / 4;
        }
    }
    cpysub(os, od, ws, hs, width);
}

/* pixel to subband coef */
static void
p2sbc(DSV_COEFS *dc, DSV_PLANE *p)
{
    int x, y;
    DSV_SBC *d;

    d = dc->data;
    for (y = 0; y < dc->height; y++) {
        uint8_t *line = DSV_GET_LINE(p, y);
        for (x = 0; x < dc->width; x++) {
            /* subtract 128 to center plane around zero */
            d[x] = line[x] - 128;
        }
        d += dc->width;
    }
}

/* C.3.3 Subband Recomposition */
static void
sbc2p(DSV_PLANE *p, DSV_COEFS *dc)
{
    int x, y;
    DSV_SBC *d, v;

    d = dc->data;
    for (y = 0; y < p->h; y++) {
        uint8_t *line = DSV_GET_LINE(p, y);
        for (x = 0; x < p->w; x++) {
            v = (d[x] + 128);
            line[x] = CLAMP(v, 0, 255);
        }
        d += dc->width;
    }
}

/* C.3.3 Subband Recomposition - num_levels */
static int
nlevels(int w, int h)
{
    int lb2, mx;

    mx = MAX(w, h);
    lb2 = dsv_lb2(mx);
    if (mx > (1 << lb2)) {
        lb2++;
    }
    return lb2;
}

extern void
dsv_fwd_sbt(DSV_PLANE *src, DSV_COEFS *dst, DSV_FMETA *fm)
{
    int w, h, lvls, l, ovf_safety;
    DSV_SBC *temp_buf_pad, *temp_buf_line;

    w = dst->width;
    h = dst->height;

    p2sbc(dst, src);

    lvls = nlevels(w, h);
    temp_buf_pad = fm->transform_buf + w;
    temp_buf_line = fm->transform_buf + ((w + 2) * (h + 2));

    for (l = 1; l <= lvls; l++) {
        int sw, sh;
        sw = DSV_ROUND_SHIFT(w, l - 1);
        sh = DSV_ROUND_SHIFT(h, l - 1);

        ovf_safety = OVF_SAFETY_CONDITION;
        if (fm->params->lossless) {
            if (l >= 1 && l <= (lvls - 2)) {
                fwd_2d(temp_buf_line, dst->data, sw, sh, w, dwt_forward, 1, 1);
            } else {
                fwd(dst->data, temp_buf_pad, w, h, l, ovf_safety);
            }
            continue;
        }
        if (LLI_CONDITION) {
            fwd_2d(temp_buf_line, dst->data, sw, sh, w, dwt_forward, 2, 2);
        } else if (CC_CONDITION) {
            fwd_2d(temp_buf_line, dst->data, sw, sh, w, dwt_forward, 2, 1);
        } else if (L2A_CONDITION) {
            fwd_L2a_2d(temp_buf_pad, dst->data, w, h, l, fm);
        } else if (L1_CONDITION) {
            fwd_L1a_2d(temp_buf_pad, dst->data, w, h, l, fm);
        } else {
            fwd(dst->data, temp_buf_pad, w, h, l, ovf_safety);
        }
    }
}

/* C.3.3 Subband Recomposition */
extern void
dsv_inv_sbt(DSV_PLANE *dst, DSV_COEFS *src, int q, DSV_FMETA *fm)
{
    int w, h, lvls, l, hqpLH, hqpHL, ovf_safety;
    DSV_SBC *temp_buf_pad, *temp_buf_line;

    w = src->width;
    h = src->height;

    lvls = nlevels(w, h);
    temp_buf_pad = fm->transform_buf + w;
    temp_buf_line = fm->transform_buf + ((w + 2) * (h + 2));

    for (l = lvls; l > 0; l--) {
        int sw, sh;
        sw = DSV_ROUND_SHIFT(w, l - 1);
        sh = DSV_ROUND_SHIFT(h, l - 1);

        if (l >= 4) {
            hqpLH = dsv_lfquant(fm, q);
            hqpHL = hqpLH;
        } else {
            hqpLH = dsv_hfquant(fm, q, 1, 3 - l);
            hqpHL = dsv_hfquant(fm, q, 2, 3 - l);
        }
        ovf_safety = OVF_SAFETY_CONDITION;

        if (fm->params->lossless) {
            if (l >= 1 && l <= (lvls - 2)) {
                inv_2d(temp_buf_line, src->data, sw, sh, w, dwt_inverse, 1, 1);
            } else {
                inv(src->data, temp_buf_pad, w, h, l, 0, 0, ovf_safety);
            }
            continue;
        }
        if (LLI_CONDITION) {
            inv_2d(temp_buf_line, src->data, sw, sh, w, dwt_inverse, 2, 2);
        } else if (CC_CONDITION) {
            inv_2d(temp_buf_line, src->data, sw, sh, w, dwt_inverse, 2, 1);
        } else if (L2A_CONDITION) {
            inv_L2a_2d(temp_buf_pad, src->data, w, h, l, fm);
        } else if (L1_CONDITION) {
            inv_2d(temp_buf_line, src->data, sw, sh, w, dwt_inverse, 2, 4);
        } else {
            inv(src->data, temp_buf_pad, w, h, l, hqpLH, hqpHL, ovf_safety);
        }
    }

    sbc2p(dst, src);
}
