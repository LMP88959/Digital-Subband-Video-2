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

/* B. Bitstream */

extern void
dsv_bs_init(DSV_BS *s, uint8_t *buffer)
{
    s->start = buffer;
    s->pos = 0;
}

extern void
dsv_bs_align(DSV_BS *s)
{
    if (dsv_bs_aligned(s)) {
        return; /* already aligned */
    }
    s->pos = ((s->pos + 7) & ((unsigned) (~0) << 3)); /* byte align */
}

extern void
dsv_bs_concat(DSV_BS *s, uint8_t *data, int len)
{
    if (!dsv_bs_aligned(s)) {
        DSV_ERROR(("concat to unaligned bs"));
    }
    if (len == 0) {
        return;
    }
    memcpy(s->start + dsv_bs_ptr(s), data, len);
    s->pos += len * 8;
}

/* static versions to possibly make the compiler more likely to inline */
static void
local_put_bit(DSV_BS *s, int v)
{
    if (v) {
        s->start[dsv_bs_ptr(s)] |= 1 << (7 - (s->pos & 7));
    }
    s->pos++;
}

static void
local_put_one(DSV_BS *s)
{
    s->start[dsv_bs_ptr(s)] |= 1 << (7 - (s->pos & 7));
    s->pos++;
}

static unsigned
local_get_bit(DSV_BS *s)
{
    unsigned out;

    out = s->start[dsv_bs_ptr(s)] >> (7 - (s->pos & 7));
    s->pos++;

    return out & 1;
}

static void
local_put_bits(DSV_BS *s, unsigned n, unsigned v)
{
    unsigned rem, bit;
    uint8_t data;

    while (n > 0) {
        rem = 8 - (s->pos & 7);
        rem = MIN(n, rem);
        bit = (7 - (s->pos & 7)) - rem + 1;
        data = (v >> (n - rem)) & ((1 << rem) - 1);
        s->start[dsv_bs_ptr(s)] |= data << bit;
        n -= rem;
        s->pos += rem;
    }
}

extern void
dsv_bs_put_bit(DSV_BS *s, int v)
{
    local_put_bit(s, v);
}

extern unsigned
dsv_bs_get_bit(DSV_BS *s)
{
    return local_get_bit(s);
}

extern void
dsv_bs_put_bits(DSV_BS *s, unsigned n, unsigned v)
{
    local_put_bits(s, n, v);
}

extern unsigned
dsv_bs_get_bits(DSV_BS *s, unsigned n)
{
    unsigned rem, bit, out = 0;

    while (n > 0) {
        rem = 8 - (s->pos & 7);
        rem = MIN(n, rem);
        bit = (7 - (s->pos & 7)) - rem + 1;
        out <<= rem;
        out |= (s->start[dsv_bs_ptr(s)] & (((1 << rem) - 1) << bit)) >> bit;
        n -= rem;
        s->pos += rem;
    }
    return out;
}

/* B. Encoding Type: unsigned interleaved exp-Golomb code (UEG) */
extern void
dsv_bs_put_ueg(DSV_BS *s, unsigned v)
{
    int i, n_bits;
    unsigned x;

    v++;
    x = v;
    for (n_bits = -1; x; n_bits++) {
        x >>= 1;
    }
    for (i = 0; i < n_bits; i++) {
        s->pos++; /* equivalent to putting a zero, assuming buffer was clear */
        local_put_bit(s, v & (1 << (n_bits - 1 - i)));
    }
    local_put_one(s);
}

/* B. Encoding Type: unsigned interleaved exp-Golomb code (UEG) */
extern unsigned
dsv_bs_get_ueg(DSV_BS *s)
{
    unsigned v = 1;

    while (!local_get_bit(s)) {
        v = (v << 1) | local_get_bit(s);
    }
    return v - 1;
}

/* B. Encoding Type: signed interleaved exp-Golomb code (SEG) */
extern void
dsv_bs_put_seg(DSV_BS *bs, int v)
{
    int s;

    if (v < 0) {
        s = 1;
        v = -v;
    } else {
        s = 0;
    }
    dsv_bs_put_ueg(bs, v);
    if (v) {
        local_put_bit(bs, s);
    }
}

/* B. Encoding Type: signed interleaved exp-Golomb code (SEG) */
extern int
dsv_bs_get_seg(DSV_BS *s)
{
    int v;

    v = dsv_bs_get_ueg(s);
    if (v && local_get_bit(s)) {
        return -v;
    }
    return v;
}

/* B. Encoding Type: non-zero interleaved exp-Golomb code (NEG) */
extern void
dsv_bs_put_neg(DSV_BS *bs, int v)
{
    int s;

    if (v < 0) {
        s = 1;
        v = -v;
    } else {
        s = 0;
    }
    dsv_bs_put_ueg(bs, v - 1);
    if (v) {
        local_put_bit(bs, s);
    }
}

/* B. Encoding Type: non-zero interleaved exp-Golomb code (NEG) */
extern int
dsv_bs_get_neg(DSV_BS *bs)
{
    int v;

    v = dsv_bs_get_ueg(bs) + 1;
    if (v && local_get_bit(bs)) {
        return -v;
    }
    return v;
}

/* B. Encoding Format: Zero Bit Run-Length Encoding (ZBRLE) */
extern void
dsv_bs_init_rle(DSV_ZBRLE *rle, uint8_t *buf)
{
    memset(rle, 0, sizeof(*rle));
    dsv_bs_init(&rle->bs, buf);
}

/* B. Encoding Format: Zero Bit Run-Length Encoding (ZBRLE) */
extern int
dsv_bs_end_rle(DSV_ZBRLE *rle, int read)
{
    if (read) {
        if (rle->nz > 1) { /* early termination */
            DSV_ERROR(("%d remaining in run", rle->nz));
        }
        return 0;
    }
    dsv_bs_put_ueg(&rle->bs, rle->nz);
    rle->nz = 0;
    dsv_bs_align(&rle->bs);
    return dsv_bs_ptr(&rle->bs);
}

/* B. Encoding Format: Zero Bit Run-Length Encoding (ZBRLE) */
extern void
dsv_bs_put_rle(DSV_ZBRLE *rle, int b)
{
    if (b) {
        dsv_bs_put_ueg(&rle->bs, rle->nz);
        rle->nz = 0;
        return;
    }
    rle->nz++;
}

/* B. Encoding Format: Zero Bit Run-Length Encoding (ZBRLE) */
extern int
dsv_bs_get_rle(DSV_ZBRLE *rle)
{
    if (rle->nz == 0) {
        rle->nz = dsv_bs_get_ueg(&rle->bs);
        return (rle->nz == 0);
    }
    rle->nz--;
    return (rle->nz == 0);
}
