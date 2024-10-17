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

static void
quality2quant(DSV_ENCODER *enc, DSV_ENCDATA *d)
{
    int q;

    if (d->params.has_ref) {
        DSV_INFO(("P FRAME!"));
    } else {
        DSV_INFO(("I FRAME!"));
    }

    q = enc->rc_qual;
    if (enc->rc_mode != DSV_RATE_CONTROL_CRF) {
        DSV_META *vfmt = d->params.vidmeta;
        int fps, bpf, needed_bpf, dir, delta, low_p, minq;

        fps = (vfmt->fps_num << 5) / vfmt->fps_den;
        if (fps == 0) {
            fps = 1;
        }
        /* bpf = bytes per frame */
        needed_bpf = ((enc->bitrate << 5) / fps) >> 3;

        bpf = enc->bpf_avg;
        if (bpf == 0) {
            bpf = needed_bpf;
        }
        dir = (bpf - needed_bpf) > 0 ? -1 : 1;

        delta = (abs(bpf - needed_bpf) << 9) / needed_bpf;
        if (dir == 1) {
            delta *= 2;
        }
        delta = (q * delta >> 9);

        enc->max_q_step = CLAMP(enc->max_q_step, 1, DSV_RC_QUAL_MAX);

        /* limit delta by a different amount depending on direction */
        if (dir > 0) {
            if (delta > enc->max_q_step * 1) {
                delta = enc->max_q_step * 1;
            }
        } else {
            if (delta > enc->max_q_step * 8) {
                delta = enc->max_q_step * 8;
            }
        }
        delta *= dir;

        q += delta;
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
        DSV_INFO(("RC Q = %d delta = %d bpf: %d, avg: %d, dif: %d",
                q, delta, needed_bpf, bpf, abs(bpf - needed_bpf)));
        enc->rc_qual = q;
    } else {
        q = enc->quality;
        enc->rc_qual = q;
    }
    d->quant = qual_to_qp(q);

    DSV_DEBUG(("frame quant = %d from %d", d->quant, q));
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
mk_pyramid(DSV_ENCODER *enc, DSV_ENCDATA *d)
{
    int i, fmt;
    DSV_FRAME *prev;
    int orig_w, orig_h;

    fmt = d->padded_frame->format;
    orig_w = d->padded_frame->width;
    orig_h = d->padded_frame->height;

    prev = d->padded_frame;
    for (i = 0; i < enc->pyramid_levels; i++) {
        d->pyramid[i] = dsv_mk_frame(
                fmt,
                DSV_ROUND_SHIFT(orig_w, i + 1),
                DSV_ROUND_SHIFT(orig_h, i + 1),
                1);
        /* only do luma plane because motion estimation does not use chroma */
        dsv_ds2x_frame_luma(d->pyramid[i], prev);
        dsv_extend_frame_luma(d->pyramid[i]);
        prev = d->pyramid[i];
    }
}

static int
motion_est(DSV_ENCODER *enc, DSV_ENCDATA *d)
{
    int i, intra_pct;
    DSV_PARAMS *p = &d->params;
    DSV_HME hme;
    DSV_ENCDATA *ref = d->refdata;
    int blks, closeness, scene_change_blocks = 0;
    int sc, high_intra;
    int gopdiv;

    memset(&hme, 0, sizeof(hme));
    hme.enc = enc;
    hme.params = &d->params;

    hme.src[0] = d->padded_frame;
    hme.ref[0] = ref->padded_frame;
    hme.recon = ref->recon_frame;
    for (i = 0; i < enc->pyramid_levels; i++) {
        hme.src[i + 1] = d->pyramid[i];
        hme.ref[i + 1] = ref->pyramid[i];
    }

    intra_pct = dsv_hme(&hme, &scene_change_blocks);
    d->final_mvs = hme.mvf[0]; /* save result of HME */
    for (i = 1; i < enc->pyramid_levels + 1; i++) {
        if (hme.mvf[i]) {
            dsv_free(hme.mvf[i]);
        }
    }
    DSV_DEBUG(("intra block percent for frame %d = %d%%", d->fnum, intra_pct));
    DSV_DEBUG(("raw scene change block pct for frame %d = %d%%", d->fnum, scene_change_blocks));
    /* scale based on distance to last I frame */
    gopdiv = abs(enc->gop) * 3 / 4;
    closeness = (int) d->fnum - (int) enc->prev_gop;
    blks = scene_change_blocks * closeness / MAX(gopdiv, 1);
    blks = MAX(blks, scene_change_blocks * 3 / 4);
    DSV_DEBUG(("adj scene change blocks for frame %d = %d%%", d->fnum, blks));

    sc = (enc->do_scd && (blks > enc->scene_change_delta));
    high_intra = intra_pct > enc->intra_pct_thresh;
    if (sc || high_intra) {
        p->has_ref = 0;
        if (sc) {
            DSV_INFO(("scene change %d [%d > %d]", closeness, blks, enc->scene_change_delta));
        }
        if (high_intra) {
            DSV_INFO(("too much intra, inserting I frame %d%%", intra_pct));
        }
        return 1;
    }
    return 0;
}

/* B.2.3.4 Motion Data */
static void
encode_motion(DSV_ENCDATA *d, DSV_BS *bs, int *stats)
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

            dsv_bs_put_rle(&rle, (stats[DSV_MODE_STAT] == DSV_ONE_MARKER) ? mv->mode : !mv->mode);
            dsv_bs_put_rle(&prrle, (stats[DSV_EPRM_STAT] == DSV_ONE_MARKER) ? mv->eprm : !mv->eprm);

            if (mv->mode == DSV_MODE_INTER) {
                /* B.2.3.4 Motion Data - Motion Vector Prediction */
                if (!mv->skip) {
                    int x, y;
                    dsv_movec_pred(d->final_mvs, params, i, j, &x, &y);

                    dsv_bs_put_seg(mbs + DSV_SUB_MV_X, mv->u.mv.x - x);
                    dsv_bs_put_seg(mbs + DSV_SUB_MV_Y, mv->u.mv.y - y);
                }
            } else {
                /* B.2.3.4 Motion Data - Intra Sub-Block Mask */
                if (mv->submask == DSV_MASK_ALL_INTRA) {
                    dsv_bs_put_bit(mbs + DSV_SUB_SBIM, 1);
                } else {
                    dsv_bs_put_bit(mbs + DSV_SUB_SBIM, 0);
                    dsv_bs_put_bits(mbs + DSV_SUB_SBIM, 4, mv->submask);
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
    if (fps < 24) {
        dsf = 6;
    } else if (fps < 48) {
        dsf = 4;
    } else if (fps < 64) {
        dsf = 2;
    } else {
        dsf = 0;
    }
    for (i = 0; i < nblk; i++) {
        int ax, ay, stable = 0;
        if (d->params.has_ref) {
            DSV_MV *mv = &d->final_mvs[i];
            enc->blockdata[i] = 0;
            if (mv->mode == DSV_MODE_INTER) {
                stable = mv->skip;
                if (!stable) {
                    enc->stability[i].x += abs(mv->u.mv.x) >> dsf;
                    enc->stability[i].y += abs(mv->u.mv.y) >> dsf;
                } else {
                    mv->u.all = 0;
                }
            } else {
                stable = 0;
                enc->blockdata[i] |= DSV_IS_INTRA;
            }
            stable = !!stable;
            enc->blockdata[i] |= stable << DSV_SKIP_BIT;
        } else {
            DSV_MV *mv = &intramv[i];
            if (d->fnum > 0 && enc->do_temporal_aq) {
                ax = enc->stability[i].x / avgdiv;
                ay = enc->stability[i].y / avgdiv;
                stable = (ax == 0 && ay == 0);
            } else {
                stable = mv->skip;
            }
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

        ring = !!intramv[i].use_ringing;
        maintain = !!intramv[i].maintain;

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
            if (mv->mode == DSV_MODE_INTER) {
                stable = mv->skip;
            } else {
                stable = 0;
            }
            stable = !!stable;

            /* get stats on mode and EPRM bits */
            stats[DSV_MODE_STAT] += (!!mv->mode) ? 1 : -1;
            stats[DSV_EPRM_STAT] += (!!mv->eprm) ? 1 : -1;
        } else {
            DSV_MV *mv = &intramv[i];
            if (d->fnum > 0 && enc->do_temporal_aq) {
                ax = enc->stability[i].x / avgdiv;
                ay = enc->stability[i].y / avgdiv;
                stable = (ax == 0 && ay == 0);
            } else {
                stable = mv->skip;
            }

            stats[DSV_MAINTAIN_STAT] += (!!mv->maintain) ? 1 : -1;
            stats[DSV_RINGING_STAT] += (!!mv->use_ringing) ? 1 : -1;
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
        intramv = dsv_intra_analysis(d->padded_frame, d->pyramid[enc->pyramid_levels - 1], enc->pyramid_levels, &d->params);
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
    } else {
        dsv_bs_put_bit(&bs, stats[DSV_MAINTAIN_STAT]);
        dsv_bs_put_bit(&bs, stats[DSV_RINGING_STAT]);
    }
    dsv_bs_put_bits(&bs, DSV_MAX_QP_BITS, d->quant);
    dsv_bs_align(&bs);

    /* encode AQ metadata */
    encode_stable_blocks(enc, d, &bs, intramv, stats);
    if (d->params.has_ref) {
        dsv_sub_pred(d->final_mvs, &d->params, d->prediction, d->residual, d->refdata->recon_frame);
        dsv_bs_align(&bs);
        /* encode motion vecs and intra blocks */
        encode_motion(d, &bs, stats);
    } else {
        encode_intra_meta(enc, d, &bs, intramv, stats);
        dsv_free(intramv);
    }

    /* B.2.3.5 Image Data */
    dsv_bs_align(&bs);
    fm.params = &d->params;
    fm.blockdata = enc->blockdata;
    fm.isP = d->params.has_ref;
    dsv_mk_coefs(coefs, enc->vidmeta.subsamp, width, height);

    /* encode the residual image */
    for (i = 0; i < 3; i++) {
        fm.cur_plane = i;
        dsv_fwd_sbt(&d->residual->planes[i], &coefs[i], &fm);
        dsv_encode_plane(&bs, &coefs[i], d->quant, &fm);
        dsv_inv_sbt(&d->residual->planes[i], &coefs[i], d->quant, &fm);
    }

    if (coefs[0].data) { /* only the first pointer is actual allocated data */
        dsv_free(coefs[0].data);
        coefs[0].data = NULL;
    }

    dsv_bs_align(&bs);

    output_buf->len = dsv_bs_ptr(&bs);

    if (d->params.has_ref) {
        /* add cached prediction frame onto residual to reconstruct frame */
        dsv_add_res(d->final_mvs, &fm, d->quant, d->residual, d->prediction);
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

    p = &d->params;
    p->vidmeta = &enc->vidmeta;
    p->effort = enc->effort;
    p->do_psy = enc->do_psy;

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

    mk_pyramid(enc, d);
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
    if (d->params.has_ref) {
        forced_intra = motion_est(enc, d);
    }
    if (enc->variable_i_interval && forced_intra) {
        enc->prev_gop = d->fnum;
    }
    quality2quant(enc, d);

    dsv_frame_copy(d->residual, d->padded_frame);

    encode_picture(enc, d, output_buf);

    if (d->params.is_ref && enc->gop != DSV_GOP_INTRA) {
        d->recon_frame = dsv_extend_frame(dsv_frame_ref_inc(d->residual));
    }

    if (d->final_mvs) {
        dsv_free(d->final_mvs);
        d->final_mvs = NULL;
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
    enc->max_q_step = 1;
    enc->min_quality = DSV_QUALITY_PERCENT(1);
    enc->max_quality = DSV_QUALITY_PERCENT(95);
    enc->min_I_frame_quality = DSV_QUALITY_PERCENT(5);
    enc->bpf_total = 0;
    enc->bpf_avg = 0;

    enc->intra_pct_thresh = 50;
    enc->stable_refresh = 14;
    enc->scene_change_delta = 4;
    enc->do_scd = 1;
    enc->variable_i_interval = 1;
    enc->skip_block_thresh = 0;
    enc->block_size_override_x = -1;
    enc->block_size_override_y = -1;
    enc->do_temporal_aq = 1;
    enc->do_psy = 1;
    enc->do_dark_intra_boost = 1;
}

extern void
dsv_enc_start(DSV_ENCODER *enc)
{
    enc->quality = CLAMP(enc->quality, 0, DSV_RC_QUAL_MAX);
    if (enc->rc_mode != DSV_RATE_CONTROL_CRF) {
        enc->rc_qual = enc->quality;
        enc->avg_P_frame_q = enc->quality * 4 / 5;
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
    }
    bufs[nbuf++] = outbuf;

    if (d->params.has_ref) {
        enc->refresh_ctr++; /* for averaging stable blocks */
    }
    /* rate control statistics */
    if (enc->rc_mode != DSV_RATE_CONTROL_CRF) {
        enc->bpf_total += outbuf.len;
        enc->bpf_reset++;
        if (d->params.has_ref) {
            enc->total_P_frame_q += enc->rc_qual;
            enc->avg_P_frame_q = enc->total_P_frame_q / enc->bpf_reset;
        }
        enc->bpf_avg = enc->bpf_total / enc->bpf_reset;
        if (enc->bpf_reset >= DSV_BPF_RESET) {
            enc->bpf_total = enc->bpf_avg;
            enc->total_P_frame_q = enc->total_P_frame_q / enc->bpf_reset;
            enc->bpf_reset = 1;
        }
    }

    encdat_unref(enc, d);

    set_link_offsets(enc, &bufs[nbuf - 1], 0);
    return nbuf;
}
