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
#include "dsv_encoder.h"

/* Hierarchical Zero Coefficient Coding */

#define EOP_SYMBOL 0x55 /* B.2.3.5 Image Data - Coefficient Coding */

#define MAXLVL   3
#define NSUBBAND 4 /* 0 = LL, 1 = LH, 2 = HL, 3 = HH */
#define LH       1
#define HL       2
#define HH       3

#define MINQP    3
#define MINQUANT (1 << MINQP)  /* C.2 MINQUANT */

#define RUN_BITS 24

/* C.1 Subband Order and Traversal */
static int
subband(int level, int sub, int w, int h)
{
    int offset = 0;
    if (sub & 1) { /* L */
        offset += DSV_ROUND_SHIFT(w, MAXLVL - level);
    }
    if (sub & 2) { /* H */
        offset += DSV_ROUND_SHIFT(h, MAXLVL - level) * w;
    }
    return offset;
}

/* C.1 Subband Order and Traversal */
static int
dimat(int level, int v) /* dimension at level */
{
    return DSV_ROUND_SHIFT(v, MAXLVL - level);
}

static int
fix_quant(int q)
{
    return q * 3 / 2;
}

/* larger dimensions -> higher freq is less important */
extern int
dsv_spatial_psy_factor(DSV_PARAMS *p, int subband)
{
    int scale, lo, hi;
    /* between CIF and FHD */
    if (subband == LH) {
        lo = DSV_UDIV_ROUND_UP(352, p->blk_w);
        hi = DSV_UDIV_ROUND_UP(1920, p->blk_w);
        scale = p->nblocks_h;
    } else if (subband == HL) {
        lo = DSV_UDIV_ROUND_UP(288, p->blk_h);
        hi = DSV_UDIV_ROUND_UP(1080, p->blk_h);
        scale = p->nblocks_v;
    } else {
        lo = DSV_UDIV_ROUND_UP(352, p->blk_w) * DSV_UDIV_ROUND_UP(288, p->blk_h);
        hi = DSV_UDIV_ROUND_UP(1920, p->blk_w) * DSV_UDIV_ROUND_UP(1080, p->blk_h);
        scale = p->nblocks_h * p->nblocks_v;
    }
    scale = MAX(0, scale - lo);
    return (scale << 7) / (hi - lo);
}

static int
lfquant(int q, DSV_FMETA *fm)
{
    int psyfac;

    psyfac = dsv_spatial_psy_factor(fm->params, HH);

    q -= (q * psyfac >> (7 + 3));
    q = MAX(q, MINQUANT);
    /* prevent important lower level coefficients from getting destroyed */
    if (fm->cur_plane) {
        if (q > 256) {
            q = 256 + q / 4;
        }
        return MIN(q, 768);
    }
    return MIN(q, 3072);
}

static int
hfquant(DSV_FMETA *fm, int q, int s, int l)
{
    int psyfac, chroma;

    chroma = (fm->cur_plane != 0);
    psyfac = dsv_spatial_psy_factor(fm->params, s);
    q /= 2;

    psyfac = q * psyfac >> (7 + (fm->isP ? 0 : 1));

    if (chroma) {
        /* reduce based on subsampling */
        int tl;
        tl = l - 2;
        if (s == LH) {
            tl += DSV_FORMAT_H_SHIFT(fm->params->vidmeta->subsamp);
        } else if (s == HL) {
            tl += DSV_FORMAT_V_SHIFT(fm->params->vidmeta->subsamp);
        }
        q = (q * 6) / (4 - tl);
    } else {
        /* reduce higher frequencies appropriately */
        if (l == (MAXLVL - 2)) {
            q += psyfac / 2;
        } else if (l == (MAXLVL - 1)) {
            q += psyfac;
        }
    }

    if (fm->isP) {
        if (l != (MAXLVL - 1)) {
            if (l == (MAXLVL - 3)) {
                q *= 2;
                q -= psyfac;
            } else {
                q -= psyfac / 2;
            }
        }
        return MAX(q / 4, MINQUANT);
    }
    q = q * (15 + 3 * l) / 16;
    if (!chroma) {
        if (l == (MAXLVL - 3)) { /* luma level 3 (Haar) needs to be quantized less */
            q = (q * 3) / 8;
        } else if (s == HH) { /* quantize HH more */
            q *= 2;
        }
    } else {
        q /= 4;
        if (s == HH) { /* quantize HH more */
            q *= 2;
        }
    }
    return MAX(q, MINQUANT);
}

#define TMQ4POS_P(tmq, flags)                                        \
    if ((flags) & (DSV_IS_EPRM | DSV_IS_STABLE | DSV_IS_INTRA)) {    \
        tmq = (tmq) * 3 >> 2;                                        \
    }

#define TMQ4POS_I(tmq, flags, l)                                     \
    switch (l) {                                                     \
        case MAXLVL - 3:                                             \
            break;                                                   \
        default:                                                     \
        case MAXLVL - 2:                                             \
            switch ((flags) & (DSV_IS_STABLE | DSV_IS_MAINTAIN)) {   \
                case DSV_IS_STABLE:                                  \
                    tmq /= 3;                                        \
                    break;                                           \
                case DSV_IS_MAINTAIN:                                \
                    tmq /= (((flags) & DSV_IS_RINGING) ? 6 : 3);     \
                    break;                                           \
                case DSV_IS_MAINTAIN | DSV_IS_STABLE:                \
                    tmq >>= 2;                                       \
                    break;                                           \
                default:                                             \
                    break;                                           \
            }                                                        \
            break;                                                   \
        case MAXLVL - 1:                                             \
            switch ((flags) & (DSV_IS_STABLE | DSV_IS_MAINTAIN)) {   \
                case DSV_IS_STABLE:                                  \
                    tmq >>= 2;                                       \
                    break;                                           \
                case DSV_IS_MAINTAIN:                                \
                    tmq >>= (((flags) & DSV_IS_RINGING) ? 2 : 1);    \
                    break;                                           \
                case DSV_IS_MAINTAIN | DSV_IS_STABLE:                \
                    tmq >>= 2;                                       \
                    break;                                           \
                default:                                             \
                    break;                                           \
            }                                                        \
            break;                                                   \
    }



#define quantSUB(v, q, sub) ((((v) >= 0 ? (v) - (sub) : (v) + (sub)) / (q)))

static int
quantRI(int v, int q)
{
    if (abs(v) < q * 7 / 8) {
        return 0;
    }
    if (v < 0) {
        return (v - (q / 3)) / q;
    }
    return (v + (q / 3)) / q;
}

#define quantS(v, q) ((v) / (q))

#define dequantL(v,q) (isP ? dequantD(v,q) : dequantS(v,q))
#define dequantH(v,q) (dequantD(v, q))

/* uses estimator which saturates the value more */
static DSV_SBC
dequantS(int v, unsigned q)
{
    return (v * q) + ((v < 0) ? -(q * 2 / 3) : (q * 2 / 3));
}

/* default estimator */
static DSV_SBC
dequantD(int v, unsigned q)
{
    return (v * q) + ((v < 0) ? -(q / 2) : (q / 2));
}

static void
hzcc_enc(DSV_BS *bs, DSV_SBC *src, int w, int h, int q, DSV_FMETA *fm)
{
    int x, y, l, s, o, v;
    int sw, sh;
    int bx, by;
    int dbx, dby;
    int qp;
    int run = 0;
    int nruns = 0;
    DSV_SBC *srcp;
    int startp, endp;
    int isP;

    dsv_bs_align(bs);
    startp = dsv_bs_ptr(bs);
    dsv_bs_put_bits(bs, RUN_BITS, 0);
    dsv_bs_align(bs);

    q = fix_quant(q);

    s = l = 0;
    isP = fm->isP;

    sw = dimat(l, w);
    sh = dimat(l, h);
    qp = lfquant(q, fm);

    /* write the 'LL' part */
    o = subband(l, s, w, h);
    src[0] = 0;
    srcp = src + o;

    if (fm->params->lossless) {
        for (y = 0; y < sh; y++) {
            for (x = 0; x < sw; x++) {
                v = srcp[x];
                if (v) {
                    dsv_bs_put_ueg(bs, run);
                    dsv_bs_put_neg(bs, v);
                    run = -1;
                    nruns++;
                }
                run++;
            }
            srcp += w;
        }
        for (l = 0; l < MAXLVL; l++) {
            sw = dimat(l, w);
            sh = dimat(l, h);
            /* C.2.4 Higher Level Subbands */
            for (s = 1; s < NSUBBAND; s++) {
                o = subband(l, s, w, h);

                srcp = src + o;
                for (y = 0; y < sh; y++) {
                    for (x = 0; x < sw; x++) {
                        v = srcp[x];
                        if (v) {
                            dsv_bs_put_ueg(bs, run);
                            dsv_bs_put_neg(bs, v);
                            run = -1;
                            nruns++;
                        } else {
                            srcp[x] = 0;
                        }
                        run++;
                    }
                    srcp += w;
                }
            }
        }
    } else {
        /* C.2.3 LL Subband */
        for (y = 0; y < sh; y++) {
            for (x = 0; x < sw; x++) {
                v = quantS(srcp[x], qp);
                if (v) {
                    srcp[x] = dequantL(v, qp);
                    dsv_bs_put_ueg(bs, run);
                    dsv_bs_put_neg(bs, v);
                    run = -1;
                    nruns++;
                } else {
                    srcp[x] = 0;
                }
                run++;
            }
            srcp += w;
        }

        for (l = 0; l < MAXLVL; l++) {
            uint8_t *blockrow;
            DSV_SBC *parent;
            int psyluma;

            sw = dimat(l, w);
            sh = dimat(l, h);
            dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / sw;
            dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / sh;
            qp = q;
            psyluma = (fm->params->do_psy & (isP ? DSV_PSY_P_VISUAL_MASKING : DSV_PSY_I_VISUAL_MASKING))
                        && !fm->cur_plane && (l != (MAXLVL - 3));
            /* C.2.4 Higher Level Subbands */
            for (s = 1; s < NSUBBAND; s++) {
                int par;
                par = subband(l - 1, s, w, h);
                o = subband(l, s, w, h);
                qp = hfquant(fm, q, s, l);

                srcp = src + o;
                by = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    parent = src + par + ((y >> 1) * w);
                    for (x = 0; x < sw; x++) {
                        int tmq = qp;
                        int flags = blockrow[bx >> DSV_BLOCK_INTERP_P];
                        if (isP) {
                            TMQ4POS_P(tmq, flags);
                            if (psyluma && (flags & DSV_IS_SIMCMPLX)) {
                                v = quantSUB(srcp[x], tmq, tmq >> 2);
                            } else {
                                v = quantS(srcp[x], tmq);
                            }
                        } else {
                            TMQ4POS_I(tmq, flags, l);
                            /* psychovisual: visual masking */
                            if (psyluma && !(flags & DSV_IS_STABLE)) {
                                v = 0;
                                if (srcp[x]) {
                                    int parc = parent[x >> 1];
                                    if (parc) {
                                        int absrc, tm;

                                        absrc = abs(srcp[x]);
                                        tm = (q * abs(parc) / absrc) >> (7 - l);
                                        if (tm < tmq && tm < absrc) {
                                            v = quantSUB(srcp[x], tmq, tm);
                                        }
                                    } else {
                                        v = quantRI(srcp[x], tmq);
                                    }
                                }
                            } else {
                                v = quantS(srcp[x], tmq);
                            }
                        }
                        if (v) {
                            srcp[x] = dequantH(v, tmq);
                            dsv_bs_put_ueg(bs, run);
                            dsv_bs_put_neg(bs, v);
                            run = -1;
                            nruns++;
                        } else {
                            srcp[x] = 0;
                        }
                        run++;
                        bx += dbx;
                    }
                    srcp += w;
                    by += dby;
                }
            }
        }
    }

    dsv_bs_align(bs);
    endp = dsv_bs_ptr(bs);
    dsv_bs_set(bs, startp);
    dsv_bs_put_bits(bs, RUN_BITS, nruns);
    dsv_bs_set(bs, endp);
    dsv_bs_align(bs);
}

static void
hzcc_dec(DSV_BS *bs, unsigned bufsz, DSV_COEFS *dst, int q, DSV_FMETA *fm)
{
    int x, y, l, s, o, v;
    int qp;
    int sw, sh;
    int bx, by;
    int dbx, dby;
    int run, runs;
    DSV_SBC *out = dst->data;
    DSV_SBC *outp;
    int w = dst->width;
    int h = dst->height;
    int isP;

    dsv_bs_align(bs);
    runs = dsv_bs_get_bits(bs, RUN_BITS);
    dsv_bs_align(bs);

    q = fix_quant(q);
    s = l = 0;
    isP = fm->isP;

    sw = dimat(l, w);
    sh = dimat(l, h);
    qp = lfquant(q, fm);

    o = subband(l, s, w, h);
    outp = out + o;

    run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;

    if (fm->params->lossless) {
        /* C.2.3 LL Subband */
        for (y = 0; y < sh; y++) {
            for (x = 0; x < sw; x++) {
                if (!run--) {
                    v = dsv_bs_get_neg(bs);
                    run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;
                    if (dsv_bs_ptr(bs) >= bufsz) {
                        return;
                    }
                    outp[x] = v;
                }
            }
            outp += w;
        }
        for (l = 0; l < MAXLVL; l++) {
            sw = dimat(l, w);
            sh = dimat(l, h);
            /* C.2.4 Higher Level Subband Dequantization */
            for (s = 1; s < NSUBBAND; s++) {
                o = subband(l, s, w, h);
                outp = out + o;
                for (y = 0; y < sh; y++) {
                    for (x = 0; x < sw; x++) {
                        if (!run--) {
                            v = dsv_bs_get_neg(bs);
                            run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;
                            if (dsv_bs_ptr(bs) >= bufsz) {
                                return;
                            }
                            outp[x] = v;
                        }
                    }
                    outp += w;
                }
            }
        }
    } else {
        /* C.2.3 LL Subband */
        for (y = 0; y < sh; y++) {
            for (x = 0; x < sw; x++) {
                if (!run--) {
                    v = dsv_bs_get_neg(bs);
                    run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;
                    if (dsv_bs_ptr(bs) >= bufsz) {
                        return;
                    }
                    outp[x] = dequantL(v, qp);
                }
            }
            outp += w;
        }
        for (l = 0; l < MAXLVL; l++) {
            uint8_t *blockrow;

            sw = dimat(l, w);
            sh = dimat(l, h);
            dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / sw;
            dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / sh;
            qp = q;
            /* C.2.4 Higher Level Subband Dequantization */
            for (s = 1; s < NSUBBAND; s++) {
                o = subband(l, s, w, h);
                qp = hfquant(fm, q, s, l);

                outp = out + o;
                by = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    for (x = 0; x < sw; x++) {
                        if (!run--) {
                            int tmq = qp;
                            int flags = blockrow[bx >> DSV_BLOCK_INTERP_P];
                            v = dsv_bs_get_neg(bs);
                            run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;
                            if (dsv_bs_ptr(bs) >= bufsz) {
                                return;
                            }
                            if (isP) {
                                TMQ4POS_P(tmq, flags);
                            } else {
                                TMQ4POS_I(tmq, flags, l);
                            }
                            outp[x] = dequantH(v, tmq);
                        }
                        bx += dbx;
                    }
                    outp += w;
                    by += dby;
                }
            }
        }
    }
    dsv_bs_align(bs);
}

extern void
dsv_encode_plane(DSV_BS *bs, DSV_COEFS *src, int q, DSV_FMETA *fm)
{
    int startp, endp, w, h;
    DSV_SBC LL, *d = src->data;

    w = src->width;
    h = src->height;

    dsv_bs_align(bs);
    startp = dsv_bs_ptr(bs);

    dsv_bs_put_bits(bs, 32, 0);

    LL = d[0]; /* save the LL value because we don't want to quantize it */
    dsv_bs_put_seg(bs, LL);
    hzcc_enc(bs, d, w, h, q, fm);
    d[0] = LL; /* restore unquantized LL */

    dsv_bs_put_bits(bs, 8, EOP_SYMBOL); /* 'end of plane' symbol */
    dsv_bs_align(bs);

    endp = dsv_bs_ptr(bs);
    dsv_bs_set(bs, startp);
    dsv_bs_put_bits(bs, 32, (endp - startp) - 4);
    dsv_bs_set(bs, endp);
    dsv_bs_align(bs);
    DSV_INFO(("encoded plane (%dx%d) to %u bytes. quant = %d", src->width, src->height, endp - startp, q));
}

/* B.2.3.5 Image Data - Coefficient Decoding */
extern int
dsv_decode_plane(DSV_BS *bs, DSV_COEFS *dst, int q, DSV_FMETA *fm)
{
    int success = 1;
    unsigned plen;

    dsv_bs_align(bs);

    plen = dsv_bs_get_bits(bs, 32);

    dsv_bs_align(bs);
    if (plen > 0 && plen < (dst->width * dst->height * sizeof(DSV_SBC) * 2)) {
        DSV_SBC LL;
        unsigned start = dsv_bs_ptr(bs);

        LL = dsv_bs_get_seg(bs);
        hzcc_dec(bs, start + plen, dst, q, fm);
        dst->data[0] = LL;

        /* error detection */
        if (dsv_bs_get_bits(bs, 8) != EOP_SYMBOL) {
            DSV_ERROR(("bad eop, frame data incomplete and/or corrupt"));
            success = 0;
        }
        dsv_bs_align(bs);

        dsv_bs_set(bs, start);
        dsv_bs_skip(bs, plen);
    } else {
        DSV_ERROR(("plane length was strange: %d", plen));
        success = 0;
    }
    return success;
}
