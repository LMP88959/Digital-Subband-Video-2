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

#include "util.h"
#include "dsv_encoder.h"

/* totally based on heuristics */
extern unsigned
estimate_bitrate(int quality, int gop, DSV_META *md)
{
    int bpf; /* bytes per frame */
    int bps; /* bytes per second */
    int maxdimratio;
    int fps;

    fps = (md->fps_num + md->fps_den / 2) / md->fps_den;
    switch (md->subsamp) {
        case DSV_SUBSAMP_444:
        default:
            bpf = 352 * 288 * 3;
            break;
        case DSV_SUBSAMP_422:
        case DSV_SUBSAMP_UYVY:
            bpf = 352 * 288 * 2;
            break;
        case DSV_SUBSAMP_420:
        case DSV_SUBSAMP_411:
            bpf = 352 * 288 * 3 / 2;
            break;
        case DSV_SUBSAMP_410:
            bpf = 352 * 288 * 9 / 8;
            break;
    }
    if (gop == DSV_GOP_INTRA) {
        bpf *= 4;
    }
    if (md->width < 320 && md->height < 240) {
        bpf /= 4;
    }
    maxdimratio = (((md->width + md->height) / 2) << 8) / 352;
    bpf = bpf * maxdimratio >> 8;
    bps = bpf * fps;
    return (bps / ((26 - quality / 4))) * 3 / 2;
}

extern unsigned
estimate_quality(int bps, int gop, DSV_META *md)
{
    int q;
    int bestq = 50, best = INT_MAX;
    /* don't care about q=100 */
    for (q = 0; q < 100; q++) {
        int rate, dif;

        rate = estimate_bitrate(q, gop, md);
        dif = abs(rate - bps);
        if (dif < best) {
            bestq = q;
            best = dif;
        }
    }
    return CLAMP(bestq, 0, 99);
}

extern void
conv444to422(DSV_PLANE *srcf, DSV_PLANE *dstf)
{
    int i, j, w, h, n;
    uint8_t *src, *dst;

    w = srcf->w;
    h = srcf->h;
    src = srcf->data;
    dst = dstf->data;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i += 2) {
            n = (i < w - 1) ? i + 1 : w - 1;
            dst[i >> 1] = (src[i] + src[n] + 1) >> 1;
        }
        src += srcf->stride;
        dst += dstf->stride;
    }
}

extern void
conv422to420(DSV_PLANE *srcf, DSV_PLANE *dstf)
{
    int i, j, w, h, n, s;
    uint8_t *src, *dst;

    w = srcf->w;
    h = srcf->h;
    src = srcf->data;
    dst = dstf->data;
    s = srcf->stride;
    for (i = 0; i < w; i++) {
        for (j = 0; j < h; j += 2) {
            n = (j < h - 1) ? j + 1 : h - 1;
            dst[dstf->stride * (j >> 1)] = (src[s * j] + src[s * n] + 1) >> 1;
        }
        src++;
        dst++;
    }
}

extern void
conv411to420(DSV_PLANE *srcf, DSV_PLANE *dstf)
{
    int i, j, w, h, n, s;
    uint8_t *src, *dst;

    w = srcf->w;
    h = srcf->h;
    src = srcf->data;
    dst = dstf->data;
    s = srcf->stride;
    for (i = 0; i < w * 2; i++) {
        for (j = 0; j < h; j += 2) {
            n = (j < h - 1) ? j + 1 : h - 1;
            dst[i + dstf->stride * (j >> 1)] = (src[(i >> 1) + s * j] + src[(i >> 1) + s * n] + 1) >> 1;
        }
    }
}

extern void
conv410to420(DSV_PLANE *srcf, DSV_PLANE *dstf)
{
    int i, j, w, h;
    uint8_t *src, *dst;

    w = srcf->w * 2;
    h = srcf->h * 2;
    src = srcf->data;
    dst = dstf->data;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            dst[i + dstf->stride * j] = src[(i >> 1) + srcf->stride * (j >> 1)];
        }
    }
}

#define FINISHED_TAGS 2
static int
read_token(FILE *in, char *line, char delim)
{
    int i;
    int c, ret = 1;

    i = 0;
    c = fgetc(in);
    while (c != delim) {
        if (c == EOF) {
            return 0;
        }
        if (c == '\n') {
            ret = FINISHED_TAGS;
            break;
        }
        if (i >= 255) {
            DSV_ERROR(("Y4M parse error!"));
            return 0;
        }
        line[i] = c;
        i++;
        c = fgetc(in);
    }
    line[i] = '\0';
    return ret;
}

extern int
dsv_y4m_read_hdr(FILE *in, int *w, int *h, int *subsamp, int *framerate, int *aspect)
{
#define Y4M_HDR "YUV4MPEG2 "
#define Y4M_TAG_DELIM 0x20
#define Y4M_EARLY_EOF \
        do if (res == 0) { \
            DSV_ERROR(("parsing Y4M: early EOF")); \
            return 0; \
        } while (0)
    char line[256];
    int interlace = 0;
    int c = 0, res;
    if (fread(line, 1, sizeof(Y4M_HDR) - 1, in) != (sizeof(Y4M_HDR) - 1)) {
        DSV_ERROR(("Bad Y4M header"));
        return 0;
    }
    c = fgetc(in);
    *subsamp = DSV_SUBSAMP_420; /* default */
    while (c != '\n') {
        switch (c) {
            case 'W':
                res = read_token(in, line, Y4M_TAG_DELIM);
                Y4M_EARLY_EOF;

                *w = atoi(line);
                if (*w <= 0) {
                    DSV_ERROR(("parsing Y4M: bad width %d", *w));
                    return 0;
                }
                if (res == FINISHED_TAGS) {
                    goto done;
                }
                break;
            case 'H':
                res = read_token(in, line, Y4M_TAG_DELIM);
                Y4M_EARLY_EOF;

                *h = atoi(line);
                if (*h <= 0) {
                    DSV_ERROR(("parsing Y4M: bad height %d", *h));
                    return 0;
                }
                if (res == FINISHED_TAGS) {
                    goto done;
                }
                break;
            case 'F':
                if (!read_token(in, line, ':')) {
                    DSV_ERROR(("parsing Y4M: early EOF"));
                    return 0;
                }
                framerate[0] = atoi(line);
                res = read_token(in, line, Y4M_TAG_DELIM);
                Y4M_EARLY_EOF;

                framerate[1] = atoi(line);
                if (res == FINISHED_TAGS) {
                    goto done;
                }
                break;
            case 'I':
                res = read_token(in, line, Y4M_TAG_DELIM);
                Y4M_EARLY_EOF;

                interlace = line[0];
                if (res == FINISHED_TAGS) {
                    goto done;
                }
                break;
            case 'A':
                if (!read_token(in, line, ':')) {
                    DSV_ERROR(("parsing Y4M: early EOF"));
                    return 0;
                }
                aspect[0] = atoi(line);
                res = read_token(in, line, Y4M_TAG_DELIM);
                Y4M_EARLY_EOF;

                aspect[1] = atoi(line);
                if (res == FINISHED_TAGS) {
                    goto done;
                }
                break;
            case 'C':
                res = read_token(in, line, Y4M_TAG_DELIM);
                Y4M_EARLY_EOF;

                if (line[0] == '4' && line[1] == '2' && line[2] == '0') {
                    *subsamp = DSV_SUBSAMP_420;
                } else if (line[0] == '4' && line[1] == '1' && line[2] == '1') {
                    *subsamp = DSV_SUBSAMP_411;
                } else if (line[0] == '4' && line[1] == '1' && line[2] == '0') {
                    *subsamp = DSV_SUBSAMP_410;
                } else if (line[0] == '4' && line[1] == '2' && line[2] == '2') {
                    *subsamp = DSV_SUBSAMP_422;
                } else if (line[0] == '4' && line[1] == '4' && line[2] == '4') {
                    *subsamp = DSV_SUBSAMP_444;
                } else {
                    DSV_ERROR(("Bad Y4M subsampling: %s", line));
                }
                if (res == FINISHED_TAGS) {
                    goto done;
                }
                break;
            case 'X':
                res = read_token(in, line, Y4M_TAG_DELIM);
                Y4M_EARLY_EOF;

                if (res == FINISHED_TAGS) {
                    goto done;
                }
                break;
        }
        c = fgetc(in);
    }
done:
    if (interlace != 'p') {
        DSV_WARNING(("DSV does not explicitly support interlaced video."));
    }
    return 1;
}

extern int
dsv_y4m_read_seq(FILE *in, uint8_t *o, int w, int h, int subsamp)
{
    size_t npix, chrsz = 0;
    size_t nread;
#define Y4M_FRAME_HDR "FRAME\n"
    size_t hdrsz;
    char line[8];
    hdrsz = sizeof(Y4M_FRAME_HDR) - 1;

    if (in == NULL) {
        return -1;
    }
    nread = fread(line, 1, hdrsz, in);
    if (nread != hdrsz) {
        if (nread == 0) {
            return -2;
        }
        DSV_ERROR(("failed read"));
        return -1;
    }
    if (memcmp(line, Y4M_FRAME_HDR, hdrsz) != 0) {
        DSV_ERROR(("bad Y4M frame header [%s]", line));
        return -1;
    }
    npix = w * h;
    switch (subsamp) {
        case DSV_SUBSAMP_444:
            chrsz = npix;
            break;
        case DSV_SUBSAMP_422:
            chrsz = (w / 2) * h;
            break;
        case DSV_SUBSAMP_420:
        case DSV_SUBSAMP_411:
            chrsz = npix / 4;
            break;
        case DSV_SUBSAMP_410:
            chrsz = npix / 16;
            break;
        default:
            DSV_ERROR(("unsupported format"));
            DSV_ASSERT(0);
            break;
    }
    if (fread(o, 1, npix + chrsz + chrsz, in) != (npix + chrsz + chrsz)) {
        long int pos = ftell(in);
        if (pos < 0) {
            return -1;
        }
        if ((pos % (npix + chrsz + chrsz)) == 0) {
            return -2;
        }
        return -1;
    }
    return 0;
}

extern void
dsv_y4m_write_hdr(FILE *out, int w, int h, int subsamp, int fpsn, int fpsd, int aspn, int aspd)
{
    char buf[256];
    char *subs = "420";

    switch (subsamp) {
         case DSV_SUBSAMP_444:
             subs = "444";
             break;
         case DSV_SUBSAMP_422:
             subs = "422";
             break;
         case DSV_SUBSAMP_420:
             subs = "420";
             break;
         case DSV_SUBSAMP_411:
             subs = "411";
             break;
         case DSV_SUBSAMP_410:
             subs = "410";
             break;
         default:
             DSV_ERROR(("unsupported format"));
             DSV_ASSERT(0);
             break;
     }

    sprintf(buf, "YUV4MPEG2 W%d H%d F%d:%d A%d:%d Ip C%s\n",
                        w, h, fpsn, fpsd, aspn, aspd, subs);
    fwrite(buf, 1, strlen(buf), out);
}

extern void
dsv_y4m_write_frame_hdr(FILE *out)
{
    char *fh = "FRAME\n";
    fwrite(fh, 1, strlen(fh), out);
}
