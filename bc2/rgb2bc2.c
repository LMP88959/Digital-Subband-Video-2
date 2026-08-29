/*****************************************************************************/
/*
 * BC2 Color Space Conversion Library
 *
 *     -
 *    =--  2025-2026 EMMIR
 *   ==---  Envel Graphics
 *  ===----
 *
 *   GitHub : https://github.com/LMP88959
 *   YouTube: https://www.youtube.com/@EMMIR_KC/videos
 *   Discord: https://discord.com/invite/hdYctSmyQJ
 */
/*****************************************************************************/
/*
 * This software was designed and written by EMMIR, 2025-2026 of Envel Graphics
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bc2.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define FMT_444 0
#define FMT_422 1
#define FMT_420 2
#define FMT_411 3
#define FMT_410 4

#define USE_STDIO_CHAR '-'

static uint8_t *rgb;
static uint8_t *bc2;
static uint8_t *tmp1, *tmp2;
static int limited_luma = 1;
static int readingstdio = 0;
static int fancy_subsampling = 1;

static int
readrgb(FILE *in, int fno, int width, int height)
{
    uint32_t npix, offset;
    int ret = 0;
    if (in == NULL || fno < 0) {
        return -1;
    }
    npix = width * height * 3;
    offset = fno * npix;
    if (!readingstdio) {
        if (fseek(in, offset, SEEK_SET)) {
            fprintf(stderr, "seek error\n");
            ret = -1; goto err;
        }
    }
    if (fread(rgb, 1, npix, in) != npix) {
        fprintf(stderr, "didnt read enough pixels\n");
        ret = -1; goto err;
    }
err:
    return ret;
}

static int
writebc2(FILE *out, int width, int height, int subsamp)
{
    uint32_t npix, chrsz;
    int ret = 0;
    if (out == NULL)
        return -1;
    npix = width * height;
    switch (subsamp) {
        case FMT_444:
            chrsz = npix;
            break;
        case FMT_422:
            chrsz = (width / 2) * height;
            break;
        case FMT_420:
        case FMT_411:
            chrsz = npix / 4;
            break;
        case FMT_410:
            chrsz = npix / 16;
            break;
        default:
            perror("selected format is currently unsupported for writing");
            exit(-1);
            break;
    }
    if (fwrite(bc2, 1, npix, out) != npix) {
        fprintf(stderr, "e\n");
        ret = -1; goto err;
    }
    if (fwrite(bc2 + npix, 1, chrsz * 2, out) != (chrsz * 2)) {
        fprintf(stderr, "f\n");
        ret = -1; goto err;
    }
err:
    return ret;
}

#define UAVG2(a, b) ((unsigned) ((a) + (b) + 1) >> 1)
#define UAVG4(a, b, c, d) ((unsigned) ((a) + (b) + (c) + (d) + 2) >> 2)

static int
subsamp420(int c00, int c10, int c01, int c11, int lav, int cav)
{
    int lo, hi, l1 = c10, h1 = c00, l2 = c11, h2 = c01;

    /* fastest min/max of 4 values */
    if (c00 < c10) {
        l1 = c00;
        h1 = c10;
    }
    if (c01 < c11) {
        l2 = c01;
        h2 = c11;
    }
    lo = (l1 < l2) ? l1 : l2;
    hi = (h1 > h2) ? h1 : h2;
    cav += ((256 - lav) * (cav - 128) / 256);
    if (cav < lo) {
        return lo;
    }
    return cav > hi ? hi : cav;
}

static int
subsamp422(int c00, int c10, int lav, int cav)
{
    int lo, hi, l1 = c10, h1 = c00;

    if (c00 < c10) {
        l1 = c00;
        h1 = c10;
    }

    lo = l1;
    hi = h1;
    cav += ((256 - lav) * (cav - 128) / 256);
    if (cav < lo) {
        return lo;
    }
    return cav > hi ? hi : cav;
}

static int
subsamp411(int c0, int c1, int c2, int c3, int lav, int cav)
{
    int lo, hi, l1 = c1, h1 = c0, l2 = c3, h2 = c2;

    if (c0 < c1) {
        l1 = c0;
        h1 = c1;
    }
    if (c2 < c3) {
        l2 = c2;
        h2 = c3;
    }
    lo = (l1 < l2) ? l1 : l2;
    hi = (h1 > h2) ? h1 : h2;
    cav += ((256 - lav) * (cav - 128) / 256);
    if (cav < lo) {
        return lo;
    }
    return cav > hi ? hi : cav;
}

static void
rgb2bc2_420(int width, int height)
{
    int i, j, pitch;
    uint8_t *r, *g, *b, *rgbp;
    uint8_t *y, *u, *v;
    uint8_t *pu1, *pu2, *pu3, *pu4;
    uint8_t *pv1, *pv2, *pv3, *pv4;

    pitch = width * 3;

    y = bc2;
    u = tmp1;
    v = tmp2;
    rgbp = rgb;

    for (j = 0; j < height; j++) {
        r = rgbp + 0;
        g = rgbp + 1;
        b = rgbp + 2;
        for (i = 0; i < width; i++) {
            SRGB_TO_BC2(*r, *g, *b, *y++, *u++, *v++, !limited_luma);
            r += 3;
            g += 3;
            b += 3;
        }
        rgbp += pitch;
    }

    u = bc2 + width * height;
    v = u + width * height / 4;

    pu1 = tmp1;
    pu2 = pu1 + 1;
    pu3 = pu1 + width;
    pu4 = pu3 + 1;

    pv1 = tmp2;
    pv2 = pv1 + 1;
    pv3 = pv1 + width;
    pv4 = pv3 + 1;
    for (j = 0; j < height; j += 2) {
        for (i = 0; i < width; i += 2) {
            if (fancy_subsampling) {
                int l00, l10, l01, l11, lav;
                int c00, c10, c01, c11, cav;
                int var;

                l00 = bc2[(i + 0) + (j + 0) * width];
                l10 = bc2[(i + 1) + (j + 0) * width];
                l01 = bc2[(i + 0) + (j + 1) * width];
                l11 = bc2[(i + 1) + (j + 1) * width];
                lav = UAVG4(l00, l10, l01, l11);
                var = abs(l00 - lav) + abs(l10 - lav) + abs(l01 - lav) + abs(l11 - lav);
                if (var < 96) {
                    *u++ = UAVG4(*pu1, *pu2, *pu3, *pu4);
                    *v++ = UAVG4(*pv1, *pv2, *pv3, *pv4);
                } else {
                    c00 = *pu1;
                    c10 = *pu2;
                    c01 = *pu3;
                    c11 = *pu4;
                    cav = UAVG4(c00, c10, c01, c11);
                    *u++ = subsamp420(c00, c10, c01, c11, lav, cav);
                    c00 = *pv1;
                    c10 = *pv2;
                    c01 = *pv3;
                    c11 = *pv4;
                    cav = UAVG4(c00, c10, c01, c11);
                    *v++ = subsamp420(c00, c10, c01, c11, lav, cav);
                }
            } else {
                *u++ = UAVG4(*pu1, *pu2, *pu3, *pu4);
                *v++ = UAVG4(*pv1, *pv2, *pv3, *pv4);
            }
            pu1 += 2;
            pu2 += 2;
            pu3 += 2;
            pu4 += 2;
            pv1 += 2;
            pv2 += 2;
            pv3 += 2;
            pv4 += 2;
        }
        pu1 += width;
        pu2 += width;
        pu3 += width;
        pu4 += width;
        pv1 += width;
        pv2 += width;
        pv3 += width;
        pv4 += width;
    }
}

static void
rgb2bc2_422(int width, int height)
{
    int lumasz = width * height;
    int chromasz = (width / 2) * height;
    int yp = 0;
    int up = lumasz;
    int vp = lumasz + chromasz;
    int i, j;

    for (j = 0; j < height; j++) {
        uint8_t *rgbp = rgb + (j * width * 3);
        for (i = 0; i < width; i += 2) {
            int l0, l1, lav;
            int c0a, c1a, c0b, c1b;
            SRGB_TO_BC2(*rgbp++, *rgbp++, *rgbp++, l0, c0a, c1a, !limited_luma);
            SRGB_TO_BC2(*rgbp++, *rgbp++, *rgbp++, l1, c0b, c1b, !limited_luma);
            bc2[yp++] = l0;
            bc2[yp++] = l1;
            if (fancy_subsampling) {
                int var;
                lav = UAVG2(l0, l1);
                var = abs(l0 - lav) + abs(l1 - lav);
                if (var < 48) {
                    bc2[up++] = UAVG2(c0a, c0b);
                    bc2[vp++] = UAVG2(c1a, c1b);
                } else {
                    bc2[up++] = subsamp422(c0a, c0b, lav, UAVG2(c0a, c0b));
                    bc2[vp++] = subsamp422(c1a, c1b, lav, UAVG2(c1a, c1b));
                }
            } else {
                bc2[up++] = UAVG2(c0a, c0b);
                bc2[vp++] = UAVG2(c1a, c1b);
            }
        }
    }
}

static void
rgb2bc2_411(int width, int height)
{
    int lumasz = width * height;
    int chromasz = (width / 4) * height;
    int yp = 0;
    int up = lumasz;
    int vp = lumasz + chromasz;
    int i, j;

    for (j = 0; j < height; j++) {
        uint8_t *rgbp = rgb + (j * width * 3);
        for (i = 0; i < width; i += 4) {
            int l0, l1, l2, l3, lav0, lav1;
            int c0a, c1a, c0b, c1b, c0c, c1c, c0d, c1d;
            SRGB_TO_BC2(*rgbp++, *rgbp++, *rgbp++, l0, c0a, c1a, !limited_luma);
            SRGB_TO_BC2(*rgbp++, *rgbp++, *rgbp++, l1, c0b, c1b, !limited_luma);
            SRGB_TO_BC2(*rgbp++, *rgbp++, *rgbp++, l2, c0c, c1c, !limited_luma);
            SRGB_TO_BC2(*rgbp++, *rgbp++, *rgbp++, l3, c0d, c1d, !limited_luma);
            bc2[yp++] = l0;
            bc2[yp++] = l1;
            bc2[yp++] = l2;
            bc2[yp++] = l3;
            if (fancy_subsampling) {
                int varA, varB;
                lav0 = UAVG2(l0, l1);
                lav1 = UAVG2(l2, l3);
                varA = abs(l0 - lav0) + abs(l1 - lav0);
                varB = abs(l2 - lav1) + abs(l3 - lav1);
                if (varA < 48 || varB < 48) {
                    bc2[up++] = UAVG4(c0a, c0b, c0c, c0d);
                    bc2[vp++] = UAVG4(c1a, c1b, c1c, c1d);
                } else {
                    bc2[up++] = subsamp411(c0a, c0b, c0c, c0d, lav0, UAVG4(c0a, c0b, c0c, c0d));
                    bc2[vp++] = subsamp411(c1a, c1b, c1c, c1d, lav1, UAVG4(c1a, c1b, c1c, c1d));
                }
            } else {
                bc2[up++] = UAVG4(c0a, c0b, c0c, c0d);
                bc2[vp++] = UAVG4(c1a, c1b, c1c, c1d);
            }
        }
    }

}

static void
rgb2bc2_444(int width, int height)
{
    int i, j;
    uint8_t *r, *g, *b, *rgbp;
    uint8_t *y, *u, *v;
    int pitch;
    pitch = width * 3;

    y = bc2;
    u = bc2 + 1 * width * height;
    v = bc2 + 2 * width * height;
    rgbp = rgb;

    for (i = 0; i < height; i++) {
        r = rgbp + 0;
        g = rgbp + 1;
        b = rgbp + 2;
        for (j = 0; j < width; j++) {
            SRGB_TO_BC2(*r, *g, *b, *y++, *u++, *v++, !limited_luma);

            r += 3;
            g += 3;
            b += 3;
        }
        rgbp += pitch;
    }
}

static void
y4m_write_hdr(FILE *out, int w, int h, int subsamp, int fpsn, int fpsd, int aspn, int aspd)
{
    char buf[256];
    char *subs = "420";

    switch (subsamp) {
         case FMT_444:
             subs = "444";
             break;
         case FMT_422:
             subs = "422";
             break;
         case FMT_420:
             subs = "420";
             break;
         case FMT_411:
             subs = "411";
             break;
         case FMT_410:
             subs = "410";
             break;
         default:
             perror("unsupported format");
             exit(-1);
             break;
     }

    sprintf(buf, "YUV4MPEG2 W%d H%d F%d:%d A%d:%d Ip C%s\n",
                        w, h, fpsn, fpsd, aspn, aspd, subs);
    fwrite(buf, 1, strlen(buf), out);
}

static void
y4m_write_frame_hdr(FILE *out)
{
    char *fh = "FRAME\n";
    fwrite(fh, 1, strlen(fh), out);
}

int
main(int argc, char **argv)
{
    FILE *fin, *fout;
    char *fpref_in, *fpref_out;
    int width, height, fps, format;
    int npix;
    unsigned nfr = 0;
    unsigned fno = 0;
    if (argc < 9) {
        fprintf(stderr, "%s <width> <height> <fps> <format> <full_luma_range> <fancy_subsampling> <num_frames[0=inf]> <in_name> <out_name>\n", argv[0]);
        fprintf(stderr, "all parameters are required and must be in the order listed.\n");
        fprintf(stderr, "if <in_name> is -, stdin is used\n");
        fprintf(stderr, "if <out_name> is -, stdout is used\n");

        fprintf(stderr, "---formats---\n");
        fprintf(stderr, "0 = 444\n");
        fprintf(stderr, "1 = 422\n");
        fprintf(stderr, "2 = 420\n");
        fprintf(stderr, "3 = 411\n");
        fprintf(stderr, "4 = 410 (currently unimplemented)\n");
        return EXIT_SUCCESS;
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    width = atoi(argv[1]);
    height = atoi(argv[2]);
    fps = atoi(argv[3]);
    format = atoi(argv[4]);
    limited_luma = !atoi(argv[5]);
    fancy_subsampling = !!atoi(argv[6]);
    nfr = atoi(argv[7]);
    fpref_in = argv[8];
    fpref_out = argv[9];
    fprintf(stderr, "reading: %s\nwriting: %s\n%dx%d\n", fpref_in, fpref_out, width, height);
    if (fpref_in[0] == USE_STDIO_CHAR) {
        fin = stdin;
        readingstdio = 1;
    } else {
        fin = fopen(fpref_in, "rb");
        if (fin == NULL) {
            fprintf(stderr, "error opening input file %s\n", fpref_in);
            return EXIT_FAILURE;
        }
    }

    if (fpref_out[0] == USE_STDIO_CHAR) {
        fout = stdout;
    } else {
        fout = fopen(fpref_out, "wb");
        if (fout == NULL) {
            fprintf(stderr, "error opening output file %s\n", fpref_out);
            return EXIT_FAILURE;
        }
    }
    npix = width * height;
    rgb = calloc(1, npix * 3);
    bc2 = calloc(1, npix * 3);

    tmp1 = calloc(1, npix * 3);
    tmp2 = calloc(1, npix * 3);
    bc2_init();
    y4m_write_hdr(fout, width, height, format, fps, 1, 1, 1);
    while ((nfr ? fno <= nfr : 1) && readrgb(fin, fno, width, height) >= 0) {
        y4m_write_frame_hdr(fout);
        switch (format) {
            case FMT_444:
                rgb2bc2_444(width, height);
                break;
            case FMT_422:
                rgb2bc2_422(width, height);
                break;
            case FMT_420:
                rgb2bc2_420(width, height);
                break;
            case FMT_411:
                rgb2bc2_411(width, height);
                break;
            default:
                perror("selected format is currently unsupported for writing");
                exit(-1);
                break;
        }
        writebc2(fout, width, height, format);
        fno++;
    }
    fprintf(stderr, "processed %d frames\n", fno);
    free(rgb);
    free(bc2);
    free(tmp1);
    free(tmp2);
    if (fpref_in[0] != USE_STDIO_CHAR) {
        fclose(fin);
    }
    if (fpref_out[0] != USE_STDIO_CHAR) {
        fclose(fout);
    }
    return EXIT_SUCCESS;
}
