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
#include "dsv_encoder.h"

/* Hierarchical Zero Coefficient Coding */

#define EOP_SYMBOL 0x55 /* B.2.3.5 Image Data - Coefficient Coding */

#define MAXLVL   3
#define LVL1     (MAXLVL - 1) /* highest freq */
#define LVL2     (MAXLVL - 2) /* second highest freq */
#define LVL3     (MAXLVL - 3) /* third highest freq */
#define NSUBBAND 4 /* 0 = LL, 1 = LH, 2 = HL, 3 = HH */
#define LH       1
#define HL       2
#define HH       3

#define RUN_BITS 24

/* C.1 Subband Order and Traversal */
static int
subband(int level, int sub, int w, int h)
{
    int offset = 0;
    int shift = MAXLVL - level;
    DSV_ASSERT(shift >= 0);

    if (sub & 1) { /* L */
        offset += DSV_ROUND_SHIFT(w, shift);
    }
    if (sub & 2) { /* H */
        offset += DSV_ROUND_SHIFT(h, shift) * w;
    }
    return offset;
}

/* C.1 Subband Order and Traversal */
static int
dimat(int level, int v) /* dimension at level */
{
    return DSV_ROUND_SHIFT(v, MAXLVL - level);
}

/* larger dimensions -> higher freq is less important.
 * lower resolutions have a smaller factor and
 * higher resolutions have a larger factor
 * CIF(352x288)=0, FHD(1920x1080)=128 */
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
    scale = (scale << 7) / MAX(hi - lo, 1);
    return CLAMP(scale, 0, 128);
}

extern int
dsv_lfquant(DSV_FMETA *fm, int q)
{
    int psyfac;

    psyfac = dsv_spatial_psy_factor(fm->params, HH);

    q -= (q * psyfac >> (7 + (fm->isP ? 1 : 3)));
    q = MAX(q / 2, 2);
    /* prevent important lower level coefficients from getting destroyed */
    if (fm->cur_plane) {
        if (q > 256) {
            q = 256 + q / 4;
        }
        return MIN(q, 768);
    }
    return MIN(q, 3072);
}

extern int
dsv_hfquant(DSV_FMETA *fm, int q, int s, int l)
{
    int psyq, psyfac, chroma;

    chroma = (fm->cur_plane != 0);
    psyfac = dsv_spatial_psy_factor(fm->params, s);

    q /= 2;

    psyq = q * psyfac >> (7 + (fm->isP ? 0 : 1));

    if (chroma) {
        /* reduce based on subsampling */
        int tl;
        tl = l - 2;
        if (s == LH) {
            tl += DSV_FORMAT_H_SHIFT(fm->params->vidmeta->subsamp);
        } else if (s == HL) {
            tl += DSV_FORMAT_V_SHIFT(fm->params->vidmeta->subsamp);
        }
        q = (q * 6) / MAX(4 - tl, 1);
    } else {
        /* reduce higher frequencies appropriately */
        if (l == LVL2) {
            q += psyq / 2;
        } else if (l == LVL1) {
            q += psyq;
        }
    }
    if (fm->isP) {
        int div = 16;
        if (l == LVL3) {
            q = q * 3 / 2;
        }
        if (l != LVL1) {
            div += dsv_flb2(psyfac / 16) >> (5 + l);
        }
        return MAX(q * 4 / div, DSV_MINQUANT);
    }
    if (s == HH) { /* quantize HH more */
        q *= (l * 2 + 1);
    }
    q += q * l / 4;
    if (!chroma) {
        q = (q * (l * 3 + 3)) / 8;
    } else {
        q /= 4;
    }
    return MAX(q, DSV_MINQUANT);
}

#define TMQ4POS_I(tmq, flags, l)                                            \
        switch (l) {                                                        \
            case MAXLVL - 3:                                                \
                break;                                                      \
            default:                                                        \
            case MAXLVL - 2:                                                \
                switch ((flags) & (DSV_IS_STABLE | DSV_IS_MAINTAIN)) {      \
                    case DSV_IS_STABLE:                                     \
                        if (!parc) { tmq = tmq * 2 / 5; } else { tmq /= 3; }\
                        break;                                              \
                    case DSV_IS_MAINTAIN:                                   \
                        tmq >>= (((flags) & DSV_IS_RINGING) ?  2 : !parc);  \
                        break;                                              \
                    case DSV_IS_MAINTAIN | DSV_IS_STABLE:                   \
                        if (parc) { tmq /= 4; } else { tmq /= 6; }          \
                        break;                                              \
                    default:                                                \
                        break;                                              \
                }                                                           \
                break;                                                      \
            case MAXLVL - 1:                                                \
                switch ((flags) & (DSV_IS_STABLE | DSV_IS_MAINTAIN)) {      \
                    case DSV_IS_STABLE:                                     \
                        if (!parc) { tmq /= 3; } else { tmq /= 4; }         \
                        break;                                              \
                    case DSV_IS_MAINTAIN:                                   \
                        tmq >>= (((flags) & DSV_IS_RINGING) ? 2 : !parc);   \
                        break;                                              \
                    case DSV_IS_MAINTAIN | DSV_IS_STABLE:                   \
                        if (parc) { tmq /= 6; } else { tmq /= 8; }          \
                        break;                                              \
                    default:                                                \
                        break;                                              \
                }                                                           \
                break;                                                      \
        }

static int
quantLUMA_L(int v, int q)
{
    if (v < 0) {
        return -((-v) + q) / (2 * q);
    }
    return (v + q) / (2 * q);
}

static int
quantLUMA_INTRA(int v, int q)
{
    if (v < 0) {
        return -((-v) + ((q * 3 + 4) >> 3)) / (2 * q);
    }
    return (v + ((q * 3 + 4) >> 3)) / (2 * q);
}

static int
quantCHROMA(int v, int q)
{
    if (v < 0) {
        return -((-v) + ((q * 3 + 2) >> 2)) / (2 * q);
    }
    return (v + ((q * 3 + 2) >> 2)) / (2 * q);
}

static int
quantSUB(int v, int q, int sub, int subfac)
{
    if (v < 0) {
        return -(((-v) - sub) + (q * subfac >> 8)) / (2 * q);
    }
    return ((v - sub) + (q * subfac >> 8)) / (2 * q);
}

static int
quantSUB_P(int v, int q, int sub, int subfac)
{
    if (v < 0) {
        return -((-v) - (sub + (q * subfac >> 8))) / (2 * q);
    }
    return (v - (sub + (q * subfac >> 8))) / (2 * q);
}

#define quantDZ(v, q) ((v) / (2 * (q)))

#define quantSL(v, q) (isP ? \
        quantDZ(v,q) : \
        (fm->cur_plane ? quantCHROMA(v,q) : quantLUMA_L(v,q)))

#define dequantU(v, q) ((v) * 2 * (q)) /* uniform */
#define dequantDZ(v, q) ((q) * ((v) * 2 + ((v) >= 0 ? 1 : -1))) /* deadzone */
#define dequant(v, q, isP) (isP ? dequantDZ(v,q) : dequantU(v,q))

#define PUTV(bs, v)  (dsv_bs_put_nrice(bs, v, &vk, &vavg))
#define GETV(bs)     (dsv_bs_get_nrice(bs, &vk, &vavg))

static void
hzcc_enc(DSV_BS *bs, DSV_SBC *src, int w, int h, int q, DSV_FMETA *fm)
{
    int x, y, l, s, o, v;
    int sw, sh;
    int qp;
    int run = 0;
    int nruns = 0;
    DSV_SBC *srcp;
    int startp, endp;
    int isP;
    unsigned vk = 0, vavg = 0; /* for Rice encoding */

    dsv_bs_align(bs);
    startp = dsv_bs_ptr(bs);
    dsv_bs_put_bits(bs, RUN_BITS, 0);
    dsv_bs_align(bs);

    s = l = 0;
    isP = fm->isP;

    sw = dimat(l, w);
    sh = dimat(l, h);
    qp = dsv_lfquant(fm, q);

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
                            PUTV(bs, v);
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
                v = quantSL(srcp[x], qp);
                if (v) {
                    srcp[x] = dequant(v, qp, isP);
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
            uint8_t *sbfacrow;
            int psyI, psyP;
            int dbx, dby, dvx, dvy;
            int bx, by, vx, vy;

            sw = dimat(l, w);
            sh = dimat(l, h);
            dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / sw;
            dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / sh;
            dvx = ((fm->params->nblocks_h * 2) << DSV_BLOCK_INTERP_P) / sw;
            dvy = ((fm->params->nblocks_v * 2) << DSV_BLOCK_INTERP_P) / sh;
            qp = q;
            psyI = (fm->params->do_psy & DSV_PSY_I_VISUAL_MASKING);
            psyP = (fm->params->do_psy & DSV_PSY_P_VISUAL_MASKING);
            /* C.2.4 Higher Level Subbands */
            for (s = 1; s < NSUBBAND; s++) {
                int par;
                DSV_SBC *parent;

                par = subband(l - 1, s, w, h);
                o = subband(l, s, w, h);
                qp = dsv_hfquant(fm, q, s, l);

                srcp = src + o;
                by = 0;
                vy = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    vx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    sbfacrow = fm->sb_facs + (vy >> DSV_BLOCK_INTERP_P) * 2 * fm->params->nblocks_h;

                    parent = src + par + ((y >> 1) * w);
                    for (x = 0; x < sw; x++) {
                        int tmq = qp;
                        int flags = blockrow[bx >> DSV_BLOCK_INTERP_P];
                        int parc = parent[x >> 1];
                        int varr = sbfacrow[vx >> DSV_BLOCK_INTERP_P];

                        if (isP) {
                            if (fm->cur_plane) {
                                v = quantDZ(srcp[x], tmq);
                            } else if (psyP) {
                                if (flags & DSV_IS_SIMCMPLX) {
                                    v = quantSUB_P(srcp[x], tmq, tmq >> 2, varr);
                                } else {
                                    v = quantSUB_P(srcp[x], tmq, -(tmq >> 2), varr);
                                }
                            } else {
                                v = quantDZ(srcp[x], tmq);
                            }
                        } else {
                            TMQ4POS_I(tmq, flags, l);
                            /* psychovisual: visual masking */
                            if (fm->cur_plane) {
                                v = quantCHROMA(srcp[x], tmq);
                            } else if (psyI) {
                                if (flags & DSV_IS_RINGING) {
                                    v = quantSUB(srcp[x], tmq, -tmq / 4, varr);
                                } else {
                                    int edge = DSV_SIGNOF(parc) == DSV_SIGNOF(srcp[x]);
                                    v = quantSUB(srcp[x], tmq,
                                            (edge || parc) ? (-tmq / 4) : (-tmq / 8),
                                                    (edge && varr >= 160) ? 0 : varr);
                                }
                            } else {
                                v = quantLUMA_INTRA(srcp[x], tmq);
                            }
                        }
                        if (v) {
                            srcp[x] = dequant(v, tmq, isP);
                            dsv_bs_put_ueg(bs, run);
                            PUTV(bs, v);
                            run = -1;
                            nruns++;
                        } else {
                            srcp[x] = 0;
                        }
                        run++;
                        bx += dbx;
                        vx += dvx;
                    }
                    srcp += w;
                    by += dby;
                    vy += dvy;
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
    unsigned vk = 0, vavg = 0; /* for Rice decoding */

    dsv_bs_align(bs);
    runs = dsv_bs_get_bits(bs, RUN_BITS);
    dsv_bs_align(bs);

    s = l = 0;
    isP = fm->isP;

    sw = dimat(l, w);
    sh = dimat(l, h);
    qp = dsv_lfquant(fm, q);

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
                            v = GETV(bs);
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
                    outp[x] = dequant(v, qp, isP);
                }
            }
            outp += w;
        }
        for (l = 0; l < MAXLVL; l++) {
            uint8_t *blockrow;
            DSV_SBC *parent;

            sw = dimat(l, w);
            sh = dimat(l, h);
            dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / sw;
            dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / sh;
            qp = q;
            /* C.2.4 Higher Level Subband Dequantization */
            for (s = 1; s < NSUBBAND; s++) {
                int par;
                par = subband(l - 1, s, w, h);
                o = subband(l, s, w, h);
                qp = dsv_hfquant(fm, q, s, l);

                outp = out + o;
                by = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    parent = out + par + ((y >> 1) * w);
                    for (x = 0; x < sw; x++) {
                        if (!run--) {
                            int tmq = qp;
                            v = GETV(bs);
                            run = (runs-- > 0) ? dsv_bs_get_ueg(bs) : INT_MAX;
                            if (dsv_bs_ptr(bs) >= bufsz) {
                                return;
                            }
                            if (!isP) {
                                int flags = blockrow[bx >> DSV_BLOCK_INTERP_P];
                                int parc = parent[x >> 1];
                                TMQ4POS_I(tmq, flags, l);
                            }
                            outp[x] = dequant(v, tmq, isP);
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
