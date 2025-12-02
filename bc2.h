/*****************************************************************************/
/*
 * BC2 Color Space Conversion Library
 *
 *     -
 *    =--     2025 EMMIR
 *   ==---  Envel Graphics
 *  ===----
 *
 *   GitHub : https://github.com/LMP88959
 *   YouTube: https://www.youtube.com/@EMMIR_KC/videos
 *   Discord: https://discord.com/invite/hdYctSmyQJ
 */
/*****************************************************************************/
/*
 * This software was designed and written by EMMIR, 2025 of Envel Graphics
 * If you release anything with it, a comment in your code/README saying
 * where you got this code would be a nice gesture but it’s not mandatory.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef BC2_H
#define BC2_H

#include <stdint.h>

/*
 * BC2 is a pseudo-perceptual lossy (irreversible) color space designed to have
 * relatively low decoding complexity and be integer-only while also performing
 * (perceptually) better than YCbCr for lossy compression.
 *
 * Other recent perceptual color spaces include XYB (from JPEG-XL) and Oklab.
 * BC2 is different because its primary focus is speed not accuracy.
 *
 * Naming:
 *
 * B stands for brightness - representing something similar to luminance
 * BC2 == BC^2 == BCC
 * The 1st C stands for Chroma Significant (green-red)
 * The 2nd C stands for Chroma Insignificant (blue-yellow)
 *
 * Besides the two chroma channels, the ^2 also refers to the fact that the
 * BC2 color space approximates the forward and inverse gamma curve by
 * replacing it with squaring and sqrt, respectively, for increased performance.
 */

/* NOTE this implementation operates on:
 * =========================================
 *        sRGB as 8-bit per channel:
 *     R[0...255] G[0...255] B[0...255]
 * =========================================
 *        BC2 as 8-bit per channel:
 *   BR[0...255] CS[48...251] CI[45...235]
 *
 *
 *    if "full_range" is set to 0,
 *       BR ranges from [16...235]
 *
 *     CS and CI are centered around 128
 *    where 128 means there is no chroma
 *      contribution from that channel
 * =========================================
 */


#define SRGB_TO_BC2(r,g,b, br,cs,ci, full_range)        \
do {                                                    \
    int fr, fg, fb;                                     \
    int tb, ts, ti;                                     \
    fr = bc2sqrndtab[r];                                \
    fg = bc2sqrndtab[g];                                \
    fb = bc2sqrndtab[b] * 20; /* premultiply */         \
    tb = bc2sqrttab[(81 * fr + 139 * fg + fb) / 240];   \
    ts = bc2sqrttab[(51 * fr + 169 * fg + fb) / 240];   \
    ti = bc2sqrttab[(11 * fr + 9 * fg + fb) / 40];      \
    fr = (tb + ts + 4) / 8;                             \
    fg = (tb - ts);                                     \
    fb = ((ti + 2) / 4) - fr;                           \
    if (full_range) {                                   \
        br = bc2clipbuf[fr];                            \
    } else {                                            \
        br = ((bc2clipbuf[fr] * 219 + 254) / 255) + 16; \
    }                                                   \
    cs = bc2clipbuf[fg + 128];                          \
    ci = bc2clipbuf[fb + 128];                          \
} while(0)

    
#define BC2_TO_SRGB(br,cs,ci, r,g,b, full_range)        \
do {                                                    \
    int fr, fg, fb;                                     \
    int tb, ts, ti;                                     \
    fr = (full_range) ? ((br) * 8) : bc2expand[br];     \
    fg = ((cs) - 128);                                  \
    fb = ((ci) - 128) * 8;                              \
    tb = fr + fg;                                       \
    ts = fr - fg;                                       \
    ti = fr + fb;                                       \
    tb *= tb;                                           \
    ts *= ts;                                           \
    ti *= ti;                                           \
    r = bc2revmap[(32 * tb - 26 * ts - ti) / 2048];     \
    g = bc2revmap[(-8 * tb + 14 * ts - ti) / 2048];     \
    b = bc2revmap[(-14 * tb + 8 * ts + 11 * ti) / 2048];\
} while(0)

extern uint16_t bc2sqrttab[256 * 256];
extern uint16_t bc2sqrndtab[256];
extern int16_t bc2expand[256];
extern uint8_t *bc2revmap;
extern uint8_t *bc2clipbuf;

/*
 * It is necessary to call this function at least once
 * before using the conversion macros
 */
extern void bc2_init(void);

#endif

