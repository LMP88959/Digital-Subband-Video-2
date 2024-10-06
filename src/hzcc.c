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

/* Hierarchical Zero Coefficient Coding */

#define EOP_SYMBOL 0x55 /* B.2.3.5 Image Data - Coefficient Coding */

#define CHROMA_LIMIT 512  /* C.2 Dequantization - chroma limit */

#define MAXLVL   3
#define NSUBBAND 4 /* 0 = LL, 1 = LH, 2 = HL, 3 = HH */
#define HH       3

#define MINQP    4
#define MINQUANT (1 << MINQP)  /* C.2 MINQUANT */

#define QP_I     4
#define QP_P     3

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

/* C.2.2 Quantization Parameter Derivation
 *     - dynamic_adjust */
static int
dyn_adjust(int q, int f)
{
    return q + ((DSV_MAX_QP - q) >> (DSV_MAX_QP_BITS - f));
}

static int
fix_quant(int q, DSV_FMETA *fm)
{
    if (fm->cur_plane != 0) {
        if (q > CHROMA_LIMIT) {
            q = CHROMA_LIMIT;
        }
    }
    if (!fm->isP) {
        q = dyn_adjust(q, 6);
    } else {
        q = dyn_adjust(q, 5);
    }
    return q;
}

/* C.2.2 Quantization Parameter Derivation
 *     - get_quant_lower_frequency */
static int
lfquant(DSV_FMETA *fm, int q, int s)
{
    if (fm->cur_plane == 0) {
        q = dyn_adjust(q, 8);
    } else {
        q = dyn_adjust(q, 4);
    }
    if (fm->isP) {
        if (fm->cur_plane == 0) {
            return q / 8;
        }
        return q / 4;
    }
    if (s != HH) { /* quantize HH more */
        q /= 2;
    }
    if (q < MINQUANT) {
        q = MINQUANT;
    }
    return q;
}

/* C.2.2 Quantization Parameter Derivation
 *     - get_quant_highest_frequency */
static int
hfquant(DSV_FMETA *fm, int qlb2, int s, int *qp_h)
{
    int qp, qph;

    qlb2 += (DSV_MAX_QP_BITS - qlb2) >> 2;
    if (fm->cur_plane == 0) {
        if (fm->isP) {
            qp = MAX(qlb2 - QP_P, MINQP);
            qph = qp;
        } else {
            qp = MAX(qlb2, MINQP);
            qph = MAX(qp - QP_I, MINQP);
        }
    } else {
        qp = MAX(qlb2, MINQP);
        qph = qp;
    }
    if (!fm->isP && s == HH) {
        qp += 2;
        qph += 2;
    }
    *qp_h = qph;
    return qp;
}

static int
tmq4pos(int q, int bits, int l, int c, int isP)
{
    if (isP) {
        if (bits & DSV_IS_SKIP) {
            return q << 1;
        }
        if (!c && (bits & DSV_IS_INTRA)) {
            return q;
        }
        return q + (q >> 1);
    }
    bits &= (DSV_IS_STABLE | DSV_IS_MAINTAIN);
    /* harder to see high freq chroma especially among visually significant luma */
    if (c && l == (MAXLVL - 2) && bits) {
        q <<= 1;
    }

    switch (bits) {
        case DSV_IS_STABLE:
            switch (l) {
                case MAXLVL - 3:
                    break;
                case MAXLVL - 2:
                    q >>= 1;
                    break;
            }
            break;
        case DSV_IS_MAINTAIN:
            switch (l) {
                case MAXLVL - 3:
                    break;
                case MAXLVL - 2:
                    q /= 3;
                    break;
            }
            break;
        case DSV_IS_MAINTAIN | DSV_IS_STABLE:
            switch (l) {
                case MAXLVL - 3:
                    q -= q >> 2;
                    break;
                case MAXLVL - 2:
                    q >>= 2;
                    break;
            }
            break;
        default:
            break;
    }

    return q + 1;
}

static int
quant(DSV_SBC v, int q)
{
    return ((v >= 0 ? v - (q >> 3) : v + (q >> 3)) / q);
}

/* C.2.1 Dequantization Functions - dequantize_lower_frequency */
static DSV_SBC
dequant(int v, int q)
{
    if (v < 0) {
        return ((v * q) - (q >> 1));
    }
    return ((v * q) + (q >> 1));
}

static int
quantH(DSV_SBC v, unsigned q)
{
    return (v < 0) ? -((-v) >> q) : (v >> q);
}

/* C.2.1 dequantize_highest_frequency */
static DSV_SBC
dequantH(int v, unsigned q)
{
    if (v < 0) {
        return -((-v << q) + ((1 << q) >> 1));
    }
    return ((v << q) + ((1 << q) >> 1));
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
    int stored_v = 0;
    DSV_SBC *srcp;
    int startp, endp;

    dsv_bs_align(bs);
    startp = dsv_bs_ptr(bs);
    dsv_bs_put_bits(bs, RUN_BITS, 0);
    dsv_bs_align(bs);

    q = fix_quant(q, fm);

    s = l = 0;

    sw = dimat(l, w);
    sh = dimat(l, h);
    qp = q;

    /* write the 'LL' part */
    o = subband(l, s, w, h);
    src[0] = 0;
    srcp = src + o;

    /* C.2.3 LL Subband */
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            v = quant(srcp[x], qp);
            if (v) {
                srcp[x] = dequant(v, qp);
                dsv_bs_put_ueg(bs, run);
                if (stored_v) {
                    dsv_bs_put_neg(bs, stored_v);
                }
                run = -1;
                nruns++;
                stored_v = v;
            } else {
                srcp[x] = 0;
            }
            run++;
        }
        srcp += w;
    }

    for (l = 0; l < MAXLVL; l++) {
        uint8_t *blockrow;
        int tmq;

        sw = dimat(l, w);
        sh = dimat(l, h);
        dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / sw;
        dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / sh;
        qp = q;
        if (l == (MAXLVL - 1)) {
            int qlb2 = dsv_lb2(qp);

            /* C.2.5 Highest Level Subband */
            for (s = 1; s < NSUBBAND; s++) {
                int qp_h;

                o = subband(l, s, w, h);
                qp = hfquant(fm, qlb2, s, &qp_h);
                srcp = src + o;
                by = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    for (x = 0; x < sw; x++) {
                        tmq = (blockrow[bx >> DSV_BLOCK_INTERP_P] & DSV_IS_STABLE) ? qp_h : qp;
                        v = quantH(srcp[x], tmq);
                        if (v) {
                            srcp[x] = dequantH(v, tmq);
                            dsv_bs_put_ueg(bs, run);
                            if (stored_v) {
                                dsv_bs_put_neg(bs, stored_v);
                            }
                            run = -1;
                            nruns++;
                            stored_v = v;
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
        } else {
            /* C.2.4 Higher Level Subbands */
            for (s = 1; s < NSUBBAND; s++) {
                o = subband(l, s, w, h);
                qp = lfquant(fm, q, s);
                srcp = src + o;
                by = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    for (x = 0; x < sw; x++) {
                        tmq = tmq4pos(qp, blockrow[bx >> DSV_BLOCK_INTERP_P], l, fm->cur_plane, fm->isP);
                        v = quant(srcp[x], tmq);
                        if (v) {
                            srcp[x] = dequant(v, tmq);
                            dsv_bs_put_ueg(bs, run);
                            if (stored_v) {
                                dsv_bs_put_neg(bs, stored_v);
                            }
                            run = -1;
                            nruns++;
                            stored_v = v;
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

    if (stored_v) {
        dsv_bs_put_neg(bs, stored_v);
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

    dsv_bs_align(bs);
    runs = dsv_bs_get_bits(bs, RUN_BITS);
    dsv_bs_align(bs);
    if (runs-- > 0) {
        run = dsv_bs_get_ueg(bs);
    } else {
        run = INT_MAX;
    }
    q = fix_quant(q, fm);
    s = l = 0;

    sw = dimat(l, w);
    sh = dimat(l, h);
    qp = q;

    o = subband(l, s, w, h);
    outp = out + o;

    /* C.2.3 LL Subband */
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            if (!run--) {
                run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;
                v = dsv_bs_get_neg(bs);
                if (dsv_bs_ptr(bs) >= bufsz) {
                    return;
                }
                outp[x] = dequant(v, qp);
            }
        }
        outp += w;
    }

    for (l = 0; l < MAXLVL; l++) {
        uint8_t *blockrow;
        int tmq;

        sw = dimat(l, w);
        sh = dimat(l, h);
        dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / sw;
        dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / sh;
        qp = q;
        /* C.2.5 Highest Level Subband Dequantization */
        if (l == (MAXLVL - 1)) {
            int qlb2 = dsv_lb2(qp);

            for (s = 1; s < NSUBBAND; s++) {
                int qp_h;

                o = subband(l, s, w, h);
                qp = hfquant(fm, qlb2, s, &qp_h);
                outp = out + o;
                by = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    for (x = 0; x < sw; x++) {
                        if (!run--) {
                            run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;
                            v = dsv_bs_get_neg(bs);
                            if (dsv_bs_ptr(bs) >= bufsz) {
                                return;
                            }
                            tmq = (blockrow[bx >> DSV_BLOCK_INTERP_P] & DSV_IS_STABLE) ? qp_h : qp;
                            outp[x] = dequantH(v, tmq);
                        }
                        bx += dbx;
                    }
                    outp += w;
                    by += dby;
                }
            }
        } else {
            /* C.2.4 Higher Level Subband Dequantization */
            for (s = 1; s < NSUBBAND; s++) {
                o = subband(l, s, w, h);
                qp = lfquant(fm, q, s);
                outp = out + o;
                by = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    for (x = 0; x < sw; x++) {
                        if (!run--) {
                            run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;
                            v = dsv_bs_get_neg(bs);
                            if (dsv_bs_ptr(bs) >= bufsz) {
                                return;
                            }
                            tmq = tmq4pos(qp, blockrow[bx >> DSV_BLOCK_INTERP_P], l, fm->cur_plane, fm->isP);
                            outp[x] = dequant(v, tmq);
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
    int w = src->width;
    int h = src->height;
    DSV_SBC *d = src->data;
    int LL, startp, endp;

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
extern void
dsv_decode_plane(uint8_t *in, unsigned s, DSV_COEFS *dst, int q, DSV_FMETA *fm)
{
    DSV_BS bs;
    int LL;

    dsv_bs_init(&bs, in);
    LL = dsv_bs_get_seg(&bs);
    hzcc_dec(&bs, s, dst, q, fm);

    /* error detection */
    if (dsv_bs_get_bits(&bs, 8) != EOP_SYMBOL) {
        DSV_ERROR(("bad eop, frame data incomplete and/or corrupt"));
    }
    dsv_bs_align(&bs);

    dst->data[0] = LL;
}
