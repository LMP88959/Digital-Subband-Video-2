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

#include "dsv.h"
#include "dsv_encoder.h"
#include "dsv_decoder.h"
#include "util.h"

#include <errno.h>

/*
 *
 * effort:
 *
 *   0  : [UNUSED]
 *   1  : [UNUSED]
 *   2  : diagonal full-pel estimation
 *   3  : [UNUSED]
 *   4  : hpel
 *   5  : detail intra test
 *   6  : cleanup intra test
 *   7  : metadata stats
 *   8  : low-mo qpel
 *   9  : full qpel
 *   10 : full subpel
 *
 */

#define DRV_HEADER "Envel Graphics DSV v2.%d codec by EMMIR 2024. "\
                   "encoder v%d. "  \
                   "decoder v%d.\n", \
                    DSV_VERSION_MINOR, DSV_ENCODER_VERSION, DSV_DECODER_VERSION

static int encoding = 0;
static char *progname = NULL;
static int dooverwrite = 1;
static int verbose = 0;

#define INP_FMT_444  0
#define INP_FMT_422  1
#define INP_FMT_420  2
#define INP_FMT_411  3
#define INP_FMT_UYVY 4

#define RC_ABR 0
#define RC_CRF 1

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
rc_to_rc(int v)
{
    switch (v) {
        case RC_ABR:
            return DSV_RATE_CONTROL_ABR;
        case RC_CRF:
            return DSV_RATE_CONTROL_CRF;
    }
    return DSV_RATE_CONTROL_ABR;
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
    }
    return DSV_SUBSAMP_420;
}

struct PARAM {
   char *prefix;
   int value;
   int min, max;
   int (*convert)(int);
   char *desc;
};

static struct PARAM enc_params[] = {
    { "qp", DSV_USER_QUAL_TO_RC_QUAL(-1), -1, 100, pct_to_qual,
            "quality percent. If -1 and ABR mode, estimate good qp for desired bitrate. If -1 and CRF mode, default to 85. -1 = default" },
    { "effort", DSV_MAX_EFFORT - 1, DSV_MIN_EFFORT, DSV_MAX_EFFORT, NULL,
            "encoder effort. 0 = least effort, 10 = most effort. higher value -> better video, slower encoding. default = 10" },
    { "w", 352, 16, (1 << 24), NULL,
            "width of input video. 352 = default" },
    { "h", 288, 16, (1 << 24), NULL,
            "height of input video. 288 = default" },
    { "gop", 12, 0, DSV_GOP_INF, NULL,
            "Group Of Pictures length. 0 = intra frames only, 12 = default" },
    { "fmt", DSV_SUBSAMP_420, 0, 4, fmt_to_subsamp,
            "chroma subsampling format of input video. 0 = 4:4:4, 1 = 4:2:2, 2 = 4:2:0, 3 = 4:1:1, 4 = 4:2:2 UYVY, 2 = default" },
    { "nfr", -1, -1, INT_MAX, NULL,
            "number of frames to compress. -1 means as many as possible. -1 = default" },
    { "sfr", 0, 0, INT_MAX, NULL,
            "frame number to start compressing at. 0 = default" },
    { "fps_num", 30, 1, (1 << 24), NULL,
            "fps numerator of input video. 30 = default" },
    { "fps_den", 1, 1, (1 << 24), NULL,
            "fps denominator of input video. 1 = default" },
    { "aspect_num", 1, 1, (1 << 24), NULL,
            "aspect ratio numerator of input video. 1 = default" },
    { "aspect_den", 1, 1, (1 << 24), NULL,
            "aspect ratio denominator of input video. 1 = default" },
    { "ipct", 67, 0, 100, NULL,
            "percentage threshold of intra blocks in an inter frame after which it is simply made into an intra frame. 50 = default" },
    { "pyrlevels", 0, 0, DSV_MAX_PYRAMID_LEVELS, NULL,
            "number of pyramid levels to use in hierarchical motion estimation. 0 means auto-determine. 0 = default" },
    { "rc_mode", DSV_RATE_CONTROL_ABR, RC_CRF, RC_ABR, rc_to_rc,
            "rate control mode. 0 = single pass average bitrate (ABR), 1 = constant rate factor (CRF). 0 = default" },
    { "kbps", AUTO_BITRATE, AUTO_BITRATE, INT_MAX, to_bps,
            "ONLY FOR ABR RATE CONTROL: bitrate in kilobits per second. 0 = auto-estimate needed bitrate for desired qp. 0 = default" },
    { "maxqstep", DSV_USER_QUAL_TO_RC_QUAL(1) / 4, 1, DSV_RC_QUAL_MAX, NULL,
            "max quality step for ABR, absolute quant amount in range [1, 400]. 1 = default (0.25%)" },
    { "minqp", DSV_USER_QUAL_TO_RC_QUAL(0), 0, 100, pct_to_qual,
            "minimum quality. 0 = default" },
    { "maxqp", DSV_USER_QUAL_TO_RC_QUAL(100), 0, 100, pct_to_qual,
            "maximum quality. 100 = default" },
    { "iminqp", DSV_USER_QUAL_TO_RC_QUAL(5), 0, 100, pct_to_qual,
            "minimum quality for intra frames. 20 = default" },
    { "stabref", 0, 0, INT_MAX, NULL,
            "period (in # of frames) to refresh the stability block tracking. 0 = auto-determine. 0 = default" },
    { "scd", 1, 0, 1, NULL,
            "do scene change detection. 1 = default" },
    { "tempaq", 1, 0, 1, NULL,
            "do temporal adaptive quantization. If disabled, spatial methods will be used instead. 1 = default" },
    { "bszx", -1, -1, 1, NULL,
            "override block sizes in the x (horizontal) direction. -1 = auto-determine. -1 = default. 0 = 16, 1 = 32" },
    { "bszy", -1, -1, 1, NULL,
            "override block sizes in the y (vertical) direction. -1 = auto-determine. -1 = default. 0 = 16, 1 = 32" },
    { "scpct", 55, 0, 100, NULL,
            "scene change percentage. 55 = default" },
    { "skipthresh", 0, -1, INT_MAX, NULL,
           "skip block threshold. -1 = disable. 0 = default, larger value means more likely to mark a block as skipped." },
    { "varint", 1, 0, 1, NULL,
            "intra frames that are created outside of the normal GOP cycle reset the GOP cycle if 1. 1 = default" },
    { "psy", 1, 0, 1, NULL,
           "enable/disable psychovisual optimizations. 1 = default" },
    { "y4m", 0, 0, 1, NULL,
            "set to 1 if input is in Y4M format, 0 if raw YUV. 0 = default" },
    { NULL, 0, 0, 0, NULL, "" }
};

static struct PARAM dec_params[] = {
    { "out420p", 0, 0, 1, NULL,
            "convert video to 4:2:0 chroma subsampling before saving output. 0 = default" },
    { "drawinfo", 0, 0, (DSV_DRAW_STABHQ | DSV_DRAW_MOVECS | DSV_DRAW_IBLOCK), NULL,
            "draw debugging information on the decoded frames (bit OR together to get multiple at the same time):\n\t\t1 = draw stability info\n\t\t2 = draw motion vectors\n\t\t4 = draw intra subblocks. 0 = default" },
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
usage_general(void)
{
    char *p = progname;

    printf(DRV_HEADER);
    printf("usage: %s <e|d> [options]\n", p);
    printf("for more information about running the encoder: %s e help\n", p);
    printf("for more information about running the decoder: %s d help\n", p);
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
usage_encoder(void)
{
    char *p = progname;

    printf(DRV_HEADER);
    printf("usage: %s e [options]\n", p);
    printf("sample usage: %s e -inp=video.yuv -out=compressed.dsv -w=352 -h=288 -fps_num=24 -fps_den=1 -qp=85 -gop=15\n", p);
    print_params(enc_params);
}

static void
usage_decoder(void)
{
    char *p = progname;

    printf(DRV_HEADER);
    printf("usage: %s d [options]\n", p);
    printf("sample usage: %s d -inp=video.dsv -out=decompressed.yuv -out420p=1\n", p);
    print_params(dec_params);
}

static void
usage(void)
{
    if (encoding) {
        usage_encoder();
    } else {
        usage_decoder();
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
    int ret = 1;

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
        ret = 0;
    }

    if (n[0] != USE_STDIO_CHAR) {
        fclose(fp);
    }
    return ret;
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

        if (!dsv_y4m_read_hdr(inpfile, &md.width, &md.height, &md.subsamp, fr, asp)) {
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

    enc.scene_change_delta = get_optval(enc_params, "scpct");
    enc.do_scd = get_optval(enc_params, "scd");
    enc.intra_pct_thresh = get_optval(enc_params, "ipct");
    enc.quality = get_optval(enc_params, "qp");
    enc.skip_block_thresh = get_optval(enc_params, "skipthresh");

    enc.rc_mode = get_optval(enc_params, "rc_mode");
    spec_bps = get_optval(enc_params, "kbps");
    if (enc.quality == DSV_USER_QUAL_TO_RC_QUAL(-1)) {
        int qual;

        if (enc.rc_mode == DSV_RATE_CONTROL_CRF || spec_bps == AUTO_BITRATE) {
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
    enc.max_q_step = get_optval(enc_params, "maxqstep");
    enc.min_quality = get_optval(enc_params, "minqp");
    enc.max_quality = get_optval(enc_params, "maxqp");
    enc.min_I_frame_quality = get_optval(enc_params, "iminqp");

    enc.pyramid_levels = get_optval(enc_params, "pyrlevels");
    enc.stable_refresh = get_optval(enc_params, "stabref");
    if (enc.stable_refresh == 0) {
        enc.stable_refresh = CLAMP(enc.gop, 1, 14);
    }
    enc.do_temporal_aq = get_optval(enc_params, "tempaq");
    enc.variable_i_interval = get_optval(enc_params, "varint");
    enc.block_size_override_x = get_optval(enc_params, "bszx");
    enc.block_size_override_y = get_optval(enc_params, "bszy");
    enc.effort = get_optval(enc_params, "effort");
    enc.do_psy = get_optval(enc_params, "psy");

    frno = get_optval(enc_params, "sfr");
    nfr = get_optval(enc_params, "nfr");
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
        if (maxframe > 0 && frno >= (unsigned) maxframe) {
            goto end_of_stream;
        }
        if (y4m_in) {
            if (dsv_y4m_read_seq(inpfile, picture, w, h, md.subsamp) < 0) {
                DSV_ERROR(("failed to read frame %d", frno));
                goto end_of_stream;
            }
        } else {
            if (opts.inp[0] == USE_STDIO_CHAR) {
                if (dsv_yuv_read_seq(inpfile, picture, w, h, md.subsamp) < 0) {
                    DSV_ERROR(("failed to read frame %d", frno));
                    goto end_of_stream;
                }
            } else {
                if (dsv_yuv_read(inpfile, frno, picture, w, h, md.subsamp) < 0) {
                    DSV_ERROR(("failed to read frame %d", frno));
                    goto end_of_stream;
                }
            }
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
        dsv_enc_end_of_stream(&enc, bufs);
        savebuffer(&bufs[0]);
        dsv_buf_free(&bufs[0]);
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
        printf("target bitrate = %d bps  %d KBps  %d kbps\n",
                enc.bitrate, enc.bitrate / (8 * 1024), enc.bitrate / 1024);
    }

    writefile(opts.out, enc_buf, bufsz);
    if (verbose) {
        printf("saved video file\n");
    }
    dsv_enc_free(&enc);

    if (opts.inp[0] != USE_STDIO_CHAR) {
        fclose(inpfile);
    }
    return EXIT_SUCCESS;
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
    /* DSV_INFO(("DSV version 1.%d", hdr[4])); */
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
    int code;
    DSV_FNUM frameno = 0;
    int to_420p;
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
                if (dsv_yuv_write(outfile, frameno, f420->planes) < 0) {
                    DSV_ERROR(("failed to write frame %d", frameno));
                }
                dsv_frame_ref_dec(f420);
            } else {
                if (dsv_yuv_write(outfile, frameno, frame->planes) < 0) {
                    DSV_ERROR(("failed to write frame %d", frameno));
                }
            }
            if (verbose) {
                printf("\rdecoded frame %d", frameno);
                fflush(stdout);
            }
            dsv_frame_ref_dec(frame);
        }
    }
    if (verbose) {
        printf("\n");
    }
    DSV_INFO(("freeing decoder"));
    dsv_dec_free(&dec);
    dsv_free(meta);

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
    int ret;

    progname = argv[0];
    /* default to stdin/out */
    opts.inp = "-";
    opts.out = "-";

    ret = split_paths(argc, argv);

    dsv_memory_report();
    return ret;
}
