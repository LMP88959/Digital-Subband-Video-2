/*****************************************************************************/
/*
 * Digital Subband Video 1
 *   DSV-1
 *
 *     -
 *    =--  2023-2024 EMMIR
 *   ==---  Envel Graphics
 *  ===----
 *
 *   GitHub : https://github.com/LMP88959
 *   YouTube: https://www.youtube.com/@EMMIR_KC/videos
 *   Discord: https://discord.com/invite/hdYctSmyQJ
 */
/*****************************************************************************/

#ifndef _UTIL_H_
#define _UTIL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "dsv.h"

/* compute approximate bitrate for desired quality
 *
 * a quality of 80 for a CIF format video (4:2:0)
 *  at 30 fps will get you 1.0 MBits/s
 */
extern unsigned estimate_bitrate(int quality, int gop, DSV_META *md);

/* compute approximate quality for desired bitrate
 */
extern unsigned estimate_quality(int bps, int gop, DSV_META *md);

extern void conv444to422(DSV_PLANE *srcf, DSV_PLANE *dstf);
extern void conv422to420(DSV_PLANE *srcf, DSV_PLANE *dstf);
extern void conv411to420(DSV_PLANE *srcf, DSV_PLANE *dstf);
extern void conv410to420(DSV_PLANE *srcf, DSV_PLANE *dstf);

extern int dsv_y4m_read_hdr(FILE *in, int *w, int *h, int *subsamp, int *framerate, int *aspect);
extern int dsv_y4m_read_seq(FILE *in, uint8_t *o, int w, int h, int subsamp);
extern void dsv_y4m_write_hdr(FILE *out, int w, int h, int subsamp, int fpsn, int fpsd, int aspn, int aspd);
extern void dsv_y4m_write_frame_hdr(FILE *out);

#ifdef __cplusplus
}
#endif

#endif
