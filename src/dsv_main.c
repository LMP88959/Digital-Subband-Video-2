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

#include "dsv.h"
#include "dsv_encoder.h"
#include "dsv_decoder.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

/*
 *
 * effort:
 *
 *   0  :
 *   1  :
 *   2  : diagonal full-pel estimation
 *   3  :
 *   4  : hpel
 *   5  :
 *   6  : chroma intra test
 *   7  : metadata stats
 *   8  : qpel
 *   9  :
 *   10 :
 *
 */
#define DRV_HEADER "Envel Graphics DSV v2.%d codec by EMMIR 2024-2025. "\
                   "encoder v%d. "  \
                   "decoder v%d. build %d\n", \
                    DSV_VERSION_MINOR, \
                    DSV_ENCODER_VERSION, DSV_DECODER_VERSION, DSV_VERSION_BUILD

static int encoding = 0;
static char *progname = NULL;
static int dooverwrite = 1;
static int verbose = 0;

#define INP_FMT_444  0
#define INP_FMT_422  1
#define INP_FMT_420  2
#define INP_FMT_411  3
#define INP_FMT_410  4
#define INP_FMT_UYVY 5

#define AUTO_BITRATE 0

#define USE_STDIO_CHAR '-'

static int
pct_to_qual(int v)
{
    return DSV_USER_QUAL_TO_RC_QUAL(v);
}

static int
to_bps(int v)
{
    return v * 1024;
}

static int
fmt_to_subsamp(int fmt)
{
    switch (fmt) {
        case INP_FMT_444:
            return DSV_SUBSAMP_444;
        case INP_FMT_422:
            return DSV_SUBSAMP_422;
        case INP_FMT_UYVY:
            return DSV_SUBSAMP_UYVY;
        case INP_FMT_420:
            return DSV_SUBSAMP_420;
        case INP_FMT_411:
            return DSV_SUBSAMP_411;
        case INP_FMT_410:
            return DSV_SUBSAMP_410;
    }
    return DSV_SUBSAMP_420;
}

struct PARAM {
   char *prefix;
   int value;
   int min, max;
   int (*convert)(int);
   char *desc;
   char *extra;
};

static struct PARAM enc_params[] = {
    { "qp", DSV_USER_QUAL_TO_RC_QUAL(-1), -1, 100, pct_to_qual,
            "quality percent. 100 = mathematically lossless mode. If -1 and ABR mode, it will auto-estimate a good starting qp for desired bitrate. If -1 and CRF mode, default to 85. -1 = default",
            "if ABR mode, the qp specified here will be the starting qp which will influence the quality of the beginning of your encoded video"},
    { "effort", DSV_MAX_EFFORT, DSV_MIN_EFFORT, DSV_MAX_EFFORT, NULL,
            "encoder effort. 0 = least effort, 10 = most effort. higher value -> better video, slower encoding. default = 10",
            "does not change decoding speed"},
    { "w", 352, 16, (1 << 24), NULL,
            "width of input video. 352 = default",
            "must be divisible by two"},
    { "h", 288, 16, (1 << 24), NULL,
            "height of input video. 288 = default",
            "must be divisible by two"},
    { "gop", -1, -1, DSV_GOP_INF, NULL,
            "Group Of Pictures length. 0 = intra frames only, -1 = set to framerate (e.g 30fps source -> 30 GOP length), -1 = default",
            "a good value is generally between 0.5 seconds and 10 seconds. e.g at 24 fps, GOP length of 12 is 0.5 seconds"},
    { "fmt", DSV_SUBSAMP_420, 0, INP_FMT_UYVY, fmt_to_subsamp,
            "chroma subsampling format of input video. 0 = 4:4:4, 1 = 4:2:2, 2 = 4:2:0, 3 = 4:1:1, 4 = 4:1:0, 5 = 4:2:2 UYVY, 2 = default",
            "4:1:0 is one chroma sample per 4x4 luma block"},
    { "nfr", -1, -1, INT_MAX, NULL,
            "number of frames to compress. -1 means as many as possible. -1 = default",
            "unlike -sfr, this parameter works when piping from stdin"},
    { "sfr", 0, 0, INT_MAX, NULL,
            "frame number to start compressing at. 0 = default",
            "does not work when piping from stdin"},
    { "noeos", 0, 0, 1, NULL,
            "do not write EOS packet at the end of the compressed stream. 0 = default",
            "useful for multithreaded encoding via concatenation"},
    { "fps_num", 30, 1, (1 << 24), NULL,
            "fps numerator of input video. 30 = default",
            "used for rate control in ABR mode, otherwise it's just metadata for playback"},
    { "fps_den", 1, 1, (1 << 24), NULL,
            "fps denominator of input video. 1 = default",
            "used for rate control in ABR mode, otherwise it's just metadata for playback"},
    { "aspect_num", 1, 1, (1 << 24), NULL,
            "aspect ratio numerator of input video. 1 = default",
            "only used as metadata for playback"},
    { "aspect_den", 1, 1, (1 << 24), NULL,
            "aspect ratio denominator of input video. 1 = default",
            "only used as metadata for playback"},
    { "ipct", 90, 0, 100, NULL,
            "percentage threshold of intra blocks in an inter frame after which it is simply made into an intra frame. 90 = default",
            "can be used as a sort of scene change detection alternative if SCD is disabled"},
    { "pyrlevels", 0, 0, DSV_MAX_PYRAMID_LEVELS, NULL,
            "number of pyramid levels to use in hierarchical motion estimation. 0 means auto-determine. 0 = default",
            "less than 3 levels gives noticeably bad results"},
    { "rc_mode", DSV_RATE_CONTROL_CRF, DSV_RATE_CONTROL_CRF, DSV_RATE_CONTROL_CQP, NULL,
            "rate control mode. 0 = constant rate factor (CRF), 1 = single pass average bitrate (ABR), 2 = constant quantization parameter (CQP). 0 = default",
            "ABR is recommended for hitting a target file size"},
    { "rc_pergop", 0, 0, 1, NULL,
            "for non-CQP rate control. 0 = quality is updated per frame, 1 = quality is updated per GOP. 0 = default",
            "per GOP can be better for visual consistency"},
    { "kbps", AUTO_BITRATE, AUTO_BITRATE, INT_MAX, to_bps,
            "ONLY FOR ABR RATE CONTROL: bitrate in kilobits per second. 0 = auto-estimate needed bitrate for desired qp. 0 = default",
            "adheres to specified frame rate"},
    { "minqstep", DSV_USER_QUAL_TO_RC_QUAL(1) / 2, 1, DSV_RC_QUAL_MAX, NULL,
            "min quality step when decreasing quality for CRF/ABR rate control, any step smaller in magnitude than minqstep will be set to zero, absolute quant amount in range [1, 400]. 2 = default (0.5%)",
            "generally not necessary to modify"},
    { "maxqstep", DSV_USER_QUAL_TO_RC_QUAL(1) / 4, 1, DSV_RC_QUAL_MAX, NULL,
            "max quality step for CRF/ABR rate control, absolute quant amount in range [1, 400]. 1 = default (0.25%)",
            "generally not necessary to modify"},
    { "minqp", -1, -1, 100, pct_to_qual,
            "minimum quality. -1 = auto, -1 = default",
            "use it to limit the CRF/ABR rate control algorithm"},
    { "maxqp", -1, -1, 100, pct_to_qual,
            "maximum quality. -1 = auto, -1 = default",
            "use it to limit the CRF/ABR rate control algorithm"},
    { "iminqp", -1, -1, 100, pct_to_qual,
            "minimum quality for intra frames. -1 = auto, -1 = default",
            "use it to limit the CRF/ABR rate control algorithm"},
    { "stabref", 0, 0, INT_MAX, NULL,
            "period (in # of frames) to refresh the stability block tracking. 0 = auto-determine. 0 = default",
            "recommended to keep as auto-determine but good values are typically between half the framerate and twice the framerate"},
    { "scd", 1, 0, 1, NULL,
            "do scene change detection. 1 = default",
            "let the encoder insert intra frames when it decides that the scene has changed (sufficient difference between consecutive frames)"},
    { "tempaq", 1, 0, 1, NULL,
            "do temporal adaptive quantization. If disabled, spatial methods will be used instead. 1 = default",
            "recommended to keep enabled, increases quality on features of the video that stay still"},
    { "bszx", -1, -1, 1, NULL,
            "override block sizes in the x (horizontal) direction. -1 = auto-determine. -1 = default. 0 = 16, 1 = 32",
            "16 is recommended for < 1920x1080 content"},
    { "bszy", -1, -1, 1, NULL,
            "override block sizes in the y (vertical) direction. -1 = auto-determine. -1 = default. 0 = 16, 1 = 32",
            "16 is recommended for < 1920x1080 content"},
    { "scpct", 90, 0, 100, NULL,
            "scene change percentage. 90 = default",
            "decrease to make scene changes more common, increase to make them more infrequent"},
    { "skipthresh", 0, -1, INT_MAX, NULL,
           "skip block threshold. -1 = disable. 0 = default, larger value means more likely to mark a block as skipped.",
            "generally not necessary to modify"},
    { "varint", 1, 0, 1, NULL,
            "intra frames that are created outside of the normal GOP cycle reset the GOP cycle if 1. 1 = default",
            "generally good to keep this enabled unless you absolutely need an intra frame to exist every 'GOP' frames"},
    { "psy", DSV_PSY_ALL, 0, DSV_PSY_ALL, NULL,
           "enable/disable psychovisual optimizations. 255 = default",
           "can hurt or help depending on content. can be beneficial to try both and see which is better.\n"
           "\t\tcurrently defined bits (bit OR together to get multiple at the same time):\n"
           "\t\t1 = adaptive quantization\n"
           "\t\t2 = content analysis\n"
           "\t\t4 = I-frame visual masking\n"
           "\t\t8 = P-frame visual masking\n"
           "\t\t16 = adaptive ringing transform\n"
            },
    { "dib", 1, 0, 1, NULL,
           "enable/disable boosting the quality of dark intra frames. 1 = default",
           "helps retain details in darker scenes"},
    { "y4m", 0, 0, 1, NULL,
            "set to 1 if input is in YUV4MPEG2 (Y4M) format, 0 if raw YUV. 0 = default",
            "not all metadata will be passed through, Y4M parser is not a complete parser and some inputs could result in error"},
    { "ifilter", 1, 0, 1, NULL,
            "enable/disable intra frame deringing filter (essentially free assuming reasonable GOP length). 1 = default",
            "helps reduce ringing introduced at lower bit rates due to longer subband filters"},
    { "pfilter", 1, 0, 1, NULL,
            "enable/disable inter frame cleanup filter (small decoding perf hit but very noticeable increase in quality). 1 = default",
            "beneficial to coding efficiency and visual quality, highly recommended to keep enabled UNLESS source is very noisy"},
    { "psharp", 1, 0, 1, NULL,
            "inter frame sharpening. 0 = disabled, 1 = enabled, 1 = default",
            "smart image sharpening, helps reduce blurring in motion"},
    { NULL, 0, 0, 0, NULL, "", "" }
};

static struct PARAM dec_params[] = {
    { "out420p", 0, 0, 1, NULL,
            "convert video to 4:2:0 chroma subsampling before saving output. 0 = default",
            NULL},
    { "y4m", 0, 0, 1, NULL,
            "write output as a YUV4MPEG2 (Y4M) file. 0 = default",
            NULL},
    { "postsharp", 0, 0, 1, NULL,
            "postprocessing/decoder side frame sharpening. 0 = disabled, 1 = enabled, 0 = default",
            NULL},
    { "drawinfo", 0, 0, (DSV_DRAW_STABHQ | DSV_DRAW_MOVECS | DSV_DRAW_IBLOCK), NULL,
            "draw debugging information on the decoded frames (bit OR together to get multiple at the same time):\n\t\t1 = draw stability info\n\t\t2 = draw motion vectors\n\t\t4 = draw intra subblocks. 0 = default",
            NULL},
    { NULL, 0, 0, 0, NULL, "", "" }
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
usage_general(void)
{
    char *p = progname;

    printf(DRV_HEADER);
    printf("usage: %s <e|d> [options]\n", p);
    printf("for more information about running the encoder: %s e help\n", p);
    printf("for more information about running the decoder: %s d help\n", p);
    printf("for verbose information about encoder parameters: %s e vhelp\n", p);
    printf("for verbose information about decoder parameters: %s d vhelp\n", p);
}

static void
print_params(struct PARAM *pars, int extra)
{
    int i;

    printf("------------------------------------------------------------\n");
    for (i = 0; pars[i].prefix != NULL; i++) {
        struct PARAM *par = &pars[i];

        printf("\t-%s : %s\n", par->prefix, par->desc);
        printf("\t      [min = %d, max = %d]\n", par->min, par->max);
        if (extra && par->extra) {
            printf("\textra info: %s\n\n", par->extra);
        }

    }
    printf("\t-inp= : input file. NOTE: if not specified, defaults to stdin\n");
    printf("\t-out= : output file. NOTE: if not specified, defaults to stdout\n");
    printf("\t-y : do not prompt for confirmation when potentially overwriting an existing file\n");
    printf("\t-l<n> : set logging level to n (0 = none, 1 = error, 2 = warning, 3 = info, 4 = debug/all)\n");
    printf("\t-v : set verbose\n");
}

static void
usage_encoder(int extra)
{
    char *p = progname;

    printf(DRV_HEADER);
    printf("usage: %s e [options]\n", p);
    printf("sample usage: %s e -inp=video.yuv -out=compressed.dsv -w=352 -h=288 -fps_num=24 -fps_den=1 -qp=85 -gop=15\n", p);
    print_params(enc_params, extra);
}

static void
usage_decoder(int extra)
{
    char *p = progname;

    printf(DRV_HEADER);
    printf("usage: %s d [options]\n", p);
    printf("sample usage: %s d -inp=video.dsv -out=decompressed.yuv -out420p=1\n", p);
    print_params(dec_params, extra);
}

static void
usage(int extra)
{
    if (encoding) {
        usage_encoder(extra);
    } else {
        usage_decoder(extra);
    }
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

    if (strcmp("vhelp", p) == 0) {
        return -1;
    }
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
        dsv_set_log_level(lvl);
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

    if (encoding) {
        params = enc_params;
    } else {
        params = dec_params;
    }
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
        usage(0);
        return 0;
    }
    argc--;
    argv++;
    while (argc > 0) {
        i = get_param(*argv);
        if (i == 0) {
            usage(0);
            return 0;
        }
        if (i == -1) {
            usage(1);
            return 0;
        }
        argv += i;
        argc -= i;
    }
    return 1;
}

static uint8_t *enc_buf = NULL;
static unsigned bufsz = 0;

static uint8_t *
mrealloc(uint8_t *p, unsigned sz)
{
    if (p == NULL) {
        return malloc(sz);
    }
    return realloc(p, sz);
}

static void
savebuffer(DSV_BUF *buffer)
{
    enc_buf = mrealloc(enc_buf, bufsz + buffer->len);
    memcpy(enc_buf + bufsz, buffer->data, buffer->len);
    bufsz += buffer->len;
}

static int
writefile(char *n, unsigned char *buf, long len)
{
    FILE *fp;

    if (n[0] == USE_STDIO_CHAR) {
        fp = stdout;
    } else {
        fp = fopen(n, "wb");
        if (fp == NULL) {
            perror("unable to open file");
            return 0;
        }
    }
    if (fwrite(buf, 1, len, fp) != (unsigned) len) {
        perror("unable to write file");
        goto err;
    }

    if (n[0] != USE_STDIO_CHAR) {
        fclose(fp);
    }
    return 1;
err:
    if (n[0] != USE_STDIO_CHAR) {
        fclose(fp);
    }
    return 0;
}

static int
encode(void)
{
    uint8_t *picture;
    DSV_BUF bufs[4];
    DSV_FRAME *frame;
    DSV_META md;
    DSV_ENCODER enc;
    int i, run, spec_bps;
    int w, h, fps;
    int maxframe;
    FILE *inpfile;
    unsigned frno = 0;
    int nfr;
    int y4m_in = 0;
    int write_eos = 1;
    int no_more_data = 0;
    size_t full_hdrsz = 0;

    if (verbose) {
        printf(DRV_HEADER);
        printf("\n");
    }

    w = get_optval(enc_params, "w");
    h = get_optval(enc_params, "h");
    dsv_enc_init(&enc);

    md.width = w;
    md.height = h;

    md.subsamp = get_optval(enc_params, "fmt");
    md.fps_num = get_optval(enc_params, "fps_num");
    md.fps_den = get_optval(enc_params, "fps_den");
    md.aspect_num = get_optval(enc_params, "aspect_num");
    md.aspect_den = get_optval(enc_params, "aspect_den");
    md.inter_sharpen = get_optval(enc_params, "psharp");

    if (opts.inp[0] == USE_STDIO_CHAR) {
        inpfile = stdin;
        if (verbose) {
            printf("reading from stdin\n");
        }
    } else {
        inpfile = fopen(opts.inp, "rb");
        if (inpfile == NULL) {
            printf("error opening input file %s\n", opts.inp);
            return EXIT_FAILURE;
        }
    }
    y4m_in = get_optval(enc_params, "y4m");
    if (y4m_in) {
        int fr[2] = { 1, 1 };
        int asp[2] = { 1, 1 };

        if (!dsv_y4m_read_hdr(inpfile, &md.width, &md.height, &md.subsamp, fr, asp, &full_hdrsz)) {
            printf("bad Y4M file %s\n", opts.inp);
            return EXIT_FAILURE;
        }
        w = md.width;
        h = md.height;
        md.fps_num = fr[0];
        md.fps_den = fr[1];
        if (md.fps_den <= 0) {
            DSV_WARNING(("fps denominator was <= 0. Setting to 1."));
            md.fps_den = 1;
        }
        md.aspect_num = asp[0];
        md.aspect_den = asp[1];
    }
    fps = (md.fps_num + md.fps_den / 2) / md.fps_den;
    if (w <= 0 || h <= 0) {
        DSV_ERROR(("given dimensions were strange: %dx%d", w, h));
        return EXIT_FAILURE;
    }
    if (fps <= 0) {
        DSV_WARNING(("given frame rate was <= 0! setting to 1/1"));
        md.fps_num = 1;
        md.fps_den = 1;
        fps = 1;
    }
    dsv_enc_set_metadata(&enc, &md);

#define EXTRA_PAD 1
    picture = malloc(w * h * (3 + EXTRA_PAD)); /* allocate extra to be safe */

    enc.gop = get_optval(enc_params, "gop");
    if (enc.gop < 0) {
        enc.gop = fps;
    }
    enc.scene_change_pct = get_optval(enc_params, "scpct");
    enc.do_scd = get_optval(enc_params, "scd");
    enc.intra_pct_thresh = get_optval(enc_params, "ipct");
    enc.quality = get_optval(enc_params, "qp");
    enc.skip_block_thresh = get_optval(enc_params, "skipthresh");

    enc.rc_mode = get_optval(enc_params, "rc_mode");
    enc.rc_pergop = get_optval(enc_params, "rc_pergop");
    spec_bps = get_optval(enc_params, "kbps");
    if (enc.quality == DSV_USER_QUAL_TO_RC_QUAL(-1)) {
        int qual;

        if (enc.rc_mode != DSV_RATE_CONTROL_ABR || spec_bps == AUTO_BITRATE) {
            qual = 85;
        } else {
            qual = estimate_quality(spec_bps, enc.gop, &md);
        }
        enc.quality = DSV_USER_QUAL_TO_RC_QUAL(qual);
    }
    if (spec_bps == AUTO_BITRATE) {
        enc.bitrate = estimate_bitrate(enc.quality * 100 / DSV_RC_QUAL_MAX, enc.gop, &md);
    } else {
        enc.bitrate = spec_bps;
    }
    enc.min_q_step = get_optval(enc_params, "minqstep");
    enc.max_q_step = get_optval(enc_params, "maxqstep");
    enc.min_quality = get_optval(enc_params, "minqp");
    enc.max_quality = get_optval(enc_params, "maxqp");
    enc.min_I_frame_quality = get_optval(enc_params, "iminqp");
    switch (enc.rc_mode) {
        case DSV_RATE_CONTROL_CRF:
            if (enc.min_quality < 0) {
                enc.min_quality = enc.quality - DSV_USER_QUAL_TO_RC_QUAL(5);
            }
            if (enc.min_I_frame_quality < 0) {
                enc.min_I_frame_quality = enc.quality - DSV_USER_QUAL_TO_RC_QUAL(2);
            }
            if (enc.max_quality < 0) {
                enc.max_quality = DSV_RC_QUAL_MAX;
            }
            break;
        case DSV_RATE_CONTROL_ABR:
            if (enc.min_quality < 0) {
                enc.min_quality = 0;
            }
            if (enc.min_I_frame_quality < 0) {
                enc.min_I_frame_quality = DSV_USER_QUAL_TO_RC_QUAL(5);
            }
            if (enc.max_quality < 0) {
                enc.max_quality = DSV_RC_QUAL_MAX;
            }
            break;
        default:
            if (enc.min_quality < 0) {
                enc.min_quality = 0;
            }
            if (enc.min_I_frame_quality < 0) {
                enc.min_I_frame_quality = DSV_USER_QUAL_TO_RC_QUAL(5);
            }
            if (enc.max_quality < 0) {
                enc.max_quality = DSV_RC_QUAL_MAX;
            }
            break;
    }

    enc.min_quality = CLAMP(enc.min_quality, 0, DSV_RC_QUAL_MAX);
    enc.min_I_frame_quality = CLAMP(enc.min_I_frame_quality, 0, DSV_RC_QUAL_MAX);
    enc.max_quality = CLAMP(enc.max_quality, 0, DSV_RC_QUAL_MAX);

    enc.pyramid_levels = get_optval(enc_params, "pyrlevels");
    enc.stable_refresh = get_optval(enc_params, "stabref");
    if (enc.stable_refresh == 0) {
        enc.stable_refresh = CLAMP(fps, 1, 60);
    }
    enc.do_temporal_aq = get_optval(enc_params, "tempaq");
    enc.variable_i_interval = get_optval(enc_params, "varint");
    enc.block_size_override_x = get_optval(enc_params, "bszx");
    enc.block_size_override_y = get_optval(enc_params, "bszy");
    enc.effort = get_optval(enc_params, "effort");
    enc.do_psy = get_optval(enc_params, "psy");
    enc.do_dark_intra_boost = get_optval(enc_params, "dib");
    enc.do_intra_filter = get_optval(enc_params, "ifilter");
    enc.do_inter_filter = get_optval(enc_params, "pfilter");

    frno = get_optval(enc_params, "sfr");
    nfr = get_optval(enc_params, "nfr");
    write_eos = !get_optval(enc_params, "noeos");
    if (nfr > 0) {
        maxframe = frno + nfr;
    } else {
        maxframe = -1;
    }

    DSV_INFO(("starting encoder"));
    dsv_enc_start(&enc);
    run = 1;

    while (run) {
        int state;
        int frame_read = 0;
        if (maxframe > 0 && frno >= (unsigned) maxframe) {
            goto end_of_stream;
        }
        if (y4m_in) {
            if (opts.inp[0] == USE_STDIO_CHAR) {
                frame_read = dsv_y4m_read_seq(inpfile, picture, w, h, md.subsamp);
            } else {
                frame_read = dsv_y4m_read(inpfile, frno, full_hdrsz, picture, w, h, md.subsamp);
            }
        } else {
            if (opts.inp[0] == USE_STDIO_CHAR) {
                frame_read = dsv_yuv_read_seq(inpfile, picture, w, h, md.subsamp);
            } else {
                frame_read = dsv_yuv_read(inpfile, frno, picture, w, h, md.subsamp);
            }
        }
        if (frame_read < 0) {
            if (frame_read == -1) {
                DSV_ERROR(("failed to read frame %d", frno));
            }
            no_more_data = 1;
            goto end_of_stream;
        }
        frame = dsv_load_planar_frame(md.subsamp, picture, w, h);

        if (verbose) {
            printf("encoding frame %d\r", frno);
            fflush(stdout);
        } else {
            DSV_INFO(("encoding frame %d", frno));
        }
        state = dsv_enc(&enc, frame, bufs);

        run = !(state & DSV_ENC_FINISHED);
        state &= DSV_ENC_NUM_BUFS;
        if (verbose && state) {
            if (state > 1) {
                printf("encoded frame %d to %d bytes\n", frno, bufs[0].len + bufs[1].len);
            } else {
                printf("encoded frame %d to %d bytes\n", frno, bufs[0].len);
            }
            fflush(stdout);
        }
        for (i = 0; i < state; i++) {
            savebuffer(&bufs[i]);
            dsv_buf_free(&bufs[i]);
        }
        frno++;
        continue;
end_of_stream:
        if (write_eos || (!write_eos && no_more_data && bufsz > 0)) {
            dsv_enc_end_of_stream(&enc, bufs);
            savebuffer(&bufs[0]);
            dsv_buf_free(&bufs[0]);
        }
        break;
    }

    if (verbose) {
        /* KBps = kiloBYTES, kbps = kiloBITS */
        int bpf, bps, kbps, mbps;

        bpf = (bufsz * 8) / frno;
        bps = bpf * fps;
        kbps = bps / 1024;
        mbps = kbps / 1024;
        printf("\nencoded %d bytes @ %d bps, %d kbps, %d KBps, %d mbps. fps = %d, bpf = %d\n",
                bufsz, bps, kbps, kbps / 8, mbps, fps, bpf);
        if (enc.rc_mode == DSV_RATE_CONTROL_ABR) {
            printf("target bitrate = %d bps  %d KBps  %d kbps\n",
                    enc.bitrate, enc.bitrate / (8 * 1024), enc.bitrate / 1024);
        }
    }

    writefile(opts.out, enc_buf, bufsz);
    if (verbose) {
        printf("saved video file\n");
    }
    dsv_enc_free(&enc);

    if (opts.inp[0] != USE_STDIO_CHAR) {
        fclose(inpfile);
    }
    return no_more_data ? -2 : EXIT_SUCCESS;
}

#define DSV_PKT_ERR_EOF -1
#define DSV_PKT_ERR_OOB -2 /* out of bytes */
#define DSV_PKT_ERR_PSZ -3 /* bad packet size */
#define DSV_PKT_ERR_4CC -4 /* bad 4cc */

static int
read_packet(FILE *f, DSV_BUF *rb, int *packet_type)
{
    int n, size;
    uint8_t hdr[DSV_PACKET_HDR_SIZE];

    n = fread(hdr, 1, DSV_PACKET_HDR_SIZE, f);
    if (n == 0) {
        DSV_ERROR(("no data"));
        return DSV_PKT_ERR_EOF;
    }
    if (n < DSV_PACKET_HDR_SIZE) {
        DSV_ERROR(("not enough bytes"));
        return DSV_PKT_ERR_OOB;
    }

    if (hdr[0] != DSV_FOURCC_0 || hdr[1] != DSV_FOURCC_1 || hdr[2] != DSV_FOURCC_2 || hdr[3] != DSV_FOURCC_3) {
        DSV_ERROR(("bad 4cc (%c %c %c %c, %d %d %d %d",
                hdr[0],hdr[1],hdr[2],hdr[3],
                hdr[0],hdr[1],hdr[2],hdr[3]));
        return DSV_PKT_ERR_4CC;
    }
    /* DSV_INFO(("DSV version 2.%d", hdr[4])); */
    size = (hdr[DSV_PACKET_NEXT_OFFSET + 0] << 24) |
           (hdr[DSV_PACKET_NEXT_OFFSET + 1] << 16) |
           (hdr[DSV_PACKET_NEXT_OFFSET + 2] << 8) |
           (hdr[DSV_PACKET_NEXT_OFFSET + 3]);
    if (size == 0) {
        size = DSV_PACKET_HDR_SIZE;
    }
    if (size < DSV_PACKET_HDR_SIZE) {
        DSV_ERROR(("bad packet size"));
        return DSV_PKT_ERR_PSZ;
    }
    *packet_type = hdr[DSV_PACKET_TYPE_OFFSET];
    dsv_mk_buf(rb, size);
    memcpy(rb->data, hdr, DSV_PACKET_HDR_SIZE);
    n = fread(rb->data + DSV_PACKET_HDR_SIZE, 1, size - DSV_PACKET_HDR_SIZE, f);
    if (n < size - DSV_PACKET_HDR_SIZE) {
        DSV_ERROR(("did not read enough data: %d", size - DSV_PACKET_HDR_SIZE));
        dsv_buf_free(rb);
        return DSV_PKT_ERR_OOB;
    }

    return 1;
}

static int
decode(void)
{
    DSV_DECODER dec;
    DSV_BUF buffer;
    DSV_META *meta = NULL;
    DSV_FRAME *frame;
    int code, first = 1;
    DSV_FNUM dec_frameno = 0;
    DSV_FNUM frameno = 0;
    int to_420p, as_y4m, postsharp;
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
    memset(&dec, 0, sizeof(dec));
    to_420p = get_optval(dec_params, "out420p");
    as_y4m = get_optval(dec_params, "y4m");
    postsharp = get_optval(dec_params, "postsharp");
    dec.draw_info = get_optval(dec_params, "drawinfo");
    if (verbose) {
        printf(DRV_HEADER);
        printf("\n");
    }
    while (1) {
        int packet_type;

        if (read_packet(inpfile, &buffer, &packet_type) < 0) {
            DSV_ERROR(("error reading packet"));
            break;
        }

        code = dsv_dec(&dec, &buffer, &frame, &frameno);

        if (code == DSV_DEC_GOT_META) {
            static int got_it_once = 0;
            /* TODO: check if parameters changed mid-video? */
            if (!got_it_once) {
                meta = dsv_get_metadata(&dec);
                got_it_once = 1;
                DSV_INFO(("got metadata"));
            }
        } else {
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
            if (to_420p && meta->subsamp != DSV_SUBSAMP_420) {
                DSV_FRAME *f420 = dsv_mk_frame(DSV_SUBSAMP_420, frame->width, frame->height, 0);
                if (meta->subsamp == DSV_SUBSAMP_444) {
                    DSV_FRAME *f422 = dsv_mk_frame(DSV_SUBSAMP_422, frame->width, frame->height, 0);
                    conv444to422(&frame->planes[1], &f422->planes[1]);
                    conv444to422(&frame->planes[2], &f422->planes[2]);
                    conv422to420(&f422->planes[1], &f420->planes[1]);
                    conv422to420(&f422->planes[2], &f420->planes[2]);
                    dsv_frame_ref_dec(f422);
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
                if (postsharp) {
                    dsv_post_process(f420->planes + 0);
                }
                if (as_y4m) {
                    if (first) {
                        dsv_y4m_write_hdr(outfile, meta->width, meta->height,
                               DSV_SUBSAMP_420, meta->fps_num, meta->fps_den,
                               meta->aspect_num, meta->aspect_den);
                        first = 0;
                    }
                    dsv_y4m_write_frame_hdr(outfile);
                }
                if (dsv_yuv_write_seq(outfile, f420->planes) < 0) {
                    DSV_ERROR(("failed to write frame (ID %u, actual %u)", frameno, dec_frameno));
                }
                dsv_frame_ref_dec(f420);
            } else {
                if (as_y4m) {
                    if (first) {
                        dsv_y4m_write_hdr(outfile, meta->width, meta->height, meta->subsamp,
                                meta->fps_num, meta->fps_den,
                                meta->aspect_num, meta->aspect_den);
                        first = 0;
                    }
                    dsv_y4m_write_frame_hdr(outfile);
                }
                if (postsharp) {
                    DSV_FRAME *tfr = dsv_clone_frame(frame, 0);
                    dsv_post_process(tfr->planes + 0);
                    if (dsv_yuv_write_seq(outfile, tfr->planes) < 0) {
                        DSV_ERROR(("failed to write frame (ID %u, actual %u)", frameno, dec_frameno));
                    }
                    dsv_frame_ref_dec(tfr);
                } else {
                    if (dsv_yuv_write_seq(outfile, frame->planes) < 0) {
                        DSV_ERROR(("failed to write frame (ID %u, actual %u)", frameno, dec_frameno));
                    }
                }
            }
            if (verbose) {
                printf("\rdecoded frame (ID %u, actual %u)", frameno, dec_frameno);
                fflush(stdout);
            }
            dec_frameno++;
            dsv_frame_ref_dec(frame);
        }
    }
    if (verbose) {
        printf("\n");
    }
    DSV_INFO(("freeing decoder"));
    dsv_dec_free(&dec);
    if (meta) {
        dsv_free(meta);
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

    if (encoding) {
        return encode();
    }

    return decode();
}

static int
split_paths(int argc, char **argv)
{
    dsv_set_log_level(DSV_LEVEL_WARNING);

    if (argc < 2) {
        goto badarg;
    }
    if (argv[1][0] == 'e') {
        encoding = 1;
        return startup(argc - 1, argv + 1);
    }
    if (argv[1][0] == 'd') {
        encoding = 0;
        return startup(argc - 1, argv + 1);
    }
badarg:
    usage_general();
    return EXIT_SUCCESS;
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

    ret = split_paths(argc, argv);

    dsv_memory_report();
    return ret;
}
