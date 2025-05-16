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

#include "dsv_internal.h"

static DSV_FRAME *
alloc_frame(void)
{
    DSV_FRAME *frame;

    frame = dsv_alloc(sizeof(*frame));
    frame->refcount = 1;
    return frame;
}

extern void
dsv_mk_coefs(DSV_COEFS *c, int format, int width, int height)
{
    int h_shift, v_shift;
    int chroma_width;
    int chroma_height;
    int c0len, c1len, c2len;

    h_shift = DSV_FORMAT_H_SHIFT(format);
    v_shift = DSV_FORMAT_V_SHIFT(format);
    chroma_width = DSV_ROUND_SHIFT(width, h_shift);
    chroma_height = DSV_ROUND_SHIFT(height, v_shift);
    chroma_width = DSV_ROUND_POW2(chroma_width, 1);
    chroma_height = DSV_ROUND_POW2(chroma_height, 1);
    c[0].width = width;
    c[0].height = height;

    c0len = c[0].width * c[0].height;

    c[1].width = chroma_width;
    c[1].height = chroma_height;

    c1len = c[1].width * c[1].height;

    c[2].width = chroma_width;
    c[2].height = chroma_height;

    c2len = c[2].width * c[2].height;
    c[0].data = dsv_alloc((c0len + c1len + c2len) * sizeof(DSV_SBC));
    c[1].data = c[0].data + c0len;
    c[2].data = c[0].data + c0len + c1len;
}

extern DSV_FRAME *
dsv_mk_frame(int format, int width, int height, int border)
{
    DSV_FRAME *f = alloc_frame();
    int h_shift, v_shift;
    int chroma_width;
    int chroma_height;
    int ext = 0;

    f->format = format;
    f->width = width;
    f->height = height;
    f->border = !!border;

    if (f->border) {
        ext = DSV_FRAME_BORDER;
    }

    h_shift = DSV_FORMAT_H_SHIFT(format);
    v_shift = DSV_FORMAT_V_SHIFT(format);
    chroma_width = DSV_ROUND_SHIFT(width, h_shift);
    chroma_height = DSV_ROUND_SHIFT(height, v_shift);

    f->planes[0].format = format;
    f->planes[0].w = width;
    f->planes[0].h = height;
    f->planes[0].stride = DSV_ROUND_POW2((width + ext * 2), 4);

    f->planes[0].len = f->planes[0].stride * (f->planes[0].h + ext * 2);

    f->planes[1].format = format;
    f->planes[1].w = chroma_width;
    f->planes[1].h = chroma_height;
    f->planes[1].stride = DSV_ROUND_POW2((chroma_width + ext * 2), 4);

    f->planes[1].len = f->planes[1].stride * (f->planes[1].h + ext * 2);

    f->planes[2].format = format;
    f->planes[2].w = chroma_width;
    f->planes[2].h = chroma_height;
    f->planes[2].stride = DSV_ROUND_POW2((chroma_width + ext * 2), 4);

    f->planes[2].len = f->planes[2].stride * (f->planes[2].h + ext * 2);

    f->alloc = dsv_alloc(f->planes[0].len + f->planes[1].len + f->planes[2].len);

    f->planes[0].data = f->alloc + f->planes[0].stride * ext + ext;
    f->planes[1].data = f->alloc + f->planes[0].len + f->planes[1].stride * ext + ext;
    f->planes[2].data = f->alloc + f->planes[0].len + f->planes[1].len + f->planes[2].stride * ext + ext;

    return f;
}

extern DSV_FRAME *
dsv_load_planar_frame(int format, void *data, int width, int height)
{
    DSV_FRAME *f = alloc_frame();
    int hs = 0, vs = 0;
    f->format = format;
    hs = DSV_FORMAT_H_SHIFT(format);
    vs = DSV_FORMAT_V_SHIFT(format);

    f->width = width;
    f->height = height;

    f->planes[0].format = f->format;
    f->planes[0].w = width;
    f->planes[0].h = height;
    f->planes[0].stride = width;
    f->planes[0].data = data;
    f->planes[0].len = f->planes[0].stride * f->planes[0].h;

    width = DSV_ROUND_SHIFT(width, hs);
    height = DSV_ROUND_SHIFT(height, vs);

    f->planes[1].format = f->format;
    f->planes[1].w = width;
    f->planes[1].h = height;
    f->planes[1].stride = f->planes[1].w;
    f->planes[1].len = f->planes[1].stride * f->planes[1].h;
    f->planes[1].data = f->planes[0].data + f->planes[0].len;

    f->planes[2].format = f->format;
    f->planes[2].w = width;
    f->planes[2].h = height;
    f->planes[2].stride = f->planes[2].w;
    f->planes[2].len = f->planes[2].stride * f->planes[2].h;
    f->planes[2].data = f->planes[1].data + f->planes[1].len;
    return f;
}

extern DSV_FRAME *
dsv_clone_frame(DSV_FRAME *s, int border)
{
    DSV_FRAME *d;

    d = dsv_mk_frame(s->format, s->width, s->height, border);
    dsv_frame_copy(d, s);
    return d;
}

extern DSV_FRAME *
dsv_frame_ref_inc(DSV_FRAME *frame)
{
    DSV_ASSERT(frame && frame->refcount > 0);
    frame->refcount++;
    return frame;
}

extern void
dsv_frame_ref_dec(DSV_FRAME *frame)
{
    DSV_ASSERT(frame && frame->refcount > 0);

    frame->refcount--;
    if (frame->refcount == 0) {
        if (frame->alloc) {
            dsv_free(frame->alloc);
        }
        dsv_free(frame);
    }
}

extern void
dsv_frame_copy(DSV_FRAME *dst, DSV_FRAME *src)
{
    int i, c;

    for (c = 0; c < 3; c++) {
        DSV_PLANE *cs, *cd;
        uint8_t *sp, *dp;

        cs = src->planes + c;
        cd = dst->planes + c;
        sp = cs->data;
        dp = cd->data;
        for (i = 0; i < dst->planes[c].h; i++) {
            memcpy(dp, sp, src->planes[c].w);
            sp += cs->stride;
            dp += cd->stride;
        }
    }
    if (dst->border) {
        dsv_extend_frame(dst);
    }
}

/* 2x downsample luma plane, simple averaging */
extern void
dsv_ds2x_frame_luma(DSV_FRAME *dst, DSV_FRAME *src)
{
    int i, j;
    DSV_PLANE *s = src->planes + 0;
    DSV_PLANE *d = dst->planes + 0;

    for (j = 0; j < d->h; j++) {
        uint8_t *sp, *dp;
        int bp = 0;

        sp = DSV_GET_LINE(s, (j << 1));
        dp = DSV_GET_LINE(d, j);

        for (i = 0; i < d->w; i++) {
            int p1, p2, p3, p4;
            p1 = sp[bp];
            p2 = sp[bp + 1];
            p3 = sp[bp + s->stride];
            p4 = sp[bp + 1 + s->stride];
            dp[i] = ((p1 + p2 + p3 + p4 + 2) >> 2);
            bp += 2;
        }
    }
}

#define SUBDIV 4

#define MKHORIZ(start)       \
        p[(i + 0 + start)] + \
        p[(i + 1 + start)] + \
        p[(i + 2 + start)] + \
        p[(i + 3 + start)]

#define MKVERT(start)                 \
        p[(i + 0 + start) * stride] + \
        p[(i + 1 + start) * stride] + \
        p[(i + 2 + start) * stride] + \
        p[(i + 3 + start) * stride]

static void
downsample_strip(DSV_FRAME *frame, int plane, int pos, uint8_t *out)
{
    int i, o = 0;
    uint8_t *p;
    DSV_PLANE *pl;
    unsigned stride;
    int len, rem, sum = 0;

    pl = &frame->planes[plane];
    stride = pl->stride;

    switch (pos) {
        default:
            DSV_ASSERT(0);
            return;
        case 0:
            len = pl->h & ~(SUBDIV - 1);
            rem = pl->h & (SUBDIV - 1);

            p = pl->data + 0;
            for (i = 0; i < len; i += SUBDIV) {
#if SUBDIV == 16
                out[o++] = (MKVERT(0) + MKVERT(4) + MKVERT(8) + MKVERT(12) + 8) >> 4;
#elif SUBDIV == 8
                out[o++] = (MKVERT(0) + MKVERT(4) + 4) >> 3;
#elif SUBDIV == 4
                out[o++] = (MKVERT(0) + 2) >> 2;
#endif
            }
            if (rem) {
                p = pl->data + len * stride;
                for (i = 0; i < rem; i++) {
                    sum += p[i * stride];
                }
                out[o] = sum / rem;
            }
            break;
        case 1:
            len = pl->h & ~(SUBDIV - 1);
            rem = pl->h & (SUBDIV - 1);

            p = pl->data + (pl->w - 1);
            for (i = 0; i < len; i += SUBDIV) {
#if SUBDIV == 16
                out[o++] = (MKVERT(0) + MKVERT(4) + MKVERT(8) + MKVERT(12) + 8) >> 4;
#elif SUBDIV == 8
                out[o++] = (MKVERT(0) + MKVERT(4) + 4) >> 3;
#elif SUBDIV == 4
                out[o++] = (MKVERT(0) + 2) >> 2;
#endif
            }
            if (rem) {
                p = pl->data + (pl->w - 1) + len * stride;
                for (i = 0; i < rem; i++) {
                    sum += p[i * stride];
                }
                out[o] = sum / rem;
            }
            break;
        case 2:
            len = pl->w & ~(SUBDIV - 1);
            rem = pl->w & (SUBDIV - 1);

            p = pl->data;
            for (i = 0; i < len; i += SUBDIV) {
#if SUBDIV == 16
                out[o++] = (MKHORIZ(0) + MKHORIZ(4) + MKHORIZ(8) + MKHORIZ(12) + 8) >> 4;
#elif SUBDIV == 8
                out[o++] = (MKHORIZ(0) + MKHORIZ(4) + 4) >> 3;
#elif SUBDIV == 4
                out[o++] = (MKHORIZ(0) + 2) >> 2;
#endif
            }
            if (rem) {
                p = pl->data + len;
                for (i = 0; i < rem; i++) {
                    sum += p[i];
                }
                out[o] = sum / rem;
            }
            break;
        case 3:
            len = pl->w & ~(SUBDIV - 1);
            rem = pl->w & (SUBDIV - 1);

            p = pl->data + (pl->h - 1) * stride;
            for (i = 0; i < len; i += SUBDIV) {
#if SUBDIV == 16
                out[o++] = (MKHORIZ(0) + MKHORIZ(4) + MKHORIZ(8) + MKHORIZ(12) + 8) >> 4;
#elif SUBDIV == 8
                out[o++] = (MKHORIZ(0) + MKHORIZ(4) + 4) >> 3;
#elif SUBDIV == 4
                out[o++] = (MKHORIZ(0) + 2) >> 2;
#endif
            }
            if (rem) {
                p = pl->data + len + (pl->h - 1) * stride;
                for (i = 0; i < rem; i++) {
                    sum += p[i];
                }
                out[o] = sum / rem;
            }
            break;
    }
}

static void
extend_plane(DSV_FRAME *frame, int p)
{
    int i, j;
    DSV_PLANE *c = frame->planes + p;
    int width = c->w;
    int height = c->h;
    int total_w = width + DSV_FRAME_BORDER * 2;
    uint8_t *dst, *line;
    uint8_t *ls, *rs, *ts, *bs; /* left, right, top, bottom strips */
    int tl, tr, bl, br; /* top left, top right, bottom left, bottom right */

    ls = dsv_alloc(4 * height / SUBDIV);
    rs = dsv_alloc(4 * height / SUBDIV);
    ts = dsv_alloc(4 * width / SUBDIV);
    bs = dsv_alloc(4 * width / SUBDIV);
    downsample_strip(frame, p, 0, ls);
    downsample_strip(frame, p, 1, rs);
    downsample_strip(frame, p, 2, ts);
    downsample_strip(frame, p, 3, bs);
    tl = (ts[0] + ls[0] + 1) >> 1;
    tr = (ts[(width / SUBDIV) - 1] + rs[0] + 1) >> 1;
    bl = (ls[(height / SUBDIV) - 1] + bs[0] + 1) >> 1;
    br = (bs[(width / SUBDIV) - 1] + rs[(height / SUBDIV) - 1] + 1) >> 1;

    for (j = 0; j < height; j++) {
        line = DSV_GET_LINE(c, j);

        memset(line - DSV_FRAME_BORDER, ls[j / SUBDIV], DSV_FRAME_BORDER);
        for (i = width; i < (width + 1); i++) {
            line[i] = line[width - 1];
        }
        memset(line + width, rs[j / SUBDIV], DSV_FRAME_BORDER);
    }
    for (j = 0; j < DSV_FRAME_BORDER; j++) {
        dst = DSV_GET_XY(c, -DSV_FRAME_BORDER, -j - 1);
        memset(dst, tl, DSV_FRAME_BORDER);
        for (i = DSV_FRAME_BORDER; i < (total_w - DSV_FRAME_BORDER); i++) {
            dst[i] = ts[(i - DSV_FRAME_BORDER) / SUBDIV];
        }
        memset(dst + total_w - DSV_FRAME_BORDER, tr, DSV_FRAME_BORDER);
        dst = DSV_GET_XY(c, -DSV_FRAME_BORDER, height + j);
        memset(dst, bl, DSV_FRAME_BORDER);
        for (i = DSV_FRAME_BORDER; i < (total_w - DSV_FRAME_BORDER); i++) {
            dst[i] = bs[(i - DSV_FRAME_BORDER) / SUBDIV];
        }
        memset(dst + total_w - DSV_FRAME_BORDER, br, DSV_FRAME_BORDER);
    }

    dsv_free(ls);
    dsv_free(rs);
    dsv_free(ts);
    dsv_free(bs);
}

extern DSV_FRAME *
dsv_extend_frame_luma(DSV_FRAME *frame)
{
    if (!frame->border || (DSV_FRAME_BORDER <= 0)) {
        return frame;
    }
    extend_plane(frame, 0);
    return frame;
}

extern DSV_FRAME *
dsv_extend_frame(DSV_FRAME *frame)
{
    int i;

    if (!frame->border || (DSV_FRAME_BORDER <= 0)) {
        return frame;
    }
    for (i = 0; i < 3; i++) {
        extend_plane(frame, i);
    }
    return frame;
}

extern void
dsv_plane_xy(DSV_FRAME *frame, DSV_PLANE *out, int c, int x, int y)
{
    DSV_PLANE *p = frame->planes + c;

    out->format = p->format;
    out->data = DSV_GET_XY(p, x, y);
    out->stride = p->stride;
    out->w = MAX(0, p->w - x);
    out->h = MAX(0, p->h - y);
}
