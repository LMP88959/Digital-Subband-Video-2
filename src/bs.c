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
dsv_bs_init(DSV_BS *bs, uint8_t *buffer)
{
    bs->start = buffer;
    bs->pos = 0;
}

extern void
dsv_bs_align(DSV_BS *bs)
{
    if (dsv_bs_aligned(bs)) {
        return; /* already aligned */
    }
    bs->pos = ((bs->pos + 7) & ((unsigned) (~0) << 3)); /* byte align */
}

extern void
dsv_bs_concat(DSV_BS *bs, uint8_t *data, int len)
{
    if (!dsv_bs_aligned(bs)) {
        DSV_ERROR(("concat to unaligned bs"));
    }
    if (len == 0) {
        return;
    }
    memcpy(bs->start + dsv_bs_ptr(bs), data, len);
    bs->pos += len * 8;
}

/* static versions to possibly make the compiler more likely to inline */
static void
local_put_bit(DSV_BS *bs, int v)
{
    if (v) {
        bs->start[dsv_bs_ptr(bs)] |= 1 << (7 - (bs->pos & 7));
    }
    bs->pos++;
}

static void
local_put_one(DSV_BS *bs)
{
    bs->start[dsv_bs_ptr(bs)] |= 1 << (7 - (bs->pos & 7));
    bs->pos++;
}

static unsigned
local_get_bit(DSV_BS *bs)
{
    unsigned out;

    out = bs->start[dsv_bs_ptr(bs)] >> (7 - (bs->pos & 7));
    bs->pos++;

    return out & 1;
}

static void
local_put_bits(DSV_BS *bs, unsigned n, unsigned v)
{
    unsigned rem, bit;
    uint8_t data;

    while (n > 0) {
        rem = 8 - (bs->pos & 7);
        rem = MIN(n, rem);
        bit = (7 - (bs->pos & 7)) - rem + 1;
        data = (v >> (n - rem)) & ((1 << rem) - 1);
        bs->start[dsv_bs_ptr(bs)] |= data << bit;
        n -= rem;
        bs->pos += rem;
    }
}

extern void
dsv_bs_put_bit(DSV_BS *bs, int v)
{
    local_put_bit(bs, v);
}

extern unsigned
dsv_bs_get_bit(DSV_BS *bs)
{
    return local_get_bit(bs);
}

extern void
dsv_bs_put_bits(DSV_BS *bs, unsigned n, unsigned v)
{
    local_put_bits(bs, n, v);
}

extern unsigned
dsv_bs_get_bits(DSV_BS *bs, unsigned n)
{
    unsigned rem, bit, out = 0;

    while (n > 0) {
        rem = 8 - (bs->pos & 7);
        rem = MIN(n, rem);
        bit = (7 - (bs->pos & 7)) - rem + 1;
        out <<= rem;
        out |= (bs->start[dsv_bs_ptr(bs)] & (((1 << rem) - 1) << bit)) >> bit;
        n -= rem;
        bs->pos += rem;
    }
    return out;
}

/* B. Encoding Type: unsigned interleaved exp-Golomb code (UEG) */
extern void
dsv_bs_put_ueg(DSV_BS *bs, unsigned v)
{
    int i, n_bits;
    unsigned x;

    v++;
    x = v;
    for (n_bits = -1; x; n_bits++) {
        x >>= 1;
    }
    for (i = 0; i < n_bits; i++) {
        bs->pos++; /* equivalent to putting a zero, assuming buffer was clear */
        local_put_bit(bs, v & (1 << (n_bits - 1 - i)));
    }
    local_put_one(bs);
}

/* B. Encoding Type: unsigned interleaved exp-Golomb code (UEG) */
extern unsigned
dsv_bs_get_ueg(DSV_BS *bs)
{
    unsigned v = 1;

    while (!local_get_bit(bs)) {
        v = (v << 1) | local_get_bit(bs);
    }
    return v - 1;
}

static unsigned
s2u(int v)
{
    int uv = -2 * v - 1;
    return (unsigned) (uv ^ (uv >> (sizeof(int) * CHAR_BIT - 1)));
}

static int
u2s(unsigned uv)
{
    int v = (int) (uv + (unsigned) 1);
    return v & 1 ? v >> 1 : -(v >> 1);
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
dsv_bs_get_seg(DSV_BS *bs)
{
    int v;

    v = dsv_bs_get_ueg(bs);
    if (v && local_get_bit(bs)) {
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

/* B. Encoding Type: adaptive Rice code (URC) */
extern void
dsv_bs_put_rice(DSV_BS *bs, unsigned v, int *rk, int damp)
{
    unsigned k, q;

    k = (*rk) >> damp;
    q = v >> k;
    if (q) {
        (*rk)++;
    } else if ((*rk) > 0) {
        (*rk)--;
    }
    bs->pos += q; /* equivalent to putting 'q' zeroes, assuming buffer was clear */
    local_put_one(bs);
    local_put_bits(bs, k, v);
}

/* B. Encoding Type: adaptive Rice code (URC) */
extern unsigned
dsv_bs_get_rice(DSV_BS *bs, int *rk, int damp)
{
    int k = (*rk) >> damp;
    unsigned q = 0;
    while (!local_get_bit(bs)) {
        q++;
    }
    if (q) {
        (*rk)++;
    } else if ((*rk) > 0) {
        (*rk)--;
    }
    return (q << k) | dsv_bs_get_bits(bs, k);
}

/* B. Encoding Type: non-zero adaptive Rice code (NRC) */
extern void
dsv_bs_put_nrice(DSV_BS *bs, int v, int *rk, int damp)
{
    dsv_bs_put_rice(bs, s2u(v) - 1, rk, damp);
}

/* B. Encoding Type: non-zero adaptive Rice code (NRC) */
extern int
dsv_bs_get_nrice(DSV_BS *bs, int *rk, int damp)
{
    return u2s(dsv_bs_get_rice(bs, rk, damp) + 1);
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
