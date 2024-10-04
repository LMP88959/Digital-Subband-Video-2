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

#include "dsv_internal.h"

char *dsv_lvlname[DSV_LEVEL_DEBUG + 1] = {
    "NONE",
    "ERROR",
    "WARNING",
    "INFO",
    "DEBUG"
};

static int lvl = DSV_LEVEL_ERROR;

extern void
dsv_set_log_level(int level)
{
    lvl = level;
}

extern int
dsv_get_log_level(void)
{
    return lvl;
}

#if DSV_MEMORY_STATS
static unsigned allocated = 0;
static unsigned freed = 0;
static unsigned allocated_bytes = 0;
static unsigned freed_bytes = 0;
static unsigned peak_alloc = 0;

extern void *
dsv_alloc(int size)
{
    void *p;

    p = calloc(1, size + 16);
    *((int32_t *) p) = size;
    allocated++;
    allocated_bytes += size;
    if (peak_alloc < (allocated_bytes - freed_bytes)) {
        peak_alloc = (allocated_bytes - freed_bytes);
    }
    return (uint8_t *) p + 16;
}

extern void
dsv_free(void *ptr)
{
    uint8_t *p;
    freed++;
    p = ((uint8_t *) ptr) - 16;
    freed_bytes += *((int32_t *) p);
    if (peak_alloc < (allocated_bytes - freed_bytes)) {
        peak_alloc = (allocated_bytes - freed_bytes);
    }
    free(p);
}

extern void
dsv_memory_report(void)
{
    DSV_DEBUG(("n alloc: %u", allocated));
    DSV_DEBUG(("n freed: %u", freed));
    DSV_DEBUG(("alloc bytes: %u", allocated_bytes));
    DSV_DEBUG(("freed bytes: %u", freed_bytes));
    DSV_DEBUG(("bytes not freed: %d", allocated_bytes - freed_bytes));
    DSV_DEBUG(("peak alloc: %u", peak_alloc));
}
#else
extern void *
dsv_alloc(int size)
{
    return calloc(1, size);
}

extern void
dsv_free(void *ptr)
{
    free(ptr);
}

extern void
dsv_memory_report(void)
{
    DSV_DEBUG(("memory stats are disabled"));
}
#endif

extern int
dsv_yuv_write(FILE *out, int fno, DSV_PLANE *p)
{
    size_t lens[3];
    size_t offset, framesz;
    int c, y;

    if (out == NULL) {
        return -1;
    }
    if (fno < 0) {
        return -1;
    }
    lens[0] = p[0].w * p[0].h;
    lens[1] = p[1].w * p[1].h;
    lens[2] = p[2].w * p[2].h;
    framesz = lens[0] + lens[1] + lens[2];
    offset = fno * framesz;

    if (fseek(out, offset, SEEK_SET)) {
        return -1;
    }
    for (c = 0; c < 3; c++) {
        for (y = 0; y < p[c].h; y++) {
            uint8_t *line = DSV_GET_LINE(&p[c], y);
            if (fwrite(line, p[c].w, 1, out) != 1) {
                return -1;
            }
        }
    }
    return 0;
}

extern int
dsv_yuv_write_seq(FILE *out, DSV_PLANE *p)
{
    int c, y;

    if (out == NULL) {
        return -1;
    }
    for (c = 0; c < 3; c++) {
        for (y = 0; y < p[c].h; y++) {
            uint8_t *line = DSV_GET_LINE(&p[c], y);
            if (fwrite(line, p[c].w, 1, out) != 1) {
                return -1;
            }
        }
    }
    return 0;
}

extern int
dsv_yuv_read(FILE *in, int fno, uint8_t *o, int width, int height, int subsamp)
{
    size_t npix, offset, chrsz = 0;

    if (in == NULL) {
        return -1;
    }
    if (fno < 0) {
        return -1;
    }

    npix = width * height;

    /* special case, UYVY 4:2:2 */
    if (subsamp == DSV_SUBSAMP_UYVY) {
        uint8_t *y = o;
        uint8_t *u = o + width * height;
        uint8_t *v = u + (width / 2) * height;
        uint8_t *tline;
        int i, j;
        unsigned linebytes = width * 2;

        tline = dsv_alloc(linebytes);
        offset = fno * npix * 2;
        if (fseek(in, offset, SEEK_SET)) {
            return -1;
        }
        for (j = 0; j < height; j++) {
            uint8_t *tlp = tline;
            if (fread(tline, 1, linebytes, in) != linebytes) {
                return -1;
            }
            for (i = 0; i < (width / 2); i++) {
                *u++ = *tlp++;
                *y++ = *tlp++;
                *v++ = *tlp++;
                *y++ = *tlp++;
            }
        }
        dsv_free(tline);

        return 0;
    }

    switch (subsamp) {
        case DSV_SUBSAMP_444:
            offset = fno * npix * 3;
            chrsz = npix;
            break;
        case DSV_SUBSAMP_422:
            offset = fno * npix * 2;
            chrsz = (width / 2) * height;
            break;
        case DSV_SUBSAMP_420:
        case DSV_SUBSAMP_411:
            offset = fno * npix * 3 / 2;
            chrsz = npix / 4;
            break;
        default:
            DSV_ERROR(("unsupported format"));
            DSV_ASSERT(0);
            break;
    }

    if (fseek(in, offset, SEEK_SET)) {
        return -1;
    }
    if (fread(o, 1, npix + chrsz + chrsz, in) != (npix + chrsz + chrsz)) {
        return -1;
    }
    return 0;
}

extern int
dsv_yuv_read_seq(FILE *in, uint8_t *o, int width, int height, int subsamp)
{
    size_t npix, chrsz = 0;

    if (in == NULL) {
        return -1;
    }
    npix = width * height;
    switch (subsamp) {
        case DSV_SUBSAMP_444:
            chrsz = npix;
            break;
        case DSV_SUBSAMP_422:
            chrsz = (width / 2) * height;
            break;
        case DSV_SUBSAMP_420:
        case DSV_SUBSAMP_411:
            chrsz = npix / 4;
            break;
        default:
            DSV_ERROR(("unsupported format"));
            DSV_ASSERT(0);
            break;
    }
    if (fread(o, 1, npix + chrsz + chrsz, in) != (npix + chrsz + chrsz)) {
        return -1;
    }
    return 0;
}

extern void
dsv_buf_free(DSV_BUF *buf)
{
    if (buf->data) {
        dsv_free(buf->data);
        buf->data = NULL;
    }
}

extern void
dsv_mk_buf(DSV_BUF *buf, int size)
{
    memset(buf, 0, sizeof(*buf));
    buf->data = dsv_alloc(size);
    buf->len = size;
}

static int
pred(int left, int top, int topleft)
{
    int dif = left + top - topleft;
    if (abs(dif - left) < abs(dif - top)) {
        return left;
    }
    return top;
}

/* B.2.3.4 Motion Data - Motion Vector Prediction */
extern void
dsv_movec_pred(DSV_MV *vecs, DSV_PARAMS *p, int x, int y, int *px, int *py)
{
    DSV_MV *mv;
    int vx[3] = { 0, 0, 0 };
    int vy[3] = { 0, 0, 0 };

    if (x > 0) { /* left */
        mv = (vecs + y * p->nblocks_h + (x - 1));
        if (mv->mode == DSV_MODE_INTER) {
            vx[0] = mv->u.mv.x;
            vy[0] = mv->u.mv.y;
        }
    }
    if (y > 0) { /* top */
        mv = (vecs + (y - 1) * p->nblocks_h + x);
        if (mv->mode == DSV_MODE_INTER) {
            vx[1] = mv->u.mv.x;
            vy[1] = mv->u.mv.y;
        }
    }
    if (x > 0 && y > 0) { /* top-left */
        mv = (vecs + (y - 1) * p->nblocks_h + (x - 1));
        if (mv->mode == DSV_MODE_INTER) {
            vx[2] = mv->u.mv.x;
            vy[2] = mv->u.mv.y;
        }
    }

    *px = pred(vx[0], vx[1], vx[2]);
    *py = pred(vy[0], vy[1], vy[2]);
}

extern int
dsv_lb2(unsigned n)
{
    unsigned i = 1, log2 = 0;

    while (i < n) {
        i <<= 1;
        log2++;
    }
    return log2;
}
