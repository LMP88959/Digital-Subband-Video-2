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

#define RC_QUAL_PCT(pct) ((pct) * DSV_RC_QUAL_SCALE)

static void
encdat_ref(DSV_ENCDATA *d)
{
    DSV_ASSERT(d && d->refcount > 0);
    d->refcount++;
}

static void
encdat_unref(DSV_ENCODER *enc, DSV_ENCDATA *d)
{
    int i;

    DSV_ASSERT(d && d->refcount > 0);
    d->refcount--;

    if (d->refcount != 0) {
        return;
    }

    if (d->padded_frame) {
        dsv_frame_ref_dec(d->padded_frame);
    }
    for (i = 0; i < enc->pyramid_levels; i++) {
        if (d->pyramid[i]) {
            dsv_frame_ref_dec(d->pyramid[i]);
        }
    }
    if (d->recon_frame) {
        dsv_frame_ref_dec(d->recon_frame);
        d->recon_frame = NULL;
    }
    if (d->residual) {
        dsv_frame_ref_dec(d->residual);
    }
    if (d->prediction) {
        dsv_frame_ref_dec(d->prediction);
    }
    if (d->refdata) {
        encdat_unref(enc, d->refdata);
        d->refdata = NULL;
    }
    if (d->final_mvs) {
        dsv_free(d->final_mvs);
        d->final_mvs = NULL;
    }

    dsv_free(d);
}

static int
sample_point(int v)
{
    int whole, frac, ifrac, lo, hi, qp;
    v = (100 * DSV_RC_QUAL_SCALE) - v;
    whole = v / (10 * DSV_RC_QUAL_SCALE);
    frac = v % (10 * DSV_RC_QUAL_SCALE);
    ifrac = (10 * DSV_RC_QUAL_SCALE) - frac;
    lo = 1 << (whole + 0);
    hi = 1 << (whole + 1);

    qp = ((ifrac * lo + frac * hi) / (10 * DSV_RC_QUAL_SCALE)) - 1;

    qp = CLAMP(qp * 4, 0, DSV_MAX_QP);

    return qp;
}

static int
qual_to_qp(int v)
{
    int a, b, actv, frac;
    /* hardcode highest quality points */
    int d_hi = (100 * DSV_RC_QUAL_SCALE) - v;
    if (d_hi < 60) {
        return (d_hi + 16);
    }
    v = (v * 2);
    actv = v / 3;
    frac = v % 3;
    a = sample_point(actv);
    b = sample_point(actv + 1);

    return ((a * (3 - frac) + frac * b) / 3);
}

static unsigned
frame_luma_avg(DSV_FRAME *dst)
{
    int i, j;
    unsigned avg = 0;
    DSV_PLANE *d = dst->planes + 0;

    for (j = 0; j < d->h; j++) {
        uint8_t *dp;
        unsigned rav = 0;

        dp = DSV_GET_LINE(d, j);

        for (i = 0; i < d->w; i++) {
            rav += dp[i];
        }
        avg += rav / d->w;
    }
    return avg / d->h;
}

/* all of this rate control stuff is highly experimental */
static int
scene_complexity(DSV_ENCODER *enc, DSV_MV *vecs, DSV_PARAMS *p, DSV_ENCDATA *d)
{
    int i, j;
    int complexity = 0;
    int maxpot;
    DSV_MV *mv;
    if (enc->rc_mode == DSV_RATE_CONTROL_ABR) {
#define PENALIZE_COST 1
#if PENALIZE_COST
        maxpot = dsv_mv_cost(vecs, p, 0, 0, 64, 64, enc->prev_quant, 0);
#else
        maxpot = 64 + 64;
#endif
        maxpot += 12; /* avg max intra penalty */
        maxpot += 64; /* avg max err guess */
        maxpot = (maxpot * (p->nblocks_h * p->nblocks_v) + 1) >> 1;
        for (j = 0; j < p->nblocks_v; j++) {
            for (i = 0; i < p->nblocks_h; i++) {
                mv = &vecs[i + j * p->nblocks_h];

                if (!DSV_MV_IS_SKIP(mv)) {
#if PENALIZE_COST
                    complexity += dsv_mv_cost(vecs, p, i, j, mv->u.mv.x, mv->u.mv.y, enc->prev_quant, 0);
#else
                    complexity += abs(DSV_SAR(mv->u.mv.x, 2));
                    complexity += abs(DSV_SAR(mv->u.mv.y, 2));
#endif
                    complexity += (int) mv->err - (int) d->avg_err;
                }

                if (DSV_MV_IS_INTRA(mv)) {
                    if (mv->submask == DSV_MASK_ALL_INTRA) {
                        complexity += 16;
                    } else {
                        complexity += 4;
                    }
                }
            }
        }
    } else if (enc->rc_mode == DSV_RATE_CONTROL_CRF) {
        maxpot = 70; /* avg max intra penalty */
        maxpot *= p->nblocks_h * p->nblocks_v;
        for (j = 0; j < p->nblocks_v; j++) {
            for (i = 0; i < p->nblocks_h; i++) {
                mv = &vecs[i + j * p->nblocks_h];

                if (DSV_MV_IS_SKIP(mv)) {
                    complexity -= 100;
                } else {
                    complexity += dsv_mv_cost(vecs, p, i, j, mv->u.mv.x, mv->u.mv.y, enc->prev_quant, 0);
                }

                if (DSV_MV_IS_INTRA(mv)) {
                    if (mv->submask == DSV_MASK_ALL_INTRA) {
                        complexity += 100;
                    } else {
                        complexity += 40;
                    }
                }
            }
        }
    } else {
        complexity = 0;
    }

    if (complexity <= 0) {
        return 0;
    }
    return complexity * 100 / maxpot;
}

static void
quality2quant(DSV_ENCODER *enc, DSV_ENCDATA *d, DSV_FNUM prev_I)
{
    int q;

    if (d->params.has_ref) {
        DSV_INFO(("P FRAME!"));
    } else {
        DSV_INFO(("I FRAME!"));
    }

    q = enc->rc_qual;

    if (enc->rc_mode == DSV_RATE_CONTROL_CRF) {
        int minq, maxq;
        int plex, plexsq, dir, moving_targ, clamped_avg;
        int anchor;

        minq = d->params.has_ref ? enc->min_quality : enc->min_I_frame_quality;
        maxq = enc->max_quality;
        plex = CLAMP(enc->curr_complexity, 0, 100);
        plex -= 50;
        if (plex <= 0) {
            dir = 1; /* increase quality for a low complexity scene */
        } else {
            dir = -1; /* decrease quality for a high complexity scene */
        }
        plex = abs(plex);
        if (plex > 4) {
            plexsq = (plex * plex + 32) >> 6;
        } else {
            plexsq = plex;
        }
        anchor = CLAMP(enc->quality, minq, maxq);
        if (d->fnum > 0 && d->params.has_ref) {
            int dist, gop, closeness, qa, step, erradd, errsq;
            dist = abs((int) d->fnum - (int) prev_I);
            /* see how close we are to the previous I frame relative to the gop length */
            gop = CLAMP(enc->gop, 1, 600);
            errsq = (plexsq + (d->avg_err * d->avg_err >> 2) + 1) >> 1;
            plexsq = 0;
            erradd = CLAMP(errsq, RC_QUAL_PCT(0), RC_QUAL_PCT(16));
            if (dist >= enc->gop / 2) {
                step = erradd;
                dist = abs((int) d->fnum - ((int) prev_I + (int) gop / 2));
                closeness = (step * dist / MAX(gop / 2, 1));
                closeness = step - closeness;
            } else {
                step = erradd;
                dist = abs((int) d->fnum - (int) prev_I);
                closeness = (step * dist / MAX(gop / 2, 1));
            }
            qa = CLAMP(closeness, RC_QUAL_PCT(0), step);
            anchor += qa;

            anchor = CLAMP(anchor, minq, maxq);
        }

        clamped_avg = MAX(enc->rf_avg, enc->quality);
        moving_targ = (3 * anchor + 1 * clamped_avg + 2) >> 2;
        q = moving_targ + dir * plexsq;
        DSV_INFO(("    COMPLEXITY: %d%%", enc->curr_complexity));
        DSV_INFO(("    ANCHOR: %d    RF_AVG: %d", anchor, enc->rf_avg));
        DSV_INFO(("      TARGET: %d", moving_targ));
        DSV_INFO(("    DIR: %d   PLEXSQ: %d", dir, plexsq));
        DSV_INFO(("    PRE-CLAMP Q: %d", q));
        q = CLAMP(q, minq, maxq);
        enc->rc_qual = MAX(q, 0);
    } else if (enc->rc_mode == DSV_RATE_CONTROL_ABR) {
        DSV_META *vfmt = d->params.vidmeta;
        int fps, rf, target_rf, dir, delta, low_p, minq;

        fps = (vfmt->fps_num << 5) / vfmt->fps_den;
        if (fps == 0) {
            fps = 1;
        }
        if (enc->prev_complexity < 0) {
            enc->prev_complexity = enc->curr_complexity;
        }
        if (enc->rc_mode == DSV_RATE_CONTROL_CRF) {
            /* CRF, targeting this quality level */
            target_rf = MAX(enc->quality, 1);
        } else {
            /* ABR, targeting this # of bytes per frame */
            target_rf = (((enc->bitrate << 5) / fps) >> 3);
        }
        rf = enc->rf_avg;
        if (rf == 0) {
            rf = target_rf;
        }

        dir = (rf - target_rf) > 0 ? -1 : 1;
        enc->min_q_step = CLAMP(enc->min_q_step, 1, DSV_RC_QUAL_MAX);
        enc->max_q_step = CLAMP(enc->max_q_step, 1, DSV_RC_QUAL_MAX);

        /* limit delta by a different amount depending on direction */
        if (!d->params.has_ref) {
            if (enc->rc_mode == DSV_RATE_CONTROL_CRF) {
                delta = (abs(rf - target_rf) * RC_QUAL_PCT(50)) / target_rf;
                if (dir < 0 && delta > RC_QUAL_PCT(2)) {
                    delta = 0;
                }
                q = MAX(q, enc->avg_P_frame_q) + dir * delta;
            } else {
                unsigned dif = abs(rf - target_rf);
                if (dif > 32768) {
                    dif = 32768;
                }
                delta = (dif * dif) / ((dir > 0 ? 32 : 64) * target_rf);

                if (delta > RC_QUAL_PCT(12)) {
                    delta -= RC_QUAL_PCT(8);
                } else if (delta > RC_QUAL_PCT(8)) {
                    delta -= RC_QUAL_PCT(4);
                } else if (delta > RC_QUAL_PCT(4)) {
                    delta -= RC_QUAL_PCT(2);
                }
            }

            delta = MIN(delta, RC_QUAL_PCT(25));
            q = MAX(q, enc->avg_P_frame_q) + dir * delta;
            /* hint towards expected future complexity */
            if (enc->prev_complexity < 15) {
                q += RC_QUAL_PCT(2);
            } else if (enc->prev_complexity < 30) {
                q += RC_QUAL_PCT(1);
            } else if (enc->prev_complexity > 40) {
                q -= RC_QUAL_PCT(1);
            } else if (enc->prev_complexity > 60) {
                q -= RC_QUAL_PCT(2);
            }
            enc->prev_I_frame_quality = q;
        } else {
            delta = (abs(rf - target_rf) * RC_QUAL_PCT(100)) / target_rf;

            if (dir < 0 && delta < enc->min_q_step) {
                delta = 0;
            }
            if (enc->rc_mode == DSV_RATE_CONTROL_CRF) {
                delta = MIN(delta, enc->max_q_step * (dir > 0 ? 4 : 1));
            } else {
                delta = MIN(delta, enc->max_q_step * (dir > 0 ? 1 : 8));
            }
            q += dir * delta;
        }

        low_p = enc->avg_P_frame_q - RC_QUAL_PCT(4);
        low_p = CLAMP(low_p, enc->min_quality, enc->max_quality);
        minq = d->params.has_ref ? low_p : enc->min_I_frame_quality;
        if (enc->do_dark_intra_boost && !d->params.has_ref) {
            unsigned la = frame_luma_avg(d->pyramid[enc->pyramid_levels - 1]);
            if (la < 80) {
                int step = (80 - la) / 5;
                q += CLAMP(step, 5, 16);
            }
        }

        q = CLAMP(q, minq, enc->max_quality);
        q = CLAMP(q, 0, DSV_RC_QUAL_MAX); /* double validate range */

        DSV_INFO(("pcom: %d, ccom: %d", enc->prev_complexity, enc->curr_complexity));
        DSV_INFO(("RC Q: %d, delta: %d, targ_rf: %d, avg_rf: %d, dif: %d",
                q, delta, target_rf, rf, abs(rf - target_rf)));
        enc->rc_qual = q;
        enc->prev_complexity = enc->curr_complexity;
        if (enc->rc_pergop) {
            q = enc->prev_I_frame_quality;
            q = CLAMP(q, enc->min_quality, enc->max_quality);
        } else if (d->fnum > 0 && d->params.has_ref) {
            int dist, gop, closeness, qa, step, erradd;

            dist = abs((int) d->fnum - (int) prev_I);
            /* see how close we are to the previous I frame relative to the gop length */
            gop = CLAMP(enc->gop, 1, 60);
            if (dist >= enc->gop / 2) {
                step = RC_QUAL_PCT(8);
                dist = abs((int) d->fnum - ((int) prev_I + (int) gop / 2));
                closeness = (step * dist / MAX(gop / 2, 1));
                closeness = step - closeness;
            } else {
                step = RC_QUAL_PCT(8);
                dist = abs((int) d->fnum - (int) prev_I);
                closeness = (step * dist / MAX(gop / 2, 1));
            }
            qa = CLAMP(closeness, RC_QUAL_PCT(0), step);
            q += qa / 2;
            erradd = CLAMP((d->avg_err * d->avg_err) >> 1, RC_QUAL_PCT(0), RC_QUAL_PCT(16));
            q -= erradd;

            q = CLAMP(q, low_p, enc->max_quality);
            /* less than 2 sec gop length, anchor to I frame quality */
            if (enc->gop <= (2 * fps >> 5)) {
                /* prevent quality pulsing */
                if (enc->prev_I_frame_quality < q) {
                    q = enc->prev_I_frame_quality;
                } else {
                    q = (3 * q + 1 * enc->prev_I_frame_quality) >> 2;
                }
                q = CLAMP(q, enc->min_quality, enc->max_quality);
            }
        }

    } else {
        /* CQP */
        q = enc->quality;
        enc->rc_qual = q;
    }
    d->quant = qual_to_qp(q);
    if (d->params.lossless) {
        d->quant = 1;
    }
    enc->prev_quant = d->quant;

    DSV_INFO(("frame quant = %d from quality (%d/%d)%%", d->quant, q, DSV_RC_QUAL_SCALE));
}

/* B.1 Packet Header Link Offsets */
static void
set_link_offsets(DSV_ENCODER *enc, DSV_BUF *buffer, int is_eos)
{
    uint8_t *data = buffer->data;
    unsigned next_link;
    unsigned prev_start = DSV_PACKET_PREV_OFFSET;
    unsigned next_start = DSV_PACKET_NEXT_OFFSET;

    next_link = is_eos ? 0 : buffer->len;

    data[prev_start + 0] = (enc->prev_link >> 24) & 0xff;
    data[prev_start + 1] = (enc->prev_link >> 16) & 0xff;
    data[prev_start + 2] = (enc->prev_link >>  8) & 0xff;
    data[prev_start + 3] = (enc->prev_link >>  0) & 0xff;

    data[next_start + 0] = (next_link >> 24) & 0xff;
    data[next_start + 1] = (next_link >> 16) & 0xff;
    data[next_start + 2] = (next_link >>  8) & 0xff;
    data[next_start + 3] = (next_link >>  0) & 0xff;

    enc->prev_link = next_link;
}

static void
mk_pyramid(DSV_ENCODER *enc, DSV_FRAME *frame, DSV_FRAME **pyramid)
{
    int i, fmt;
    DSV_FRAME *prev;
    int orig_w, orig_h;

    fmt = frame->format;
    orig_w = frame->width;
    orig_h = frame->height;

    prev = frame;
    for (i = 0; i < enc->pyramid_levels; i++) {
        pyramid[i] = dsv_mk_frame(
                fmt,
                DSV_ROUND_SHIFT(orig_w, i + 1),
                DSV_ROUND_SHIFT(orig_h, i + 1),
                1);
        /* only do luma plane because motion estimation does not use chroma */
        dsv_ds2x_frame_luma(pyramid[i], prev);
        dsv_extend_frame_luma(pyramid[i]);
        prev = pyramid[i];
    }
}

static int
motion_est(DSV_ENCODER *enc, DSV_ENCDATA *d)
{
    DSV_PARAMS *p = &d->params;
    DSV_HME hme;
    DSV_ENCDATA *ref = d->refdata;
    int blks, closeness, scene_change_blocks = 0;
    int i, sc, intra_pct, high_intra;
    int gopdiv, complexity;
    DSV_FRAME *pyramid[DSV_MAX_PYRAMID_LEVELS];

    memset(&pyramid, 0, sizeof(pyramid));
    memset(&hme, 0, sizeof(hme));
    hme.enc = enc;
    hme.params = &d->params;
    hme.quant = enc->prev_quant; /* previous quant */
    hme.src[0] = d->padded_frame;

    hme.ref_mvf = ref->final_mvs;
    hme.ref[0] = ref->recon_frame;
    hme.ogr[0] = ref->padded_frame;
    mk_pyramid(enc, ref->recon_frame, pyramid);
    for (i = 0; i < enc->pyramid_levels; i++) {
        hme.src[i + 1] = d->pyramid[i];
        hme.ref[i + 1] = pyramid[i];
        hme.ogr[i + 1] = ref->pyramid[i];
    }
    intra_pct = dsv_hme(&hme, &scene_change_blocks, &d->avg_err);
    d->final_mvs = hme.mvf[0]; /* save result of HME */
    for (i = 1; i < enc->pyramid_levels + 1; i++) {
        if (hme.mvf[i]) {
            dsv_free(hme.mvf[i]);
        }
    }
    for (i = 0; i < enc->pyramid_levels; i++) {
        if (pyramid[i]) {
            dsv_frame_ref_dec(pyramid[i]);
        }
    }

    DSV_DEBUG(("intra block percent for frame %d = %d%%", d->fnum, intra_pct));
    DSV_DEBUG(("raw scene change block pct for frame %d = %d%%", d->fnum, scene_change_blocks));
    /* scale based on distance to last I frame */
    gopdiv = abs(enc->gop) * 3 / 4;
    closeness = (int) d->fnum - (int) enc->prev_gop;
    complexity = scene_complexity(enc, d->final_mvs, p, d);
    DSV_DEBUG(("avg err for frame %d = %d", d->fnum, d->avg_err));
    DSV_DEBUG(("complexity for frame %d = %d", d->fnum, complexity));
    blks = scene_change_blocks * closeness / MAX(gopdiv, 1);
    blks = MAX(blks, scene_change_blocks * 3 / 4);
    DSV_DEBUG(("adj scene change blocks for frame %d = %d%%", d->fnum, blks));
    enc->curr_intra_pct = intra_pct;
    sc = (enc->do_scd && (blks > enc->scene_change_pct));
    high_intra = intra_pct > enc->intra_pct_thresh;
    if (sc || high_intra) {
        p->has_ref = 0;
        if (sc) {
            DSV_INFO(("scene change %d [%d > %d]", closeness, blks, enc->scene_change_pct));
        }
        if (high_intra) {
            DSV_INFO(("too much intra, inserting I frame %d%%", intra_pct));
        }
        return 1;
    }
    /* only update complexity if intra frame was not inserted */
    enc->curr_complexity = complexity;
    return 0;
}

/* B.2.3.4 Motion Data */
static void
encode_motion(DSV_ENCODER *enc, DSV_ENCDATA *d, DSV_BS *bs, int *stats)
{
    uint8_t *bufs[DSV_SUB_NSUB];
    DSV_PARAMS *params = &d->params;
    int i, j;
    DSV_BS mbs[DSV_SUB_NSUB];
    DSV_ZBRLE rle, prrle;
    unsigned upperbound;
    int mesize = 0;

    upperbound = (params->nblocks_h * params->nblocks_v * 32);

    for (i = 0; i < DSV_SUB_NSUB; i++) {
        bufs[i] = dsv_alloc(upperbound);

        if (i == DSV_SUB_MODE) {
            dsv_bs_init_rle(&rle, bufs[i]);
        } else if (i == DSV_SUB_EPRM) {
            dsv_bs_init_rle(&prrle, bufs[i]);
        } else {
            dsv_bs_init(&mbs[i], bufs[i]);
        }
    }

    for (j = 0; j < params->nblocks_v; j++) {
        for (i = 0; i < params->nblocks_h; i++) {
            int idx = i + j * params->nblocks_h;
            DSV_MV *mv = &d->final_mvs[idx];
            int intra = DSV_MV_IS_INTRA(mv);

            dsv_bs_put_rle(&rle, (stats[DSV_MODE_STAT] == DSV_ONE_MARKER) ? intra : !intra);
            dsv_bs_put_rle(&prrle, (stats[DSV_EPRM_STAT] == DSV_ONE_MARKER) ? DSV_MV_IS_EPRM(mv) : !DSV_MV_IS_EPRM(mv));
            enc->blockdata[idx] |= (!!DSV_MV_IS_EPRM(mv)) << DSV_EPRM_BIT;

            /* B.2.3.4 Motion Data - Motion Vector Prediction */
            if (!DSV_MV_IS_SKIP(mv)) {
                int cvx, cvy;
                int x, y;
                dsv_movec_pred(d->final_mvs, params, i, j, &x, &y);
                if (intra) {
                    /* remove sub-pel precision for intra blocks */
                    x = DSV_SAR(x, 2);
                    y = DSV_SAR(y, 2);
                    cvx = DSV_SAR(mv->u.mv.x, 2);
                    cvy = DSV_SAR(mv->u.mv.y, 2);
                    mv->u.mv.x = cvx * 4; /* necessary to make prediction possible */
                    mv->u.mv.y = cvy * 4;
                } else {
                    cvx = mv->u.mv.x;
                    cvy = mv->u.mv.y;
                }
                dsv_bs_put_seg(mbs + DSV_SUB_MV_X, cvx - x);
                dsv_bs_put_seg(mbs + DSV_SUB_MV_Y, cvy - y);
                if (dsv_neighbordif(d->final_mvs, params, i, j) > DSV_NDIF_THRESH) {
                    enc->blockdata[idx] |= (1 << DSV_STABLE_BIT);
                }
            } else {
                enc->blockdata[idx] |= (1 << DSV_STABLE_BIT);
            }

            if (intra) {
                /* B.2.3.4 Motion Data - Intra Sub-Block Mask */
                if (mv->submask == DSV_MASK_ALL_INTRA) {
                    dsv_bs_put_bit(mbs + DSV_SUB_SBIM, 1);
                } else {
                    dsv_bs_put_bit(mbs + DSV_SUB_SBIM, 0);
                    dsv_bs_put_bits(mbs + DSV_SUB_SBIM, 4, mv->submask);
                }
                if (mv->dc & DSV_SRC_DC_PRED) { /* use source DC prediction */
                    dsv_bs_put_bit(mbs + DSV_SUB_SBIM, 1);
                    dsv_bs_put_bits(mbs + DSV_SUB_SBIM, 8, mv->dc & 0xff);
                } else {
                    dsv_bs_put_bit(mbs + DSV_SUB_SBIM, 0);
                }
            }
        }
    }

    for (i = 0; i < DSV_SUB_NSUB; i++) {
        int bytes;
        uint8_t *data;

        dsv_bs_align(bs);
        if (i == DSV_SUB_MODE) {
            bytes = dsv_bs_end_rle(&rle, 0);
            data = bufs[i];
        } else if (i == DSV_SUB_EPRM) {
            bytes = dsv_bs_end_rle(&prrle, 0);
            data = bufs[i];
        } else {
            dsv_bs_align(&mbs[i]);
            bytes = dsv_bs_ptr(&mbs[i]);
            data = mbs[i].start;
        }

        dsv_bs_put_ueg(bs, bytes);
        dsv_bs_align(bs);
        dsv_bs_concat(bs, data, bytes);
        mesize += bytes;
        dsv_free(bufs[i]);
    }
    DSV_DEBUG(("motion bytes %d", mesize));
}

/* B.2.3.1 Stability Blocks */
static void
encode_stable_blocks(DSV_ENCODER *enc, DSV_ENCDATA *d, DSV_BS *bs, DSV_MV *intramv, int *stats)
{
    uint8_t *stabbuf;
    DSV_PARAMS *params = &d->params;
    int i, nblk, avgdiv, bytes;
    DSV_ZBRLE stabrle;
    unsigned upperbound;
    int fps, dsf; /* downscale factor for stability */

    nblk = params->nblocks_h * params->nblocks_v;
    upperbound = (nblk * 32);

    stabbuf = dsv_alloc(upperbound);
    dsv_bs_init_rle(&stabrle, stabbuf);

    if (enc->refresh_ctr >= enc->stable_refresh) {
        enc->refresh_ctr = 0;
        memset(enc->stability, 0, sizeof(*enc->stability) * nblk);
    }
    avgdiv = enc->refresh_ctr;
    if (avgdiv <= 0) {
        avgdiv = 1;
    }
    fps = DSV_UDIV_ROUND(params->vidmeta->fps_num, params->vidmeta->fps_den);
    /* low framerate means we see individual frames longer so it's important
     * to increase the downscale factor to increase the threshold
     * at which motion is considered significant.
     *
     * and on the other end, high framerate means we see individual frames
     * for a shorter amount of time and so a smaller threshold makes sense.
     */
    if (fps <= 24) {
        dsf = 6;
    } else if (fps <= 30) {
        dsf = 4;
    } else if (fps <= 60) {
        dsf = 2;
    } else {
        dsf = 0;
    }
    for (i = 0; i < nblk; i++) {
        int stable = 0;
        if (d->params.has_ref) {
            DSV_MV *mv = &d->final_mvs[i];
            enc->blockdata[i] = 0;
            if (DSV_MV_IS_SKIP(mv)) {
                mv->u.all = 0;
            }
            if (DSV_MV_IS_INTRA(mv)) {
                stable = 0;
                enc->blockdata[i] |= DSV_IS_INTRA;
            } else {
                stable = DSV_MV_IS_SKIP(mv);
                if (!stable) {
                    enc->stability[i].x += abs(mv->u.mv.x) >> dsf;
                    enc->stability[i].y += abs(mv->u.mv.y) >> dsf;
                } else {
                    mv->u.all = 0;
                }
            }
            stable = !!stable;
            enc->blockdata[i] |= stable << DSV_SKIP_BIT;
            enc->blockdata[i] |= (!!DSV_MV_IS_SIMCMPLX(mv)) << DSV_SIMCMPLX_BIT;
        } else {
            DSV_MV *mv = &intramv[i];
            stable = 0;
            if (d->fnum > 0 && enc->do_temporal_aq) {
                int ax, ay;

                ax = enc->stability[i].x / avgdiv;
                ay = enc->stability[i].y / avgdiv;
                stable = (ax == 0 && ay == 0);
            }
            stable |= !!DSV_MV_IS_SKIP(mv);
            enc->blockdata[i] = stable << DSV_STABLE_BIT;
        }
        dsv_bs_put_rle(&stabrle, (stats[DSV_STABLE_STAT] == DSV_ONE_MARKER) ? (stable & 1) : !(stable & 1));
    }
    dsv_bs_align(bs);
    bytes = dsv_bs_end_rle(&stabrle, 0);
    dsv_bs_put_ueg(bs, bytes);
    dsv_bs_align(bs);
    dsv_bs_concat(bs, stabbuf, bytes);
    dsv_free(stabbuf);
    DSV_DEBUG(("stab bytes %d", bytes));
}

/* B.2.3.2 Ringing Blocks & B.2.3.3 Maintain Blocks */
static void
encode_intra_meta(DSV_ENCODER *enc, DSV_ENCDATA *d, DSV_BS *bs, DSV_MV *intramv, int *stats)
{
    uint8_t *buf_r, *buf_m;
    DSV_PARAMS *params = &d->params;
    int i, nblk, bytes;
    DSV_ZBRLE rle_r, rle_m;
    unsigned upperbound;

    nblk = params->nblocks_h * params->nblocks_v;
    upperbound = (nblk * 32);

    buf_r = dsv_alloc(upperbound);
    buf_m = dsv_alloc(upperbound);
    dsv_bs_init_rle(&rle_r, buf_r);
    dsv_bs_init_rle(&rle_m, buf_m);

    for (i = 0; i < nblk; i++) {
        int ring, maintain;
        DSV_MV *mv = &intramv[i];

        ring = !!DSV_MV_IS_RINGING(mv);
        maintain = !!DSV_MV_IS_MAINTAIN(mv);

        enc->blockdata[i] |= (ring << DSV_RINGING_BIT);
        enc->blockdata[i] |= (maintain << DSV_MAINTAIN_BIT);

        dsv_bs_put_rle(&rle_r, (stats[DSV_RINGING_STAT] == DSV_ONE_MARKER) ? ring : !ring);
        dsv_bs_put_rle(&rle_m, (stats[DSV_MAINTAIN_STAT] == DSV_ONE_MARKER) ? maintain : !maintain);
    }

    dsv_bs_align(bs);
    bytes = dsv_bs_end_rle(&rle_r, 0);
    dsv_bs_put_ueg(bs, bytes);
    dsv_bs_align(bs);
    dsv_bs_concat(bs, buf_r, bytes);
    dsv_free(buf_r);
    DSV_DEBUG(("ringing bytes %d", bytes));

    dsv_bs_align(bs);
    bytes = dsv_bs_end_rle(&rle_m, 0);
    dsv_bs_put_ueg(bs, bytes);
    dsv_bs_align(bs);
    dsv_bs_concat(bs, buf_m, bytes);
    dsv_free(buf_m);
    DSV_DEBUG(("maintain bytes %d", bytes));
}

static void
encode_packet_hdr(DSV_BS *bs, int pkt_type)
{
    dsv_bs_put_bits(bs, 8, DSV_FOURCC_0);
    dsv_bs_put_bits(bs, 8, DSV_FOURCC_1);
    dsv_bs_put_bits(bs, 8, DSV_FOURCC_2);
    dsv_bs_put_bits(bs, 8, DSV_FOURCC_3);
    dsv_bs_put_bits(bs, 8, DSV_VERSION_MINOR);

    dsv_bs_put_bits(bs, 8, pkt_type);

    /* reserve space for link offsets */
    dsv_bs_put_bits(bs, 32, 0);
    dsv_bs_put_bits(bs, 32, 0);
}

/* B.2.1 Metadata Packet */
static void
encode_metadata(DSV_ENCODER *enc, DSV_BUF *buf)
{
    DSV_BS bs;
    unsigned next_link;
    DSV_META *meta = &enc->vidmeta;
    unsigned next_start = DSV_PACKET_NEXT_OFFSET;

    dsv_mk_buf(buf, 64);

    dsv_bs_init(&bs, buf->data);

    encode_packet_hdr(&bs, DSV_PT_META);

    dsv_bs_put_ueg(&bs, meta->width);
    dsv_bs_put_ueg(&bs, meta->height);

    dsv_bs_put_ueg(&bs, meta->subsamp);

    dsv_bs_put_ueg(&bs, meta->fps_num);
    dsv_bs_put_ueg(&bs, meta->fps_den);

    dsv_bs_put_ueg(&bs, meta->aspect_num);
    dsv_bs_put_ueg(&bs, meta->aspect_den);

    dsv_bs_put_ueg(&bs, meta->inter_sharpen);
    dsv_bs_align(&bs);

    next_link = dsv_bs_ptr(&bs);
    buf->data[next_start + 0] = (next_link >> 24) & 0xff;
    buf->data[next_start + 1] = (next_link >> 16) & 0xff;
    buf->data[next_start + 2] = (next_link >>  8) & 0xff;
    buf->data[next_start + 3] = (next_link >>  0) & 0xff;

    buf->len = next_link; /* trim length to actual size */
}

static void
gather_stats(DSV_ENCODER *enc, DSV_ENCDATA *d, DSV_MV *intramv, int *stats)
{
    DSV_PARAMS *params = &d->params;
    int i, nblk, avgdiv, temp_rc;

    nblk = params->nblocks_h * params->nblocks_v;

    temp_rc = enc->refresh_ctr;
    if (enc->refresh_ctr >= enc->stable_refresh) {
        temp_rc = 0;
    }
    avgdiv = temp_rc;
    if (avgdiv <= 0) {
        avgdiv = 1;
    }

    for (i = 0; i < nblk; i++) {
        int ax, ay, stable = 0;
        if (d->params.has_ref) {
            DSV_MV *mv = &d->final_mvs[i];
            if (DSV_MV_IS_INTRA(mv)) {
                stable = 0;
            } else {
                stable = !!DSV_MV_IS_SKIP(mv);
            }

            /* get stats on mode and EPRM bits */
            stats[DSV_MODE_STAT] += (!!DSV_MV_IS_INTRA(mv)) ? 1 : -1;
            stats[DSV_EPRM_STAT] += (!!DSV_MV_IS_EPRM(mv)) ? 1 : -1;
        } else {
            DSV_MV *mv = &intramv[i];
            if (d->fnum > 0 && enc->do_temporal_aq) {
                ax = enc->stability[i].x / avgdiv;
                ay = enc->stability[i].y / avgdiv;
                stable = (ax == 0 && ay == 0);
            } else {
                stable = !!DSV_MV_IS_SKIP(mv);
            }

            stats[DSV_MAINTAIN_STAT] += (!!DSV_MV_IS_MAINTAIN(mv)) ? 1 : -1;
            stats[DSV_RINGING_STAT] += (!!DSV_MV_IS_RINGING(mv)) ? 1 : -1;
        }
        stats[DSV_STABLE_STAT] += (stable & 1) ? 1 : -1;
    }
}

static void
encode_picture(DSV_ENCODER *enc, DSV_ENCDATA *d, DSV_BUF *output_buf)
{
    DSV_BS bs;
    unsigned upperbound;
    DSV_FMETA fm;
    DSV_COEFS coefs[3];
    int i, width, height;
    DSV_MV *intramv = NULL;
    int stats[DSV_MAX_STAT];

    width = enc->vidmeta.width;
    height = enc->vidmeta.height;
    upperbound = width * height;
    switch (enc->vidmeta.subsamp) {
        case DSV_SUBSAMP_444:
            upperbound *= 6;
            break;
        case DSV_SUBSAMP_422:
        case DSV_SUBSAMP_UYVY:
            upperbound *= 4;
            break;
        case DSV_SUBSAMP_420:
        case DSV_SUBSAMP_411:
        case DSV_SUBSAMP_410:
            upperbound *= 2;
            break;
        default:
            DSV_ASSERT(0);
            break;
    }

    dsv_mk_buf(output_buf, upperbound);

    dsv_bs_init(&bs, output_buf->data);
    /* B.2.3 Picture Packet */
    encode_packet_hdr(&bs, DSV_MAKE_PT(d->params.is_ref, d->params.has_ref));

    dsv_bs_align(&bs);
    dsv_bs_put_bits(&bs, 32, d->fnum);

    if (!d->params.has_ref) {
        intramv = dsv_intra_analysis(d->padded_frame, &d->params);
    }

    memset(stats, DSV_ONE_MARKER, sizeof(stats));
    if (enc->effort >= 7) {
        gather_stats(enc, d, intramv, stats);
        for (i = 0; i < DSV_MAX_STAT; i++) {
            stats[i] = stats[i] > 0 ? DSV_ZERO_MARKER : DSV_ONE_MARKER;
        }
    } else {
        /* any stat not listed here defaults to a one marker */
        stats[DSV_MAINTAIN_STAT] = DSV_ZERO_MARKER;
        stats[DSV_RINGING_STAT] = DSV_ZERO_MARKER;
    }

    dsv_bs_align(&bs);
    /* encode the block sizes */
    dsv_bs_put_ueg(&bs, dsv_lb2(d->params.blk_w) - 4);
    dsv_bs_put_ueg(&bs, dsv_lb2(d->params.blk_h) - 4);
    dsv_bs_align(&bs);

    dsv_bs_put_bit(&bs, stats[DSV_STABLE_STAT]);
    if (d->params.has_ref) {
        dsv_bs_put_bit(&bs, stats[DSV_MODE_STAT]);
        dsv_bs_put_bit(&bs, stats[DSV_EPRM_STAT]);
        dsv_bs_put_bit(&bs, enc->do_inter_filter);
    } else {
        dsv_bs_put_bit(&bs, stats[DSV_MAINTAIN_STAT]);
        dsv_bs_put_bit(&bs, stats[DSV_RINGING_STAT]);
        dsv_bs_put_bit(&bs, enc->do_intra_filter);
    }
    dsv_bs_put_bits(&bs, DSV_MAX_QP_BITS, d->quant);
    dsv_bs_align(&bs);

    /* encode AQ metadata */
    encode_stable_blocks(enc, d, &bs, intramv, stats);
    if (d->params.has_ref) {
        dsv_sub_pred(d->final_mvs, &d->params, d->prediction, d->residual, d->refdata->recon_frame);
        dsv_bs_align(&bs);
        /* encode motion vecs and intra blocks */
        encode_motion(enc, d, &bs, stats);
    } else {
        encode_intra_meta(enc, d, &bs, intramv, stats);
        dsv_free(intramv);
    }

    /* B.2.3.5 Image Data */
    dsv_bs_align(&bs);
    fm.params = &d->params;
    fm.blockdata = enc->blockdata;
    fm.isP = d->params.has_ref;
    fm.fnum = d->fnum;

    dsv_mk_coefs(coefs, enc->vidmeta.subsamp, width, height);

    /* encode the residual image */
    for (i = 0; i < 3; i++) {
        fm.cur_plane = i;
        dsv_fwd_sbt(&d->residual->planes[i], &coefs[i], &fm);
        dsv_encode_plane(&bs, &coefs[i], d->quant, &fm);
        dsv_inv_sbt(&d->residual->planes[i], &coefs[i], d->quant, &fm);

        if (!fm.isP) {
            dsv_intra_filter(d->quant, &d->params, &fm, i, &d->residual->planes[i], enc->do_intra_filter);
        }
    }
    if (coefs[0].data) { /* only the first pointer is actual allocated data */
        dsv_free(coefs[0].data);
        coefs[0].data = NULL;
    }

    dsv_bs_align(&bs);

    output_buf->len = dsv_bs_ptr(&bs);
    if (d->params.has_ref) {
        /* add cached prediction frame onto residual to reconstruct frame */
        dsv_add_res(d->final_mvs, &fm, d->quant, d->residual, d->prediction, enc->do_inter_filter);
    }
}

static int
size4dim(int dim)
{
    if (dim > 1280) {
        return DSV_MAX_BLOCK_SIZE;
    }
    return DSV_MIN_BLOCK_SIZE;
}

static int
encode_one_frame(DSV_ENCODER *enc, DSV_ENCDATA *d, DSV_BUF *output_buf)
{
    DSV_PARAMS *p;
    int i, w, h;
    int gop_start = 0;
    int forced_intra = 0;
    DSV_FNUM prev_I;

    p = &d->params;
    p->vidmeta = &enc->vidmeta;
    p->effort = enc->effort;
    p->do_psy = enc->do_psy;
    prev_I = enc->prev_gop;
    p->temporal_mc = DSV_TEMPORAL_MC(d->fnum);
    p->lossless = (enc->quality == DSV_RC_QUAL_MAX);
    
    w = p->vidmeta->width;
    h = p->vidmeta->height;

    p->blk_w = size4dim(w);
    p->blk_h = size4dim(h);
    if (abs(w - h) < MIN(w, h)) {
        /* mostly square frame, give it square blocks */
        int mins = MIN(p->blk_w, p->blk_h);
        p->blk_w = p->blk_h = mins;
    }
    p->blk_w = CLAMP(p->blk_w, DSV_MIN_BLOCK_SIZE, DSV_MAX_BLOCK_SIZE);
    p->blk_h = CLAMP(p->blk_h, DSV_MIN_BLOCK_SIZE, DSV_MAX_BLOCK_SIZE);

    if (enc->block_size_override_x >= 0) {
        p->blk_w = 16 << enc->block_size_override_x;
        p->blk_w = CLAMP(p->blk_w, DSV_MIN_BLOCK_SIZE, DSV_MAX_BLOCK_SIZE);
    }
    if (enc->block_size_override_y >= 0) {
        p->blk_h = 16 << enc->block_size_override_y;
        p->blk_h = CLAMP(p->blk_h, DSV_MIN_BLOCK_SIZE, DSV_MAX_BLOCK_SIZE);
    }
    p->nblocks_h = DSV_UDIV_ROUND_UP(w, p->blk_w);
    p->nblocks_v = DSV_UDIV_ROUND_UP(h, p->blk_h);
    DSV_DEBUG(("block size %dx%d", p->blk_w, p->blk_h));
    if (enc->stability == NULL) {
        enc->stability = dsv_alloc(sizeof(*enc->stability) * p->nblocks_h * p->nblocks_v);
        enc->blockdata = dsv_alloc(p->nblocks_h * p->nblocks_v);
    }

    if (enc->pyramid_levels == 0) {
        int maxdim, lvls;

        maxdim = MIN(w, h);
        lvls = dsv_lb2(maxdim);
        maxdim = MAX(d->params.nblocks_h, d->params.nblocks_v);
        /* important for HBM, otherwise we'll be doing extra work for no reason */
        while ((1 << lvls) > maxdim) {
            lvls--;
        }

        enc->pyramid_levels = CLAMP(lvls, 3, DSV_MAX_PYRAMID_LEVELS);
    }

    DSV_DEBUG(("gop length %d", enc->gop));

    mk_pyramid(enc, d->padded_frame, d->pyramid);
    if (enc->force_metadata || ((enc->prev_gop + enc->gop) <= d->fnum)) {
        gop_start = 1;
        enc->prev_gop = d->fnum;
        enc->force_metadata = 0;
    }

    if (enc->gop == DSV_GOP_INTRA) {
        d->params.is_ref = 0;
        d->params.has_ref = 0;
    } else {
        d->params.is_ref = 1;
        if (gop_start) {
            d->params.has_ref = 0;
        } else {
            d->params.has_ref = 1;
            d->refdata = enc->ref;
            encdat_ref(d->refdata);
        }
        if (enc->ref) {
            encdat_unref(enc, enc->ref);
            enc->ref = NULL;
        }
        enc->ref = d;
        encdat_ref(d);
    }
    d->avg_err = 0;
    if (d->params.has_ref) {
        forced_intra = motion_est(enc, d);
    }
    if (enc->variable_i_interval && forced_intra) {
        enc->prev_gop = d->fnum;
    }
    quality2quant(enc, d, prev_I);

    dsv_frame_copy(d->residual, d->padded_frame);

    encode_picture(enc, d, output_buf);

    if (d->params.is_ref && enc->gop != DSV_GOP_INTRA) {
        d->recon_frame = dsv_extend_frame(dsv_frame_ref_inc(d->residual));
    }

    if (d->refdata) {
        encdat_unref(enc, d->refdata);
        d->refdata = NULL;
    }

    if (!d->params.is_ref) {
        for (i = 0; i < enc->pyramid_levels; i++) {
            if (d->pyramid[i]) {
                dsv_frame_ref_dec(d->pyramid[i]);
                d->pyramid[i] = NULL;
            }
        }
    }
    return gop_start;
}

extern void
dsv_enc_init(DSV_ENCODER *enc)
{
    memset(enc, 0, sizeof(*enc));
    enc->prev_gop = -1;

    /* default config */
    enc->quality = DSV_QUALITY_PERCENT(85);
    enc->gop = 12;
    enc->pyramid_levels = 0;
    enc->rc_mode = DSV_RATE_CONTROL_CRF;
    enc->bitrate = INT_MAX;
    enc->rc_pergop = 0;
    enc->min_q_step = 4;
    enc->max_q_step = 1;
    enc->min_quality = DSV_QUALITY_PERCENT(1);
    enc->max_quality = DSV_QUALITY_PERCENT(95);
    enc->min_I_frame_quality = DSV_QUALITY_PERCENT(5);
    enc->rf_total = 0;
    enc->rf_avg = 0;
    enc->prev_complexity = -1;
    enc->curr_complexity = -1;
    enc->prev_quant = 0;

    enc->intra_pct_thresh = 50;
    enc->stable_refresh = 14;
    enc->scene_change_pct = 55;
    enc->do_scd = 1;
    enc->variable_i_interval = 1;
    enc->skip_block_thresh = 0;
    enc->block_size_override_x = -1;
    enc->block_size_override_y = -1;
    enc->do_temporal_aq = 1;
    enc->do_psy = DSV_PSY_ALL;
    enc->do_dark_intra_boost = 1;
    enc->do_intra_filter = 1;
    enc->do_inter_filter = 1;
}

extern void
dsv_enc_start(DSV_ENCODER *enc)
{
    enc->quality = CLAMP(enc->quality, 0, DSV_RC_QUAL_MAX);
    switch (enc->rc_mode) {
        case DSV_RATE_CONTROL_CRF:
            enc->rc_qual = CLAMP(enc->quality * 2, enc->min_I_frame_quality, enc->max_quality);
            enc->rf_avg = enc->rc_qual;
            enc->avg_P_frame_q = enc->quality;
            break;
        case DSV_RATE_CONTROL_ABR:
            enc->rc_qual = enc->quality;
            enc->avg_P_frame_q = enc->quality * 4 / 5;
            break;
        case DSV_RATE_CONTROL_CQP:
            break;
    }

    enc->force_metadata = 1;
}

extern void
dsv_enc_free(DSV_ENCODER *enc)
{
    if (enc->ref) {
        encdat_unref(enc, enc->ref);
        enc->ref = NULL;
    }
    if (enc->stability) {
        dsv_free(enc->stability);
        enc->stability = NULL;
    }

    if (enc->blockdata) {
        dsv_free(enc->blockdata);
        enc->blockdata = NULL;
    }
}

extern void
dsv_enc_set_metadata(DSV_ENCODER *enc, DSV_META *md)
{
    memcpy(&enc->vidmeta, md, sizeof(DSV_META));
}

extern void
dsv_enc_force_metadata(DSV_ENCODER *enc)
{
    enc->force_metadata = 1;
}

/* B.2.2 End of Stream Packet */
extern void
dsv_enc_end_of_stream(DSV_ENCODER *enc, DSV_BUF *bufs)
{
    DSV_BS bs;

    dsv_mk_buf(&bufs[0], DSV_PACKET_HDR_SIZE);
    dsv_bs_init(&bs, bufs[0].data);

    encode_packet_hdr(&bs, DSV_PT_EOS);

    set_link_offsets(enc, &bufs[0], 1);
    DSV_INFO(("creating end of stream packet"));
}

extern int
dsv_enc(DSV_ENCODER *enc, DSV_FRAME *frame, DSV_BUF *bufs)
{
    DSV_ENCDATA *d;
    int w, h;
    int nbuf = 0;
    DSV_BUF outbuf;

    if (frame == NULL) {
        DSV_ERROR(("null frame passed to encoder!"));
        return 0;
    }
    if (bufs == NULL) {
        DSV_ERROR(("null buffer list passed to encoder!"));
        return 0;
    }
    d = dsv_alloc(sizeof(DSV_ENCDATA));

    d->refcount = 1;

    w = enc->vidmeta.width;
    h = enc->vidmeta.height;
    d->residual = dsv_mk_frame(enc->vidmeta.subsamp, w, h, 1);
    d->prediction = dsv_mk_frame(enc->vidmeta.subsamp, w, h, 1);

    d->padded_frame = dsv_clone_frame(frame, 1);
    dsv_extend_frame(d->padded_frame);
    dsv_frame_ref_dec(frame);

    d->fnum = enc->next_fnum++;

    if (encode_one_frame(enc, d, &outbuf)) {
        DSV_BUF metabuf;
        encode_metadata(enc, &metabuf);
        /* send metadata first, then compressed frame */
        bufs[nbuf++] = metabuf;
        set_link_offsets(enc, &bufs[nbuf - 1], 0);
    }
    bufs[nbuf++] = outbuf;
    set_link_offsets(enc, &bufs[nbuf - 1], 0);

    if (d->params.has_ref) {
        enc->refresh_ctr++; /* for averaging stable blocks */
    }
    /* rate control statistics */
    if (enc->rc_mode != DSV_RATE_CONTROL_CQP) {
        if (enc->rc_mode == DSV_RATE_CONTROL_CRF) {
            enc->rf_total += enc->rc_qual;
        } else {
            enc->rf_total += outbuf.len;
        }
        enc->rf_reset++;
        if (d->params.has_ref) {
            enc->total_P_frame_q += enc->rc_qual;
            enc->avg_P_frame_q = enc->total_P_frame_q / enc->rf_reset;
        }
        enc->rf_avg = enc->rf_total / enc->rf_reset;
        if (enc->rf_reset >= DSV_RF_RESET) {
            enc->rf_total = enc->rf_avg;
            enc->total_P_frame_q = enc->total_P_frame_q / enc->rf_reset;
            enc->rf_reset = 1;
        }
    }

    encdat_unref(enc, d);

    return nbuf;
}
