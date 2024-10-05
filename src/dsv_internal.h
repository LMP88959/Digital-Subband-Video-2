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

#ifndef _DSV_INTERNAL_H_
#define _DSV_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#include "dsv.h"

/* subsections of the encoded data */
#define DSV_SUB_MODE 0 /* block modes */
#define DSV_SUB_MV_X 1 /* motion vector x coordinates */
#define DSV_SUB_MV_Y 2 /* motion vector y coordinates */
#define DSV_SUB_SBIM 3 /* sub block intra masks */
#define DSV_SUB_EPRM 4 /* expanded prediction range mode */
#define DSV_SUB_NSUB 5

#define DSV_FRAME_BORDER DSV_MAX_BLOCK_SIZE

typedef struct {
    DSV_PARAMS *params;
    uint8_t *blockdata; /* block bitmasks for adaptive things */
    uint8_t cur_plane;
    uint8_t isP; /* is P frame */
} DSV_FMETA; /* frame metadata */

typedef struct {
    uint8_t *start;
    unsigned pos;
} DSV_BS;

extern void dsv_bs_init(DSV_BS *bs, uint8_t *buffer);

extern void dsv_bs_align(DSV_BS *bs);

/* macros for really simple operations */
#define dsv_bs_aligned(bs) (((bs)->pos & 7) == 0)
#define dsv_bs_ptr(bs) ((bs)->pos / 8)
#define dsv_bs_set(bs, ptr) ((bs)->pos = (ptr) * 8)
#define dsv_bs_skip(bs, n_bytes) ((bs)->pos += (n_bytes) * 8)

extern void dsv_bs_concat(DSV_BS *bs, uint8_t *data, int len);

extern void dsv_bs_put_bit(DSV_BS *bs, int value);
extern unsigned dsv_bs_get_bit(DSV_BS *bs);

extern void dsv_bs_put_bits(DSV_BS *bs, unsigned n, unsigned value);
extern unsigned dsv_bs_get_bits(DSV_BS *bs, unsigned n);

extern void dsv_bs_put_ueg(DSV_BS *bs, unsigned value);
extern unsigned dsv_bs_get_ueg(DSV_BS *bs);

extern void dsv_bs_put_seg(DSV_BS *bs, int value);
extern int dsv_bs_get_seg(DSV_BS *bs);
extern void dsv_bs_put_neg(DSV_BS *bs, int value);
extern int dsv_bs_get_neg(DSV_BS *bs);

typedef struct {
    DSV_BS bs;
    int nz;
} DSV_ZBRLE;

extern void dsv_bs_init_rle(DSV_ZBRLE *rle, uint8_t *buf);
extern int dsv_bs_end_rle(DSV_ZBRLE *rle, int read);

extern void dsv_bs_put_rle(DSV_ZBRLE *rle, int b);
extern int dsv_bs_get_rle(DSV_ZBRLE *rle);

#define DSV_STABLE_BIT   0
#define DSV_MAINTAIN_BIT 1
#define DSV_SKIP_BIT     2
#define DSV_RINGING_BIT  3
#define DSV_INTRA_BIT    4

#define DSV_IS_STABLE     (1 << DSV_STABLE_BIT)
#define DSV_IS_MAINTAIN   (1 << DSV_MAINTAIN_BIT)
#define DSV_IS_SKIP       (1 << DSV_SKIP_BIT)
#define DSV_IS_RINGING    (1 << DSV_RINGING_BIT)
#define DSV_IS_INTRA      (1 << DSV_INTRA_BIT)

extern void dsv_fwd_sbt(DSV_PLANE *src, DSV_COEFS *dst, DSV_FMETA *fm);
extern void dsv_inv_sbt(DSV_PLANE *dst, DSV_COEFS *src, int q, DSV_FMETA *fm);

extern void dsv_encode_plane(DSV_BS *bs, DSV_COEFS *src, int q, DSV_FMETA *fm);
extern void dsv_decode_plane(uint8_t *in, unsigned s, DSV_COEFS *dst, int q, DSV_FMETA *fm);

extern int dsv_lb2(unsigned n);

extern void dsv_movec_pred(DSV_MV *vecs, DSV_PARAMS *p, int x, int y, int *px, int *py);
extern DSV_MV *dsv_intra_analysis(DSV_FRAME *src, DSV_FRAME *small_frame, int scale, DSV_PARAMS *params);

/* D.1 Luma Half-Pixel Filter */

#define DSV_HPF(a,b,c,d)  (19*((b)+(c))-3*((a)+(d)))

#define DSV_HP_ADD 16
#define DSV_HP_SHF 5

/* C.2 fixed point precision for determining what block a pixel lies in */
#define DSV_BLOCK_INTERP_P      14

extern void dsv_sub_pred(DSV_MV *mv, DSV_PARAMS *p, DSV_FRAME *pred, DSV_FRAME *resd, DSV_FRAME *ref);
extern void dsv_add_pred(DSV_MV *mv, DSV_FMETA *fm, int q, DSV_FRAME *resd, DSV_FRAME *out, DSV_FRAME *ref);
extern void dsv_add_res(DSV_MV *mv, DSV_FMETA *fm, int q, DSV_FRAME *resd, DSV_FRAME *pred);

#ifdef __cplusplus
}
#endif

#endif
