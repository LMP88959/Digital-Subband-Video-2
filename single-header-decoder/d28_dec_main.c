/*****************************************************************************/
/*
 * Digital Subband Video 2.8 Single-Header Decoder Implementation
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

#define _DSV2_IMPL_
#define _DSV2_MEMORY_STATS_
#include "dsv28dec.h"

#include <errno.h>

#define VERSION_BUILD 0

#define DRV_HEADER "Envel Graphics DSV v2.%d decoder by EMMIR 2024-2025. "\
                   "build %d\n", \
                    DSV_VERSION_MINOR, VERSION_BUILD

static char *progname = NULL;
static int dooverwrite = 1;
static int verbose = 0;

#define USE_STDIO_CHAR '-'

struct PARAM {
   char *prefix;
   int value;
   int min, max;
   int (*convert)(int);
   char *desc;
};

static struct PARAM dec_params[] = {
    { "out420p", 0, 0, 1, NULL,
            "convert video to 4:2:0 chroma subsampling before saving output. 0 = default"},
    { "y4m", 0, 0, 1, NULL,
            "write output as a YUV4MPEG2 (Y4M) file. 0 = default"},
    { NULL, 0, 0, 0, NULL, "" }
};

static struct {
    char *inp; /* input file path */
    char *out; /* output file path */
} opts;

static int
get_optval(struct PARAM *pars, char *name)
{
    int i;
    for (i = 0; pars[i].prefix != NULL; i++) {
        struct PARAM *par = &pars[i];
        if (strcmp(par->prefix, name) == 0) {
            return par->value;
        }
    }
    return 0;
}

static void
print_params(struct PARAM *pars)
{
    int i;

    printf("------------------------------------------------------------\n");
    for (i = 0; pars[i].prefix != NULL; i++) {
        struct PARAM *par = &pars[i];

        printf("\t-%s : %s\n", par->prefix, par->desc);
        printf("\t      [min = %d, max = %d]\n", par->min, par->max);
    }
    printf("\t-inp= : input file. NOTE: if not specified, defaults to stdin\n");
    printf("\t-out= : output file. NOTE: if not specified, defaults to stdout\n");
    printf("\t-y : do not prompt for confirmation when potentially overwriting an existing file\n");
    printf("\t-l<n> : set logging level to n (0 = none, 1 = error, 2 = warning, 3 = info, 4 = debug/all)\n");
    printf("\t-v : set verbose\n");
}

static void
usage(void)
{
    char *p = progname;

    printf(DRV_HEADER);
    printf("usage: %s [options]\n", p);
    printf("sample usage: %s -inp=video.dsv -out=decompressed.yuv -out420p=1\n", p);
    print_params(dec_params);
}

static int
stoint(char *s, int *err)
{
    char *tail;
    long val;

    errno = 0;
    *err = 0;
    val = strtol(s, &tail, 10);
    if (errno == ERANGE) {
        printf("integer out of integer range\n");
        *err = 1;
    } else if (errno != 0) {
        printf("bad string: %s\n", strerror(errno));
        *err = 1;
    } else if (*tail != '\0') {
        printf("integer contained non-numeric characters\n");
        *err = 1;
    }
    return val;
}

static int
fileexist(char *n)
{
    FILE *fp = fopen(n, "r");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
}

static int
promptoverwrite(char *fn)
{
    if (dooverwrite && fileexist(fn)) {
        do {
            char c = 0;
            printf("\n--- file (%s) already exists, overwrite? (y/n)\n", fn);
            scanf(" %c", &c);
            if (c == 'y' || c == 'Y') {
                return 1;
            }
            if (c == 'n' || c == 'N') {
                return 0;
            }
        } while (1);
    }
    return 1;
}

static int
prefixcmp(char *pref, char **s)
{
    int plen = strlen(pref);
    if (!strncmp(pref, *s, plen)) {
        *s += plen;
        return 1;
    }
    return 0;
}

static int
get_param(char *argv)
{
    int i;
    char *p = argv;
    int err = 0;
    struct PARAM *params;

    if (strcmp("help", p) == 0) {
        return 0;
    }
    if (*p != '-') {
        printf("strange argument: %s\n", p);
        return 0;
    }

    p++;
    if (strcmp("v", p) == 0) {
        verbose = 1;
        return 1;
    }
    if (strcmp("y", p) == 0) {
        dooverwrite = 0;
        return 1;
    }
    if (prefixcmp("l", &p)) {
        int lvl = stoint(p, &err);
        if (err) {
            printf("error reading argument: l\n");
            return 0;
        }
        lvl = CLAMP(lvl, 0, 4);
        d28_set_log_level(lvl);
        return 1;
    }
    if (prefixcmp("inp=", &p)) {
        opts.inp = p;
        return 1;
    }
    if (prefixcmp("out=", &p)) {
        opts.out = p;
        return 1;
    }

    params = dec_params;
    for (i = 0; params[i].prefix != NULL; i++) {
        struct PARAM *par = &params[i];
        char buf[512];
        snprintf(buf, sizeof(buf) - 1, "%s=", par->prefix);
        if (!prefixcmp(buf, &p)) {
            continue;
        }
        par->value = CLAMP(par->value, par->min, par->max);
        par->value = par->convert ? par->convert(stoint(p, &err)) : stoint(p, &err);
        if (err) {
            printf("error reading argument: %s\n", par->prefix);
            return 0;
        }
        return 1;
    }
    printf("unrecognized argument(s)\n");
    return 0;
}

static int
init_params(int argc, char **argv)
{
    int i;

    if (argc == 1) {
        printf("not enough args!\n");
        usage();
        return 0;
    }
    argc--;
    argv++;
    while (argc > 0) {
        i = get_param(*argv);
        if (i == 0) {
            usage();
            return 0;
        }
        argv += i;
        argc -= i;
    }
    return 1;
}

static void
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

static void
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

static void
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

static void
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

static DSV_FRAME *
convert_to_420(DSV_FRAME *frame, DSV_META *meta)
{
    DSV_FRAME *f420 = d28_mk_frame(DSV_SUBSAMP_420, frame->width, frame->height, 0);
    if (meta->subsamp == DSV_SUBSAMP_444) {
        DSV_FRAME *f422 = d28_mk_frame(DSV_SUBSAMP_422, frame->width, frame->height, 0);
        conv444to422(&frame->planes[1], &f422->planes[1]);
        conv444to422(&frame->planes[2], &f422->planes[2]);
        conv422to420(&f422->planes[1], &f420->planes[1]);
        conv422to420(&f422->planes[2], &f420->planes[2]);
        d28_frame_ref_dec(f422);
    } else if (meta->subsamp == DSV_SUBSAMP_422 || meta->subsamp == DSV_SUBSAMP_UYVY) {
        conv422to420(&frame->planes[1], &f420->planes[1]);
        conv422to420(&frame->planes[2], &f420->planes[2]);
    } else if (meta->subsamp == DSV_SUBSAMP_411) {
        conv411to420(&frame->planes[1], &f420->planes[1]);
        conv411to420(&frame->planes[2], &f420->planes[2]);
    } else if (meta->subsamp == DSV_SUBSAMP_410) {
        conv410to420(&frame->planes[1], &f420->planes[1]);
        conv410to420(&frame->planes[2], &f420->planes[2]);
    }
    { /* copy luma 1:1 */
        DSV_PLANE *cs = frame->planes;
        DSV_PLANE *cd = f420->planes;
        unsigned int rowlen = frame->planes[0].w;
        int i;
        for (i = 0; i < f420->planes[0].h; i++) {
            memcpy(DSV_GET_LINE(cd, i), DSV_GET_LINE(cs, i), rowlen);
        }
    }
    return f420;
}

static void
y4m_write_hdr(FILE *out, int w, int h, int subsamp, int fpsn, int fpsd, int aspn, int aspd)
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

static void
y4m_write_frame_hdr(FILE *out)
{
    char *fh = "FRAME\n";
    fwrite(fh, 1, strlen(fh), out);
}

static int
decode(void)
{
    DSV_DECODER dec;
    DSV_META *meta = NULL;
    DSV_FRAME *frame;
    int code, first = 1;
    DSV_FNUM dec_frameno = 0;
    DSV_FNUM frameno = 0;
    int to_420p, as_y4m;
    FILE *inpfile, *outfile;

    if (opts.inp[0] == USE_STDIO_CHAR) {
        inpfile = stdin;
    } else {
        inpfile = fopen(opts.inp, "rb");
        if (inpfile == NULL) {
            printf("error opening input file %s\n", opts.inp);
            return EXIT_FAILURE;
        }
    }

    if (opts.out[0] == USE_STDIO_CHAR) {
        outfile = stdout;
    } else {
        outfile = fopen(opts.out, "wb");
        if (outfile == NULL) {
            printf("error opening output file %s\n", opts.out);
            return EXIT_FAILURE;
        }
    }
    /* zero decoder struct */
    memset(&dec, 0, sizeof(dec));
    to_420p = get_optval(dec_params, "out420p");
    as_y4m = get_optval(dec_params, "y4m");
    if (verbose) {
        printf(DRV_HEADER);
        printf("\n");
    }
    while (1) {
        DSV_BUF buffer;
        int packet_type = 0;
        uint8_t hdr[DSV_PACKET_HDR_SIZE];
        size_t n;

        /* ideally, seek to a packet by searching for 'DSV28' in the byte stream */

        /* attempt to read header from input stream */
        n = fread(hdr, 1, DSV_PACKET_HDR_SIZE, inpfile);
        /* process potential packet header */
        if (d28_mk_packet_buf(hdr, n, &buffer, &packet_type) < 0) {
            DSV_ERROR(("error reading packet header"));
            break;
        }
        /* good packet header, so we read the number of bytes the packet contains */
        n = fread(buffer.data + DSV_PACKET_HDR_SIZE, 1, buffer.len - DSV_PACKET_HDR_SIZE, inpfile);
        if (n < buffer.len - DSV_PACKET_HDR_SIZE) {
            d28_buf_free(&buffer);
            DSV_ERROR(("error reading packet payload"));
            break;
        }
        /* give the full packet to the decoder */
        code = d28_dec(&dec, &buffer, &frame, &frameno);

        if (code == DSV_DEC_GOT_META) {
            static int got_it_once = 0;
            /* TODO: check if parameters changed mid-video? */
            if (!got_it_once) {
                meta = d28_get_metadata(&dec);
                got_it_once = 1;
                DSV_INFO(("got metadata"));
            }
        } else {
            int subsamp = DSV_SUBSAMP_420;
            if (code == DSV_DEC_EOS) {
                DSV_INFO(("got end of stream"));
                break;
            }
            if (code != DSV_DEC_OK || (frame == NULL)) {
                continue;
            }
            if (meta == NULL) {
                DSV_ERROR(("no metadata!"));
                break;
            }
            if (!to_420p) {
                subsamp = meta->subsamp;
            }
            if (as_y4m) {
                if (first) {
                    y4m_write_hdr(outfile, meta->width, meta->height, subsamp,
                            meta->fps_num, meta->fps_den,
                            meta->aspect_num, meta->aspect_den);
                    first = 0;
                }
                y4m_write_frame_hdr(outfile);
            }
            if (to_420p && meta->subsamp != DSV_SUBSAMP_420) {
                DSV_FRAME *f420 = convert_to_420(frame, meta);
                if (d28_yuv_write_seq(outfile, f420->planes) < 0) {
                    DSV_ERROR(("failed to write frame (ID %u, actual %u)", frameno, dec_frameno));
                }
                d28_frame_ref_dec(f420);
            } else {
                if (d28_yuv_write_seq(outfile, frame->planes) < 0) {
                    DSV_ERROR(("failed to write frame (ID %u, actual %u)", frameno, dec_frameno));
                }
            }
            if (verbose) {
                printf("\rdecoded frame (ID %u, actual %u)", frameno, dec_frameno);
                fflush(stdout);
            }
            dec_frameno++;
            d28_frame_ref_dec(frame);
        }
    }
    if (verbose) {
        printf("\n");
    }
    DSV_INFO(("freeing decoder"));
    d28_dec_free(&dec);
    if (meta) {
        d28_free(meta);
    }
    if (opts.inp[0] != USE_STDIO_CHAR) {
        fclose(inpfile);
    }
    if (opts.out[0] != USE_STDIO_CHAR) {
        fclose(outfile);
    }
    return EXIT_SUCCESS;
}

static int
startup(int argc, char **argv)
{
    if (!init_params(argc, argv)) {
        return EXIT_SUCCESS;
    }
    if (!promptoverwrite(opts.out)) {
        return EXIT_FAILURE;
    }
    return decode();
}

int
main(int argc, char **argv)
{
    static char standard[2] = { USE_STDIO_CHAR, '\0' };
    int ret;

    progname = argv[0];
    /* default to stdin/out */
    opts.inp = standard;
    opts.out = standard;

    d28_set_log_level(DSV_LEVEL_WARNING);
    ret = startup(argc, argv);
    /* d28_set_log_level(DSV_LEVEL_DEBUG); */
    d28_memory_report();
    return ret;
}
