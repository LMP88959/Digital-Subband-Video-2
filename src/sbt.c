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

#define IS_P    (fm->isP)
#define IS_LUMA (fm->cur_plane == 0)

#define LL_CONDITION   (IS_LUMA  && (!IS_P && (l == 3 || l == 4)))
#define L2A_CONDITION  (IS_LUMA  && (!IS_P && (l == 2)))
#define L2_CONDITION   (!IS_LUMA && (!IS_P && (l == 2)))
#define L1_CONDITION   (IS_LUMA  && (!IS_P && (l == 1)))

#define FWD_SCALENONE(x) (x)
#define INV_SCALENONE(x) (x)

#define FWD_SCALE20(x) ((x) * 2)
#define INV_SCALE20(x) ((x) / 2)

#define FWD_SCALE25(x) (5 * (x) / 2)
#define INV_SCALE25(x) (2 * (x) / 5)


/* C.3 Subband Transforms
 *
 * P frames are exclusively Haar
 * I frames have some non-Haar filters.
 *    - the second highest frequency level of the luma plane has support
 *       for adaptive filtering.
 *    - Haar is used for every level of the luma plane besides
 *       the four highest frequency levels.
 *    - chroma is exclusively Haar besides the second highest frequency level.
 *
 * The second level is very important psychovisually. It is the 'bridge' between
 * blurry and sharp and should be preserved and emphasized as much as possible.
 */

static DSV_SBC *temp_buf = NULL;
static int temp_bufsz = 0;

static void
alloc_temp(int size)
{
    if (temp_bufsz < size) {
        temp_bufsz = size;

        if (temp_buf) {
            dsv_free(temp_buf);
            temp_buf = NULL;
        }

        temp_buf = dsv_alloc(temp_bufsz * sizeof(DSV_SBC));
        if (temp_buf == NULL) {
            DSV_ERROR(("out of memory"));
        }
    }
}

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

/* C.3 Rounding Divisions */
static int
round2(int v)
{
    if (v < 0) {
        return -(((-v) + 1) >> 1);
    }
    return (v + 1) >> 1;
}

static int
round4(int v)
{
    if (v < 0) {
        return -(((-v) + 2) >> 2);
    }
    return (v + 2) >> 2;
}

static int
reflect(int i, int n)
{
    if (i < 0) {
        i = -i;
    }
    if (i >= n) {
        i = n + n - i;
    }
    return i;
}

/* Naming:
 *
 * in this comment, X means placeholder/wildcard character
 *
 * XX0 = main filter coefficient
 * XXS = filter normalization shift
 * XXA = filter rounding addition
 *
 * RXX = ringing filter
 * SXX = standard filter (as opposed to ringing filter)
 * LXX = lower level standard filter
 *
 * X1X = level 1 filter (highest frequency)
 * X2X = level 2 filter (second highest frequency)
 * XLX = lower level filter
 */
#define LL0 5
#define LLS 5
#define LLA (1 << (LLS - 1))

#define L10 17
#define L1S 6
#define L1A (1 << (L1S - 1))

/* a poor filter with heavy ringing actually gives us benefits
 * with regard to perceptual quality for natural images by
 * giving the image noise around high frequency details --
 * preserving them better while also creating a general illusion of detail */

#define R20 3
#define R2S 2
#define R2A (1 << (R2S - 1))

#define S10 5
#define S1S 3
#define S1A (1 << (S1S - 1))

#define S20 3
#define S2S 3
#define S2A (1 << (S2S - 1))

/* reflected get */
#define rg(x, s) reflect(x, n) * s

/* scaling + reordering coefficients from LHLHLHLH to LLLLHHHH */
#define SCALE_PACK(scaleL, scaleH, s)                 \
  for (i = 0; i < even_n; i += 2) {                   \
      out[(i + 0) / 2 * s] = scaleL(in[(i + 0) * s]); \
      out[(i + h) / 2 * s] = scaleH(in[(i + 1) * s]); \
  }                                                   \
  if (n & 1) {                                        \
      out[(n - 1) / 2 * s] = scaleL(in[(n - 1) * s]); \
  }

#define UNSCALE_UNPACK(scaleL, scaleH, s)             \
  for (i = 0; i < even_n; i += 2) {                   \
      out[(i + 0) * s] = scaleL(in[(i + 0) / 2 * s]); \
      out[(i + 1) * s] = scaleH(in[(i + h) / 2 * s]); \
  }                                                   \
  if (n & 1) {                                        \
      out[(n - 1) * s] = scaleL(in[(n - 1) / 2 * s]); \
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
  v[i * s] op (-v[rg(i - 3, s)] +                   \
          C0 * (v[(i - 1) * s] + v[(i + 1) * s]) -  \
                v[rg(i + 3, s)] + CA) >> CS


#define DO_5_TAP_LO(v, C0, CA, CS, op, s) \
  v[0] op v[s] >> 1;                      \
  for (i = 2; i < even_n; i += 2) {       \
      MAKE_5_TAP(v, C0, CA, CS, op, s);   \
  }

#define DO_5_TAP_HI(v, C0, CA, CS, op, s)                     \
  for (i = 1; i < n - 3; i += 2) {                            \
      MAKE_5_TAP(v, C0, CA, CS, op, s);                       \
  }                                                           \
  while (i < n - 1) {                                         \
      v[i * s] op (v[(i - 1) * s] + v[(i + 1) * s] + 1) >> 1; \
      i += 2;                                                 \
  }                                                           \
  if (!(n & 1)) {                                             \
      v[(n - 1) * s] op v[(n - 2) * s];                       \
  }

#define DO_5_TAP_LO_A(v, C0, CA, CS, R0, RA, RS, op, s)             \
  delta *= 2;                                                       \
  v[0] op v[s] >> 1;                                                \
  for (i = 2; i < even_n; i += 2) {                                 \
      if (sb[(sbp >> DSV_BLOCK_INTERP_P) * sbs] & DSV_IS_RINGING) { \
          MAKE_5_TAP(v, R0, RA, RS, op, s);                         \
      } else {                                                      \
          MAKE_5_TAP(v, C0, CA, CS, op, s);                         \
      }                                                             \
      sbp += delta;                                                 \
  }

static void
filterLL(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    DO_SIMPLE_HI(in, -=, s);
    DO_5_TAP_LO(in, LL0, LLA, LLS, +=, s);
    SCALE_PACK(FWD_SCALE20, FWD_SCALE20, s);
}

static void
ifilterLL(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK(INV_SCALE20, INV_SCALE20, s);
    DO_5_TAP_LO(out, LL0, LLA, LLS, -=, s);
    DO_SIMPLE_HI(out, +=, s);
}

static void
filterL2(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    DO_SIMPLE_HI(in, -=, s);
    DO_SIMPLE_LO(in, +=, s);
    SCALE_PACK(FWD_SCALE25, FWD_SCALE20, s);
}

static void
ifilterL2(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK(INV_SCALE25, INV_SCALE20, s);
    DO_SIMPLE_LO(out, -=, s);
    DO_SIMPLE_HI(out, +=, s);
}

static void
filterL2_a(DSV_SBC *out, DSV_SBC *in, int n, int s, uint8_t *sb, int delta, int sbs)
{
    int i, sbp = 0, even_n = n & ~1, h = n + (n & 1);
    DO_SIMPLE_HI(in, -=, s);
    DO_5_TAP_LO_A(in, S20, S2A, S2S, R20, R2A, R2S, +=, s);
    SCALE_PACK(FWD_SCALE25, FWD_SCALE20, s);
}

static void
ifilterL2_a(DSV_SBC *out, DSV_SBC *in, int n, int s, uint8_t *sb, int delta, int sbs)
{
    int i, sbp = 0, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK(INV_SCALE25, INV_SCALE20, s);
    DO_5_TAP_LO_A(out, S20, S2A, S2S, R20, R2A, R2S, -=, s);
    DO_SIMPLE_HI(out, +=, s);
}

static void
filterL1(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    DO_5_TAP_HI(in, S10, S1A, S1S, -=, s);
    DO_5_TAP_LO(in, L10, L1A, L1S, +=, s);
    SCALE_PACK(FWD_SCALE20, FWD_SCALENONE, s);
}

static void
ifilterL1(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK(INV_SCALE20, INV_SCALENONE, s);
    DO_5_TAP_LO(out, L10, L1A, L1S, -=, s);
    DO_5_TAP_HI(out, S10, S1A, S1S, +=, s);
}

#define fwd_2d(tmp, in, fw, fh, lvl, filter) \
{ \
    int i, j, sw, sh; \
    sw = DSV_ROUND_SHIFT(fw, lvl - 1); \
    sh = DSV_ROUND_SHIFT(fh, lvl - 1); \
    for (j = 0; j < sh; j++) { \
        filter(tmp + fw * j, in + fw * j, sw, 1); \
    } \
    for (i = 0; i < sw; i++) { \
        filter(in + i, tmp + i, sh, fw); \
    } \
}

#define inv_2d(tmp, in, fw, fh, lvl, ifilter) \
{ \
    int i, j, sw, sh; \
    sw = DSV_ROUND_SHIFT(fw, lvl - 1); \
    sh = DSV_ROUND_SHIFT(fh, lvl - 1); \
    for (i = 0; i < sw; i++) { \
        ifilter(tmp + i, in + i, sh, fw); \
    } \
    for (j = 0; j < sh; j++) { \
        ifilter(in + fw * j, tmp + fw * j, sw, 1); \
    } \
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
fwd(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl)
{
    DSV_SBC *os, *od, *dpLL, *dpLH, *dpHL, *dpHH;
    int x0, x1, x2, x3;
    int x, y, woff, hoff, ws, hs, oddw, oddh;
    int idx;

    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    oddw = (ws & 1);
    oddh = (hs & 1);
    os = src;
    od = dst;

    dpLL = dst;
    dpLH = dst + woff;
    dpHL = dst + hoff * width;
    dpHH = dst + woff + (hoff) * width;
    for (y = 0; y < hs - oddh; y += 2) {
        DSV_SBC *spA, *spB;

        spA = src + (y + 0) * width;
        spB = src + (y + 1) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            x0 = spA[x + 0];
            x1 = spA[x + 1];
            x2 = spB[x + 0];
            x3 = spB[x + 1];

            dpLL[idx] = (x0 + x1 + x2 + x3); /* LL */
            dpLH[idx] = (x0 - x1 + x2 - x3); /* LH */
            dpHL[idx] = (x0 + x1 - x2 - x3); /* HL */
            dpHH[idx] = (x0 - x1 - x2 + x3); /* HH */
        }
        if (oddw) {
            x0 = spA[x + 0];
            x2 = spB[x + 0];

            dpLL[idx] = 2 * (x0 + x2); /* LL */
            dpHL[idx] = 2 * (x0 - x2); /* HL */
        }
        dpLL += width;
        dpLH += width;
        dpHL += width;
        dpHH += width;
    }
    if (oddh) {
        DSV_SBC *spA = src + (y + 0) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            x0 = spA[x + 0];
            x1 = spA[x + 1];

            dpLL[idx] = 2 * (x0 + x1); /* LL */
            dpLH[idx] = 2 * (x0 - x1); /* LH */
        }
        if (oddw) {
            x0 = spA[x + 0];

            dpLL[idx] = (x0 * 4); /* LL */
        }
    }
    cpysub(os, od, ws, hs, width);
}

/* C.3.1.1 Haar Simple Inverse Transform */
static void
inv_simple(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl)
{
    int x, y, woff, hoff, ws, hs, oddw, oddh;
    int LL, LH, HL, HH;
    int idx;
    DSV_SBC *os, *od, *spLL, *spLH, *spHL, *spHH;

    os = src;
    od = dst;

    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    oddw = (ws & 1);
    oddh = (hs & 1);

    spLL = src;
    spLH = src + woff;
    spHL = src + hoff * width;
    spHH = src + woff + hoff * width;
    for (y = 0; y < hs - oddh; y += 2) {
        DSV_SBC *dpA, *dpB;

        dpA = dst + (y + 0) * width;
        dpB = dst + (y + 1) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            LL = spLL[idx];
            LH = spLH[idx];
            HL = spHL[idx];
            HH = spHH[idx];

            dpA[x + 0] = (LL + LH + HL + HH) / 4; /* LL */
            dpA[x + 1] = (LL - LH + HL - HH) / 4; /* LH */
            dpB[x + 0] = (LL + LH - HL - HH) / 4; /* HL */
            dpB[x + 1] = (LL - LH - HL + HH) / 4; /* HH */
        }
        if (oddw) {
            LL = spLL[idx];
            HL = spHL[idx];

            dpA[x + 0] = (LL + HL) / 4; /* LL */
            dpB[x + 0] = (LL - HL) / 4; /* HL */
        }
        spLL += width;
        spLH += width;
        spHL += width;
        spHH += width;
    }
    if (oddh) {
        DSV_SBC *dpA = dst + (y + 0) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            LL = spLL[idx];
            LH = spLH[idx];

            dpA[x + 0] = (LL + LH) / 4; /* LL */
            dpA[x + 1] = (LL - LH) / 4; /* LH */
        }
        if (oddw) {
            LL = spLL[idx];

            dpA[x + 0] = (LL / 4); /* LL */
        }
    }
    cpysub(os, od, ws, hs, width);
}

/* C.3.1.2 Haar Filtered Inverse Transform */
static void
inv(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl, int hqp)
{
    int x, y, woff, hoff, ws, hs, oddw, oddh;
    int LL, LH, HL, HH;
    int idx;
    DSV_SBC *os, *od, *spLL, *spLH, *spHL, *spHH;

    os = src;
    od = dst;

    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    oddw = (ws & 1);
    oddh = (hs & 1);

    spLL = src;
    spLH = src + woff;
    spHL = src + hoff * width;
    spHH = src + woff + hoff * width;
    for (y = 0; y < hs - oddh; y += 2) {
        DSV_SBC *dpA, *dpB;
        int inY = y > 0 && y < (hs - oddh - 1);

        dpA = dst + (y + 0) * width;
        dpB = dst + (y + 1) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            int inX = x > 0 && x < (ws - oddw - 1);
            int nudge, t, lp, ln, mn, mx;

            LL = spLL[idx];
            LH = spLH[idx];
            HL = spHL[idx];
            HH = spHH[idx];

            if (inX) {
                lp = spLL[idx - 1]; /* prev */
                ln = spLL[idx + 1]; /* next */
                mx = LL - ln; /* find difference between LL values */
                mn = lp - LL;
                if (mn > mx) {
                    t = mn;
                    mn = mx;
                    mx = t;
                }
                mx = MIN(mx, 0); /* must be negative or zero */
                mn = MAX(mn, 0); /* must be positive or zero */
                /* if they are not zero, then there is a potential
                 * consistent smooth gradient between them */
                if (mx != mn) {
                    t = round4(lp - ln);
                    nudge = round2(CLAMP(t, mx, mn) - (LH * 2));
                    LH += CLAMP(nudge, -hqp, hqp); /* nudge LH to smooth it */
                }
            }
            if (inY) { /* do the same as above but in the Y direction */
                lp = spLL[idx - width];
                ln = spLL[idx + width];
                mx = LL - ln;
                mn = lp - LL;
                if (mn > mx) {
                    t = mn;
                    mn = mx;
                    mx = t;
                }
                mx = MIN(mx, 0);
                mn = MAX(mn, 0);
                if (mx != mn) {
                    t = round4(lp - ln);
                    nudge = round2(CLAMP(t, mx, mn) - (HL * 2));
                    HL += CLAMP(nudge, -hqp, hqp); /* nudge HL to smooth it */
                }
            }

            dpA[x + 0] = (LL + LH + HL + HH) / 4; /* LL */
            dpA[x + 1] = (LL - LH + HL - HH) / 4; /* LH */
            dpB[x + 0] = (LL + LH - HL - HH) / 4; /* HL */
            dpB[x + 1] = (LL - LH - HL + HH) / 4; /* HH */
        }
        if (oddw) {
            LL = spLL[idx];
            HL = spHL[idx];

            dpA[x + 0] = (LL + HL) / 4; /* LL */
            dpB[x + 0] = (LL - HL) / 4; /* HL */
        }
        spLL += width;
        spLH += width;
        spHL += width;
        spHH += width;
    }
    if (oddh) {
        DSV_SBC *dpA = dst + (y + 0) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            LL = spLL[idx];
            LH = spLH[idx];

            dpA[x + 0] = (LL + LH) / 4; /* LL */
            dpA[x + 1] = (LL - LH) / 4; /* LH */
        }
        if (oddw) {
            LL = spLL[idx];

            dpA[x + 0] = LL / 4; /* LL */
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
    for (y = 0; y < p->h; y++) {
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
            line[x] = v > 255 ? 255 : v < 0 ? 0 : v;
        }
        d += dc->width;
    }
}

/* C.3.3 Subband Recomposition - num_levels */
static int
nlevels(int w, int h)
{
    int lb2, mx;

    mx = (w > h) ? w : h;
    lb2 = dsv_lb2(mx);
    if (mx > (1 << lb2)) {
        lb2++;
    }
    return lb2;
}

extern void
dsv_fwd_sbt(DSV_PLANE *src, DSV_COEFS *dst, DSV_FMETA *fm)
{
    int lvls, l;
    int w = dst->width;
    int h = dst->height;
    DSV_SBC *temp_buf_pad;

    p2sbc(dst, src);

    lvls = nlevels(w, h);
    alloc_temp((w + 2) * (h + 2));
    temp_buf_pad = temp_buf + w;

    for (l = 1; l <= lvls; l++) {
        if (LL_CONDITION) {
            fwd_2d(temp_buf_pad, dst->data, w, h, l, filterLL);
        } else if (L2_CONDITION) {
            fwd_2d(temp_buf_pad, dst->data, w, h, l, filterL2);
        } else if (L2A_CONDITION) {
            fwd_L2a_2d(temp_buf_pad, dst->data, w, h, l, fm);
        } else if (L1_CONDITION) {
            fwd_2d(temp_buf_pad, dst->data, w, h, l, filterL1);
        } else {
            fwd(dst->data, temp_buf_pad, w, h, l);
        }
    }
}

/* C.3.3 Subband Recomposition */
extern void
dsv_inv_sbt(DSV_PLANE *dst, DSV_COEFS *src, int q, DSV_FMETA *fm)
{
    int lvls, l;
    int w = src->width;
    int h = src->height;
    DSV_SBC *temp_buf_pad;
    int hqp;

    lvls = nlevels(w, h);
    alloc_temp((w + 2) * (h + 2));
    temp_buf_pad = temp_buf + w;

    hqp = (fm->cur_plane == 0) ? (q / 8) : (q / 2);
    for (l = lvls; l > 0; l--) {
        if (LL_CONDITION) {
            inv_2d(temp_buf_pad, src->data, w, h, l, ifilterLL);
        } else if (L2_CONDITION) {
            inv_2d(temp_buf_pad, src->data, w, h, l, ifilterL2);
        } else if (L2A_CONDITION) {
            inv_L2a_2d(temp_buf_pad, src->data, w, h, l, fm);
        } else if (L1_CONDITION) {
            inv_2d(temp_buf_pad, src->data, w, h, l, ifilterL1);
        } else {
            if (fm->cur_plane == 0 || !fm->isP) {
                inv(src->data, temp_buf_pad, w, h, l, hqp);
            } else {
                inv_simple(src->data, temp_buf_pad, w, h, l);
            }
        }
    }

    sbc2p(dst, src);
}
