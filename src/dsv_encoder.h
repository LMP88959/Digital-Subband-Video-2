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

#ifndef _DSV_ENCODER_H_
#define _DSV_ENCODER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "dsv_internal.h"

#define DSV_ENCODER_VERSION 11

#define DSV_GOP_INTRA 0
#define DSV_GOP_INF   INT_MAX

#define DSV_ENC_NUM_BUFS  0x03 /* mask */
#define DSV_ENC_FINISHED  0x04

#define DSV_MIN_EFFORT 0
#define DSV_MAX_EFFORT 10

#define DSV_RATE_CONTROL_CRF  0 /* constant rate factor */
#define DSV_RATE_CONTROL_ABR  1 /* one pass average bitrate */
#define DSV_RATE_CONTROL_CQP  2 /* constant quantization parameter */

#define DSV_MAX_PYRAMID_LEVELS 5

#define DSV_RC_QUAL_SCALE 4
#define DSV_RC_QUAL_MAX ((DSV_MAX_QUALITY * DSV_RC_QUAL_SCALE))
#define DSV_USER_QUAL_TO_RC_QUAL(user) ((user) * DSV_RC_QUAL_SCALE)
#define DSV_MAX_QUALITY (100)
#define DSV_QUALITY_PERCENT(pct) (pct)

typedef struct _DSV_ENCDATA {
    int refcount;

    DSV_FNUM fnum;
    DSV_FRAME *padded_frame;
    DSV_FRAME *pyramid[DSV_MAX_PYRAMID_LEVELS];
    DSV_FRAME *residual;
    DSV_FRAME *prediction;
    DSV_FRAME *recon_frame;

    DSV_PARAMS params;

    int quant;

    struct _DSV_ENCDATA *refdata;

    DSV_MV *final_mvs;
    int avg_err;
} DSV_ENCDATA;

typedef struct {
    int quality;

    int effort; /* encoder effort. DSV_MIN_EFFORT...DSV_MAX_EFFORT */

    int gop; /* GOP (Group of Pictures) length */

    int do_scd; /* scene change detection */
    int do_temporal_aq; /* toggle temporal adaptive quantization for I frames */
#define DSV_PSY_ADAPTIVE_QUANT      (1 << 0)
#define DSV_PSY_CONTENT_ANALYSIS    (1 << 1)
#define DSV_PSY_I_VISUAL_MASKING    (1 << 2)
#define DSV_PSY_P_VISUAL_MASKING    (1 << 3)
#define DSV_PSY_ADAPTIVE_RINGING    (1 << 4)

#define DSV_PSY_ALL 0xff
    int do_psy; /* BITFIELD enable psychovisual optimizations */
    int do_dark_intra_boost; /* boost quality in dark intra frames */
    int do_intra_filter; /* deringing filter on intra frames */
    int do_inter_filter; /* cleanup filter on inter frames */

    /* threshold for skip block determination. -1 = disable
    Larger value = more likely to mark it as skipped */
    int skip_block_thresh;

    int block_size_override_x;
    int block_size_override_y;

    int variable_i_interval; /* intra frame insertions reset GOP counter */
    /* rate control */
    int rc_mode; /* rate control mode */

    /* approximate average bitrate desired */
    unsigned bitrate;
    /* for ABR */
    int rc_pergop; /* update rate control per GOP instead of per frame */
    int min_q_step;
    int max_q_step;
    int min_quality;
    int max_quality;
    int min_I_frame_quality;
    int prev_I_frame_quality;

    int intra_pct_thresh; /* 0-100% */
    int scene_change_pct;
    unsigned stable_refresh; /* # frames after which stability accum resets */
    int pyramid_levels;

    /* used internally */
    unsigned rc_qual;
    /* bpf = bytes per frame
     * rf = rate factor (the factor being optimized for) */
#define DSV_RF_RESET 256 /* # frames after which average RF resets */
    unsigned rf_total;
    unsigned rf_reset;
    int rf_avg;
    int total_P_frame_q;
    int avg_P_frame_q;
    int prev_complexity;
    int curr_complexity;
    int curr_intra_pct;

    DSV_FNUM next_fnum;
    DSV_ENCDATA *ref;
    DSV_META vidmeta;
    int prev_link;
    int force_metadata;

    struct DSV_STAB_ACC {
        int32_t x;
        int32_t y;
    } *stability;
    unsigned refresh_ctr;
    uint8_t *blockdata;
    uint8_t *intra_map;

    DSV_FNUM prev_gop;
    int prev_quant;
} DSV_ENCODER;

extern void dsv_enc_init(DSV_ENCODER *enc);
extern void dsv_enc_free(DSV_ENCODER *enc);
extern void dsv_enc_set_metadata(DSV_ENCODER *enc, DSV_META *md);
extern void dsv_enc_force_metadata(DSV_ENCODER *enc);

extern void dsv_enc_start(DSV_ENCODER *enc);

/* returns number of buffers available in bufs ptr */
extern int dsv_enc(DSV_ENCODER *enc, DSV_FRAME *frame, DSV_BUF *bufs);
extern void dsv_enc_end_of_stream(DSV_ENCODER *enc, DSV_BUF *bufs);

/* used internally */
typedef struct {
    DSV_PARAMS *params;
    DSV_FRAME *src[DSV_MAX_PYRAMID_LEVELS + 1];
    DSV_FRAME *ref[DSV_MAX_PYRAMID_LEVELS + 1]; /* reconstructed reference frame */
    DSV_FRAME *ogr[DSV_MAX_PYRAMID_LEVELS + 1]; /* original reference frame */
    DSV_MV *mvf[DSV_MAX_PYRAMID_LEVELS + 1];
    DSV_MV *ref_mvf;
    DSV_ENCODER *enc;
    int quant;
} DSV_HME;

extern int dsv_hme(DSV_HME *hme, int *scene_change_blocks, int *avg_err);

#ifdef __cplusplus
}
#endif

#endif
