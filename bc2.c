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

#include "bc2.h"

#include <string.h>

/* number of input samples in the reverse mapping */
#define REV_SAMPLES 2560

/* negative/positive padding */
#define REVMAP_NEGPAD 2048
#define REVMAP_POSPAD 3584
#define CLIP_NEGPAD 384
#define CLIP_POSPAD 384
static uint8_t revmap_store[REVMAP_NEGPAD + REV_SAMPLES + REVMAP_POSPAD];
static uint8_t clip_store[CLIP_NEGPAD + 256 + CLIP_POSPAD];

uint16_t bc2sqrttab[2048];
uint16_t bc2sqrndtab[256];
int16_t bc2expand[256];
uint8_t *bc2revmap = revmap_store + REVMAP_NEGPAD;
uint8_t *bc2clipbuf = clip_store + CLIP_NEGPAD;

static uint32_t
iisqrt(uint32_t n)
{
    uint32_t pos, res, rem;

    if (n == 0) {
        return 0;
    }
    res = 0;
    pos = 1 << 30;
    rem = n;

    while (pos > rem) {
        pos >>= 2;
    }
    while (pos) {
        uint32_t dif = res + pos;
        res >>= 1;
        if (rem >= dif) {
            rem -= dif;
            res += pos;
        }
        pos >>= 2;
    }
    return res;
}

/* 1. square 8 bit number,
 * 2. round it (visually determined),
 * 3. don't scale all the way back down, leave 3 bits of precision
 */
static uint16_t
sqrrnd(uint8_t x)
{
    /* rounding table */
    static uint8_t rtab[16] = {
            31, 95, 127, 127,
            191, 191, 191, 191,
            255, 255, 255, 255,
            255, 255, 255, 255
    };
    return (x * x + rtab[x >> 4]) >> 5;
}

/* precomputes forward/inverse mappings + clipping tables */
extern void
bc2_init(void)
{
    static int init = 0;
    int i, c;

    if (init) {
        return;
    }

    for (i = 0; i < 2048; i++) {
        bc2sqrttab[i] = iisqrt(i * 2048);
    }

    memset(clip_store, 0, CLIP_NEGPAD);
    memset(revmap_store, 0, REVMAP_NEGPAD);
    for (i = 0; i < 256; i++) {
        bc2sqrndtab[i] = sqrrnd(i);
        bc2clipbuf[i] = i;
        bc2expand[i] = 16 * (i - 16) * 255 / 219;
    }
    for (i = 0; i < REV_SAMPLES; i++) {
        c = iisqrt((i * 32 * 41) / 50);
        bc2revmap[i] = CLAMP(c, 0, 255);
    }
    memset(clip_store + CLIP_NEGPAD + 256, 255, CLIP_POSPAD);
    memset(revmap_store + REVMAP_NEGPAD + REV_SAMPLES, 255, REVMAP_POSPAD);

    init = 1;
}

