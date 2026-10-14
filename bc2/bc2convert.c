/*****************************************************************************/
/*
 * Convert a BC2 Y4M file to a raw 24-bit RGB stream, PPM format, or YUV Y4M
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

/* compile with: cc -O3 -o bc2convert bc2.c bc2convert.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc2.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef CLAMP
#define CLAMP(x, a, b) ((x) < (a) ? (a) : ((x) > (b) ? (b) : (x)))
#endif

#define USE_STDIO_CHAR '-'

#define OUT_RGB 0
#define OUT_PPM 1
#define OUT_Y4M 2

#define YC_BT601 0
#define YC_BT709 1

#define RANGE_FULL    0
#define RANGE_LIMITED 1

#define FREE_AND_NULL(ptr) \
    if (ptr) {             \
        free(ptr);         \
        ptr = NULL;        \
    }

static int
read_y4m_hdr(FILE *in, int *w, int *h, char *subsamp_str, int max_subsamp_len, char *hdr, int max_hdr_len)
{
#define Y4M_HDR "YUV4MPEG2"
    int i;

    if (!fgets(hdr, max_hdr_len, in)) {
        fprintf(stderr, "Could not read Y4M header\n");
        return 0;
    }

    if (strncmp(hdr, Y4M_HDR, sizeof(Y4M_HDR) - 1) != 0) {
        fprintf(stderr, "Bad Y4M header\n");
        return 0;
    }

    strcpy(subsamp_str, "420");
    *w = 0;
    *h = 0;

    for (i = (sizeof(Y4M_HDR) - 1); hdr[i] != '\n' && hdr[i] != '\0'; i++) {
        if (hdr[i] != ' ') {
            continue;
        }
        i++;
        if (hdr[i] == 'W') {
            *w = atoi(&hdr[i + 1]);
        } else if (hdr[i] == 'H') {
            *h = atoi(&hdr[i + 1]);
        } else if (hdr[i] == 'C') {
            int csp = 0;
            i++;
            while (hdr[i] != ' ' && hdr[i] != '\n' && hdr[i] != '\0') {
                if (csp < max_subsamp_len) {
                    subsamp_str[csp++] = hdr[i];
                }
                i++;
            }
            subsamp_str[csp] = '\0';
            i--;
        }
    }

    return (*w > 0 && *h > 0);
}

static void
bicubic_upsample(uint8_t *dst, int dstlen, uint8_t *src, int srclen)
{
    int i, r, ratio;

    if (srclen == dstlen) {
        memcpy(dst, src, srclen);
        return;
    }

    ratio = dstlen / srclen;

    for (i = 0; i < srclen; i++) {
        dst[i * ratio] = src[i];
        /* fill out upsampled neighbors */
        for (r = 1; r < ratio; r++) {
            int s0, s1, s2, s3;
            int sum = 0;

            s0 = src[((i - 1) < 0) ? 0 : (i - 1)];
            s1 = src[i];
            s2 = src[((i + 1) >= srclen) ? (srclen - 1) : (i + 1)];
            s3 = src[((i + 2) >= srclen) ? (srclen - 1) : (i + 2)];

            if (ratio == 2) {
                sum = -7 * s0 + 53 * s1 + 21 * s2 - 3 * s3;
            } else {
                switch (r) {
                    case 1:
                        sum = -9 * s0 + 59 * s1 + 17 * s2 - 3 * s3;
                        break;
                    case 2:
                        sum = -7 * s0 + 45 * s1 + 31 * s2 - 5 * s3;
                        break;
                    default:
                        sum = -4 * s0 + 23 * s1 + 51 * s2 - 6 * s3;
                        break;
                }
            }

            sum = (sum >= 0) ? ((sum + 32) / 64) : ((sum - 32) / 64);
            dst[i * ratio + r] = CLAMP(sum, 0, 255);
        }
    }
}

typedef struct {
    char *str;
    int val;
} ARGPAIR;

static int
read_string_arg(char *arg, ARGPAIR *pairs, int npairs, int *outparam)
{
    int i;
    for (i = 0; i < npairs; i++) {
        if (strcmp(arg, pairs[i].str) == 0) {
            *outparam = pairs[i].val;
            return 1;
        }
    }
    return 0;
}

#define ARRLEN(arr) (sizeof(arr) / sizeof(arr[0]))

int
main(int argc, char **argv)
{
    FILE *fin, *fout;
    char *fpref_in, *fpref_out;
    char frame_hdr[6];
    char subsamp_str[32];
    char full_hdr[1024];
    int i, j;
    int lum_w, lum_h, chr_w, chr_h, luma_sz, chroma_sz;
    int out_format = OUT_RGB;
    int color_matrix = YC_BT601;
    int inp_range = RANGE_FULL;
    int out_range = RANGE_LIMITED;
    int max_frames = -1;
    int frames_decoded = 0;
    uint8_t *bc2buf[3] = { NULL, NULL, NULL };
    uint8_t *cs_tmp = NULL, *ci_tmp = NULL;
    uint8_t *cs_444 = NULL, *ci_444 = NULL;
    uint8_t *cs_src = NULL, *ci_src = NULL;
    uint8_t *cs_dst = NULL, *ci_dst = NULL;
    uint8_t *rgbimg = NULL;
    uint8_t *u_out = NULL, *v_out = NULL;
    int16_t *u_accum = NULL, *v_accum = NULL;
    int x_shift = 0, y_shift = 0, downsample_shift = 0;

    memset(subsamp_str, 0, sizeof(subsamp_str));
    memset(full_hdr, 0, sizeof(full_hdr));
    lum_w = 0;
    lum_h = 0;
    i = 1;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    while (i < argc && argv[i][0] == '-') {
        if (argv[i][1] == '\0') {
            /* just a -, assume it's for the stdin/stdout specification */
            break;
        }
        if (strcmp(argv[i], "-o") == 0) {
            ARGPAIR pars[] = {
                { "rgb", OUT_RGB },
                { "ppm", OUT_PPM },
                { "y4m", OUT_Y4M }
            };
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -o expects an argument (rgb|ppm|y4m)\n");
                return EXIT_FAILURE;
            }
            if (!read_string_arg(argv[i + 1], pars, ARRLEN(pars), &out_format)) {
                fprintf(stderr, "error: invalid output format '%s'. expected rgb, ppm, or y4m.\n", argv[i + 1]);
                return EXIT_FAILURE;
            }
            i += 2;
        } else if (strcmp(argv[i], "-m") == 0) {
            ARGPAIR pars[] = {
                { "601", YC_BT601 },
                { "709", YC_BT709 }
            };
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -m expects an argument (601|709)\n");
                return EXIT_FAILURE;
            }
            if (!read_string_arg(argv[i + 1], pars, ARRLEN(pars), &color_matrix)) {
                fprintf(stderr, "error: unknown matrix '%s'. expected 601 or 709.\n", argv[i + 1]);
                return EXIT_FAILURE;
            }
            i += 2;
        } else if (strcmp(argv[i], "-ir") == 0 || strcmp(argv[i], "-or") == 0) {
            ARGPAIR pars[] = {
                { "full", RANGE_FULL },
                { "limited", RANGE_LIMITED }
            };
            int tmp_range = RANGE_FULL;

            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s expects an argument (full|limited)\n", argv[i]);
                return EXIT_FAILURE;
            }
            if (!read_string_arg(argv[i + 1], pars, ARRLEN(pars), &tmp_range)) {
                fprintf(stderr, "error: invalid range '%s'. expected full or limited.\n", argv[i + 1]);
                return EXIT_FAILURE;
            }
            if (strcmp(argv[i], "-ir") == 0) {
                inp_range = tmp_range;
            } else {
                out_range = tmp_range;
            }
            i += 2;
        } else if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -n expects an integer argument\n");
                return EXIT_FAILURE;
            }
            max_frames = atoi(argv[i + 1]);
            if (max_frames <= 0) {
                fprintf(stderr, "error: invalid frame count '%s', note: it must be greater than 0.\n", argv[i + 1]);
                return EXIT_FAILURE;
            }
            i += 2;
        } else {
            fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if ((argc - i) < 2) {
        fprintf(stderr, "%s [-o rgb|ppm|y4m] [-ir full|limited] [-or full|limited] [-m 601|709] [-n nfr] <in_name> <out_name>\n", argv[0]);
        fprintf(stderr, "required:\n");
        fprintf(stderr, "  <in_name>          must be either a Y4M file or a Y4M stream (- for stdin)\n");
        fprintf(stderr, "  <out_name>         output file path (- for stdout)\n");
        fprintf(stderr, "optional:\n");
        fprintf(stderr, "  -o rgb|ppm|y4m     output stream format. default=rgb\n");
        fprintf(stderr, "  -ir full|limited   specify range of input BC2 stream. default=full\n");
        fprintf(stderr, "  -or full|limited   specify range of output Y4M stream (used for Y4M output). default=limited\n");
        fprintf(stderr, "  -m 601|709         YCbCr color matrix (used for Y4M output). default=601)\n");
        fprintf(stderr, "  -n nfr             only process a specific number of frames\n");
        return EXIT_SUCCESS;
    }

    fpref_in = argv[i];
    fpref_out = argv[i + 1];

    if (fpref_in[0] == USE_STDIO_CHAR) {
        fin = stdin;
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

    bc2_init();

    if (!read_y4m_hdr(fin, &lum_w, &lum_h, subsamp_str, sizeof(subsamp_str) - 1, full_hdr, sizeof(full_hdr) - 1)) {
        if (fpref_in[0] != USE_STDIO_CHAR) {
            fclose(fin);
        }
        if (fpref_out[0] != USE_STDIO_CHAR) {
            fclose(fout);
        }
        return EXIT_FAILURE;
    }

    if (strcmp(subsamp_str, "444") == 0) {
        chr_w = lum_w;
        chr_h = lum_h;
    } else if (strcmp(subsamp_str, "422") == 0) {
        chr_w = lum_w / 2;
        chr_h = lum_h;
    } else if (strcmp(subsamp_str, "420") == 0 || strncmp(subsamp_str, "420", 3) == 0) {
        chr_w = lum_w / 2;
        chr_h = lum_h / 2;
    } else if (strcmp(subsamp_str, "411") == 0) {
        chr_w = lum_w / 4;
        chr_h = lum_h;
    } else if (strcmp(subsamp_str, "410") == 0) {
        chr_w = lum_w / 4;
        chr_h = lum_h / 2;
    } else {
        fprintf(stderr, "Bad Y4M subsampling: '%s' ... will assume 4:2:0\n", subsamp_str);
        chr_w = lum_w / 2;
        chr_h = lum_h / 2;
    }

    luma_sz = lum_w * lum_h;
    chroma_sz = chr_w * chr_h;

    bc2buf[0] = malloc(luma_sz);
    bc2buf[1] = malloc(chroma_sz);
    bc2buf[2] = malloc(chroma_sz);

    if (!bc2buf[0] || !bc2buf[1] || !bc2buf[2]) {
        fprintf(stderr, "out of memory allocating basic frame buffers\n");
        goto cleanup;
    }

    if (out_format == OUT_RGB || out_format == OUT_PPM) {
        cs_tmp = malloc(lum_w * chr_h);
        ci_tmp = malloc(lum_w * chr_h);
        cs_444 = malloc(luma_sz);
        ci_444 = malloc(luma_sz);
        cs_src = malloc(chr_h);
        ci_src = malloc(chr_h);
        cs_dst = malloc(lum_h);
        ci_dst = malloc(lum_h);
        rgbimg = malloc(lum_w * lum_h * 3);
        if (!cs_tmp || !ci_tmp || !cs_444 || !ci_444 || !cs_src || !ci_src || !cs_dst || !ci_dst || !rgbimg) {
            fprintf(stderr, "out of memory allocating RGB engine buffers\n");
            goto cleanup;
        }
    } else if (out_format == OUT_Y4M) {
        u_out = malloc(chroma_sz);
        v_out = malloc(chroma_sz);
        if (!u_out || !v_out) {
            fprintf(stderr, "out of memory allocating Y4M engine buffers\n");
            goto cleanup;
        }
        /* passthrough of Y4M metadata */
        fputs(full_hdr, fout);
        x_shift = (lum_w / chr_w == 2) ? 1 : ((lum_w / chr_w == 4) ? 2 : 0);
        y_shift = (lum_h / chr_h == 2) ? 1 : 0;
        /* determine how much to shift based on luma vs chroma area */
        switch ((lum_w / chr_w) * (lum_h / chr_h)) {
            case 2:
                downsample_shift = 1;
                break;
            case 4:
                downsample_shift = 2;
                break;
            case 8:
                downsample_shift = 3;
                break;
            default:
                downsample_shift = 0;
                break;
        }
    }
    while (fread(frame_hdr, 1, 6, fin) == 6) {
        if (strncmp(frame_hdr, "FRAME", 5) != 0) {
            fprintf(stderr, "could not find 'FRAME' in Y4M file\n");
            goto cleanup;
        }
        fread(bc2buf[0], 1, luma_sz, fin);
        fread(bc2buf[1], 1, chroma_sz, fin);
        fread(bc2buf[2], 1, chroma_sz, fin);
        if (out_format == OUT_RGB || out_format == OUT_PPM) {
            /* separable 2D bicubic upsampling */
            for (j = 0; j < chr_h; j++) {
                bicubic_upsample(&cs_tmp[j * lum_w], lum_w, &bc2buf[1][j * chr_w], chr_w);
                bicubic_upsample(&ci_tmp[j * lum_w], lum_w, &bc2buf[2][j * chr_w], chr_w);
            }
            for (i = 0; i < lum_w; i++) {
                for (j = 0; j < chr_h; j++) {
                    cs_src[j] = cs_tmp[i + j * lum_w];
                    ci_src[j] = ci_tmp[i + j * lum_w];
                }
                bicubic_upsample(cs_dst, lum_h, cs_src, chr_h);
                bicubic_upsample(ci_dst, lum_h, ci_src, chr_h);
                for (j = 0; j < lum_h; j++) {
                    cs_444[i + j * lum_w] = cs_dst[j];
                    ci_444[i + j * lum_w] = ci_dst[j];
                }
            }
            for (j = 0; j < lum_h; j++) {
                for (i = 0; i < lum_w; i++) {
                    int br, cs, ci;
                    int r = 0, g = 0, b = 0;
                    int idx = i + j * lum_w;
                    br = bc2buf[0][idx];
                    cs = cs_444[idx];
                    ci = ci_444[idx];
                    BC2_TO_SRGB(br, cs, ci, r, g, b, !inp_range);
                    rgbimg[idx * 3 + 0] = CLAMP(r, 0, 255);
                    rgbimg[idx * 3 + 1] = CLAMP(g, 0, 255);
                    rgbimg[idx * 3 + 2] = CLAMP(b, 0, 255);
                }
            }
            if (out_format == OUT_PPM) {
                fprintf(fout, "P6\n%d %d\n255\n", lum_w, lum_h);
            }
            fwrite(rgbimg, 1, luma_sz * 3, fout);
        } else if (out_format == OUT_Y4M) {
            u_accum = calloc(chroma_sz, sizeof(*u_accum));
            v_accum = calloc(chroma_sz, sizeof(*v_accum));
            if (!u_accum || !v_accum) {
                fprintf(stderr, "out of memory allocating accumulation arrays\n");
                goto cleanup;
            }
            for (j = 0; j < lum_h; j++) {
                for (i = 0; i < lum_w; i++) {
                    int br, cs, ci;
                    int yy, cb, cr;
                    int r = 0, g = 0, b = 0;
                    int luma_idx = i + j * lum_w;
                    int chroma_idx = (i >> x_shift) + (j >> y_shift) * chr_w;

                    br = bc2buf[0][luma_idx];
                    cs = bc2buf[1][chroma_idx];
                    ci = bc2buf[2][chroma_idx];

                    BC2_TO_SRGB(br, cs, ci, r, g, b, !inp_range);
                    if (color_matrix == YC_BT709) {
                        if (out_range == RANGE_LIMITED) {
                            yy = (( 3000 * r + 10101 * g + 1020 * b + 8192) >> 14) + 16;
                            cb = ((-1711 * r - 5764  * g + 7475 * b + 8192) >> 14) + 128;
                            cr = (( 7475 * r - 6835  * g - 640  * b + 8192) >> 14) + 128;
                        } else {
                            yy = (( 3483 * r + 11718 * g + 1183 * b + 8192) >> 14);
                            cb = ((-1878 * r - 6322  * g + 8200 * b + 8192) >> 14) + 128;
                            cr = (( 8200 * r - 7441  * g - 759  * b + 8192) >> 14) + 128;
                        }
                    } else {
                        if (out_range == RANGE_LIMITED) {
                            yy = (( 4242 * r + 8327 * g + 1615 * b + 8192) >> 14) + 16;
                            cb = ((-2440 * r - 4784 * g + 7224 * b + 8192) >> 14) + 128;
                            cr = (( 7224 * r - 6049 * g - 1175 * b + 8192) >> 14) + 128;
                        } else {
                            yy = (( 4900 * r + 9617 * g + 1867 * b + 8192) >> 14);
                            cb = ((-2766 * r - 5427 * g + 8192 * b + 8192) >> 14) + 128;
                            cr = (( 8192 * r - 6860 * g - 1332 * b + 8192) >> 14) + 128;
                        }
                    }
                    bc2buf[0][luma_idx] = CLAMP(yy, 0, 255);
                    u_accum[chroma_idx] += cb;
                    v_accum[chroma_idx] += cr;
                }
            }
            for (i = 0; i < chroma_sz; i++) {
                u_out[i] = CLAMP(u_accum[i] >> downsample_shift, 0, 255);
                v_out[i] = CLAMP(v_accum[i] >> downsample_shift, 0, 255);
            }
            free(u_accum);
            free(v_accum);
            u_accum = NULL;
            v_accum = NULL;
            fwrite("FRAME\n", 1, 6, fout);
            fwrite(bc2buf[0], 1, luma_sz, fout);
            fwrite(u_out, 1, chroma_sz, fout);
            fwrite(v_out, 1, chroma_sz, fout);
        }
        frames_decoded++;
        if (max_frames > 0 && frames_decoded >= max_frames) {
            break;
        }
    }
cleanup:
    FREE_AND_NULL(bc2buf[0]);
    FREE_AND_NULL(bc2buf[1]);
    FREE_AND_NULL(bc2buf[2]);
    FREE_AND_NULL(cs_tmp);
    FREE_AND_NULL(ci_tmp);
    FREE_AND_NULL(cs_444);
    FREE_AND_NULL(ci_444);
    FREE_AND_NULL(cs_src);
    FREE_AND_NULL(ci_src);
    FREE_AND_NULL(cs_dst);
    FREE_AND_NULL(ci_dst);
    FREE_AND_NULL(rgbimg);
    FREE_AND_NULL(u_out);
    FREE_AND_NULL(v_out);
    FREE_AND_NULL(u_accum);
    FREE_AND_NULL(v_accum);
    if (fpref_in[0] != USE_STDIO_CHAR) {
        fclose(fin);
    }
    if (fpref_out[0] != USE_STDIO_CHAR) {
        fclose(fout);
    }
    return EXIT_SUCCESS;
}
