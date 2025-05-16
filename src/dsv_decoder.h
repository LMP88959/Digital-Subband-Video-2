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

#ifndef _DSV_DECODER_H_
#define _DSV_DECODER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "dsv.h"

#define DSV_DECODER_VERSION 2

typedef struct {
    DSV_PARAMS params;
    DSV_FRAME *out_frame;
    DSV_FRAME *ref_frame;

    uint8_t *blockdata;
    int refcount;
} DSV_IMAGE;

typedef struct {
    DSV_META vidmeta;
    DSV_IMAGE *ref;
#define DSV_DRAW_STABHQ 1 /* stable / high quality blocks */
#define DSV_DRAW_MOVECS 2 /* motion vectors */
#define DSV_DRAW_IBLOCK 4 /* intra subblocks */
    int draw_info; /* set by user */
    int got_metadata;
    int quant;
} DSV_DECODER;

#define DSV_DEC_OK        0
#define DSV_DEC_ERROR     1
#define DSV_DEC_EOS       2
#define DSV_DEC_GOT_META  3
#define DSV_DEC_NEED_NEXT 4

/* decode a buffer, returns a frame in *out and the frame number in *fn */
extern int dsv_dec(DSV_DECODER *d, DSV_BUF *buf, DSV_FRAME **out, DSV_FNUM *fn);

/* get the metadata that was decoded. NOTE: if no metadata has been decoded
 * yet, the returned struct will not contain any useful values. */
extern DSV_META *dsv_get_metadata(DSV_DECODER *d);

/* free anything the decoder was holding on to */
extern void dsv_dec_free(DSV_DECODER *d);

#ifdef __cplusplus
}
#endif

#endif
