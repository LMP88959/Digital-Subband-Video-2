/*****************************************************************************/
/*
 * Digital Subband Video 2.8 Single-Header Decoder Implementation
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
/*
 * This software was designed and written by EMMIR, 2024-2025 of Envel Graphics
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

#ifndef _DSV28DEC_H_
#define _DSV28DEC_H_

#ifdef __cplusplus
extern "C" {
#endif
/*
 * HOW TO INCLUDE THE DSV 2.8 DECODER IN YOUR PROGRAM:
 *
 * In one translation unit (usually .c/.cpp file), do the following:
 * #define _DSV2_IMPL_
 * #include "dsv28dec.h"
 *
 * if you need access to DSV2 types, defines, or declarations in another file,
 * all you need to do is include dsv28dec.h:
 * #include "dsv28dec.h"
 *
 *
 * OPTIONS:
 *
 * _DSV2_NO_ASSERT_ - define to remove asserts in the code
 *
 * _DSV2_NO_STDIO_ - define to omit stdio based functions, you will need to
 *                   provide your version of the following macro:
 *
 *        DSV_LOG_LVL(level, x)
 *              level - level of the log call
 *              x - printf style parameters ("format", data0, data1, etc..)
 *
 *
 * _DSV2_NO_STDINT_ - define if you don't have stdint.h available. You will
 *                    need to provide your own typedefs.
 *
 * _DSV2_NO_ALLOC_ - define to provide your own memory allocation and freeing
 *                   functions, you will need to provide your version of the
 *                   following macros:
 *
 *        DSV2_ALLOC_FUNC(num_bytes)
 *        DSV2_FREE_FUNC(pointer)
 *
 *                   !!!!! NOTE !!!!!
 *                   The alloc function MUST return a pointer to a block of
 *                   ZEROED OUT memory that is "num_bytes" in size.
 *                   If the memory returned by your alloc is not zeroed,
 *                   the behavior of this DSV2 implementation will be undefined.
 *
 *
 * _DSV2_MEMORY_STATS_ - define to enable memory counting and basic statistics
 *
 ******************************************************************************
 *
 * DECODING
 *
 * The best reference is the d28_dec_main.c program,
 * but a quick synopsis via pseudocode is given here:
 *
 * zero DSV_DECODER struct memory
 *
 * while (1) {
 *    DSV_FRAME *frame;
 *    DSV_BUF packet_buf;
 *
 *    hdr = read DSV_PACKET_HDR_SIZE bytes from source stream
 *    if (d28_mk_packet_buf(hdr, DSV_PACKET_HDR_SIZE, &buffer, &packet_type) < 0) {
 *        - packet reading error
 *        break;
 *    }
 *    read (packet_buf.len - DSV_PACKET_HDR_SIZE) bytes from source stream
 *        into packet_buf.data + DSV_PACKET_HDR_SIZE
 *
 *    code = d28_dec(packet_buf, &frame);
 *    if (code == DSV_DEC_GOT_META) {
 *        - do something with metadata
 *    } else {
 *        if (code == DSV_DEC_EOS) {
 *            - end of stream
 *            break;
 *        }
 *
 *        - do what you need to do to decoded frame
 *
 *        d28_frame_ref_dec(frame);
 *    }
 * }
 * d28_dec_free
 */

/******************************************************************************/
/******************************************************************************/
/******************************************************************************/
/******************************************************************************/
/*********************** BEGINNING OF PUBLIC INTERFACE ***********************/
/******************************************************************************/
/******************************************************************************/
/******************************************************************************/
/******************************************************************************/

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifndef _DSV2_NO_STDINT_
#include <stdint.h>
#endif
#ifndef _DSV2_NO_STDIO_
#include <stdio.h>
#endif

/* configurable */
#define DSV_PORTABLE 1

/* B.1 Packet Header */
#define DSV_FOURCC_0     'D'
#define DSV_FOURCC_1     'S'
#define DSV_FOURCC_2     'V'
#define DSV_FOURCC_3     '2'
#define DSV_VERSION_MINOR 8

/* B.1.1 Packet Type */
#define DSV_PT_META 0x00
#define DSV_PT_PIC  0x04
#define DSV_PT_EOS  0x10

#define DSV_PT_IS_PIC(x)   ((x) & DSV_PT_PIC)
#define DSV_PT_IS_REF(x)  (((x) & 0x6) == 0x6)
#define DSV_PT_HAS_REF(x)  ((x) & 0x1)

#define DSV_PACKET_HDR_SIZE (4 + 1 + 1 + 4 + 4)
#define DSV_PACKET_TYPE_OFFSET 5
#define DSV_PACKET_PREV_OFFSET 6
#define DSV_PACKET_NEXT_OFFSET 10

typedef uint32_t DSV_FNUM; /* frame number */

typedef struct {
    int width;
    int height;
    int subsamp;

    int fps_num;
    int fps_den;
    int aspect_num;
    int aspect_den;

    int inter_sharpen;

    /* 16 bits: reserved for potential future use
     * 1st bit: 0 = no reserved bits, 1 = has reserved bits
     * last 15 bits: reserved bits
     */
    int reserved;
} DSV_META;

typedef struct {
    uint8_t *data;
    int len;
    int format;
    int stride;
    int w, h;
} DSV_PLANE;

typedef struct {
    uint8_t *alloc;

    DSV_PLANE planes[3];

    int refcount;

    int format;
    int width;
    int height;

    int border;
} DSV_FRAME;

typedef struct {
    DSV_META *vidmeta;

    /* block sizes */
    int blk_w;
    int blk_h;
    /* number of blocks horizontally and vertically in the image */
    int nblocks_h;
    int nblocks_v;

    int temporal_mc; /* temporal motion compensation state */
    int lossless;

    /* 16 bits: reserved for potential future use
     * (different from metadata->reserved as this is per-frame)
     *
     * 1st bit: 0 = no reserved bits, 1 = has reserved bits
     * last 15 bits: reserved bits
     */
    int reserved;
} DSV_PARAMS;

/*********************************** DECODER **********************************/

typedef struct {
    DSV_PARAMS params;
    DSV_FRAME *out_frame;
    DSV_FRAME *ref_frame;

    uint8_t *blockdata;
    int refcount;
} DSV_IMAGE;

typedef struct {
    DSV_META vidmeta;
    DSV_IMAGE *ref;
    int got_metadata;
} DSV_DECODER;

typedef struct {
    uint8_t *data;
    unsigned len;
} DSV_BUF;

#define DSV_DEC_OK        0
#define DSV_DEC_ERROR     1
#define DSV_DEC_EOS       2
#define DSV_DEC_GOT_META  3
#define DSV_DEC_NEED_NEXT 4

#define DSV_PKT_ERR_EOF -1
#define DSV_PKT_ERR_OOB -2 /* out of bytes */
#define DSV_PKT_ERR_PSZ -3 /* bad packet size */
#define DSV_PKT_ERR_4CC -4 /* bad 4cc */

extern int d28_mk_packet_buf(uint8_t *hdr, int hdrlen, DSV_BUF *rb, int *packet_type);

/* decode a buffer, returns a frame in *out and the frame number in *fn */
extern int d28_dec(DSV_DECODER *d, DSV_BUF *buf, DSV_FRAME **out, DSV_FNUM *fn);

/* get the metadata that was decoded. NOTE: if no metadata has been decoded
 * yet, the returned struct will not contain any useful values. */
extern DSV_META *d28_get_metadata(DSV_DECODER *d);

/* free anything the decoder was holding on to */
extern void d28_dec_free(DSV_DECODER *d);

#define DSV_GET_LINE(p, y) ((p)->data + (y) * (p)->stride)
#define DSV_GET_XY(p, x, y) ((p)->data + (x) + (y) * (p)->stride)

extern DSV_FRAME *d28_mk_frame(int format, int width, int height, int border);

extern DSV_FRAME *d28_frame_ref_inc(DSV_FRAME *frame);
extern void d28_frame_ref_dec(DSV_FRAME *frame);

extern void d28_frame_copy(DSV_FRAME *dst, DSV_FRAME *src);

extern DSV_FRAME *d28_clone_frame(DSV_FRAME *f, int border);
extern DSV_FRAME *d28_extend_frame(DSV_FRAME *frame);

extern void d28_mk_buf(DSV_BUF *buf, int size);
extern void d28_buf_free(DSV_BUF *buffer);

#ifndef _DSV2_NO_STDIO_
extern int d28_yuv_write(FILE *out, int fno, DSV_PLANE *p);
extern int d28_yuv_write_seq(FILE *out, DSV_PLANE *p);
#endif

extern void *d28_alloc(int size);
extern void d28_free(void *ptr);

extern void d28_memory_report(void);

#define DSV_LEVEL_NONE    0
#define DSV_LEVEL_ERROR   1
#define DSV_LEVEL_WARNING 2
#define DSV_LEVEL_INFO    3
#define DSV_LEVEL_DEBUG   4

extern char *d28_lvlname[DSV_LEVEL_DEBUG + 1];

#ifndef _DSV2_NO_STDIO_
#define DSV_LOG_LVL(level, x) \
    do { if (level <= d28_get_log_level()) { \
      printf("[DSV][%s] ", d28_lvlname[level]); \
      printf("%s: %s(%d): ", __FILE__,  __FUNCTION__, __LINE__); \
      printf x; \
      printf("\n"); \
    }} while(0)
#endif

#define DSV_ERROR(x)   DSV_LOG_LVL(DSV_LEVEL_ERROR, x)
#define DSV_WARNING(x) DSV_LOG_LVL(DSV_LEVEL_WARNING, x)
#define DSV_INFO(x)    DSV_LOG_LVL(DSV_LEVEL_INFO, x)
#define DSV_DEBUG(x)   DSV_LOG_LVL(DSV_LEVEL_DEBUG, x)

#ifndef _DSV2_NO_ASSERT_
#define DSV_ASSERT(x) do {                  \
    if (!(x)) {                             \
        DSV_ERROR(("assert: " #x));         \
        exit(-1);                           \
    }                                       \
} while(0)
#else
#define DSV_ASSERT(x)
#endif
extern void d28_set_log_level(int level);
extern int d28_get_log_level(void);

/******************************************************************************/
/******************************************************************************/
/******************************************************************************/
/******************************************************************************/
/*************************** END OF PUBLIC INTERFACE **************************/
/******************************************************************************/
/******************************************************************************/
/******************************************************************************/
/******************************************************************************/

#ifdef _DSV2_IMPL_
#ifndef _DSV2_IMPL_GUARD_
#define _DSV2_IMPL_GUARD_

/********************************** INTERNAL **********************************/

/* B.2.3 Picture Packet */
#define DSV_MIN_BLOCK_SIZE 16
#define DSV_MAX_BLOCK_SIZE 32

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(x, a, b) ((x) < (a) ? (a) : ((x) > (b) ? (b) : (x)))
#endif
#define DSV_ROUND_SHIFT(x, shift) (((x) + (1 << (shift)) - 1) >> (shift))
#define DSV_ROUND_POW2(x, pwr) (((x) + (1 << (pwr)) - 1) & ((unsigned)(~0) << (pwr)))
#define DSV_UDIV_ROUND_UP(a,b) (((a) + (b) - 1) / (b))
#define DSV_UDIV_ROUND(a,b) (((a) + ((b) / 2)) / (b))

/* portable sar - shift arithmetic right, or floordiv_pow2 */
#if DSV_PORTABLE
#define DSV_SAR(v, s) ((v) < 0 ? ~(~(v) >> (s)) : (v) >> (s))
#else
#define DSV_SAR(v, s) ((v) >> (s))
#endif

/* rounding version */
#define DSV_SAR_R(v, s) (((v) + (1 << ((s) - 1))) >> (s))

#define DSV_FMT_FULL_V 0x0
#define DSV_FMT_DIV2_V 0x1
#define DSV_FMT_DIV4_V 0x2
#define DSV_FMT_FULL_H 0x0
#define DSV_FMT_DIV2_H 0x4
#define DSV_FMT_DIV4_H 0x8

/* unsigned 8 bit per channel required, subsampling is only for chroma.
 * Only planar YUV is supported (except for UYVY)
 */
#define DSV_SUBSAMP_444  (DSV_FMT_FULL_H | DSV_FMT_FULL_V)
#define DSV_SUBSAMP_422  (DSV_FMT_DIV2_H | DSV_FMT_FULL_V)
#define DSV_SUBSAMP_UYVY (0x10 | DSV_SUBSAMP_422)
#define DSV_SUBSAMP_420  (DSV_FMT_DIV2_H | DSV_FMT_DIV2_V)
#define DSV_SUBSAMP_411  (DSV_FMT_DIV4_H | DSV_FMT_FULL_V)
#define DSV_SUBSAMP_410  (DSV_FMT_DIV4_H | DSV_FMT_DIV4_V) /* NOTE: not actual 4:1:0, actual 4:1:0 would be quarter horizontal, half vertical */

#define DSV_FORMAT_H_SHIFT(format) (((format) >> 2) & 0x3)
#define DSV_FORMAT_V_SHIFT(format) ((format) & 0x3)

/* subband coefs */
typedef int32_t DSV_SBC;
typedef struct {
    DSV_SBC *data;
    int width;
    int height;
} DSV_COEFS;

#define DSV_NDIF_THRESH   (2 * 4)

#define DSV_STABLE_STAT   0
#define DSV_MAINTAIN_STAT 1
#define DSV_RINGING_STAT  2
#define DSV_MODE_STAT     3
#define DSV_EPRM_STAT     4
#define DSV_MAX_STAT      5
#define DSV_ONE_MARKER    0 /* a one will mark the end of the RLE */
#define DSV_ZERO_MARKER   1 /* a zero will mark the end of the RLE */

/* B.2.3.4 Motion Data - Intra Sub-Block Masks */
#define DSV_MODE_INTER   0 /* whole block is inter */
#define DSV_MODE_INTRA   1 /* some or all of the block is intra */
#define DSV_MASK_INTRA00 1 /* top left is intra */
#define DSV_MASK_INTRA01 2 /* top right is intra */
#define DSV_MASK_INTRA10 4 /* bottom left is intra */
#define DSV_MASK_INTRA11 8 /* bottom right is intra */
#define DSV_MASK_ALL_INTRA (DSV_MASK_INTRA00 | DSV_MASK_INTRA01 | DSV_MASK_INTRA10 | DSV_MASK_INTRA11)

typedef struct {
    union {
        struct {
            int16_t x;
            int16_t y;
        } mv;
        int32_t all;
    } u;
#define DSV_IS_SUBPEL(v) (((v)->u.mv.x | (v)->u.mv.y) & 3)
#define DSV_IS_QPEL(v) (((v)->u.mv.x | (v)->u.mv.y) & 1)
#define DSV_IS_DIAG(v) (((v)->u.mv.x & 3) && ((v)->u.mv.y & 3))
#define DSV_TEMPORAL_MC(fno) ((fno) % 2)

#define DSV_MV_BIT_INTRA    0
#define DSV_MV_BIT_EPRM     1
#define DSV_MV_BIT_MAINTAIN 2
#define DSV_MV_BIT_SKIP     3
#define DSV_MV_BIT_RINGING  4

#define DSV_MV_IS_INTRA(mv)     ((mv)->flags & (1 << DSV_MV_BIT_INTRA))
#define DSV_MV_IS_EPRM(mv)      ((mv)->flags & (1 << DSV_MV_BIT_EPRM))
#define DSV_MV_IS_MAINTAIN(mv)  ((mv)->flags & (1 << DSV_MV_BIT_MAINTAIN))
#define DSV_MV_IS_SKIP(mv)      ((mv)->flags & (1 << DSV_MV_BIT_SKIP))
#define DSV_MV_IS_RINGING(mv)   ((mv)->flags & (1 << DSV_MV_BIT_RINGING))

#define DSV_BIT_SET(v, b, on) ((v) &= ~(1 << (b)), (v) |= ((on) << (b)))
#define DSV_MV_SET_INTRA(mv, b)     (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_INTRA, b))
#define DSV_MV_SET_EPRM(mv, b)      (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_EPRM, b))
#define DSV_MV_SET_MAINTAIN(mv, b)  (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_MAINTAIN, b))
#define DSV_MV_SET_SKIP(mv, b)      (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_SKIP, b))
#define DSV_MV_SET_RINGING(mv, b)   (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_RINGING, b))

    uint32_t flags;
    uint16_t err;
#define DSV_SRC_DC_PRED 0x100
    uint16_t dc;
    uint8_t submask;
} DSV_MV;

/* subsections of the encoded data */
#define DSV_SUB_MODE 0 /* block modes */
#define DSV_SUB_MV_X 1 /* motion vector x coordinates */
#define DSV_SUB_MV_Y 2 /* motion vector y coordinates */
#define DSV_SUB_SBIM 3 /* sub block intra masks */
#define DSV_SUB_EPRM 4 /* expanded prediction range mode */
#define DSV_SUB_NSUB 5

#define DSV_FRAME_BORDER DSV_MAX_BLOCK_SIZE

typedef struct {
    DSV_PARAMS *params;
    uint8_t *blockdata; /* block bitmasks for adaptive things */
    uint8_t cur_plane;
    uint8_t isP; /* is P frame */
    DSV_FNUM fnum;
} DSV_FMETA; /* frame metadata */

typedef struct {
    uint8_t *start;
    unsigned pos;
} DSV_BS;

/* macros for really simple operations */
#define bs_aligned(bs) (((bs)->pos & 7) == 0)
#define bs_ptr(bs) ((bs)->pos / 8)
#define bs_set(bs, ptr) ((bs)->pos = (ptr) * 8)
#define bs_skip(bs, n_bytes) ((bs)->pos += (n_bytes) * 8)

typedef struct {
    DSV_BS bs;
    int nz;
} DSV_ZBRLE;

#define DSV_STABLE_BIT   0
#define DSV_MAINTAIN_BIT 1
#define DSV_SKIP_BIT     2
#define DSV_RINGING_BIT  3
#define DSV_INTRA_BIT    4
#define DSV_EPRM_BIT     5

#define DSV_IS_STABLE     (1 << DSV_STABLE_BIT)
#define DSV_IS_MAINTAIN   (1 << DSV_MAINTAIN_BIT)
#define DSV_IS_SKIP       (1 << DSV_SKIP_BIT)
#define DSV_IS_RINGING    (1 << DSV_RINGING_BIT)
#define DSV_IS_INTRA      (1 << DSV_INTRA_BIT)
#define DSV_IS_EPRM       (1 << DSV_EPRM_BIT)

/* D.1 Luma Half-Pixel Filter */

/* half-pixel filters used for motion compensation */
#define DSV_HPF_A(a,b,c,d) ((19*((b)+(c)))-(3*((a)+(d))))
#define DSV_HPF_B(a,b,c,d) ((20*((b)+(c)))-(4*((a)+(d))))
#define DSV_HP_SHF 5                          /* normalization shift */
#define DSV_HP_ADD (1 << (DSV_HP_SHF - 1))    /* rounding addition */

/* half-pixel filter used for motion estimation */
#define DSV_HPF_ME(a,b,c,d) ((5*((b)+(c)))-(((a)+(d))))
#define DSV_ME_HP_SHF 3
#define DSV_ME_HP_ADD (1 << (DSV_ME_HP_SHF - 1))

#define DSV_MAX_QP_BITS 12
#define DSV_MAX_QP ((1 << DSV_MAX_QP_BITS) - 1)

/* C.2 fixed point precision for determining what block a pixel lies in */
#define DSV_BLOCK_INTERP_P      14

/*********************************** GENERAL **********************************/

char *d28_lvlname[DSV_LEVEL_DEBUG + 1] = {
    "NONE",
    "ERROR",
    "WARNING",
    "INFO",
    "DEBUG"
};

static int d28_loglvl = DSV_LEVEL_ERROR;

extern void
d28_set_log_level(int level)
{
    d28_loglvl = level;
}

extern int
d28_get_log_level(void)
{
    return d28_loglvl;
}

#ifndef _DSV2_NO_ALLOC_
#define DSV2_ALLOC_FUNC(num_bytes) calloc(1, num_bytes)
#define DSV2_FREE_FUNC(pointer) free(pointer)
#endif

#ifdef _DSV2_MEMORY_STATS_
static unsigned allocated = 0;
static unsigned freed = 0;
static unsigned allocated_bytes = 0;
static unsigned freed_bytes = 0;
static unsigned peak_alloc = 0;

extern void *
d28_alloc(int size)
{
    void *p;

    p = DSV2_ALLOC_FUNC(size + 16);
    if (!p) {
        return NULL;
    }
    *((int32_t *) p) = size;
    allocated++;
    allocated_bytes += size;
    if (peak_alloc < (allocated_bytes - freed_bytes)) {
        peak_alloc = (allocated_bytes - freed_bytes);
    }
    return (uint8_t *) p + 16;
}

extern void
d28_free(void *ptr)
{
    uint8_t *p;
    freed++;
    p = ((uint8_t *) ptr) - 16;
    freed_bytes += *((int32_t *) p);
    if (peak_alloc < (allocated_bytes - freed_bytes)) {
        peak_alloc = (allocated_bytes - freed_bytes);
    }
    DSV2_FREE_FUNC(p);
}

extern void
d28_memory_report(void)
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
d28_alloc(int size)
{
    return DSV2_ALLOC_FUNC(size);
}

extern void
d28_free(void *ptr)
{
    DSV2_FREE_FUNC(ptr);
}

extern void
d28_memory_report(void)
{
    DSV_DEBUG(("memory stats are disabled"));
}
#endif

extern int
d28_mk_packet_buf(uint8_t *hdr, int hdrlen, DSV_BUF *rb, int *packet_type)
{
    int size;

    if (hdrlen == 0) {
        DSV_ERROR(("no data"));
        return DSV_PKT_ERR_EOF;
    }
    if (hdrlen < DSV_PACKET_HDR_SIZE) {
        DSV_ERROR(("not enough bytes"));
        return DSV_PKT_ERR_OOB;
    }

    if (hdr[0] != DSV_FOURCC_0 || hdr[1] != DSV_FOURCC_1 || hdr[2] != DSV_FOURCC_2 || hdr[3] != DSV_FOURCC_3) {
        DSV_ERROR(("bad 4cc (%c %c %c %c, %d %d %d %d",
                hdr[0],hdr[1],hdr[2],hdr[3],
                hdr[0],hdr[1],hdr[2],hdr[3]));
        return DSV_PKT_ERR_4CC;
    }
    if (hdr[4] != DSV_VERSION_MINOR) {
        DSV_ERROR(("bad version (%d), decoded video will likely look incorrect", hdr[4]));
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
    d28_mk_buf(rb, size);
    memcpy(rb->data, hdr, DSV_PACKET_HDR_SIZE);
    return 1;
}

#ifndef _DSV2_NO_STDIO_
extern int
d28_yuv_write(FILE *out, int fno, DSV_PLANE *p)
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
d28_yuv_write_seq(FILE *out, DSV_PLANE *p)
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
#endif

extern void
d28_buf_free(DSV_BUF *buf)
{
    if (buf->data) {
        d28_free(buf->data);
        buf->data = NULL;
    }
}

extern void
d28_mk_buf(DSV_BUF *buf, int size)
{
    memset(buf, 0, sizeof(*buf));
    buf->data = d28_alloc(size);
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
static void
mv_pred(DSV_MV *vecs, DSV_PARAMS *p, int x, int y, int *px, int *py)
{
    DSV_MV *mv;
    int vx[3] = { 0, 0, 0 };
    int vy[3] = { 0, 0, 0 };

    if (x > 0) { /* left */
        mv = (vecs + y * p->nblocks_h + (x - 1));
        vx[0] = mv->u.mv.x;
        vy[0] = mv->u.mv.y;
    }
    if (y > 0) { /* top */
        mv = (vecs + (y - 1) * p->nblocks_h + x);
        vx[1] = mv->u.mv.x;
        vy[1] = mv->u.mv.y;

    }
    if (x > 0 && y > 0) { /* top-left */
        mv = (vecs + (y - 1) * p->nblocks_h + (x - 1));
        vx[2] = mv->u.mv.x;
        vy[2] = mv->u.mv.y;
    }

    *px = pred(vx[0], vx[1], vx[2]);
    *py = pred(vy[0], vy[1], vy[2]);
}

/* how similar a motion vector is to its top / left neighbors */
static void
neighdif2(DSV_MV *vecs, DSV_PARAMS *p, int x, int y, int *dx, int *dy)
{
    DSV_MV *mv;
    DSV_MV *cmv;
    int cmx, cmy;
    int vx[2], vy[2];

    cmv = &vecs[x + y * p->nblocks_h];
    cmx = cmv->u.mv.x;
    cmy = cmv->u.mv.y;
    if (abs(cmx) < 2 && abs(cmy) < 2) {
        *dx = *dy = 0;
        return;
    }
    vx[0] = vx[1] = cmx;
    vy[0] = vy[1] = cmy;
    if (x > 0) { /* left */
        mv = (vecs + y * p->nblocks_h + (x - 1));
        if (mv->u.all && !DSV_MV_IS_SKIP(mv)) {
            vx[0] = mv->u.mv.x;
            vy[0] = mv->u.mv.y;
        }
    }
    if (y > 0) { /* top */
        mv = (vecs + (y - 1) * p->nblocks_h + x);
        if (mv->u.all && !DSV_MV_IS_SKIP(mv)) {
            vx[1] = mv->u.mv.x;
            vy[1] = mv->u.mv.y;
        }
    }
    /* magnitude of current motion vector subtracted from its left neighbor */
    *dx = abs(vx[0] - cmx) + abs(vy[0] - cmy);
    /* magnitude of current motion vector subtracted from its top neighbor */
    *dy = abs(vx[1] - cmx) + abs(vy[1] - cmy);
}

/* how similar a motion vector is to its top / left neighbors */
static int
neighdif(DSV_MV *vecs, DSV_PARAMS *p, int x, int y)
{
    int d0, d1;
    neighdif2(vecs, p, x, y, &d0, &d1);
    return (d0 + d1) / 3;
}

static int
logb2(unsigned n)
{
    unsigned i = 1, log2 = 0;

    while (i < n) {
        i <<= 1;
        log2++;
    }
    return log2;
}

/********************************** BITSTREAM *********************************/

/* B. Bitstream */

static void
bs_init(DSV_BS *bs, uint8_t *buffer)
{
    bs->start = buffer;
    bs->pos = 0;
}

static void
bs_align(DSV_BS *bs)
{
    if (bs_aligned(bs)) {
        return; /* already aligned */
    }
    bs->pos = ((bs->pos + 7) & ((unsigned) (~0) << 3)); /* byte align */
}

static unsigned
bs_get_bit(DSV_BS *bs)
{
    unsigned out;

    out = bs->start[bs_ptr(bs)] >> (7 - (bs->pos & 7));
    bs->pos++;

    return out & 1;
}

static unsigned
bs_get_bits(DSV_BS *bs, unsigned n)
{
    unsigned rem, bit, out = 0;

    while (n > 0) {
        rem = 8 - (bs->pos & 7);
        rem = MIN(n, rem);
        bit = (7 - (bs->pos & 7)) - rem + 1;
        out <<= rem;
        out |= (bs->start[bs_ptr(bs)] & (((1 << rem) - 1) << bit)) >> bit;
        n -= rem;
        bs->pos += rem;
    }
    return out;
}

/* B. Encoding Type: unsigned interleaved exp-Golomb code (UEG) */
static unsigned
bs_get_ueg(DSV_BS *bs)
{
    unsigned v = 1;

    while (!bs_get_bit(bs)) {
        v = (v << 1) | bs_get_bit(bs);
    }
    return v - 1;
}

/* B. Encoding Type: signed interleaved exp-Golomb code (SEG) */
static int
bs_get_seg(DSV_BS *bs)
{
    int v;

    v = bs_get_ueg(bs);
    if (v && bs_get_bit(bs)) {
        return -v;
    }
    return v;
}

/* B. Encoding Type: non-zero interleaved exp-Golomb code (NEG) */
static int
bs_get_neg(DSV_BS *bs)
{
    int v;

    v = bs_get_ueg(bs) + 1;
    if (v && bs_get_bit(bs)) {
        return -v;
    }
    return v;
}

/* B. Encoding Type: adaptive Rice code (URC) */
static unsigned
bs_get_rice(DSV_BS *bs, int *rk, int damp)
{
    int k = (*rk) >> damp;
    unsigned q = 0;
    while (!bs_get_bit(bs)) {
        q++;
    }
    if (q) {
        (*rk)++;
    } else if ((*rk) > 0) {
        (*rk)--;
    }
    return (q << k) | bs_get_bits(bs, k);
}

static int
u2s(unsigned uv)
{
    int v = (int) (uv + (unsigned) 1);
    return v & 1 ? v >> 1 : -(v >> 1);
}

/* B. Encoding Type: non-zero adaptive Rice code (NRC) */
static int
bs_get_nrice(DSV_BS *bs, int *rk, int damp)
{
    return u2s(bs_get_rice(bs, rk, damp) + 1);
}

/* B. Encoding Format: Zero Bit Run-Length Encoding (ZBRLE) */
static void
bs_init_rle(DSV_ZBRLE *rle, uint8_t *buf)
{
    memset(rle, 0, sizeof(*rle));
    bs_init(&rle->bs, buf);
}

/* B. Encoding Format: Zero Bit Run-Length Encoding (ZBRLE) */
static int
bs_end_rle(DSV_ZBRLE *rle)
{
    if (rle->nz > 1) { /* early termination */
        DSV_ERROR(("%d remaining in run", rle->nz));
    }
    return 0;
}

/* B. Encoding Format: Zero Bit Run-Length Encoding (ZBRLE) */
static int
bs_get_rle(DSV_ZBRLE *rle)
{
    if (rle->nz == 0) {
        rle->nz = bs_get_ueg(&rle->bs);
        return (rle->nz == 0);
    }
    rle->nz--;
    return (rle->nz == 0);
}

/************************************ FRAME ***********************************/

static DSV_FRAME *
alloc_frame(void)
{
    DSV_FRAME *frame;

    frame = d28_alloc(sizeof(*frame));
    frame->refcount = 1;
    return frame;
}

static void
mk_coefs(DSV_COEFS *c, int format, int width, int height)
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
    c[0].data = d28_alloc((c0len + c1len + c2len) * sizeof(DSV_SBC));
    c[1].data = c[0].data + c0len;
    c[2].data = c[0].data + c0len + c1len;
}

extern DSV_FRAME *
d28_mk_frame(int format, int width, int height, int border)
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

    f->alloc = d28_alloc(f->planes[0].len + f->planes[1].len + f->planes[2].len);

    f->planes[0].data = f->alloc + f->planes[0].stride * ext + ext;
    f->planes[1].data = f->alloc + f->planes[0].len + f->planes[1].stride * ext + ext;
    f->planes[2].data = f->alloc + f->planes[0].len + f->planes[1].len + f->planes[2].stride * ext + ext;

    return f;
}

extern DSV_FRAME *
d28_clone_frame(DSV_FRAME *s, int border)
{
    DSV_FRAME *d;

    d = d28_mk_frame(s->format, s->width, s->height, border);
    d28_frame_copy(d, s);
    return d;
}

extern DSV_FRAME *
d28_frame_ref_inc(DSV_FRAME *frame)
{
    DSV_ASSERT(frame && frame->refcount > 0);
    frame->refcount++;
    return frame;
}

extern void
d28_frame_ref_dec(DSV_FRAME *frame)
{
    DSV_ASSERT(frame && frame->refcount > 0);

    frame->refcount--;
    if (frame->refcount == 0) {
        if (frame->alloc) {
            d28_free(frame->alloc);
        }
        d28_free(frame);
    }
}

extern void
d28_frame_copy(DSV_FRAME *dst, DSV_FRAME *src)
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
        d28_extend_frame(dst);
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
                out[o++] = (MKVERT(0) + 2) >> 2;
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
                out[o++] = (MKVERT(0) + 2) >> 2;
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
                out[o++] = (MKHORIZ(0) + 2) >> 2;
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
                out[o++] = (MKHORIZ(0) + 2) >> 2;
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

    ls = d28_alloc(4 * height / SUBDIV);
    rs = d28_alloc(4 * height / SUBDIV);
    ts = d28_alloc(4 * width / SUBDIV);
    bs = d28_alloc(4 * width / SUBDIV);
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

    d28_free(ls);
    d28_free(rs);
    d28_free(ts);
    d28_free(bs);
}

extern DSV_FRAME *
d28_extend_frame(DSV_FRAME *frame)
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

/************************************ SBT *************************************/

/* Subband transforms */

#define IS_P    (fm->isP)
#define IS_LUMA (fm->cur_plane == 0)

#define LLI_CONDITION  (IS_LUMA  && !IS_P && (l == 4))
#define LLP_CONDITION  (IS_LUMA  &&  IS_P && (l == 4))
#define L2A_CONDITION  (IS_LUMA  && !IS_P && (l == 2))
#define CC_CONDITION   (!IS_LUMA && !IS_P && (l >= 1 && l <= (lvls - 2)))
#define L1_CONDITION   (IS_LUMA  && !IS_P && (l == 1))

/* overflow safety */
#define OVF_SAFETY_CONDITION (l >= 6 && l >= (lvls - 3) && !fm->params->lossless)

#define SHREX4I 3
#define SHREX4P 2
#define SHREX2 3
#define INV_SCALE52(x) ((x) * 2 / 5)
#define INV_SCALE20(x) ((x) / 2)
#define INV_SCALE30(x) ((x) / 3)
#define INV_SCALE40(x) ((x) / 4)
#define INV_SCALENONE(x) (x)

static DSV_SBC *temp_buf = NULL;
static int temp_bufsz = 0;

static void
alloc_temp(int size)
{
    if (temp_bufsz < size) {
        temp_bufsz = size;

        if (temp_buf) {
            d28_free(temp_buf);
            temp_buf = NULL;
        }

        temp_buf = d28_alloc(temp_bufsz * sizeof(DSV_SBC));
        if (temp_buf == NULL) {
            DSV_ERROR(("out of memory"));
        }
    }
}

static void
cpysub(DSV_SBC *dst, DSV_SBC *src, unsigned w, unsigned h, unsigned stride)
{
    w *= sizeof(DSV_SBC);
    while (h-- > 0) {
        memcpy(dst, src, w);
        src += stride;
        dst += stride;
    }
}

/* C.3 Rounding Divisions */
static int
round2(int v)
{
    return (v + (v < 0 ? -1 : 1)) / 2;
}

static int
round4(int v)
{
    return (v + (v < 0 ? -2 : 2)) / 4;
}

static int
reflect(int i, int n)
{
    if (i < 0) {
        i = -i;
    }
    if (i >= n) {
        i = n + n - i;
    }
    return i;
}

/* lower level (L4) filter */
#define LL0 17
#define LLS 7
#define LLA (1 << (LLS - 1))

/* chroma (CC) filter */
#define CC0 3
#define CCS 4
#define CCA (1 << (CCS - 1))

/* L2 ringing filter */
#define R20 3
#define R2S 3
#define R2A (1 << (R2S - 1))

/* L2 standard filter */
#define S20 9
#define S2S 5
#define S2A (1 << (S2S - 1))

/* reflected get */
#define rg(x, s) reflect(x, (n - 1)) * s

#define UNSCALE_UNPACK(scaleL, scaleH, s)             \
  for (i = 0; i < even_n; i += 2) {                   \
      out[(i + 0) * s] = scaleL(in[(i + 0) / 2 * s]); \
      out[(i + 1) * s] = scaleH(in[(i + h) / 2 * s]); \
  }                                                   \
  if (n & 1) {                                        \
      out[(n - 1) * s] = scaleL(in[(n - 1) / 2 * s]); \
  }

#define UNSCALE_UNPACK_SHREX(scaleL, scaleH, s, shrex)  \
  for (i = 0; i < even_n; i += 2) {                     \
      DSV_SBC th = scaleH(in[(i + h) / 2 * s]);         \
      out[(i + 0) * s] = scaleL(in[(i + 0) / 2 * s]);   \
      out[(i + 1) * s] = th + DSV_SAR(th, shrex);       \
  }                                                     \
  if (n & 1) {                                          \
      out[(n - 1) * s] = scaleL(in[(n - 1) / 2 * s]);   \
  }

/* simple 3 tap low/high pass filters */
#define DO_SIMPLE_HI(v, op, s)                                \
  for (i = 1; i < n - 1; i += 2) {                            \
      v[i * s] op (v[(i - 1) * s] + v[(i + 1) * s] + 1) >> 1; \
  }                                                           \
  if (!(n & 1)) {                                             \
      v[(n - 1) * s] op v[(n - 2) * s];                       \
  }

#define DO_SIMPLE_LO(v, op, s)                                 \
  v[0] op v[s] >> 1;                                           \
  for (i = 2; i < even_n; i += 2) {                            \
      v[i * s] op (v[(i - 1) * s] + v[(i + 1) * s] + 2) >> 2;  \
  }

#define DO_SIMPLE_INV(v, s) \
  v[0] -= v[s] >> 1; \
  for (i = 2; i < even_n; i += 2) { \
      v[i * s] -= (v[(i - 1) * s] + v[(i + 1) * s] + 2) >> 2; \
      v[(i - 1) * s] += (v[(i - 2) * s] + v[i * s] + 1) >> 1; \
  } \
  if (!(n & 1)) { \
      v[(n - 1) * s] += v[(n - 2) * s]; \
  }

/* 5 tap low/high pass filters with/without adaptive ringing */
#define MAKE_5_TAP(v, C0, CA, CS, op, s)            \
  v[i * s] op (-v[rg(i - 3, s)] +                   \
          C0 * (v[(i - 1) * s] + v[(i + 1) * s]) -  \
                v[rg(i + 3, s)] + CA) >> CS

#define DO_5_TAP_LO(v, C0, CA, CS, op, s) \
  v[0] op v[s] >> 1;                      \
  for (i = 2; i < even_n; i += 2) {       \
      MAKE_5_TAP(v, C0, CA, CS, op, s);   \
  }

#define DO_5_TAP_LO_A(v, C0, CA, CS, R0, RA, RS, op, s)             \
  delta *= 2;                                                       \
  v[0] op v[s] >> 1;                                                \
  for (i = 2; i < even_n; i += 2) {                                 \
      int bv = sb[(sbp >> DSV_BLOCK_INTERP_P) * sbs];               \
      if (bv & DSV_IS_RINGING) {                                    \
          MAKE_5_TAP(v, R0, RA, RS, op, s);                         \
      } else {                                                      \
          MAKE_5_TAP(v, C0, CA, CS, op, s);                         \
      }                                                             \
      sbp += delta;                                                 \
  }

static void
ifilterLLI(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK_SHREX(INV_SCALE52, INV_SCALE40, s, SHREX4I);
    /* Combined these for speed:
       DO_5_TAP_LO(out, LL0, LLA, LLS, -=, s);
       DO_SIMPLE_HI(out, +=, s);
    */
    out[0] -= out[s] >> 1;
    for (i = 2; i < even_n; i += 2) {
        MAKE_5_TAP(out, LL0, LLA, LLS, -=, s);
        out[(i - 1) * s] += (out[(i - 2) * s] + out[i * s] + 1) >> 1;
    }
    if (n & 1) {
        /* intentional use of 'i' after the for-loop */
        out[(i - 1) * s] += (out[(i - 2) * s] + out[i * s] + 1) >> 1;
    } else {
        out[(n - 1) * s] += out[(n - 2) * s];
    }
}

static void
ifilterLLP(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK_SHREX(INV_SCALE52, INV_SCALE30, s, SHREX4P);
    /* Combined these for speed:
       DO_5_TAP_LO(out, LL0, LLA, LLS, -=, s);
       DO_SIMPLE_HI(out, +=, s);
    */
    out[0] -= out[s] >> 1;
    for (i = 2; i < even_n; i += 2) {
        MAKE_5_TAP(out, LL0, LLA, LLS, -=, s);
        out[(i - 1) * s] += (out[(i - 2) * s] + out[i * s] + 1) >> 1;
    }
    if (n & 1) {
        /* intentional use of 'i' after the for-loop */
        out[(i - 1) * s] += (out[(i - 2) * s] + out[i * s] + 1) >> 1;
    } else {
        out[(n - 1) * s] += out[(n - 2) * s];
    }
}

static void
ifilterCC(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK(INV_SCALE20, INV_SCALENONE, s);
    DO_5_TAP_LO(out, CC0, CCA, CCS, -=, s);
    DO_SIMPLE_HI(out, +=, s);
}

static void
ifilterL2_a(DSV_SBC *out, DSV_SBC *in, int n, int s, uint8_t *sb, int delta, int sbs)
{
    int i, sbp = 0, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK_SHREX(INV_SCALE20, INV_SCALE30, s, SHREX2);
    DO_5_TAP_LO_A(out, S20, S2A, S2S, R20, R2A, R2S, -=, s);
    DO_SIMPLE_HI(out, +=, s);
}

static void
ifilterL1(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK(INV_SCALE20, INV_SCALE40, s);
    DO_SIMPLE_INV(out, s);
}

static void
ifilterLOSSLESS(DSV_SBC *out, DSV_SBC *in, int n, int s)
{
    int i, even_n = n & ~1, h = n + (n & 1);
    UNSCALE_UNPACK(INV_SCALENONE, INV_SCALENONE, s);
    DO_SIMPLE_LO(out, -=, s);
    DO_SIMPLE_HI(out, +=, s);
}

#define inv_2d(tmp, in, fw, fh, lvl, ifilter) \
{ \
    int i, j, sw, sh; \
    sw = DSV_ROUND_SHIFT(fw, lvl - 1); \
    sh = DSV_ROUND_SHIFT(fh, lvl - 1); \
    for (i = 0; i < sw; i++) { \
        ifilter(tmp + i, in + i, sh, fw); \
    } \
    for (j = 0; j < sh; j++) { \
        ifilter(in + fw * j, tmp + fw * j, sw, 1); \
    } \
}

static void
inv_L2a_2d(DSV_SBC *tmp, DSV_SBC *in, int sW, int sH, int lvl, DSV_FMETA *fm)
{
    int i, j, bx = 0, by = 0, dbx, dby, w, h;
    uint8_t *line;

    w = DSV_ROUND_SHIFT(sW, lvl - 1);
    h = DSV_ROUND_SHIFT(sH, lvl - 1);

    dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / w;
    dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / h;
    for (i = 0; i < w; i++) {
        line = fm->blockdata + (bx >> DSV_BLOCK_INTERP_P);
        ifilterL2_a(tmp + i, in + i, h, sW, line, dby, fm->params->nblocks_h);
        bx += dbx;
    }
    for (j = 0; j < h; j++) {
        line = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
        ifilterL2_a(in + sW * j, tmp + sW * j, w, 1, line, dbx, 1);
        by += dby;
    }
}

/* C.3.1.1 Haar Simple Inverse Transform */
static void
inv_simple(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl, int ovf_safety)
{
    int x, y, woff, hoff, ws, hs, oddw, oddh;
    int LL, LH, HL, HH;
    int idx;
    DSV_SBC *os, *od, *spLL, *spLH, *spHL, *spHH;

    os = src;
    od = dst;

    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    oddw = (ws & 1);
    oddh = (hs & 1);

    spLL = src;
    spLH = src + woff;
    spHL = src + hoff * width;
    spHH = src + woff + hoff * width;
    for (y = 0; y < hs - oddh; y += 2) {
        DSV_SBC *dpA, *dpB;

        dpA = dst + (y + 0) * width;
        dpB = dst + (y + 1) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            LL = spLL[idx] << ovf_safety;
            LH = spLH[idx];
            HL = spHL[idx];
            HH = spHH[idx];

            dpA[x + 0] = (LL + LH + HL + HH) / 4; /* LL */
            dpA[x + 1] = (LL - LH + HL - HH) / 4; /* LH */
            dpB[x + 0] = (LL + LH - HL - HH) / 4; /* HL */
            dpB[x + 1] = (LL - LH - HL + HH) / 4; /* HH */
        }
        if (oddw) {
            LL = spLL[idx] << ovf_safety;
            HL = spHL[idx];

            dpA[x + 0] = (LL + HL) / 4; /* LL */
            dpB[x + 0] = (LL - HL) / 4; /* HL */
        }
        spLL += width;
        spLH += width;
        spHL += width;
        spHH += width;
    }
    if (oddh) {
        DSV_SBC *dpA = dst + (y + 0) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            LL = spLL[idx] << ovf_safety;
            LH = spLH[idx];

            dpA[x + 0] = (LL + LH) / 4; /* LL */
            dpA[x + 1] = (LL - LH) / 4; /* LH */
        }
        if (oddw) {
            LL = spLL[idx] << ovf_safety;

            dpA[x + 0] = (LL / 4); /* LL */
        }
    }
    cpysub(os, od, ws, hs, width);
}

/* C.3.1.2 Haar Filtered Inverse Transform */
static void
inv(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl, int hqp, int ovf_safety)
{
    int x, y, woff, hoff, ws, hs, oddw, oddh;
    int LL, LH, HL, HH;
    int idx;
    DSV_SBC *os, *od, *spLL, *spLH, *spHL, *spHH;

    os = src;
    od = dst;

    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    oddw = (ws & 1);
    oddh = (hs & 1);

    spLL = src;
    spLH = src + woff;
    spHL = src + hoff * width;
    spHH = src + woff + hoff * width;
    for (y = 0; y < hs - oddh; y += 2) {
        DSV_SBC *dpA, *dpB;
        int inY = y > 0 && y < (hs - oddh - 1);

        dpA = dst + (y + 0) * width;
        dpB = dst + (y + 1) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            int inX = x > 0 && x < (ws - oddw - 1);
            int nudge, t, lp, ln, mn, mx;

            LL = spLL[idx] << ovf_safety;
            LH = spLH[idx];
            HL = spHL[idx];
            HH = spHH[idx];

            if (inX) {
                lp = spLL[idx - 1] << ovf_safety; /* prev */
                ln = spLL[idx + 1] << ovf_safety; /* next */
                mx = LL - ln; /* find difference between LL values */
                mn = lp - LL;
                if (mn > mx) {
                    t = mn;
                    mn = mx;
                    mx = t;
                }
                mx = MIN(mx, 0); /* must be negative or zero */
                mn = MAX(mn, 0); /* must be positive or zero */
                /* if they are not zero, then there is a potential
                 * consistent smooth gradient between them */
                if (mx != mn) {
                    t = round4(lp - ln);
                    nudge = round2(CLAMP(t, mx, mn) - (LH * 2));
                    LH += CLAMP(nudge, -hqp, hqp); /* nudge LH to smooth it */
                }
            }
            if (inY) { /* do the same as above but in the Y direction */
                lp = spLL[idx - width] << ovf_safety;
                ln = spLL[idx + width] << ovf_safety;
                mx = LL - ln;
                mn = lp - LL;
                if (mn > mx) {
                    t = mn;
                    mn = mx;
                    mx = t;
                }
                mx = MIN(mx, 0);
                mn = MAX(mn, 0);
                if (mx != mn) {
                    t = round4(lp - ln);
                    nudge = round2(CLAMP(t, mx, mn) - (HL * 2));
                    HL += CLAMP(nudge, -hqp, hqp); /* nudge HL to smooth it */
                }
            }

            dpA[x + 0] = (LL + LH + HL + HH) / 4; /* LL */
            dpA[x + 1] = (LL - LH + HL - HH) / 4; /* LH */
            dpB[x + 0] = (LL + LH - HL - HH) / 4; /* HL */
            dpB[x + 1] = (LL - LH - HL + HH) / 4; /* HH */
        }
        if (oddw) {
            LL = spLL[idx] << ovf_safety;
            HL = spHL[idx];

            dpA[x + 0] = (LL + HL) / 4; /* LL */
            dpB[x + 0] = (LL - HL) / 4; /* HL */
        }
        spLL += width;
        spLH += width;
        spHL += width;
        spHH += width;
    }
    if (oddh) {
        DSV_SBC *dpA = dst + (y + 0) * width;
        for (x = 0, idx = 0; x < ws - oddw; x += 2, idx++) {
            LL = spLL[idx] << ovf_safety;
            LH = spLH[idx];

            dpA[x + 0] = (LL + LH) / 4; /* LL */
            dpA[x + 1] = (LL - LH) / 4; /* LH */
        }
        if (oddw) {
            LL = spLL[idx] << ovf_safety;

            dpA[x + 0] = LL / 4; /* LL */
        }
    }
    cpysub(os, od, ws, hs, width);
}

/* C.3.3 Subband Recomposition */
static void
sbc2p(DSV_PLANE *p, DSV_COEFS *dc)
{
    int x, y;
    DSV_SBC *d, v;

    d = dc->data;
    for (y = 0; y < p->h; y++) {
        uint8_t *line = DSV_GET_LINE(p, y);
        for (x = 0; x < p->w; x++) {
            v = (d[x] + 128);
            line[x] = CLAMP(v, 0, 255);
        }
        d += dc->width;
    }
}

/* C.3.3 Subband Recomposition - num_levels */
static int
nlevels(int w, int h)
{
    int lb2, mx;

    mx = (w > h) ? w : h;
    lb2 = logb2(mx);
    if (mx > (1 << lb2)) {
        lb2++;
    }
    return lb2;
}

/* C.3.3 Subband Recomposition */
static void
inv_sbt(DSV_PLANE *dst, DSV_COEFS *src, int q, DSV_FMETA *fm)
{
    int w, h, lvls, l, hqp, ovf_safety;
    DSV_SBC *temp_buf_pad;

    w = src->width;
    h = src->height;

    lvls = nlevels(w, h);
    alloc_temp((w + 2) * (h + 2));
    temp_buf_pad = temp_buf + w;

    for (l = lvls; l > 0; l--) {
        hqp = (fm->cur_plane == 0) ? (q / (fm->isP ? 14 : (l > 4 ? 2 : 8))) : (q / 2);
        ovf_safety = OVF_SAFETY_CONDITION;

        if (fm->params->lossless) {
            if ((l >= 1 && l <= (lvls - 2))) {
                inv_2d(temp_buf_pad, src->data, w, h, l, ifilterLOSSLESS);
            } else {
                inv_simple(src->data, temp_buf_pad, w, h, l, ovf_safety);
            }
            continue;
        }
        if (LLI_CONDITION) {
            inv_2d(temp_buf_pad, src->data, w, h, l, ifilterLLI);
        } else if (LLP_CONDITION) {
            inv_2d(temp_buf_pad, src->data, w, h, l, ifilterLLP);
        } else if (CC_CONDITION) {
            inv_2d(temp_buf_pad, src->data, w, h, l, ifilterCC);
        } else if (L2A_CONDITION) {
            inv_L2a_2d(temp_buf_pad, src->data, w, h, l, fm);
        } else if (L1_CONDITION) {
            inv_2d(temp_buf_pad, src->data, w, h, l, ifilterL1);
        } else {
            if (fm->cur_plane == 0 || !fm->isP) {
                inv(src->data, temp_buf_pad, w, h, l, hqp, ovf_safety);
            } else {
                inv_simple(src->data, temp_buf_pad, w, h, l, ovf_safety);
            }
        }
    }

    sbc2p(dst, src);
}

/************************************ BMC *************************************/

/* Block-based Motion Compensation */

static uint8_t
clamp_u8(int v)
{
    return v > 255 ? 255 : v < 0 ? 0 : v;
}

static int
avgval(uint8_t *dec, int dw, int w, int h)
{
    int i, j;
    int avg = 0;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            avg += dec[i];
        }
        dec += dw;
    }
    return avg / (w * h);
}

/* copy directly from reference block (full-pel) */
static void
cpyblk(uint8_t *dec, uint8_t *ref, int dw, int rw, int w, int h)
{
    while (h-- > 0) {
        memcpy(dec, ref, w);
        ref += rw;
        dec += dw;
    }
}

/* D.5.2 Filtering */

#define ITEST4x4(t) (abs(e0 - avg) < (t) && \
                     abs(i0 - avg) < (t) && \
                     abs(e1 - avg) < (t) && \
                     abs(i1 - avg) < (t) && \
                     abs(e2 - avg) < (t) && \
                     abs(i2 - avg) < (t))

#define FILTER_DIM 4 /* do not touch, filters are hardcoded as 4x4 operations */


#define LPF ((5 * (e0 + i0) + 3 * (e1 + i1) + 8) >> 4)

#define FC_E1 ((3 * (avg + e1) + 2 * e2 + 4) >> 3)
#define FC_E0 ((avg + 2 * e1 + e2 + 4) >> 3)  /* assumes avg was already *= 5 */
#define FC_I0 (avg)
#define FC_I1 ((avg + 2 * i1 + i2 + 4) >> 3)  /* assumes avg was already *= 5 */

static void
ihfilter4x4(DSV_PLANE *dp, int x, int y, int edge, int threshE, int threshM)
{
    int line, top, bot;
    uint8_t *b = dp->data;
    int w = dp->w;
    int h = dp->h;
    int s = dp->stride;
    int in_edge;

    if ((x < FILTER_DIM) || (x > w - FILTER_DIM) ||
        (edge && threshE <= 0) || (threshM <= 0)) {
        return;
    }
    top = x + CLAMP(y, 0, (h - 1)) * s;
    bot = x + CLAMP(y + FILTER_DIM, 0, (h - 1)) * s;
    in_edge = x < (w - FILTER_DIM - FILTER_DIM);
    if (!edge) {
        threshE = threshM;
    }
    for (line = top; line < bot; line += s) {
        int i2, i1, i0, e0, e1, e2, avg;

        e2 = b[line - 3];
        e1 = b[line - 2];
        e0 = b[line - 1];
        i0 = b[line + 0];
        i1 = b[line + 1];
        i2 = b[line + 2];

        avg = LPF;
        if (ITEST4x4(threshE)) {
            b[line - 2] = FC_E1;
            b[line + 0] = FC_I0;
            avg *= 5;
            b[line - 1] = FC_E0;
            b[line + 1] = FC_I1;
        }

        if (in_edge) {
            int k = line + FILTER_DIM;
            i2 = b[k - 2];
            i1 = b[k - 1];
            i0 = b[k + 0];
            e0 = b[k + 1];
            e1 = b[k + 2];
            e2 = b[k + 3];

            avg = LPF;
            if (ITEST4x4(threshM)) {
                b[k + 0] = FC_I0;
                b[k + 2] = FC_E1;
                avg *= 5;
                b[k - 1] = FC_I1;
                b[k + 1] = FC_E0;
            }
        }
    }
}

static void
ivfilter4x4(DSV_PLANE *dp, int x, int y, int edge, int threshE, int threshM)
{
    int beg, end;
    int i, s2, s3;
    int w = dp->w;
    int h = dp->h;
    int s = dp->stride;
    uint8_t *b = dp->data;
    uint8_t *bk = b + FILTER_DIM * s;
    int in_edge;

    if ((y < FILTER_DIM) || (y > h - FILTER_DIM) ||
        (edge && threshE <= 0) || (threshM <= 0)) {
        return;
    }
    beg = CLAMP(x, 0, (w - 1)) + y * s;
    end = CLAMP(x + FILTER_DIM, 0, (w - 1)) + y * s;
    s2 = s * 2;
    s3 = s * 3;
    in_edge = y < (h - FILTER_DIM - FILTER_DIM);
    if (!edge) {
        threshE = threshM;
    }
    for (i = beg; i < end; i++) {
        int i2, i1, i0, e0, e1, e2, avg;

        e2 = b[i - s3];
        e1 = b[i - s2];
        e0 = b[i - s];
        i0 = b[i + 0];
        i1 = b[i + s];
        i2 = b[i + s2];

        avg = LPF;
        if (ITEST4x4(threshE)) {
            b[i - s2] = FC_E1;
            b[i + 0] = FC_I0;
            avg *= 5;
            b[i - s] = FC_E0;
            b[i + s] = FC_I1;
        }

        if (in_edge) {
            i2 = bk[i - s2];
            i1 = bk[i - s];
            i0 = bk[i + 0];
            e0 = bk[i + s];
            e1 = bk[i + s2];
            e2 = bk[i + s3];

            avg = LPF;
            if (ITEST4x4(threshM)) {
                bk[i + 0] = FC_I0;
                bk[i + s2] = FC_E1;
                avg *= 5;
                bk[i - s] = FC_I1;
                bk[i + s] = FC_E0;
            }
        }
    }
}

/* downsampled filter factor for a 4x4 block */
static unsigned
dsff4x4(uint8_t *a, int as)
{
    unsigned sh, sv;
    int dsp0, dsp1, dsp2, dsp3;

    /* create downsampled pixels */
    dsp0 = ((a[0] + a[1] + a[as + 0] + a[as + 1] + 2) >> 2);
    dsp1 = ((a[2] + a[3] + a[as + 2] + a[as + 3] + 2) >> 2);
    a += 2 * as;
    dsp2 = ((a[0] + a[1] + a[as + 0] + a[as + 1] + 2) >> 2);
    dsp3 = ((a[2] + a[3] + a[as + 2] + a[as + 3] + 2) >> 2);

    /* determine if the detail should be kept */
    sh = abs((dsp0 + dsp1) - (dsp3 + dsp2));
    sv = abs((dsp2 + dsp1) - (dsp3 + dsp0));
    if (MAX(sh, sv) < 8) {
        return 0;
    }
    /* find how much it should be smoothed, derived from 'haar' metric */
    dsp2 = 255 - dsp2;
    dsp3 = 255 - dsp3;
    sh = abs(dsp0 - dsp1 + dsp2 - dsp3) >> 0;
    sv = abs(dsp0 + dsp1 - dsp2 - dsp3) >> 2;
    if (sh > sv) {
        return (3 * sh + sv + 2) >> 2;
    }
    return (3 * sv + sh + 2) >> 2;
}

static void
haar4x4(uint8_t *src, int as, int *psh, int *psv)
{
    uint8_t *spA, *spB;
    int x0, x1, x2, x3;
    int x, y;
    int idx, HH, sh = 0, sv = 0;

    for (y = 0; y < 4; y += 2) {
        spA = src + (y + 0) * as;
        spB = src + (y + 1) * as;
        for (x = 0, idx = 0; x < 4; x += 2, idx++) {
            x0 = spA[x + 0];
            x1 = spA[x + 1];
            x2 = spB[x + 0];
            x3 = spB[x + 1];

            HH = abs(x0 - x1 - x2 + x3) >> 1;
            sh += abs(x0 - x1 + x2 - x3); /* LH */
            sv += abs(x0 + x1 - x2 - x3); /* HL */
            sh += HH; /* HH */
            sv += HH; /* HH */
        }
    }
    *psh = sh;
    *psv = sv;
}

static void
artf4x4(uint8_t *a, int as, int *psh, int *psv, int *pslh, int *pslv)
{
    int dsp0, dsp1, dsp2, dsp3, HH;
    haar4x4(a, as, psh, psv);

    /* create downsampled pixels */
    dsp0 = (a[0] + a[1] + a[as + 0] + a[as + 1] + 2) >> 2;
    dsp1 = (a[2] + a[3] + a[as + 2] + a[as + 3] + 2) >> 2;
    a += 2 * as;
    dsp2 = (a[0] + a[1] + a[as + 0] + a[as + 1] + 2) >> 2;
    dsp3 = (a[2] + a[3] + a[as + 2] + a[as + 3] + 2) >> 2;

    *pslh = abs(dsp0 - dsp1 + dsp2 - dsp3);
    *pslv = abs(dsp0 + dsp1 - dsp2 - dsp3);
    HH = abs(dsp0 - dsp1 - dsp2 + dsp3) >> 1;
    *pslh += HH;
    *pslv += HH;
}

#define HISTBITS 4
#define NHIST (1 << HISTBITS)

/* de-gradient filter */
static void
degrad4x4(uint8_t *a, int as)
{
    uint8_t hist[NHIST];
    uint16_t avgs[NHIST];
    int x, y, lo, hi, alo, ahi, flo, fhi, t;
    uint8_t *sp = a;

    memset(hist, 0, sizeof(hist));
    memset(avgs, 0, sizeof(avgs));
    /* generate histogram of quantized luma samples + averages of each bucket */
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            t = sp[x] >> (8 - HISTBITS);
            hist[t]++;
            avgs[t] += sp[x];
        }
        sp += as;
    }
    lo = -1;
    hi = -1;
    /* find darkest and brightest buckets of the histogram */
    for (x = 0; x < NHIST; x++) {
        if (hist[x]) {
            if (lo == -1) {
                lo = x;
            }
            hi = x;
        }
    }
    /* ignore mostly flat blocks */
    if (lo >= hi) {
        return;
    }
    alo = avgs[lo] / hist[lo];
    ahi = avgs[hi] / hist[hi];

    if (alo == 0) {
        alo = 1;
    }
    if (ahi == 0) {
        ahi = 1;
    }
    flo = hist[lo];
    fhi = hist[hi];
    /* midpoint brightness of the block and the epsilon around the midpoint */
    t = (alo + ahi + 1) >> 1;

    sp = a;
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            int os = sp[x];
            /* blend outliers with the low and high bucket averages */
            if (os < t) {
                sp[x] = os + ((flo * (alo - os)) / 16);
            } else if (os > t) {
                sp[x] = os + ((fhi * (ahi - os)) / 16);
            }
        }
        sp += as;
    }
}

/* non-linearize texture */
static int
curve_tex(int tt)
{
    if (tt < 8) {
        return (8 - tt) * 8;
    }
    if (tt > 192) {
        return 0;
    }
    return (tt - (8 - 1));
}

#define SB_LH       1
#define SB_HL       2
#define SB_HH       3

/* larger dimensions -> higher freq is less important */
static int
spatial_psy_factor(DSV_PARAMS *p, int subband)
{
    int scale, lo, hi;
    /* between CIF and FHD */
    if (subband == SB_LH) {
        lo = DSV_UDIV_ROUND_UP(352, p->blk_w);
        hi = DSV_UDIV_ROUND_UP(1920, p->blk_w);
        scale = p->nblocks_h;
    } else if (subband == SB_HL) {
        lo = DSV_UDIV_ROUND_UP(288, p->blk_h);
        hi = DSV_UDIV_ROUND_UP(1080, p->blk_h);
        scale = p->nblocks_v;
    } else {
        lo = DSV_UDIV_ROUND_UP(352, p->blk_w) * DSV_UDIV_ROUND_UP(288, p->blk_h);
        hi = DSV_UDIV_ROUND_UP(1920, p->blk_w) * DSV_UDIV_ROUND_UP(1080, p->blk_h);
        scale = p->nblocks_h * p->nblocks_v;
    }
    scale = MAX(0, scale - lo);
    return (scale << 7) / (hi - lo);
}

static int
compute_filter_q(DSV_PARAMS *p, int q)
{
    int psyf = spatial_psy_factor(p, -1);
    if (q > 1536) {
        q = 1536;
    }
    q += q * psyf >> (7 + 3);
    if (q < 1024) {
        q = 512 + q / 2;
    }
    return q;
}

static void
intra_filter(int q, DSV_PARAMS *p, DSV_FMETA *fm, int c, DSV_PLANE *dp, int do_filter)
{
    int i, j, x, y;
    int nsbx, nsby;
    int fthresh;
    if (p->lossless) {
        return;
    }
    if (c != 0) {
        return;
    }
    if (!do_filter) {
        return;
    }
    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;
    q = compute_filter_q(p, q);
    fthresh = 32 * (14 - logb2(q));
    for (j = 0; j < nsby; j++) {
        int fy = (j * p->nblocks_v / nsby);
        y = j * FILTER_DIM;
        if ((y + FILTER_DIM) >= dp->h) {
            continue;
        }
        for (i = 0; i < nsbx; i++) {
            int fx = (i * p->nblocks_h / nsbx);
            int flags = fm->blockdata[fx + fy * p->nblocks_h];
            int tt = 32;

            x = i * FILTER_DIM;
            if ((x + FILTER_DIM) >= dp->w) {
                continue;
            }
            if (do_filter && !(flags & DSV_IS_RINGING)) {
                int sh, sv, shl, svl;
                artf4x4(DSV_GET_XY(dp, x, y), dp->stride, &sh, &sv, &shl, &svl);
                /* only filter blocks with significant texture */
                if ((MAX(sh, sv) < 256 && MAX(sh, sv) > 8)) {
                    if (flags & (DSV_IS_MAINTAIN | DSV_IS_STABLE)) {
                        tt = dsff4x4(DSV_GET_XY(dp, x, y), dp->stride);
                        if (flags & DSV_IS_STABLE) {
                            tt = tt * 5 >> 2;
                        }
                    } else {
                        tt >>= 2;
                    }
                    tt = (tt * 2 / 3);
                    tt = (tt * q) >> DSV_MAX_QP_BITS;
                    tt = CLAMP(tt, 0, fthresh);
                    ihfilter4x4(dp, x, y, 0, tt, tt);
                    ivfilter4x4(dp, x, y, 0, tt, tt);
                    if (sh > sv) {
                        tt = (3 * sh + sv);
                    } else {
                        tt = (3 * sv + sh);
                    }
                    tt = curve_tex(tt);
                    tt = 16 + ((tt + 2) >> 2);
                    tt = (tt * q) >> DSV_MAX_QP_BITS;
                    tt = CLAMP(tt, 0, fthresh);
                    ihfilter4x4(dp, x, y, 0, tt, tt);
                    ivfilter4x4(dp, x, y, 0, tt, tt);
                }
            }
        }
    }
}

static void
luma_filter(DSV_MV *vecs, int q, DSV_PARAMS *p, DSV_PLANE *dp, int do_filter)
{
    int i, j, x, y;
    int nsbx, nsby;
    int sharpen;
#define NDCACHE_INVALID -1
    /* x, y, neighbordif_x, neighbordif_y */
    int cached[4] = { NDCACHE_INVALID, NDCACHE_INVALID, NDCACHE_INVALID, NDCACHE_INVALID };
    int fthresh;

    if (p->vidmeta->inter_sharpen) {
        sharpen = p->temporal_mc; /* sharpen when the smoother half-pel filter was used */
    } else {
        sharpen = 0;
    }
    if (p->lossless) {
        return;
    }
    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;
    q = compute_filter_q(p, q);
    fthresh = 32 * (14 - logb2(q));
    for (j = 0; j < nsby; j++) {
        int edgev, edgevs, fy;

        fy = (j * p->nblocks_v / nsby);
        edgev = ((j * FILTER_DIM) % p->blk_h) == 0;
        edgevs = ((j * FILTER_DIM) % (p->blk_h / 2)) == 0;
        y = j * FILTER_DIM;
        if ((y + FILTER_DIM) >= dp->h) {
            continue;
        }
        for (i = 0; i < nsbx; i++) {
            int edgeh, edgehs, fx, ndx, ndy, amx, amy;
            DSV_MV *mv;
            uint8_t *dxy;

            fx = (i * p->nblocks_h / nsbx);
            edgeh = ((i * FILTER_DIM) % p->blk_w) == 0;
            edgehs = ((i * FILTER_DIM) % (p->blk_w / 2)) == 0;
            mv = &vecs[fx + fy * p->nblocks_h];

            x = i * FILTER_DIM;

            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            if ((x + FILTER_DIM) >= dp->w) {
                continue;
            }

            amx = abs(mv->u.mv.x);
            amy = abs(mv->u.mv.y);

            /* if current x,y is different than what was cached, update cache */
            if (do_filter && (fx != cached[0] || fy != cached[1] || cached[2] == NDCACHE_INVALID || cached[3] == NDCACHE_INVALID)) {
                neighdif2(vecs, p, fx, fy, &ndx, &ndy);

                cached[0] = fx;
                cached[1] = fy;
                cached[2] = ndx;
                cached[3] = ndy;
            } else {
                /* use cached value */
                ndx = cached[2];
                ndy = cached[3];
            }

            dxy = DSV_GET_XY(dp, x, y);
            if (DSV_MV_IS_INTRA(mv)) {
                int intra_threshH = ((64 * q) >> DSV_MAX_QP_BITS);
                int intra_threshL = ((32 * q) >> DSV_MAX_QP_BITS);
                int intra = DSV_MV_IS_INTRA(mv);
                int tedgeh = edgeh;
                int tedgev = edgev;
                intra_threshH = CLAMP(intra_threshH, 2, 32);
                intra_threshL = CLAMP(intra_threshL, 2, 32);
                if (intra && mv->submask != DSV_MASK_ALL_INTRA) {
                    tedgeh |= edgehs;
                    tedgev |= edgevs;
                }
                ihfilter4x4(dp, x, y, tedgeh, intra_threshH, intra_threshL);
                ivfilter4x4(dp, x, y, tedgev, intra_threshH, intra_threshL);
                continue;
            }
            if (do_filter && (ndx || ndy)) {
                int tt, addx, addy, sh, sv, shl, svl;
                int intra = DSV_MV_IS_INTRA(mv);
                int eprm = DSV_MV_IS_EPRM(mv);
                int tedgeh = edgeh || eprm;
                int tedgev = edgev || eprm;
                int tndc;
                if (intra && mv->submask != DSV_MASK_ALL_INTRA) {
                    tedgeh |= edgehs;
                    tedgev |= edgevs;
                }
                tndc = (ndx + ndy + 1) >> 1;
                artf4x4(dxy, dp->stride, &sh, &sv, &shl, &svl);

                if (sh < 2 * sv && sv < 2 * sh) {
                    int ix, iy;
                    if (ndx < amx) {
                        ndx >>= 1;
                    }
                    if (ndy < amy) {
                        ndy >>= 1;
                    }
                    shl = (shl > 128) ? 0 : (128 - shl);
                    svl = (svl > 128) ? 0 : (128 - svl);

                    ix = MIN(amx, 32);
                    iy = MIN(amy, 32);
                    /* interpolate between lower freq and higher freq energies */
                    tt  = ((sh * (32 - iy) + shl * iy) + 16) >> 5;
                    tt += ((sv * (32 - ix) + svl * ix) + 16) >> 5;
                    tt = (tt + 1) >> 1;
                    if (ndx < amy && ndy < amx) { /* neighbordif not significant enough */
                        tt = 0;
                    }
                } else {
                    tt = (sh + sv + 1) >> 1;
                }
                tt = (tt * tndc + 4) >> 3;

                tt = (MIN(tt, fthresh) * q) >> DSV_MAX_QP_BITS;
                addx = (MIN(ndy, fthresh) * q) >> DSV_MAX_QP_BITS;
                addy = (MIN(ndx, fthresh) * q) >> DSV_MAX_QP_BITS;

                if (sh > 2 * sv || amy > 2 * amx) {
                    ivfilter4x4(dp, x, y, tedgev, tt + addy, tt);
                } else if (sv > 2 * sh || amx > 2 * amy) {
                    ihfilter4x4(dp, x, y, tedgeh, tt + addx, tt);
                } else {
                    ihfilter4x4(dp, x, y, tedgeh, tt + addx, tt);
                    ivfilter4x4(dp, x, y, tedgev, tt + addy, tt);
                }
            }

            if (sharpen && DSV_IS_DIAG(mv) && DSV_IS_QPEL(mv) && amx < 8 && amy < 8) {
                degrad4x4(dxy, dp->stride);
            }
        }
    }
}

static void
chroma_filter(DSV_MV *vecs, int q, DSV_PARAMS *p, DSV_PLANE *dp)
{
    int i, j, x, y, bw, bh, sh, sv;
    int intra_thresh;
    DSV_MV *mv;

    sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
    sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);

    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;
    if (p->lossless) {
        return;
    }
    intra_thresh = ((64 * q) >> DSV_MAX_QP_BITS);
    intra_thresh = CLAMP(intra_thresh, 2, 32);

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            if (!DSV_MV_IS_SKIP(mv)) {
                int z, tx = intra_thresh, ty = intra_thresh;
                if (!DSV_MV_IS_INTRA(mv)) {
                    int ndx, ndy, amx, amy;
                    neighdif2(vecs, p, i, j, &ndx, &ndy);
                    amx = abs(mv->u.mv.x);
                    amy = abs(mv->u.mv.y);
                    if (ndx < amy && ndy < amx) { /* neighbordif not significant enough */
                        tx = ty = 0;
                    } else {
                        tx = ndy;
                        ty = ndx;
                        tx = (MIN(tx, 64) * q) >> DSV_MAX_QP_BITS;
                        ty = (MIN(ty, 64) * q) >> DSV_MAX_QP_BITS;
                    }
                }

                /* only filtering top and left sides */
                for (z = 0; z < bh; z += FILTER_DIM) {
                    if ((y + z + FILTER_DIM) < dp->h) {
                        ihfilter4x4(dp, x, y + z, 0, tx, tx);
                    }
                }
                for (z = 0; z < bw; z += FILTER_DIM) {
                    if ((x + z + FILTER_DIM) < dp->w) {
                        ivfilter4x4(dp, x + z, y, 0, ty, ty);
                    }
                }
            }
        }
    }
}

static void
luma_qp(uint8_t *dec, int ds,
        uint8_t *ref, int rs,
        int bw, int bh,
        int dx, int dy, int tmc)
{
    static int16_t tbuf[(DSV_MAX_BLOCK_SIZE + 3) * DSV_MAX_BLOCK_SIZE];
    int16_t *tmp;
    int x, y, a, b, c, d, f, large_mv, dqtx, dqty;
#define BF_SHIFT  (DSV_HP_SHF + 1)
#define BF_MULADD (1 << DSV_HP_SHF)
    tmp = tbuf;

    large_mv = abs(dx) >= 8 || abs(dy) >= 8;
    dx &= 3;
    dy &= 3;
    /* large motion OR half-pel OR (temporal MC flag % 2) gets the smoother filter */
    dqtx = large_mv || !(dx & 1) || (tmc & 1);
    dqty = large_mv || !(dy & 1) || (tmc & 1);

    /* subpixel filtering is done with two different filters for each direction:
     * A - less sharp
     * B - more sharp
     *
     * temporal MC allows alternating between sightly sharper and softer filters
     * every frame which generally averages out to a better approximation over
     * longer subpel motion sequences.
     */
    for (y = 0; y < bh + 3; y++) {
        for (x = 0; x < bw; x++) {
            a = ref[x + 0];
            b = ref[x + 1];
            c = ref[x + 2];
            d = ref[x + 3];
            if (dqtx) {
                f = DSV_HPF_A(a, b, c, d);
            } else {
                f = DSV_HPF_B(a, b, c, d);
            }
            /* linear blend */
            switch (dx) {
                case 0:
                    tmp[x] = (BF_MULADD * 2 * b + BF_MULADD) >> BF_SHIFT;
                    break;
                case 1:
                    tmp[x] = (f + BF_MULADD * b + BF_MULADD) >> BF_SHIFT;
                    break;
                case 2:
                    tmp[x] = (f * 2 + BF_MULADD) >> BF_SHIFT;
                    break;
                case 3:
                    tmp[x] = (f + BF_MULADD * c + BF_MULADD) >> BF_SHIFT;
                    break;
            }
        }
        tmp += DSV_MAX_BLOCK_SIZE;
        ref += rs;
    }
    tmp -= (bh + 3) * DSV_MAX_BLOCK_SIZE;

    for (y = 0; y < bh; y++) {
        for (x = 0; x < bw; x++) {
            a = tmp[x + 0 * DSV_MAX_BLOCK_SIZE];
            b = tmp[x + 1 * DSV_MAX_BLOCK_SIZE];
            c = tmp[x + 2 * DSV_MAX_BLOCK_SIZE];
            d = tmp[x + 3 * DSV_MAX_BLOCK_SIZE];
            if (dqty) {
                f = DSV_HPF_A(a, b, c, d);
            } else {
                f = DSV_HPF_B(a, b, c, d);
            }
            /* linear blend */
            switch (dy) {
                case 0:
                    dec[x] = clamp_u8((BF_MULADD * 2 * b + BF_MULADD) >> BF_SHIFT);
                    break;
                case 1:
                    dec[x] = clamp_u8((f + BF_MULADD * b + BF_MULADD) >> BF_SHIFT);
                    break;
                case 2:
                    dec[x] = clamp_u8((f * 2 + BF_MULADD) >> BF_SHIFT);
                    break;
                case 3:
                    dec[x] = clamp_u8((f + BF_MULADD * c + BF_MULADD) >> BF_SHIFT);
                    break;
            }
        }
        dec += ds;
        tmp += DSV_MAX_BLOCK_SIZE;
    }
}

/* D.1 Chroma Sub-Pixel Filter */
static void
bilinear_sp(
        uint8_t *dec, int ds,
        uint8_t *ref, int rs,
        int w, int h, int dx, int dy, int sh, int sv)
{
    int hf, vf, hbits, vbits;
    /* 1/4-pel for no subsamp,
     * 1/8-pel for half resolution,
     * 1/16-pel for quarter resolution */
    hbits = (2 + sh);
    vbits = (2 + sv);
    hf = 1 << hbits;
    vf = 1 << vbits;
    dx &= hf - 1;
    dy &= vf - 1;

    if (dx | dy) {
        int x, y, f0, f1, f2, f3, af, sf;

        f0 = (hf - dx) * (vf - dy);
        f1 = dx * (vf - dy);
        f2 = (hf - dx) * dy;
        f3 = dx * dy;

        sf = (hbits + vbits);
        af = 1 << (sf - 1);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                dec[x] = (f0 * ref[x] +
                          f1 * ref[x + 1] +
                          f2 * ref[rs + x] +
                          f3 * ref[rs + x + 1] + af) >> sf;
            }
            dec += ds;
            ref += rs;
        }
    } else {
        cpyblk(dec, ref, ds, rs, w, h);
    }
}

static void
reconstruct(DSV_MV *vecs, DSV_PARAMS *p, int c, DSV_PLANE *refp, DSV_PLANE *resp, DSV_PLANE *outp)
{
    int i, j, r, x, y, bw, bh, sh, sv, limx, limy;
    DSV_MV *mv;
    static uint8_t temp[DSV_MAX_BLOCK_SIZE * DSV_MAX_BLOCK_SIZE];

    if (c == 0) {
        sh = 0;
        sv = 0;
    } else {
        sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
        sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    }
    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;

    limx = (outp->w - bw) + DSV_FRAME_BORDER - 1;
    limy = (outp->h - bh) + DSV_FRAME_BORDER - 1;

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            int m, n;
            int dx, dy, px, py;
            uint8_t *res, *out, *pred;

            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            res = DSV_GET_XY(resp, x, y);
            out = DSV_GET_XY(outp, x, y);
            pred = temp;

            dx = DSV_SAR(mv->u.mv.x, sh);
            dy = DSV_SAR(mv->u.mv.y, sv);

            px = x + DSV_SAR(dx, 2);
            py = y + DSV_SAR(dy, 2);

            if (DSV_MV_IS_INTRA(mv)) {
                /* D.2 Compensating Intra Blocks */
                uint8_t *dec;
                int avgc;

                px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                py = CLAMP(py, -DSV_FRAME_BORDER, limy);

                if (mv->submask == DSV_MASK_ALL_INTRA) {
                    if (c == 0 && mv->dc) { /* DC is only for luma */
                        avgc = mv->dc;
                    } else {
                        avgc = avgval(DSV_GET_XY(refp, px, py), refp->stride, bw, bh);
                    }
                    dec = temp;
                    for (r = 0; r < bh; r++) {
                        memset(dec, avgc, bw);
                        dec += DSV_MAX_BLOCK_SIZE;
                    }
                } else {
                    int f, g, sbw, sbh, mask_index;
                    uint8_t masks[4] = {
                            DSV_MASK_INTRA00,
                            DSV_MASK_INTRA01,
                            DSV_MASK_INTRA10,
                            DSV_MASK_INTRA11,
                    };
                    sbw = bw / 2;
                    sbh = bh / 2;
                    mask_index = 0;

                    for (g = 0; g <= sbh; g += sbh) {
                        for (f = 0; f <= sbw; f += sbw) {
                            dec = temp + f + g * DSV_MAX_BLOCK_SIZE;
                            if (mv->submask & masks[mask_index]) {
                                if (c == 0 && mv->dc) { /* DC is only for luma */
                                    avgc = mv->dc;
                                } else {
                                    avgc = avgval(DSV_GET_XY(refp, px + f, py + g), refp->stride, sbw, sbh);
                                }

                                for (r = 0; r < sbh; r++) {
                                    memset(dec, avgc, sbw);
                                    dec += DSV_MAX_BLOCK_SIZE;
                                }
                            } else {
                                cpyblk(dec,
                                       DSV_GET_XY(refp, px + f, py + g),
                                       DSV_MAX_BLOCK_SIZE, refp->stride, sbw, sbh);
                            }
                            mask_index++;
                        }
                    }
                }
            } else { /* inter */
                /* D.1 Compensating Inter Blocks */
                if (c == 0) {
                    if (!DSV_IS_SUBPEL(mv)) {
                        px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                        py = CLAMP(py, -DSV_FRAME_BORDER, limy);
                        cpyblk(temp, DSV_GET_XY(refp, px, py), DSV_MAX_BLOCK_SIZE, refp->stride, bw, bh);
                    } else {
                        px = CLAMP(px - 1, -DSV_FRAME_BORDER, limx);
                        py = CLAMP(py - 1, -DSV_FRAME_BORDER, limy);
                        luma_qp(temp, DSV_MAX_BLOCK_SIZE,
                                DSV_GET_XY(refp, px, py), refp->stride,
                                bw, bh, mv->u.mv.x, mv->u.mv.y, p->temporal_mc);
                    }
                } else {
                    px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                    py = CLAMP(py, -DSV_FRAME_BORDER, limy);
                    bilinear_sp(temp, DSV_MAX_BLOCK_SIZE, DSV_GET_XY(refp, px, py), refp->stride, bw, bh, mv->u.mv.x, mv->u.mv.y, sh, sv);
                }
            }
            pred = temp;
            if (p->lossless) {
                for (n = 0; n < bh; n++) {
                    for (m = 0; m < bw; m++) {
                        out[m] = (pred[m] + res[m] - 128);
                    }
                    out += outp->stride;
                    pred += DSV_MAX_BLOCK_SIZE;
                    res += resp->stride;
                }
            } else {
                /* D.4 Reconstruction */
                if (!DSV_MV_IS_EPRM(mv) || (!DSV_MV_IS_INTRA(mv) && DSV_MV_IS_SKIP(mv))) {
                    for (n = 0; n < bh; n++) {
                        for (m = 0; m < bw; m++) {
                            /* source = (prediction + residual) */
                            out[m] = clamp_u8(pred[m] + res[m] - 128);
                        }
                        out += outp->stride;
                        pred += DSV_MAX_BLOCK_SIZE;
                        res += resp->stride;
                    }
                } else {
                    for (n = 0; n < bh; n++) {
                        for (m = 0; m < bw; m++) {
                            out[m] = clamp_u8(pred[m] + (res[m] - 128) * 2);
                        }
                        out += outp->stride;
                        pred += DSV_MAX_BLOCK_SIZE;
                        res += resp->stride;
                    }
                }
            }
        }
    }
}

static void
add_pred(DSV_MV *mv, DSV_FMETA *fm, int q, DSV_FRAME *resd, DSV_FRAME *out, DSV_FRAME *ref, int do_filter)
{
    DSV_PLANE *rp, *op;
    int c;

    for (c = 0; c < 3; c++) {
        rp = resd->planes + c;
        op = out->planes + c;

        reconstruct(mv, fm->params, c, ref->planes + c, rp, op);
        if (c == 0) {
            luma_filter(mv, q, fm->params, op, do_filter);
        } else {
            chroma_filter(mv, q, fm->params, op);
        }
    }
}

/************************************ HZCC ************************************/

/* Hierarchical Zero Coefficient Coding */

#define EOP_SYMBOL 0x55 /* B.2.3.5 Image Data - Coefficient Coding */

#define MAXLVL   3
#define NSUBBAND 4 /* 0 = LL, 1 = LH, 2 = HL, 3 = HH */

#define MINQP    3
#define MINQUANT (1 << MINQP)  /* C.2 MINQUANT */

#define RUN_BITS 24

/* C.1 Subband Order and Traversal */
static int
subband(int level, int sub, int w, int h)
{
    int offset = 0;
    if (sub & 1) { /* L */
        offset += DSV_ROUND_SHIFT(w, MAXLVL - level);
    }
    if (sub & 2) { /* H */
        offset += DSV_ROUND_SHIFT(h, MAXLVL - level) * w;
    }
    return offset;
}

/* C.1 Subband Order and Traversal */
static int
dimat(int level, int v) /* dimension at level */
{
    return DSV_ROUND_SHIFT(v, MAXLVL - level);
}

static int
fix_quant(int q)
{
    return q * 3 / 2;
}

static int
lfquant(int q, DSV_FMETA *fm)
{
    int psyfac;

    psyfac = spatial_psy_factor(fm->params, SB_HH);

    q -= (q * psyfac >> (7 + 3));
    q = MAX(q, MINQUANT);
    /* prevent important lower level coefficients from getting destroyed */
    if (fm->cur_plane) {
        if (q > 256) {
            q = 256 + q / 4;
        }
        return MIN(q, 768);
    }
    return MIN(q, 3072);
}

static int
hfquant(DSV_FMETA *fm, int q, int s, int l)
{
    int psyfac, chroma;

    chroma = (fm->cur_plane != 0);
    psyfac = spatial_psy_factor(fm->params, s);
    q /= 2;

    psyfac = q * psyfac >> (7 + (fm->isP ? 0 : 1));

    if (chroma) {
        /* reduce based on subsampling */
        int tl;
        tl = l - 2;
        if (s == SB_LH) {
            tl += DSV_FORMAT_H_SHIFT(fm->params->vidmeta->subsamp);
        } else if (s == SB_HL) {
            tl += DSV_FORMAT_V_SHIFT(fm->params->vidmeta->subsamp);
        }
        q = (q * 6) / (4 - tl);
    } else {
        /* reduce higher frequencies appropriately */
        if (l == (MAXLVL - 2)) {
            q += psyfac / 2;
        } else if (l == (MAXLVL - 1)) {
            q += psyfac;
        }
    }

    if (fm->isP) {
        if (l != (MAXLVL - 1)) {
            if (l == (MAXLVL - 3)) {
                q *= 2;
                q -= psyfac;
            } else {
                q -= psyfac / 2;
            }
        }
        return MAX(q / 4, MINQUANT);
    }
    q = q * (15 + 3 * l) / 16;
    if (!chroma) {
        if (l == (MAXLVL - 3)) { /* luma level 3 (Haar) needs to be quantized less */
            q = (q * 3) / 8;
        } else if (s == SB_HH) { /* quantize HH more */
            q *= 2;
        }
    } else {
        q /= 4;
        if (s == SB_HH) { /* quantize HH more */
            q *= 2;
        }
    }
    return MAX(q, MINQUANT);
}

#define TMQ4POS_P(tmq, flags)                                     \
    if (parc || (((flags) & (DSV_IS_STABLE | DSV_IS_EPRM)))) {    \
        tmq = (tmq) * 7 >> 3;                                     \
    } else if (!parc && ((flags) & DSV_IS_INTRA)) {               \
        tmq = (tmq) * 6 >> 3;                                     \
    }

#define TMQ4POS_I(tmq, flags, l)                                         \
        switch (l) {                                                     \
            case MAXLVL - 3:                                             \
                break;                                                   \
            default:                                                     \
            case MAXLVL - 2:                                             \
                switch ((flags) & (DSV_IS_STABLE | DSV_IS_MAINTAIN)) {   \
                    case DSV_IS_STABLE:                                  \
                        tmq /= 3;                                        \
                        break;                                           \
                    case DSV_IS_MAINTAIN:                                \
                        tmq >>= (((flags) & DSV_IS_RINGING) ? 2 : !parc);\
                        break;                                           \
                    case DSV_IS_MAINTAIN | DSV_IS_STABLE:                \
                        tmq >>= 2;                                       \
                        break;                                           \
                    default:                                             \
                        break;                                           \
                }                                                        \
                break;                                                   \
            case MAXLVL - 1:                                             \
                switch ((flags) & (DSV_IS_STABLE | DSV_IS_MAINTAIN)) {   \
                    case DSV_IS_STABLE:                                  \
                        tmq >>= 2;                                       \
                        break;                                           \
                    case DSV_IS_MAINTAIN:                                \
                        tmq >>= (((flags) & DSV_IS_RINGING) ? 2 : !parc);\
                        break;                                           \
                    case DSV_IS_MAINTAIN | DSV_IS_STABLE:                \
                        tmq >>= 2 + !parc;                               \
                        break;                                           \
                    default:                                             \
                        break;                                           \
                }                                                        \
                break;                                                   \
        }


#define dequantL(v,q) (isP ? dequantD(v,q) : dequantS(v,q))
#define dequantH(v,q) (dequantD(v, q))

/* uses estimator which saturates the value more */
static DSV_SBC
dequantS(int v, unsigned q)
{
    return (v * q) + ((v < 0) ? -(q * 2 / 3) : (q * 2 / 3));
}

/* default estimator */
static DSV_SBC
dequantD(int v, unsigned q)
{
    return (v * q) + ((v < 0) ? -(q / 2) : (q / 2));
}

#define DAMP (3 + l)
#define GETV(bs)     (bs_get_nrice(bs, &vk, DAMP))

static void
hzcc_dec(DSV_BS *bs, unsigned bufsz, DSV_COEFS *dst, int q, DSV_FMETA *fm)
{
    int x, y, l, s, o, v;
    int qp;
    int sw, sh;
    int bx, by;
    int dbx, dby;
    int run, runs;
    DSV_SBC *out = dst->data;
    DSV_SBC *outp;
    int w = dst->width;
    int h = dst->height;
    int isP;
    int vk = 0;

    bs_align(bs);
    runs = bs_get_bits(bs, RUN_BITS);
    bs_align(bs);

    q = fix_quant(q);
    s = l = 0;
    isP = fm->isP;

    sw = dimat(l, w);
    sh = dimat(l, h);
    qp = lfquant(q, fm);

    o = subband(l, s, w, h);
    outp = out + o;

    run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;

    if (fm->params->lossless) {
        for (y = 0; y < sh; y++) {
            for (x = 0; x < sw; x++) {
                if (!run--) {
                    v = bs_get_neg(bs);
                    run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                    if (bs_ptr(bs) >= bufsz) {
                        return;
                    }
                    outp[x] = v;
                }
            }
            outp += w;
        }
        for (l = 0; l < MAXLVL; l++) {
            sw = dimat(l, w);
            sh = dimat(l, h);
            for (s = 1; s < NSUBBAND; s++) {
                o = subband(l, s, w, h);
                outp = out + o;
                for (y = 0; y < sh; y++) {
                    for (x = 0; x < sw; x++) {
                        if (!run--) {
                            v = GETV(bs);
                            run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                            if (bs_ptr(bs) >= bufsz) {
                                return;
                            }
                            outp[x] = v;
                        }
                    }
                    outp += w;
                }
            }
        }
    } else {
        for (y = 0; y < sh; y++) {
            for (x = 0; x < sw; x++) {
                if (!run--) {
                    v = bs_get_neg(bs);
                    run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                    if (bs_ptr(bs) >= bufsz) {
                        return;
                    }
                    outp[x] = dequantL(v, qp);
                }
            }
            outp += w;
        }
        for (l = 0; l < MAXLVL; l++) {
            uint8_t *blockrow;
            DSV_SBC *parent;

            sw = dimat(l, w);
            sh = dimat(l, h);
            dbx = (fm->params->nblocks_h << DSV_BLOCK_INTERP_P) / sw;
            dby = (fm->params->nblocks_v << DSV_BLOCK_INTERP_P) / sh;
            qp = q;
            for (s = 1; s < NSUBBAND; s++) {
                int par;
                par = subband(l - 1, s, w, h);
                o = subband(l, s, w, h);
                qp = hfquant(fm, q, s, l);

                outp = out + o;
                by = 0;
                for (y = 0; y < sh; y++) {
                    bx = 0;
                    blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                    parent = out + par + ((y >> 1) * w);
                    for (x = 0; x < sw; x++) {
                        if (!run--) {
                            int tmq = qp;
                            int flags = blockrow[bx >> DSV_BLOCK_INTERP_P];
                            int parc = parent[x >> 1];
                            v = GETV(bs);
                            run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                            if (bs_ptr(bs) >= bufsz) {
                                return;
                            }
                            if (isP) {
                                TMQ4POS_P(tmq, flags);
                            } else {
                                TMQ4POS_I(tmq, flags, l);
                            }
                            outp[x] = dequantH(v, tmq);
                        }
                        bx += dbx;
                    }
                    outp += w;
                    by += dby;
                }
            }
        }
    }
    bs_align(bs);
}

/* B.2.3.5 Image Data - Coefficient Decoding */
static int
decode_plane(DSV_BS *bs, DSV_COEFS *dst, int q, DSV_FMETA *fm)
{
    int success = 1;
    unsigned plen;

    bs_align(bs);

    plen = bs_get_bits(bs, 32);

    bs_align(bs);
    if (plen > 0 && plen < (dst->width * dst->height * sizeof(DSV_SBC) * 2)) {
        DSV_SBC LL;
        unsigned start = bs_ptr(bs);

        LL = bs_get_seg(bs);
        hzcc_dec(bs, start + plen, dst, q, fm);
        dst->data[0] = LL;

        /* error detection */
        if (bs_get_bits(bs, 8) != EOP_SYMBOL) {
            DSV_ERROR(("bad eop, frame data incomplete and/or corrupt"));
            success = 0;
        }
        bs_align(bs);

        bs_set(bs, start);
        bs_skip(bs, plen);
    } else {
        DSV_ERROR(("plane length was strange: %d", plen));
        success = 0;
    }
    return success;
}

/********************************** DECODER ***********************************/

/* B.1 Packet Header */
static int
decode_packet_hdr(DSV_BS *bs)
{
    int c0, c1, c2, c3;
    int pkt_type;
    int ver_min;

    c0 = bs_get_bits(bs, 8);
    c1 = bs_get_bits(bs, 8);
    c2 = bs_get_bits(bs, 8);
    c3 = bs_get_bits(bs, 8);
    if (c0 != DSV_FOURCC_0 || c1 != DSV_FOURCC_1 || c2 != DSV_FOURCC_2 || c3 != DSV_FOURCC_3) {
        DSV_ERROR(("bad 4cc (%c %c %c %c)\n", c0, c1, c2, c3));
        return -1;
    }

    ver_min = bs_get_bits(bs, 8);
    DSV_DEBUG(("version 2.%d", ver_min));

    /* B.1.1 Packet Type */
    pkt_type = bs_get_bits(bs, 8);
    DSV_DEBUG(("packet type %02x", pkt_type));
    /* link offsets */
    bs_get_bits(bs, 32);
    bs_get_bits(bs, 32);

    return pkt_type;
}

/* B.2.1 Metadata Packet */
static void
decode_meta(DSV_DECODER *d, DSV_BS *bs)
{
    DSV_META *fmt = &d->vidmeta;

    fmt->width = bs_get_ueg(bs);
    fmt->height = bs_get_ueg(bs);
    DSV_DEBUG(("dimensions = %d x %d", fmt->width, fmt->height));

    fmt->subsamp = bs_get_ueg(bs);
    DSV_DEBUG(("subsamp %d", fmt->subsamp));

    fmt->fps_num = bs_get_ueg(bs);
    fmt->fps_den = bs_get_ueg(bs);
    DSV_DEBUG(("fps %d/%d", fmt->fps_num, fmt->fps_den));

    fmt->aspect_num = bs_get_ueg(bs);
    fmt->aspect_den = bs_get_ueg(bs);
    DSV_DEBUG(("aspect ratio %d/%d", fmt->aspect_num, fmt->aspect_den));

    fmt->inter_sharpen = bs_get_ueg(bs);
    DSV_DEBUG(("inter sharpen %d", fmt->inter_sharpen));
    if (bs_get_bit(bs)) {
        fmt->reserved = bs_get_bits(bs, 15);
    } else {
        fmt->reserved = 0;
    }
}

/* B.2.3.4 Motion Data */
static void
decode_motion(DSV_IMAGE *img, DSV_MV *mvs, DSV_BS *inbs, DSV_BUF *buf, int *stats)
{
    DSV_PARAMS *params = &img->params;
    DSV_BS bs[DSV_SUB_NSUB];
    DSV_ZBRLE rle, prrle;
    int i, j;

    bs_align(inbs);

    for (i = 0; i < DSV_SUB_NSUB; i++) {
        int len;

        len = bs_get_ueg(inbs);
        bs_align(inbs);

        if (i == DSV_SUB_MODE) {
            bs_init_rle(&rle, buf->data + bs_ptr(inbs));
        } else if (i == DSV_SUB_EPRM) {
            bs_init_rle(&prrle, buf->data + bs_ptr(inbs));
        } else {
            bs_init(bs + i, buf->data + bs_ptr(inbs));
        }

        bs_skip(inbs, len);
    }

    for (j = 0; j < params->nblocks_v; j++) {
        for (i = 0; i < params->nblocks_h; i++) {
            DSV_MV *mv;
            int idx;
            idx = i + j * params->nblocks_h;
            mv = &mvs[idx];

            if (img->blockdata[idx] & DSV_IS_SKIP) {
                DSV_MV_SET_SKIP(mv, 1);
                mv->u.all = 0;
                img->blockdata[idx] |= (1 << DSV_STABLE_BIT);
            } else {
                /* B.2.3.4 Motion Data - Motion Vector Prediction */
                int mode, eprm, px, py;

                DSV_MV_SET_SKIP(mv, 0);

                mode = bs_get_rle(&rle);
                eprm = bs_get_rle(&prrle);
                if (stats[DSV_MODE_STAT] == DSV_ZERO_MARKER) {
                    mode = !mode;
                }
                if (stats[DSV_EPRM_STAT] == DSV_ZERO_MARKER) {
                    eprm = !eprm;
                }

                DSV_MV_SET_INTRA(mv, mode);
                DSV_MV_SET_EPRM(mv, eprm);
                img->blockdata[idx] &= ~(1 << DSV_STABLE_BIT);
                img->blockdata[idx] |= eprm << DSV_EPRM_BIT;

                mv_pred(mvs, params, i, j, &px, &py);
                if (DSV_MV_IS_INTRA(mv)) {
                    px = DSV_SAR_R(px, 2);
                    py = DSV_SAR_R(py, 2);
                }
                mv->u.mv.x = bs_get_seg(bs + DSV_SUB_MV_X) + px;
                mv->u.mv.y = bs_get_seg(bs + DSV_SUB_MV_Y) + py;
                if (DSV_MV_IS_INTRA(mv)) {
                    mv->u.mv.x *= 4; /* rescale to qpel */
                    mv->u.mv.y *= 4;

                    /* B.2.3.4 Motion Data - Intra Sub-Block Mask Decoding */
                    if (bs_get_bit(bs + DSV_SUB_SBIM)) {
                        mv->submask = DSV_MASK_ALL_INTRA;
                    } else {
                        mv->submask = bs_get_bits(bs + DSV_SUB_SBIM, 4);
                    }
                    if (bs_get_bit(bs + DSV_SUB_SBIM)) {
                        mv->dc = bs_get_bits(bs + DSV_SUB_SBIM, 8) | DSV_SRC_DC_PRED;
                    } else {
                        mv->dc = 0;
                    }
                    img->blockdata[idx] |= DSV_IS_INTRA;
                }
                if (neighdif(mvs, params, i, j) > DSV_NDIF_THRESH) {
                    img->blockdata[idx] |= (1 << DSV_STABLE_BIT);
                }

            }
        }
    }

    bs_end_rle(&rle);
    bs_end_rle(&prrle);
}

/* B.2.3.1 Stability Blocks */
static void
decode_stability_blocks(DSV_IMAGE *img, DSV_BS *inbs, DSV_BUF *buf, int isP, int *stats)
{
    DSV_PARAMS *params = &img->params;
    DSV_ZBRLE qualrle;
    int i, nblk, len;
    int shift = (isP ? DSV_SKIP_BIT : DSV_STABLE_BIT);

    bs_align(inbs);
    len = bs_get_ueg(inbs);
    bs_align(inbs);
    bs_init_rle(&qualrle, buf->data + bs_ptr(inbs));
    bs_skip(inbs, len);
    nblk = params->nblocks_h * params->nblocks_v;
    for (i = 0; i < nblk; i++) {
        int bit = bs_get_rle(&qualrle);
        if (stats[DSV_STABLE_STAT] == DSV_ZERO_MARKER) {
            bit = !bit;
        }
        img->blockdata[i] = bit << shift;
    }
    bs_end_rle(&qualrle);
}

/* B.2.3.2 Ringing Blocks & B.2.3.3 Maintain Blocks */
static void
decode_intra_meta(DSV_IMAGE *img, DSV_BS *inbs, DSV_BUF *buf, int *stats)
{
    DSV_PARAMS *params = &img->params;
    DSV_ZBRLE rle_r; /* ringing bits */
    DSV_ZBRLE rle_m; /* maintain bits */
    int i, nblk, len;

    bs_align(inbs);
    len = bs_get_ueg(inbs);
    bs_align(inbs);
    bs_init_rle(&rle_r, buf->data + bs_ptr(inbs));
    bs_skip(inbs, len);

    bs_align(inbs);
    len = bs_get_ueg(inbs);
    bs_align(inbs);
    bs_init_rle(&rle_m, buf->data + bs_ptr(inbs));
    bs_skip(inbs, len);

    nblk = params->nblocks_h * params->nblocks_v;
    for (i = 0; i < nblk; i++) {
        int bitr, bitm;

        bitr = bs_get_rle(&rle_r);
        bitm = bs_get_rle(&rle_m);
        if (stats[DSV_RINGING_STAT] == DSV_ZERO_MARKER) {
            bitr = !bitr;
        }
        if (stats[DSV_MAINTAIN_STAT] == DSV_ZERO_MARKER) {
            bitm = !bitm;
        }
        img->blockdata[i] |= (bitm << DSV_MAINTAIN_BIT);
        img->blockdata[i] |= (bitr << DSV_RINGING_BIT);
    }
    bs_end_rle(&rle_r);
    bs_end_rle(&rle_m);
}

static void
img_unref(DSV_IMAGE *img)
{
    DSV_ASSERT(img && img->refcount > 0);
    img->refcount--;

    if (img->refcount != 0) {
        return;
    }
    if (img->blockdata) {
        d28_free(img->blockdata);
        img->blockdata = NULL;
    }
    if (img->out_frame) {
        d28_frame_ref_dec(img->out_frame);
    }
    if (img->ref_frame) {
        d28_frame_ref_dec(img->ref_frame);
    }
    d28_free(img);
}

extern void
d28_dec_free(DSV_DECODER *d)
{
    if (d->ref) {
        img_unref(d->ref);
    }
}

extern DSV_META *
d28_get_metadata(DSV_DECODER *d)
{
    DSV_META *meta;

    meta = d28_alloc(sizeof(DSV_META));
    memcpy(meta, &d->vidmeta, sizeof(DSV_META));

    return meta;
}

extern int
d28_dec(DSV_DECODER *d, DSV_BUF *buffer, DSV_FRAME **out, DSV_FNUM *fn)
{
    DSV_BS bs;
    DSV_IMAGE *img;
    DSV_PARAMS *p;
    int i, quant, is_ref, has_ref, pkt_type, subsamp, do_filter;
    DSV_META *meta = &d->vidmeta;
    DSV_FRAME *residual;
    DSV_MV *mvs = NULL;
    DSV_FNUM fno;
    DSV_FMETA fm;
    int stats[DSV_MAX_STAT];
    DSV_COEFS coefs[3];

    *fn = -1;

    bs_init(&bs, buffer->data);
    pkt_type = decode_packet_hdr(&bs);

    if (pkt_type == -1) {
        d28_buf_free(buffer);
        return DSV_DEC_ERROR;
    }

    if (!DSV_PT_IS_PIC(pkt_type)) {
        int ret = DSV_DEC_ERROR;
        switch (pkt_type) {
            case DSV_PT_META:
                DSV_DEBUG(("decoding metadata"));
                decode_meta(d, &bs);
                d->got_metadata = 1;
                ret = DSV_DEC_GOT_META;
                break;
            case DSV_PT_EOS:
                DSV_DEBUG(("decoding end of stream"));
                ret = DSV_DEC_EOS;
                break;
        }
        d28_buf_free(buffer);
        return ret;
    }

    if (!d->got_metadata) {
        DSV_WARNING(("no metadata, skipping frame"));
        d28_buf_free(buffer);
        return DSV_DEC_OK;
    }

    img = d28_alloc(sizeof(DSV_IMAGE));
    img->refcount = 1;

    img->params.vidmeta = meta;

    subsamp = meta->subsamp;

    p = &img->params;
    has_ref = DSV_PT_HAS_REF(pkt_type);
    is_ref = DSV_PT_IS_REF(pkt_type);

    /* B.2.3 Picture Packet */

    /* read frame number */
    bs_align(&bs);
    fno = bs_get_bits(&bs, 32);

    /* read block sizes */
    bs_align(&bs);
    p->blk_w = 16 << bs_get_ueg(&bs);
    p->blk_h = 16 << bs_get_ueg(&bs);

    if (p->blk_w < DSV_MIN_BLOCK_SIZE || p->blk_h < DSV_MIN_BLOCK_SIZE ||
        p->blk_w > DSV_MAX_BLOCK_SIZE || p->blk_h > DSV_MAX_BLOCK_SIZE) {
        d28_buf_free(buffer);
        return DSV_DEC_ERROR;
    }
    p->nblocks_h = DSV_UDIV_ROUND_UP(meta->width, p->blk_w);
    p->nblocks_v = DSV_UDIV_ROUND_UP(meta->height, p->blk_h);

    /* read statistics bits + filter flag + quant */
    bs_align(&bs);
    memset(stats, DSV_ONE_MARKER, sizeof(stats));

    stats[DSV_STABLE_STAT] = bs_get_bit(&bs);
    if (!has_ref) {
        stats[DSV_MAINTAIN_STAT] = bs_get_bit(&bs);
        stats[DSV_RINGING_STAT] = bs_get_bit(&bs);
    } else {
        stats[DSV_MODE_STAT] = bs_get_bit(&bs);
        stats[DSV_EPRM_STAT] = bs_get_bit(&bs);
    }
    do_filter = bs_get_bit(&bs);
    quant = bs_get_bits(&bs, DSV_MAX_QP_BITS);
    p->lossless = (quant == 1);
    if (bs_get_bit(&bs)) {
        p->reserved = bs_get_bits(&bs, 15);
    } else {
        p->reserved = 0;
    }
    bs_align(&bs);
    /* read frame metadata (stability / skip, motion data / adaptive quant) */
    img->blockdata = d28_alloc(p->nblocks_h * p->nblocks_v);
    decode_stability_blocks(img, &bs, buffer, has_ref, stats);
    if (has_ref) {
        mvs = d28_alloc(sizeof(DSV_MV) * p->nblocks_h * p->nblocks_v);
        decode_motion(img, mvs, &bs, buffer, stats);
    } else {
        decode_intra_meta(img, &bs, buffer, stats);
    }

    /* B.2.3.5 Image Data */
    bs_align(&bs);

    residual = d28_mk_frame(subsamp, meta->width, meta->height, 1);
    fm.params = p;
    fm.blockdata = img->blockdata;
    fm.isP = has_ref;
    fm.fnum = fno;
    /* B.2.3.5 Image Data - Plane Decoding */
    mk_coefs(coefs, subsamp, meta->width, meta->height);

    for (i = 0; i < 3; i++) {
        fm.cur_plane = i;
        if (decode_plane(&bs, &coefs[i], quant, &fm)) {
            inv_sbt(&residual->planes[i], &coefs[i], quant, &fm);
            if (!fm.isP) {
                intra_filter(quant, p, &fm, i, &residual->planes[i], do_filter);
            }
        } else {
            DSV_ERROR(("decoding error in plane %d", i));
        }
    }

    *fn = fno;

    img->refcount++;

    if (!img->out_frame) {
        img->out_frame = d28_mk_frame(subsamp, meta->width, meta->height, 1);
    }
    if (has_ref) {
        DSV_IMAGE *ref = d->ref;
        if (ref == NULL) {
            DSV_WARNING(("reference frame not found"));
            return DSV_DEC_ERROR;
        }

        p->temporal_mc = DSV_TEMPORAL_MC(fno);
        add_pred(mvs, &fm, quant, residual, img->out_frame, ref->ref_frame, do_filter);
    } else {
        d28_frame_copy(img->out_frame, residual);
    }

    if (is_ref) {
        img->ref_frame = d28_extend_frame(d28_frame_ref_inc(img->out_frame));
    }

    /* release resources */
    if (coefs[0].data) { /* only the first pointer is actual allocated data */
        d28_free(coefs[0].data);
        coefs[0].data = NULL;
    }
    if (is_ref) {
        if (d->ref) {
            img_unref(d->ref);
        }
        img->refcount++;
        d->ref = img;
    }

    d28_frame_ref_dec(residual);
    if (mvs) {
        d28_free(mvs);
    }
    if (buffer) {
        d28_buf_free(buffer);
    }

    img_unref(img);

    *out = d28_frame_ref_inc(img->out_frame);

    img_unref(img);
    return DSV_DEC_OK;
}

#endif /* dsv2 impl guard */
#endif /* dsv2 impl */

#ifdef __cplusplus
}
#endif

#endif
