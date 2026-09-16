/*****************************************************************************/
/*
 * Digital Subband Video 2.8 Single-Header Decoder Implementation
 *   DSV-2
 *
 *     -
 *    =--  2024-2026 EMMIR
 *   ==---  Envel Graphics
 *  ===----
 *
 *   GitHub : https://github.com/LMP88959
 *   YouTube: https://www.youtube.com/@EMMIR_KC/videos
 *   Discord: https://discord.com/invite/hdYctSmyQJ
 */
/*****************************************************************************/
/*
 * This software was designed and written by EMMIR, 2024-2026 of Envel Graphics
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
 * d28_dec_init
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

#define DSV_MIN_FILTER_STR -3
#define DSV_DEF_FILTER_STR  0
#define DSV_MAX_FILTER_STR  3
    int filter_strength;

    /* 16 bits: reserved for potential future use
     * 1st bit: 0 = no reserved bits, 1 = has reserved bits
     * next 15 bits: reserved bits
     */
#define DSV_META_COLORSPACE_BIT (1 << 0)
/* rest of bits currently undefined */
    int reserved;
#define DSV_COLORSPACE_UNDEF      0 /* undefined */
#define DSV_COLORSPACE_BT601      1
#define DSV_COLORSPACE_BT709      2
#define DSV_COLORSPACE_BT2020     3
#define DSV_COLORSPACE_BT470      4
#define DSV_COLORSPACE_BC2        5
#define DSV_COLORSPACE_RESERVED_0 6
#define DSV_COLORSPACE_RESERVED_1 7
#define DSV_COLORSPACE_RESERVED_2 8
#define DSV_COLORSPACE_RESERVED_3 9
#define DSV_COLORSPACE_RESERVED_4 10
#define DSV_COLORSPACE_RESERVED_5 11

#define DSV_COLORSPACE_FULLRANGE (1 << 4)
    int colorspace;
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

/* subband coefs */
typedef int32_t DSV_SBC;

typedef struct {
    DSV_META vidmeta;
    DSV_IMAGE *ref;
    unsigned transform_buf_sz;
    DSV_SBC *transform_buf;

    int got_metadata;
    int quant;
} DSV_DECODER;

typedef struct {
    uint8_t *data;
    unsigned len;
} DSV_BUF;

#define DSV_DEC_OK        0
#define DSV_DEC_ERROR     1
#define DSV_DEC_EOS       2
#define DSV_DEC_GOT_META  3

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

extern void d28_dec_init(DSV_DECODER *d);
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

extern void *d28_alloc(int32_t size);
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
      printf("%s(%d): ",  __FUNCTION__, __LINE__); \
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
#define DSV_ROUND_POW2(x, pwr) (((unsigned)(x) + ((unsigned) 1 << (pwr)) - (unsigned) 1) & ~(((unsigned) 1 << (pwr)) - (unsigned) 1))
#define DSV_UDIV_ROUND_UP(a,b) (((a) + (b) - 1) / (b))
#define DSV_UAVG4(a, b, c, d) ((unsigned) ((a) + (b) + (c) + (d) + 2) >> 2)
#define DSV_SIGNOF(x) (((x) > 0) - ((x) < 0))

#define DSV_U2S(v) (((v) & (unsigned) 1) ? -(int)(((v) >> 1) + 1u) : (int) ((v) >> 1))

/* portable sar - shift arithmetic right, or floordiv_pow2 */
#if DSV_PORTABLE
#define DSV_SAR(v, s) ((-2 >> 1 == -1) ? ((int32_t) (v)) >> (s) : ((int32_t) (v)) / (1 << (s)))
#else
#define DSV_SAR(v, s) ((v) >> (s))
#endif

/* rounding version */
#define DSV_SAR_R(v, s) DSV_SAR((v) + (1 << ((s) - 1)), (s))

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

typedef struct {
    DSV_SBC *data;
    int width;
    int height;
} DSV_COEFS;

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
#define DSV_TEMPORAL_MC(fno) ((fno) % 2)

#define DSV_MV_BIT_INTRA    0
#define DSV_MV_BIT_EPRM     1
#define DSV_MV_BIT_SKIP     3

#define DSV_MV_IS_INTRA(mv)     ((mv)->flags & (1 << DSV_MV_BIT_INTRA))
#define DSV_MV_IS_EPRM(mv)      ((mv)->flags & (1 << DSV_MV_BIT_EPRM))
#define DSV_MV_IS_SKIP(mv)      ((mv)->flags & (1 << DSV_MV_BIT_SKIP))

#define DSV_BIT_SET(v, b, on) ((v) &= ~(unsigned) (1 << (b)), (v) |= ((on) << (b)))
#define DSV_MV_SET_INTRA(mv, b)     (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_INTRA, b))
#define DSV_MV_SET_EPRM(mv, b)      (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_EPRM, b))
#define DSV_MV_SET_SKIP(mv, b)      (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_SKIP, b))
    uint32_t flags;
#define DSV_SRC_DC_PRED 0x100
    uint16_t dc;
    uint16_t aux; /* stores auxiliary data for optimizing in-loop filtering */
    uint8_t err[3];
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
    DSV_MV *mvs;
    DSV_SBC *transform_buf; /* to store subband coefficients temporarily */
    uint8_t *blockdata; /* block bitmasks for adaptive things */
    uint8_t *sb_facs; /* subblock factors for AQ */
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

#define DSV_MAX_QP_BITS 12
#define DSV_MAX_QP ((1 << DSV_MAX_QP_BITS) - 1)

/* C.2 fixed point precision for determining what block a pixel lies in */
#define DSV_BLOCK_INTERP_P      14

#define DSV_MINQP    3
#define DSV_MINQUANT (1 << DSV_MINQP)  /* C.2 MINQUANT */

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

#define DSV_ALIGNMENT 64

static void *
d28_aligned_calloc(int32_t size)
{
    uint8_t *a = NULL;
    uint8_t *b = DSV2_ALLOC_FUNC(size + (DSV_ALIGNMENT - 1) + sizeof(void**));
    if (!b) {
        DSV_ERROR(("failed to allocate memory"));
        return NULL;
    }
    a = b + (DSV_ALIGNMENT - 1) + sizeof(void**);
    a -= (intptr_t) a & (DSV_ALIGNMENT - 1);
    memcpy((void*) ((char *) a - sizeof(void*)), &b, sizeof(void*));
    return a;
}

static void
d28_aligned_free(void *p)
{
    void *ptr;
    memcpy(&ptr, (void*) ((char*) p - sizeof(void*)), sizeof(void*));
    DSV2_FREE_FUNC(ptr);
}

extern void *
d28_alloc(int32_t size)
{
    void *p;

    p = d28_aligned_calloc(size + DSV_ALIGNMENT);
    if (!p) {
        return NULL;
    }
    *((int32_t *) p) = size;
    allocated++;
    allocated_bytes += size;
    if (peak_alloc < (allocated_bytes - freed_bytes)) {
        peak_alloc = (allocated_bytes - freed_bytes);
    }
    return (uint8_t *) p + DSV_ALIGNMENT;
}

extern void
d28_free(void *ptr)
{
    uint8_t *p;
    int32_t nbytes;

    if (ptr == NULL) {
        DSV_ERROR(("attempting to free null pointer!"));
        return;
    }
    freed++;
    p = ((uint8_t *) ptr) - DSV_ALIGNMENT;
    memcpy(&nbytes, p, sizeof(int32_t));
    freed_bytes += nbytes;

    if (peak_alloc < (allocated_bytes - freed_bytes)) {
        peak_alloc = (allocated_bytes - freed_bytes);
    }
    d28_aligned_free(p);
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
d28_alloc(int32_t size)
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
    buf->data = (uint8_t*) d28_alloc(size);
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
movec_pred(DSV_MV *vecs, DSV_PARAMS *p, int x, int y, int *px, int *py)
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
    unsigned log2 = 0;

    n -= (n != 0);
    while (n > 0) {
        log2++;
        n >>= 1;
    }
    return log2;
}

/* 1 = 256 */
static int
flogb2(unsigned n)
{
    uint32_t t, frac, whole = 0;
    if (n == 0) {
        return 0;
    }
    t = n / 2;
    while (t > 0) {
        t >>= 1;
        whole++;
    }

    if (whole > 7) {
        frac = n >> (whole - 7);
    } else {
        frac = n << (7 - whole);
    }

    return (whole << 8) + ((frac & 0x7f) << 1);
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
    bs->pos = ((bs->pos + (unsigned) 7) & (~(unsigned) 7)); /* byte align */
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
        bit = (8 - (bs->pos & 7)) - rem;
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

static unsigned
local_update_rice_k(unsigned avg)
{
    unsigned k = 0;
    avg >>= 3;
    while (avg >>= 1) {
        k++;
    }
    return k;
}

static unsigned
local_update_rice_state(unsigned ravg, unsigned v)
{
    return ravg - (ravg >> 3) + v;
}

/* B. Encoding Type: adaptive Rice code (URC) */
static unsigned
bs_get_rice(DSV_BS *bs, unsigned *rk, unsigned *avg)
{
    int k = (*rk);
    unsigned q = 0, v;
    while (!bs_get_bit(bs)) {
        q++;
    }
    v = (q << k) | bs_get_bits(bs, k);

    *avg = local_update_rice_state(*avg, v);
    *rk = local_update_rice_k(*avg);
    return v;
}

static int
u2s(unsigned uv)
{
    return DSV_U2S(uv);
}

/* B. Encoding Type: non-zero adaptive Rice code (NRC) */
static int
bs_get_nrice(DSV_BS *bs, unsigned *rk, unsigned *avg)
{
    return u2s(bs_get_rice(bs, rk, avg) + 1);
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

    frame = (DSV_FRAME*) d28_alloc(sizeof(*frame));
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
    c[0].data = (DSV_SBC*) d28_alloc((c0len + c1len + c2len) * sizeof(DSV_SBC));
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

    f->alloc = (uint8_t*) d28_alloc(f->planes[0].len + f->planes[1].len + f->planes[2].len);

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

    ls = (uint8_t*) d28_alloc(4 * height / SUBDIV);
    rs = (uint8_t*) d28_alloc(4 * height / SUBDIV);
    ts = (uint8_t*) d28_alloc(4 * width / SUBDIV);
    bs = (uint8_t*) d28_alloc(4 * width / SUBDIV);
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
#define L2A_CONDITION  (IS_LUMA  && !IS_P && (l == 2))
#define CC_CONDITION   (!IS_LUMA && !IS_P && (l >= 1 && l <= (lvls - 2)))
#define L1_CONDITION   (IS_LUMA  && !IS_P && (l == 1))

/* overflow safety */
#define OVF_SAFETY_CONDITION (l >= 11 && l >= (lvls - 3) && !fm->params->lossless)

/* pos/neg reflect */
#define RP(i, n, s) (((i) >= (n) ? (2 * (n) - (i) - 2) : (i)) * (s))
#define RN(i, s) (((i) < 0 ? -(i) : (i)) * (s))

/* L2 ringing filter */
#define R20 3
#define R2S 3
#define R2A (1 << (R2S - 1))

/* L2 standard filter */
#define S20 9
#define S2S 5
#define S2A (1 << (S2S - 1))

#define UNSCALE_UNPACK(in, out, scaleL, scaleH, n, si, so)  \
  for (i = 0; i < even_n; i += 2) {                         \
      out[(i + 0) * so] = in[(i +    0) / 2 * si] / scaleL; \
      out[(i + 1) * so] = in[(i + half) / 2 * si] / scaleH; \
  }                                                         \
  if (n & 1) {                                              \
      out[(n - 1) * so] = in[(n - 1) / 2 * si] / scaleL;    \
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

/* 5 tap low/high pass filters with/without adaptive ringing */
#define MAKE_5_TAP(v, C0, CA, CS, op, s)            \
  v[i * s] op (-v[RN(i - 3, s)] +                   \
          C0 * (v[(i - 1) * s] + v[(i + 1) * s]) -  \
                v[RP(i + 3, n, s)] + CA) >> CS

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
dwt_inverse(DSV_SBC *io, int n)
{
    int i, even_n = n & ~1;
    DO_SIMPLE_LO(io, -=, 1);
    DO_SIMPLE_HI(io, +=, 1);
}

static void
ifilterL2_a(DSV_SBC *out, DSV_SBC *in, int n, int s, uint8_t *sb, int delta, int sbs)
{
    int i, sbp = 0, even_n = n & ~1, half = n + (n & 1);
    UNSCALE_UNPACK(in, out, 2, 2, n, s, s);
    DO_5_TAP_LO_A(out, S20, S2A, S2S, R20, R2A, R2S, -=, s);
    DO_SIMPLE_HI(out, +=, s);
}


#define inv_2d(tmpbuf, img, width, height, stride, ifilter, scaleL, scaleH) \
{ \
    int i, x, y; \
    DSV_SBC *tmp = tmpbuf; \
    for (x = 0; x < width; x++) { \
        DSV_SBC *col = img + x; \
        int even_n = height & ~1, half = height + (height & 1); \
        UNSCALE_UNPACK(col, tmp, scaleL, scaleH, height, stride, 1); \
        ifilter(tmp, height); \
        for (y = 0; y < height; y++) { \
            img[y * stride + x] = tmp[y]; \
        } \
    } \
    for (y = 0; y < height; y++) { \
        int even_n = width & ~1, half = width + (width & 1); \
        DSV_SBC *row = img + y * stride; \
        UNSCALE_UNPACK(row, tmp, scaleL, scaleH, width, 1, 1); \
        ifilter(tmp, width); \
        memcpy(row, tmp, width * sizeof(*tmp)); \
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

static void
cpysub(DSV_SBC *dst, DSV_SBC *src, unsigned w, unsigned h, unsigned stride)
{
    if (stride == w) { /* the full image: the most important case with the most data being transferred */
        memcpy(dst, src, w * h * sizeof(DSV_SBC));
        return;
    }
    w *= sizeof(DSV_SBC);
    while (h-- > 0) {
        memcpy(dst, src, w);
        src += stride;
        dst += stride;
    }
}

/* C.3 Rounding Divisions */
static int
round8(int v)
{
    return (v + (v < 0 ? -4 : 4)) / 8;
}

static int
is_monotonic(int a, int b, int c, int hqp)
{
    int da, dc;
    da = b - a;
    dc = c - b;
    return da != 0 && dc != 0 && /* ensure gradient exists (not partially or totally flat) */
           (DSV_SIGNOF(da) == DSV_SIGNOF(dc)) && /* ensure gradient is monotonous */
           abs(da) < hqp && abs(dc) < hqp; /* ensure start and end values in gradient are not wildly different (for better edge preservation) */
}

/* C.3.1.2 Haar Filtered Inverse Transform */
static void
inv(DSV_SBC *src, DSV_SBC *dst, int width, int height, int lvl, int hqpLH, int hqpHL, int ovf_safety)
{
    int x, y, woff, hoff, ws, hs, oddw, oddh;
    int LL, LH, HL, HH;
    int mhqpLH, mhqpHL;
    int n2x2_w, n2x2_h;
    DSV_SBC *ll, *lh, *hl, *hh;
    DSV_SBC *spLL, *spLH, *spHL, *spHH;

    mhqpLH = 8 * hqpLH;
    mhqpHL = 8 * hqpHL;
    woff = DSV_ROUND_SHIFT(width, lvl);
    hoff = DSV_ROUND_SHIFT(height, lvl);

    ws = DSV_ROUND_SHIFT(width, lvl - 1);
    hs = DSV_ROUND_SHIFT(height, lvl - 1);
    oddw = ws & 1;
    oddh = hs & 1;
    n2x2_w = ws - oddw;
    n2x2_h = hs - oddh;

    spLL = src;
    spLH = src + woff;
    spHL = src + hoff * width;
    spHH = src + woff + hoff * width;
    for (y = 0; y < n2x2_h; y += 2) {
        DSV_SBC *dpA, *dpB;
        int inY = hqpHL && (y > 0 && y < (n2x2_h - 1));

        ll = spLL;
        lh = spLH;
        hl = spHL;
        hh = spHH;

        dpA = dst + y * width;
        dpB = dpA + width;
        for (x = 0; x < n2x2_w; x += 2) {
            int nudge, lp, ln;
            int s0, s1, d0, d1;
            int inX = hqpLH && (x > 0 && x < (n2x2_w - 1));

            LL = ll[0] * (1 << ovf_safety);
            LH = lh[0];
            HL = hl[0];
            HH = hh[0];

            if (inX) {
                lp = ll[-1] * (1 << ovf_safety); /* prev */
                ln = ll[ 1] * (1 << ovf_safety); /* next */
                if (is_monotonic(lp, LL, ln, mhqpLH)) {
                    nudge = round8(lp - ln) - LH;
                    LH += CLAMP(nudge, -hqpLH, hqpLH); /* nudge LH to smooth it */
                }
            }
            if (inY) { /* do the same as above but in the Y direction */
                lp = ll[-width] * (1 << ovf_safety);
                ln = ll[ width] * (1 << ovf_safety);
                if (is_monotonic(lp, LL, ln, mhqpHL)) {
                    nudge = round8(lp - ln) - HL;
                    HL += CLAMP(nudge, -hqpHL, hqpHL); /* nudge HL to smooth it */
                }
            }

            s0 = LL + HL;
            s1 = LL - HL;
            d0 = LH + HH;
            d1 = LH - HH;

            dpA[0] = (s0 + d0) / 4;
            dpA[1] = (s0 - d0) / 4;
            dpB[0] = (s1 + d1) / 4;
            dpB[1] = (s1 - d1) / 4;

            ll++;
            lh++;
            hl++;
            hh++;

            dpA += 2;
            dpB += 2;
        }
        if (oddw) {
            LL = ll[0] * (1 << ovf_safety);
            HL = hl[0];

            dpA[0] = (LL + HL) / 4;
            dpB[0] = (LL - HL) / 4;
        }
        spLL += width;
        spLH += width;
        spHL += width;
        spHH += width;
    }
    if (oddh) {
        DSV_SBC *dpA = dst + (hs - 1) * width;
        ll = spLL;
        lh = spLH;
        for (x = 0; x < n2x2_w; x += 2) {
            LL = ll[0] * (1 << ovf_safety);
            LH = lh[0];
            dpA[0] = (LL + LH) / 4;
            dpA[1] = (LL - LH) / 4;
            ll++;
            lh++;
            dpA += 2;
        }
        if (oddw) {
            LL = ll[0] * (1 << ovf_safety);
            dpA[0] = LL / 4;
        }
    }
    cpysub(src, dst, ws, hs, width);
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

    mx = MAX(w, h);
    lb2 = logb2(mx);
    if (mx > (1 << lb2)) {
        lb2++;
    }
    return lb2;
}

#define SB_LH       1
#define SB_HL       2
#define SB_HH       3
#define MAXLVL   3
#define LVL1     (MAXLVL - 1) /* highest freq */
#define LVL2     (MAXLVL - 2) /* second highest freq */
#define LVL3     (MAXLVL - 3) /* third highest freq */

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
    scale = (scale << 7) / MAX(hi - lo, 1);
    return CLAMP(scale, 0, 128);
}

static int
lfquant(DSV_FMETA *fm, int q)
{
    int psyfac;

    psyfac = spatial_psy_factor(fm->params, SB_HH);

    q -= (q * psyfac >> (7 + (fm->isP ? 1 : 3)));
    q = MAX(q / 2, 2);
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
    int psyq, psyfac, chroma;

    chroma = (fm->cur_plane != 0);
    psyfac = spatial_psy_factor(fm->params, s);

    q /= 2;

    psyq = q * psyfac >> (7 + (fm->isP ? 0 : 1));

    if (chroma) {
        /* reduce based on subsampling */
        int tl;
        tl = l - 2;
        if (s == SB_LH) {
            tl += DSV_FORMAT_H_SHIFT(fm->params->vidmeta->subsamp);
        } else if (s == SB_HL) {
            tl += DSV_FORMAT_V_SHIFT(fm->params->vidmeta->subsamp);
        }
        q = (q * 6) / MAX(4 - tl, 1);
    } else {
        /* reduce higher frequencies appropriately */
        if (l == LVL2) {
            q += psyq / 2;
        } else if (l == LVL1) {
            q += psyq;
        }
    }
    if (fm->isP) {
        int div = 16;
        if (l == LVL3) {
            q = q * 3 / 2;
        }
        if (l != LVL1) {
            div += flogb2(psyfac / 16) >> (5 + l);
        }
        return MAX(q * 4 / div, DSV_MINQUANT);
    }
    if (s == SB_HH) { /* quantize HH more */
        q *= (l * 2 + 1);
    }
    q += q * l / 4;
    if (!chroma) {
        q = (q * (l * 3 + 3)) / 8;
    } else {
        q /= 4;
    }
    return MAX(q, DSV_MINQUANT);
}

/* C.3.3 Subband Recomposition */
static void
inv_sbt(DSV_PLANE *dst, DSV_COEFS *src, int q, DSV_FMETA *fm)
{
    int w, h, lvls, l, hqpLH, hqpHL, ovf_safety;
    DSV_SBC *temp_buf_pad, *temp_buf_line;

    w = src->width;
    h = src->height;

    lvls = nlevels(w, h);
    temp_buf_pad = fm->transform_buf + w;
    temp_buf_line = fm->transform_buf + ((w + 2) * (h + 2));

    for (l = lvls; l > 0; l--) {
        int sw, sh;
        sw = DSV_ROUND_SHIFT(w, l - 1);
        sh = DSV_ROUND_SHIFT(h, l - 1);
        if (l >= 4) {
            hqpLH = lfquant(fm, q);
            hqpHL = hqpLH;
        } else {
            hqpLH = hfquant(fm, q, 1, 3 - l);
            hqpHL = hfquant(fm, q, 2, 3 - l);
        }
        ovf_safety = OVF_SAFETY_CONDITION;

        if (fm->params->lossless) {
            if (l >= 1 && l <= (lvls - 2)) {
                inv_2d(temp_buf_line, src->data, sw, sh, w, dwt_inverse, 1, 1);
            } else {
                inv(src->data, temp_buf_pad, w, h, l, 0, 0, ovf_safety);
            }
            continue;
        }
        if (LLI_CONDITION) {
            inv_2d(temp_buf_line, src->data, sw, sh, w, dwt_inverse, 2, 2);
        } else if (CC_CONDITION) {
            inv_2d(temp_buf_line, src->data, sw, sh, w, dwt_inverse, 2, 1);
        } else if (L2A_CONDITION) {
            inv_L2a_2d(temp_buf_pad, src->data, w, h, l, fm);
        } else if (L1_CONDITION) {
            inv_2d(temp_buf_line, src->data, sw, sh, w, dwt_inverse, 2, 4);
        } else {
            inv(src->data, temp_buf_pad, w, h, l, hqpLH, hqpHL, ovf_safety);
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
avgval(uint8_t *a, int as, int w, int h)
{
    int i, j;
    int avg = 0;
    if (w == 0 || h == 0) {
        return 0;
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            avg += a[i];
        }
        a += as;
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
#define FILTER_DIM 4 /* do not touch, filters are hardcoded as 4x4 operations */

#define FILTER_EDGE(ptr, stride, tE, tF)                                   \
    do {                                                                   \
        int i2, i1, i0, e0, e1, e2, avg;                                   \
        int d0e, d0i, d1e, d1i, d2e, d2i;                                  \
        uint8_t *curp = ptr;                                               \
                                                                           \
        e2 = curp[-3 * (stride)];                                          \
        e1 = curp[-2 * (stride)];                                          \
        e0 = curp[-1 * (stride)];                                          \
        i0 = curp[ 0 * (stride)];                                          \
        i1 = curp[ 1 * (stride)];                                          \
        i2 = curp[ 2 * (stride)];                                          \
                                                                           \
        avg = (8 * (i0 + e0) + 6 * (e1 + i1) + 2 * (e2 + i2) + 16) >> 5;   \
                                                                           \
        d0e = e0 - avg;                                                    \
        d0i = i0 - avg;                                                    \
        d1e = e1 - avg;                                                    \
        d1i = i1 - avg;                                                    \
        d2e = e2 - avg;                                                    \
        d2i = i2 - avg;                                                    \
        if (d0e < (tE) && -d0e < (tE) &&                                   \
            d0i < (tE) && -d0i < (tE) &&                                   \
            d1e < (tF) && -d1e < (tF) &&                                   \
            d1i < (tF) && -d1i < (tF) &&                                   \
            d2e < (tF) && -d2e < (tF) &&                                   \
            d2i < (tF) && -d2i < (tF)) {                                   \
            curp[-2 * (stride)] = (4 * e1 + 2 * e2 + i0 + e0 + 4) >> 3;    \
            curp[-1 * (stride)] = (2 * (i0 + e0 + e1) + i1 + e2 + 4) >> 3; \
            curp[ 0 * (stride)] = (2 * (i1 + i2 + e1) + e0 + i0 + 4) >> 3; \
        }                                                                  \
    } while (0)


static void
ihfilter4x4(DSV_PLANE *dp, int x, int y, int threshE, int threshF)
{
    uint8_t *p, *data;
    int s, f;

    if (threshE <= 0 || threshF <= 0) {
        return;
    }

    data = dp->data;
    s = dp->stride;
    p = data + x + y * s;

    for (f = 0; f < FILTER_DIM; f++) {
        FILTER_EDGE(p, 1, threshE, threshF);
        FILTER_EDGE(p + FILTER_DIM, -1, threshE, threshF);
        p += s;
    }
}


static void
ivfilter4x4(DSV_PLANE *dp, int x, int y, int threshE, int threshF)
{
    uint8_t *p, *data;
    int s, f;

    if (threshE <= 0 || threshF <= 0) {
        return;
    }

    data = dp->data;
    s = dp->stride;
    p = data + x + y * s;

    for (f = 0; f < FILTER_DIM; f++) {
        FILTER_EDGE(p, s, threshE, threshF);
        FILTER_EDGE(p + FILTER_DIM * s, -s, threshE, threshF);
        p++;
    }
}

static void
haar4x4(uint8_t *src, int as, int *psh, int *psv)
{
    uint8_t *spA, *spB;
    int x, y;
    int sh = 0, sv = 0;

    spA = src;
    spB = src + as;
    as *= 2;
    for (y = 0; y < 4; y += 2) {
        for (x = 0; x < 4; x += 2) {
            int s0, s1, d0, d1, HH;
            int x0, x1, x2, x3;

            x0 = spA[x + 0];
            x1 = spA[x + 1];
            x2 = spB[x + 0];
            x3 = spB[x + 1];

            s0 = x0 + x1;
            s1 = x2 + x3;
            d0 = x0 - x1;
            d1 = x2 - x3;

            HH = (d0 - d1) / 2;

            sh += d0 + d1 + HH; /* LH + HH */
            sv += s0 - s1 + HH; /* HL + HH  */
        }
        spA += as;
        spB += as;
    }
    *psh = sh;
    *psv = sv;
}

#define MIN4(a, b, c, d) MIN(MIN(a, b), MIN(c, d))
#define MAX4(a, b, c, d) MAX(MAX(a, b), MAX(c, d))

static int
ds4x4to2x2(uint8_t *a0, int as, int dsp[4])
{
    uint8_t *a1 = a0 + as;
    /* create downsampled pixels */
    dsp[0] = DSV_UAVG4(a0[0], a0[1], a1[0], a1[1]);
    dsp[1] = DSV_UAVG4(a0[2], a0[3], a1[2], a1[3]);
    a0 += 2 * as;
    a1 += 2 * as;
    dsp[2] = DSV_UAVG4(a0[0], a0[1], a1[0], a1[1]);
    dsp[3] = DSV_UAVG4(a0[2], a0[3], a1[2], a1[3]);

    return 2 * (MAX4(dsp[0], dsp[1], dsp[2], dsp[3]) - MIN4(dsp[0], dsp[1], dsp[2], dsp[3]));
}

static void
haar2L(uint8_t *a, int as, int *psh, int *psv, int *pslh, int *pslv, int *span)
{
    int dsp[4], HH;

    haar4x4(a, as, psh, psv);
    *span = ds4x4to2x2(a, as, dsp);
    HH = (dsp[0] - dsp[1] - dsp[2] + dsp[3]) / 2;
    *psh = abs(*psh);
    *psv = abs(*psv);
    *pslh = abs((dsp[0] - dsp[1] + dsp[2] - dsp[3]) + HH);
    *pslv = abs((dsp[0] + dsp[1] - dsp[2] - dsp[3]) + HH);
}

static void
intra_filter(int q, DSV_PARAMS *p, DSV_FMETA *fm, int c, DSV_PLANE *dp, int do_filter)
{
    int i, j, x, y;
    int nsbx, nsby;
    int fthresh, lowt;
    uint8_t *tcache = NULL;

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

    tcache = (uint8_t *) d28_alloc(nsbx * sizeof(*tcache));
    if (tcache == NULL) {
        DSV_ERROR(("out of memory"));
    }
    fthresh = q / 64;
    if (q < 128) {
        fthresh -= (128 - q) / 8;
    }
    fthresh = CLAMP(fthresh, 0, 24);
    lowt = MIN((q >> 8), 16);
    for (j = 0; j < nsby; j++) {
        int fy = j / FILTER_DIM;
        y = j * FILTER_DIM;

        memset(tcache, 0, nsbx * sizeof(*tcache));
        for (i = 0; i < nsbx; i++) {
            int fx = i / FILTER_DIM;
            int flags = fm->blockdata[fx + fy * p->nblocks_h];

            x = i * FILTER_DIM;

            if (!(flags & DSV_IS_RINGING)) {
                int sh, sv, shl, svl, span;
                int tt = 0, avg_energy;
                haar2L(DSV_GET_XY(dp, x, y), dp->stride, &sh, &sv, &shl, &svl, &span);
                avg_energy = DSV_UAVG4(sh, shl, sv, svl);

                if ((span >> 1) < MAX(lowt, avg_energy)) {
                    int tl = (shl + svl);
                    int th = (sh + sv);

                    int ad;
                    if (th > (tl * 5 / 2) || ((tl <= (avg_energy + 1)) || (th <= (avg_energy + 1)))) {
                        ad = 0;
                    } else if (tl > (th * 5 / 2)) {
                        ad = abs(tl - avg_energy);
                    } else {
                        ad = MAX(lowt, avg_energy);
                    }
                    tt = MIN(ad, fthresh);
                    tcache[i] = clamp_u8(tt);
                    ihfilter4x4(dp, x, y, tcache[i], tcache[i]);
                }
            }
        }
        for (i = 0; i < nsbx; i++) {
            int fx = i / FILTER_DIM;
            int flags = fm->blockdata[fx + fy * p->nblocks_h];

            x = i * FILTER_DIM;

            if (!(flags & DSV_IS_RINGING) && tcache[i]) {
                ivfilter4x4(dp, x, y, tcache[i], tcache[i]);
            }
        }
    }
    d28_free(tcache);
}

/* fast median of 9 values using network of 25 comparisons */
static int
median9(uint8_t *src, int x, int y, int stride)
{
    uint8_t v[9];

    memcpy(v + 0, src + (x - 1) + (y - 1) * stride, 3 * sizeof(uint8_t));
    memcpy(v + 3, src + (x - 1) + (y - 0) * stride, 3 * sizeof(uint8_t));
    memcpy(v + 6, src + (x - 1) + (y + 1) * stride, 3 * sizeof(uint8_t));

#define SORT2(a, b) do {\
        if ((b) < (a)) {\
            uint8_t t;  \
            t = (a);    \
            (a) = (b);  \
            (b) = t;    \
        }} while (0)

    SORT2(v[0], v[3]);
    SORT2(v[1], v[7]);
    SORT2(v[2], v[5]);
    SORT2(v[4], v[8]);
    SORT2(v[0], v[7]);

    SORT2(v[2], v[4]);
    SORT2(v[3], v[8]);
    SORT2(v[5], v[6]);
    SORT2(v[0], v[2]);
    SORT2(v[1], v[3]);

    SORT2(v[4], v[5]);
    SORT2(v[7], v[8]);
    SORT2(v[1], v[4]);
    SORT2(v[3], v[6]);
    SORT2(v[5], v[7]);

    SORT2(v[0], v[1]);
    SORT2(v[2], v[4]);
    SORT2(v[3], v[5]);
    SORT2(v[6], v[8]);
    SORT2(v[2], v[3]);

    SORT2(v[4], v[5]);
    SORT2(v[6], v[7]);
    SORT2(v[1], v[2]);
    SORT2(v[3], v[4]);
    SORT2(v[5], v[6]);

    return v[4];
}

static void
filter_mc_err(uint8_t *src, int stride, int w, int h, int thresh)
{
    uint8_t tmp[(DSV_MAX_BLOCK_SIZE + 2) * (DSV_MAX_BLOCK_SIZE + 2)];
    uint8_t *tmpp;
    int src_w, src_h;
    int x, y;

    /* shouldn't happen but who knows... */
    if (w <= 0 || h <= 0) {
        return;
    }
    thresh = (48 - CLAMP(thresh, 0, 48));
    if (thresh == 0) {
        return;
    }

    src_w = w + 2;
    src_h = h + 2;
    tmpp = tmp + src_w + 1;

    for (y = 0; y < src_h; y++) {
        memcpy(tmp + y * src_w, src + (y - 1) * stride - 1, src_w);
    }
    /* this bad boy gets auto-vectorized (at least in clang) */
    for (y = 0; y < h; y++) {
        int top, cur, bot;

        top = (y - 1) * src_w;
        cur = (y + 0) * src_w;
        bot = (y + 1) * src_w;

        for (x = 0; x < w; x++) {
            int c, l, r, u, d;
            int lo, hi;
            int str, med;

            c = tmpp[cur + x];
            l = tmpp[cur + x - 1];
            r = tmpp[cur + x + 1];
            u = tmpp[top + x];
            d = tmpp[bot + x];

            lo = MIN(l, r);
            hi = MAX(l, r);

            str = thresh;

            if (c > lo && c < hi) {
                lo = MIN(u, d);
                hi = MAX(u, d);

                if (c > lo && c < hi) {
                    str >>= 1;
                }
            }
            med = median9(tmpp, x, y, src_w);
            src[y * stride + x] = clamp_u8(c + ((med - c) * str / 64));
        }
    }
}

#define EDGE_4x4        3
#define EDGE_SUB        2
#define EDGE_BLOCK      1
#define EDGE_HALF_BIT   (1 << 2) /* 0 = left/top half, 1 = right/bottom half */

static int
classify_edge(int coord, int block_dim)
{
    if ((coord & (block_dim - 1)) == 0) {
        /* on block boundary */
        return EDGE_BLOCK;
    }
    if ((coord & ((block_dim / 2) - 1)) == 0) {
        /* on subblock boundary AND NOT on block boundary */
        return EDGE_SUB;
    }
    /* on 4x4 boundary AND NOT on block OR subblock boundary */
    if ((coord & (block_dim - 1)) < (block_dim / 2)) {
        return EDGE_4x4; /* left/top */
    }
    return EDGE_4x4 | EDGE_HALF_BIT; /* right/bottom */
}

static unsigned
locate_subblock(int edgehh, int edgevv)
{
    switch (edgehh) {
        case EDGE_BLOCK:
        case EDGE_4x4:
            return ((edgevv == EDGE_SUB || (edgevv & EDGE_HALF_BIT)) ? DSV_MASK_INTRA10 : DSV_MASK_INTRA00);
        case EDGE_SUB:
        case EDGE_4x4 | EDGE_HALF_BIT:
            return ((edgevv == EDGE_SUB || (edgevv & EDGE_HALF_BIT)) ? DSV_MASK_INTRA11 : DSV_MASK_INTRA01);
        default:
            break;
    }
    DSV_ASSERT(0);
    return 0;
}

#define F_BLOCK_UNALIGNED_W 1
#define F_BLOCK_UNALIGNED_H 2
#define F_BLOCK_BOTTOMMOST  4
#define F_BLOCK_RIGHTMOST   8

#define F_AUX_ND_MASK 0x1f /* mask to get neighbordif out of mv->aux */
#define F_AUX_STR_H_SHIFT 10
#define F_AUX_STR_V_SHIFT 8

static void
luma_filter(DSV_MV *vecs, int q, DSV_PARAMS *p, DSV_PLANE *dp)
{
    int i, j, x, y;
    int nsbx, nsby;
    int fthreshE, fthreshF;
    uint8_t *tcache = NULL;
    int align_flags, pos_flags;
    int psyf;
    unsigned tq = (1 + DSV_MAX_QP) - q;
    int logq = logb2(q);
    int blkshift_h, blkshift_v;
    if (p->lossless) {
        return;
    }
    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;
    blkshift_h = logb2(p->blk_w / FILTER_DIM);
    blkshift_v = logb2(p->blk_h / FILTER_DIM);
    align_flags = 0;
    if (dp->w % p->blk_w) {
        align_flags |= F_BLOCK_UNALIGNED_W;
    }
    if (dp->h % p->blk_h) {
        align_flags |= F_BLOCK_UNALIGNED_H;
    }
    tcache = (uint8_t *) d28_alloc(nsbx * sizeof(*tcache));
    if (tcache == NULL) {
        DSV_ERROR(("out of memory"));
    }
    psyf = spatial_psy_factor(p, -1) >= 32;
    fthreshE = (tq << 19) / (3 * tq * tq);
    fthreshF = 32 - (flogb2(tq / 256) >> 3) / 6;
    if (q < 128) {
        fthreshE -= (128 - q) / 3;
        fthreshF -= (128 - q) / 9;
    }
    fthreshE = CLAMP(fthreshE, 0, 0x7f);
    fthreshF = CLAMP(fthreshF, 0, 0x7f);
    for (j = 0; j < p->nblocks_v; j++) {
        int errmvy = 0;

        pos_flags = align_flags;
        if (j == (p->nblocks_v - 1)) {
            pos_flags |= F_BLOCK_BOTTOMMOST;
        }
        if ((pos_flags & (F_BLOCK_UNALIGNED_H | F_BLOCK_BOTTOMMOST)) == (F_BLOCK_UNALIGNED_H | F_BLOCK_BOTTOMMOST)) {
            errmvy = -p->nblocks_h;
        }
        for (i = 0; i < p->nblocks_h; i++) {
            DSV_MV *mv = &vecs[i + j * p->nblocks_h];
            int nd, intra;
            int errmvx = 0;
            mv->aux = 0;
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            intra = DSV_MV_IS_INTRA(mv);
            nd = neighdif(vecs, p, i, j) >> psyf;
            nd = logb2(CLAMP(nd, 0, 65535)) + p->vidmeta->filter_strength;
            mv->aux = MAX(nd, 0);
            if (i > 0) { /* left */
                DSV_MV *leftmv = (mv - 1);
                int str = 0;
                if (intra || (leftmv->flags & ((1 << DSV_MV_BIT_INTRA) | (1 << DSV_MV_BIT_SKIP)))) {
                    str++;
                }
                if (DSV_IS_SUBPEL(leftmv)) {
                    str++;
                }
                mv->aux |= str << F_AUX_STR_H_SHIFT;
            }
            if (j > 0) { /* top */
                DSV_MV *topmv = (mv - p->nblocks_h);
                int str = 0;
                if (intra || (topmv->flags & ((1 << DSV_MV_BIT_INTRA) | (1 << DSV_MV_BIT_SKIP)))) {
                    str++;
                }
                if (DSV_IS_SUBPEL(topmv)) {
                    str++;
                }
                mv->aux |= str << F_AUX_STR_V_SHIFT;
            }
            if (i == (p->nblocks_h - 1)) {
                pos_flags |= F_BLOCK_RIGHTMOST;
            }
            /* mv error is unreliable on bottom/right edge blocks in frames whose width/height is not an exact multiple of the block's dimensions */
            if ((pos_flags & (F_BLOCK_UNALIGNED_W | F_BLOCK_RIGHTMOST)) == (F_BLOCK_UNALIGNED_W | F_BLOCK_RIGHTMOST)) {
                errmvx = -1;
            }
            mv->err[0] = clamp_u8(((mv + errmvx + errmvy)->err[0] << logq) >> 9);

            x = i * p->blk_w;
            y = j * p->blk_h;
            if (mv->err[0]) {
                filter_mc_err(DSV_GET_XY(dp, x, y), dp->stride, p->blk_w, p->blk_h, q / mv->err[0]);
            }
        }
    }
    /* two horizontal passes to avoid step-like filtering artifacts */
    for (j = 0; j < nsby; j++) {
        DSV_MV *mvrow;
        int edgevtype, fy;
        fy = j >> blkshift_v;
        y = j * FILTER_DIM;

        edgevtype = classify_edge(y, p->blk_h);

        mvrow = vecs + fy * p->nblocks_h;
        for (i = 0; i < nsbx; i++) {
            int edgehtype, fx, nd;
            DSV_MV *mv;
            int tt, ht = 0, vt = 0;

            fx = i >> blkshift_h;
            x = i * FILTER_DIM;
            mv = mvrow + fx;
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            edgehtype = classify_edge(x, p->blk_w);
            nd = mv->aux & F_AUX_ND_MASK;

            tt = mv->err[0];
            if (DSV_MV_IS_INTRA(mv) && (mv->submask & locate_subblock(edgehtype, edgevtype))) {
                tt += fthreshF;
                tt = MIN(tt, fthreshE);
                ht = tt;
                vt = tt;
            } else {
                int edgeh, edgev;

                edgeh = (edgehtype & ~EDGE_HALF_BIT) == EDGE_BLOCK;
                edgev = (edgevtype & ~EDGE_HALF_BIT) == EDGE_BLOCK;

                tt = (tt << nd) >> 4;
                tt = MIN(tt, fthreshF);
                ht = tt;
                vt = tt;

                if (edgeh || edgev) {
                    int sh, sv, shl, svl, span;
                    int strh, strv;
                    strh = ((mv->aux >> F_AUX_STR_H_SHIFT) & 0x3);
                    strv = ((mv->aux >> F_AUX_STR_V_SHIFT) & 0x3);

                    haar2L(DSV_GET_XY(dp, x - 2, y - 2), dp->stride, &sh, &sv, &shl, &svl, &span);

                    span *= 2;
                    if (edgeh) {
                        ht = ((abs(span - sh) + abs(span - shl) * 2) << (nd + strh)) >> 6;
                        ht = MIN(ht, fthreshE);
                    }
                    if (edgev) {
                        vt = ((abs(span - sv) + abs(span - svl) * 2) << (nd + strv)) >> 6;
                        vt = MIN(vt, fthreshE);
                    }
                    if (sh > 2 * sv) {
                        ht >>= 1;
                    }
                    if (sv > 2 * sh) {
                        vt >>= 1;
                    }
                }
            }
            tcache[i] = vt;
            ihfilter4x4(dp, x, y, ht, MIN(ht, fthreshF));
        }
        for (i = 0; i < nsbx; i++) {
            DSV_MV *mv;
            x = i * FILTER_DIM;
            mv = mvrow + (i >> blkshift_h);
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            ivfilter4x4(dp, x, y, tcache[i], MIN(tcache[i], fthreshF));
        }
    }
    d28_free(tcache);
}

static void
chroma_filter(DSV_MV *vecs, int q, DSV_PARAMS *p, DSV_PLANE *dp)
{
    int i, j, x, y, sh, sv;
    int nsbx, nsby;
    int align_flags, pos_flags;
    int fthresh;
    unsigned tq = (1 + DSV_MAX_QP) - q;
    int logq = logb2(q);
    int blkshift_h, blkshift_v;

    if (p->lossless) {
        return;
    }
    nsbx = dp->w / FILTER_DIM;
    nsby = dp->h / FILTER_DIM;

    sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
    sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    blkshift_h = logb2((p->blk_w >> sh) / FILTER_DIM);
    blkshift_v = logb2((p->blk_h >> sv) / FILTER_DIM);
    align_flags = 0;
    if (dp->w % p->blk_w) {
        align_flags |= F_BLOCK_UNALIGNED_W;
    }
    if (dp->h % p->blk_h) {
        align_flags |= F_BLOCK_UNALIGNED_H;
    }
    fthresh = (tq << 19) / (3 * tq * tq);
    if (q < 128) {
        fthresh -= (128 - q) / 3;
    }
    fthresh = CLAMP(fthresh * 2, 0, 0x7f);
    for (j = 0; j < p->nblocks_v; j++) {
        int errmvy = 0;

        pos_flags = align_flags;
        if (j == (p->nblocks_v - 1)) {
            pos_flags |= F_BLOCK_BOTTOMMOST;
        }
        if ((pos_flags & (F_BLOCK_UNALIGNED_H | F_BLOCK_BOTTOMMOST)) == (F_BLOCK_UNALIGNED_H | F_BLOCK_BOTTOMMOST)) {
            errmvy = -p->nblocks_h;
        }
        for (i = 0; i < p->nblocks_h; i++) {
            DSV_MV *mv, *emv;
            int errmvx = 0;
            int thr, nd;
            mv = &vecs[i + j * p->nblocks_h];
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            if (i == (p->nblocks_h - 1)) {
                pos_flags |= F_BLOCK_RIGHTMOST;
            }
            /* mv error is unreliable on bottom/right edge blocks in frames whose width/height is not an exact multiple of the block's dimensions */
            if ((pos_flags & (F_BLOCK_UNALIGNED_W | F_BLOCK_RIGHTMOST)) == (F_BLOCK_UNALIGNED_W | F_BLOCK_RIGHTMOST)) {
                errmvx = -1;
            }
            emv = mv + errmvx + errmvy;
            nd = mv->aux & F_AUX_ND_MASK;
            thr = (((emv->err[1] << logq) >> 9) + 1) << nd;
            mv->err[1] = MIN(thr, fthresh);
            thr = (((emv->err[2] << logq) >> 9) + 1) << nd;
            mv->err[2] = MIN(thr, fthresh);
        }
    }
    /* two horizontal passes to avoid step-like filtering artifacts */
    for (j = 0; j < nsby; j++) {
        DSV_MV *mvrow;
        int fy;
        fy = j >> blkshift_v;
        y = j * FILTER_DIM;

        mvrow = vecs + fy * p->nblocks_h;
        for (i = 0; i < nsbx; i++) {
            DSV_MV *mv;
            mv = mvrow + (i >> blkshift_h);
            x = i * FILTER_DIM;
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            ihfilter4x4(dp + 0, x, y, mv->err[1], mv->err[1] >> 2);
            ihfilter4x4(dp + 1, x, y, mv->err[2], mv->err[2] >> 2);
        }
        for (i = 0; i < nsbx; i++) {
            DSV_MV *mv;
            mv = mvrow + (i >> blkshift_h);
            x = i * FILTER_DIM;
            if (DSV_MV_IS_SKIP(mv)) {
                continue;
            }
            ivfilter4x4(dp + 0, x, y, mv->err[1], mv->err[1] >> 2);
            ivfilter4x4(dp + 1, x, y, mv->err[2], mv->err[2] >> 2);
        }
    }
}

static void
luma_qp(uint8_t *dec, int ds,
        uint8_t *ref, int rs,
        int bw, int bh,
        int dx, int dy, int tmc)
{
    int16_t tbuf[(DSV_MAX_BLOCK_SIZE + 3) * DSV_MAX_BLOCK_SIZE];
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
predict(DSV_MV *vecs, DSV_PARAMS *p, int c, DSV_FRAME *ref, DSV_PLANE *dp)
{
    int i, j, r, x, y, bw, bh, sh, sv, limx, limy;
    DSV_PLANE *rp;
    DSV_MV *mv;

    if (c == 0) {
        sh = 0;
        sv = 0;
    } else {
        sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
        sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    }
    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;

    limx = (dp->w - bw) + DSV_FRAME_BORDER - 1;
    limy = (dp->h - bh) + DSV_FRAME_BORDER - 1;
    rp = ref->planes + c;

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            int px, py;
            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            px = x + DSV_SAR(mv->u.mv.x, 2 + sh);
            py = y + DSV_SAR(mv->u.mv.y, 2 + sv);

            if (DSV_MV_IS_INTRA(mv)) {
                /* D.2 Compensating Intra Blocks */
                uint8_t *dec;
                int avgc;

                px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                py = CLAMP(py, -DSV_FRAME_BORDER, limy);

                if (mv->submask == DSV_MASK_ALL_INTRA) {
                    if (c == 0 && (mv->dc & DSV_SRC_DC_PRED)) { /* DC is only for luma */
                        avgc = mv->dc & 0xff;
                    } else {
                        avgc = avgval(DSV_GET_XY(rp, px, py), rp->stride, bw, bh);
                    }
                    dec = DSV_GET_XY(dp, x, y);
                    for (r = 0; r < bh; r++) {
                        memset(dec, avgc, bw);
                        dec += dp->stride;
                    }
                } else {
                    int f, g, sbx, sby, sbw, sbh, mask_index;
                    uint8_t masks[4] = {
                            DSV_MASK_INTRA00,
                            DSV_MASK_INTRA01,
                            DSV_MASK_INTRA10,
                            DSV_MASK_INTRA11,
                    };
                    sbw = bw / 2;
                    sbh = bh / 2;
                    mask_index = 0;

                    for (g = 0; g <= sbh; g += (sbh + !sbh)) {
                        for (f = 0; f <= sbw; f += (sbw + !sbw)) {
                            sbx = x + f;
                            sby = y + g;
                            if (mv->submask & masks[mask_index]) {
                                if (c == 0 && (mv->dc & DSV_SRC_DC_PRED)) { /* DC is only for luma */
                                    avgc = mv->dc & 0xff;
                                } else {
                                    avgc = avgval(DSV_GET_XY(rp, px + f, py + g), rp->stride, sbw, sbh);
                                }

                                dec = DSV_GET_XY(dp, sbx, sby);
                                for (r = 0; r < sbh; r++) {
                                    memset(dec, avgc, sbw);
                                    dec += dp->stride;
                                }
                            } else {
                                cpyblk(DSV_GET_XY(dp, sbx, sby),
                                       DSV_GET_XY(rp, px + f, py + g),
                                       dp->stride, rp->stride, sbw, sbh);
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
                        cpyblk(DSV_GET_XY(dp, x, y), DSV_GET_XY(rp, px, py), dp->stride, rp->stride, bw, bh);
                    } else {
                        px = CLAMP(px - 1, -DSV_FRAME_BORDER, limx);
                        py = CLAMP(py - 1, -DSV_FRAME_BORDER, limy);
                        luma_qp(DSV_GET_XY(dp, x, y), dp->stride,
                                DSV_GET_XY(rp, px, py), rp->stride,
                                bw, bh, mv->u.mv.x, mv->u.mv.y, p->temporal_mc);
                    }
                } else {
                    px = CLAMP(px, -DSV_FRAME_BORDER, limx);
                    py = CLAMP(py, -DSV_FRAME_BORDER, limy);
                    bilinear_sp(DSV_GET_XY(dp, x, y), dp->stride, DSV_GET_XY(rp, px, py), rp->stride, bw, bh, mv->u.mv.x, mv->u.mv.y, sh, sv);
                }
            }
        }
    }
}

static void
reconstruct(DSV_MV *vecs, DSV_PARAMS *p, int c, DSV_PLANE *resp, DSV_PLANE *predp, DSV_PLANE *outp)
{
    int i, j, x, y, bw, bh, areashift, sh, sv;
    DSV_MV *mv;

    if (c == 0) {
        sh = 0;
        sv = 0;
    } else {
        sh = DSV_FORMAT_H_SHIFT(p->vidmeta->subsamp);
        sv = DSV_FORMAT_V_SHIFT(p->vidmeta->subsamp);
    }
    bw = p->blk_w >> sh;
    bh = p->blk_h >> sv;
    areashift = logb2(bw * bh);

    for (j = 0; j < p->nblocks_v; j++) {
        y = j * bh;
        for (i = 0; i < p->nblocks_h; i++) {
            int m, n;
            uint8_t *res, *pred, *out;

            x = i * bw;
            mv = &vecs[i + j * p->nblocks_h];

            res = DSV_GET_XY(resp, x, y);
            pred = DSV_GET_XY(predp, x, y);
            out = DSV_GET_XY(outp, x, y);
            if (p->lossless) {
                for (n = 0; n < bh; n++) {
                    for (m = 0; m < bw; m++) {
                        out[m] = (pred[m] + res[m] - 128);
                    }
                    pred += predp->stride;
                    res += resp->stride;
                    out += outp->stride;
                }
            } else {
                /* D.4 Reconstruction */
                if (!DSV_MV_IS_EPRM(mv) || (!DSV_MV_IS_INTRA(mv) && DSV_MV_IS_SKIP(mv))) {
                    unsigned lerr = 0;
                    for (n = 0; n < bh; n++) {
                        for (m = 0; m < bw; m++) {
                            int true_resid = res[m] - 128;
                            lerr += abs(true_resid);
                            /* source = (prediction + residual) */
                            out[m] = clamp_u8(pred[m] + true_resid);
                        }
                        pred += predp->stride;
                        res += resp->stride;
                        out += outp->stride;
                    }
                    mv->err[c] = lerr >> areashift;
                } else {
                    unsigned lerr = 0;
                    for (n = 0; n < bh; n++) {
                        for (m = 0; m < bw; m++) {
                            int true_resid = 2 * (res[m] - 128);
                            lerr += abs(true_resid);
                            out[m] = clamp_u8(pred[m] + true_resid);
                        }
                        pred += predp->stride;
                        res += resp->stride;
                        out += outp->stride;
                    }
                    mv->err[c] = lerr >> areashift;
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

        predict(mv, fm->params, c, ref, op); /* make prediction onto temp frame (out) */
        reconstruct(mv, fm->params, c, rp, op, op);
    }
    if (do_filter) {
        d28_extend_frame(out);
        luma_filter(mv, q, fm->params, out->planes + 0);
        chroma_filter(mv, q, fm->params, out->planes + 1);
    }
}

/************************************ HZCC ************************************/

/* Hierarchical Zero Coefficient Coding */

#define EOP_SYMBOL 0x55 /* B.2.3.5 Image Data - Coefficient Coding */

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

#define TMQ4POS_I(tmq, flags, l)                                            \
        switch (l) {                                                        \
            case MAXLVL - 3:                                                \
                break;                                                      \
            default:                                                        \
            case MAXLVL - 2:                                                \
                switch ((flags) & (DSV_IS_STABLE | DSV_IS_MAINTAIN)) {      \
                    case DSV_IS_STABLE:                                     \
                        if (!parc) { tmq = tmq * 2 / 5; } else { tmq /= 3; }\
                        break;                                              \
                    case DSV_IS_MAINTAIN:                                   \
                        tmq >>= (((flags) & DSV_IS_RINGING) ?  2 : !parc);  \
                        break;                                              \
                    case DSV_IS_MAINTAIN | DSV_IS_STABLE:                   \
                        if (parc) { tmq /= 4; } else { tmq /= 6; }          \
                        break;                                              \
                    default:                                                \
                        break;                                              \
                }                                                           \
                break;                                                      \
            case MAXLVL - 1:                                                \
                switch ((flags) & (DSV_IS_STABLE | DSV_IS_MAINTAIN)) {      \
                    case DSV_IS_STABLE:                                     \
                        if (!parc) { tmq /= 3; } else { tmq /= 4; }         \
                        break;                                              \
                    case DSV_IS_MAINTAIN:                                   \
                        tmq >>= (((flags) & DSV_IS_RINGING) ? 2 : !parc);   \
                        break;                                              \
                    case DSV_IS_MAINTAIN | DSV_IS_STABLE:                   \
                        if (parc) { tmq /= 6; } else { tmq /= 8; }          \
                        break;                                              \
                    default:                                                \
                        break;                                              \
                }                                                           \
                break;                                                      \
        }


#define dequantU(v, q) ((v) * 2 * (q)) /* uniform */
#define dequantDZ(v, q) ((q) * ((v) * 2 + ((v) >= 0 ? 1 : -1))) /* deadzone */
#define dequant(v, q, isP) (isP ? dequantDZ(v,q) : dequantU(v,q))

#define GETV(bs)     (bs_get_nrice(bs, &vk, &vavg))

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
    unsigned vk = 0, vavg = 0; /* for Rice decoding */

    bs_align(bs);
    runs = bs_get_bits(bs, RUN_BITS);
    bs_align(bs);

    s = l = 0;
    isP = fm->isP;

    sw = dimat(l, w);
    sh = dimat(l, h);
    qp = lfquant(fm, q);

    o = subband(l, s, w, h);
    outp = out + o;

    run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;

    bufsz *= 8; /* convert from bytes to bits to make comparison in loops a little easier */

    if (fm->params->lossless) {
        /* C.2.3 LL Subband */
        for (y = 0; y < sh; y++) {
            x = 0;
            while (x < sw) {
                if (run > 0) {
                    int rem = MIN(run, (sw - x));
                    x += rem;
                    run -= rem;
                    if (x == sw) {
                        break;
                    }
                }
                v = bs_get_neg(bs);
                run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                if (bs->pos >= bufsz) {
                    return;
                }
                outp[x++] = v;
            }
            outp += w;
        }
        for (l = 0; l < MAXLVL; l++) {
            sw = dimat(l, w);
            sh = dimat(l, h);
            /* C.2.4 Higher Level Subband Dequantization */
            for (s = 1; s < NSUBBAND; s++) {
                o = subband(l, s, w, h);
                outp = out + o;
                for (y = 0; y < sh; y++) {
                    x = 0;
                    while (x < sw) {
                        if (run > 0) {
                            int rem = MIN(run, (sw - x));
                            x += rem;
                            run -= rem;
                            if (x == sw) {
                                break;
                            }
                        }
                        v = GETV(bs);
                        run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                        if (bs->pos >= bufsz) {
                            return;
                        }
                        outp[x++] = v;
                    }
                    outp += w;
                }
            }
        }
    } else {
        /* C.2.3 LL Subband */
        for (y = 0; y < sh; y++) {
            x = 0;
            while (x < sw) {
                if (run > 0) {
                    int rem = MIN(run, (sw - x));
                    x += rem;
                    run -= rem;
                    if (x == sw) {
                        break;
                    }
                }
                v = bs_get_neg(bs);
                run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                if (bs->pos >= bufsz) {
                    return;
                }
                outp[x++] = dequant(v, qp, isP);
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
            /* C.2.4 Higher Level Subband Dequantization */
            for (s = 1; s < NSUBBAND; s++) {
                int par;
                par = subband(l - 1, s, w, h);
                o = subband(l, s, w, h);
                qp = hfquant(fm, q, s, l);

                outp = out + o;
                if (isP) {
                    /* inter frame, simple decoding loop */
                    for (y = 0; y < sh; y++) {
                        x = 0;
                        while (x < sw) {
                            if (run > 0) {
                                int rem = MIN(run, (sw - x));
                                x += rem;
                                run -= rem;
                                if (x == sw) {
                                    break;
                                }
                            }
                            v = GETV(bs);
                            run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                            if (bs->pos >= bufsz) {
                                return;
                            }
                            outp[x++] = dequant(v, qp, 1);
                        }
                        outp += w;
                    }
                } else {
                    /* intra frame, uses blockdata for AQ */
                    by = 0;
                    for (y = 0; y < sh; y++) {
                        x = 0;
                        bx = 0;
                        blockrow = fm->blockdata + (by >> DSV_BLOCK_INTERP_P) * fm->params->nblocks_h;
                        parent = out + par + ((y >> 1) * w);
                        while (x < sw) {
                            int flags, parc;
                            int tmq = qp;

                            if (run > 0) {
                                int rem = MIN(run, (sw - x));
                                x += rem;
                                bx += rem * dbx;
                                run -= rem;
                                if (x == sw) {
                                    break;
                                }
                            }
                            v = GETV(bs);
                            run = (runs-- > 0) ? bs_get_ueg(bs) : INT_MAX;
                            if (bs->pos >= bufsz) {
                                return;
                            }

                            flags = blockrow[bx >> DSV_BLOCK_INTERP_P];
                            parc = parent[x >> 1];
                            TMQ4POS_I(tmq, flags, l);

                            outp[x++] = dequant(v, tmq, 0);
                            bx += dbx;
                        }
                        outp += w;
                        by += dby;
                    }
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
static int
decode_meta(DSV_DECODER *d, DSV_BS *bs)
{
    DSV_META *fmt = &d->vidmeta;
    int w, h;

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

    fmt->filter_strength = bs_get_ueg(bs);
    fmt->filter_strength = DSV_U2S(fmt->filter_strength);
    DSV_DEBUG(("filter strength %d", fmt->filter_strength));
    if (bs_get_bit(bs)) {
        fmt->reserved = bs_get_bits(bs, 15);
        if (fmt->reserved & DSV_META_COLORSPACE_BIT) {
            fmt->colorspace = bs_get_bits(bs, 4);
            fmt->colorspace |= bs_get_bit(bs) * DSV_COLORSPACE_FULLRANGE;
            DSV_DEBUG(("colorspace %d", fmt->colorspace));
            DSV_DEBUG(("fullrange %d", !!(fmt->colorspace & DSV_COLORSPACE_FULLRANGE)));
        } else {
            fmt->colorspace = DSV_COLORSPACE_BT601;
        }
    } else {
        fmt->reserved = 0;
    }
    /* validation */
    w = fmt->width;
    h = fmt->height;
    if (w <= 0 || h <= 0) {
        DSV_ERROR(("given dimensions were strange: %dx%d", w, h));
        return 0;
    }
    if (w < 64 || h < 64) {
        DSV_ERROR(("DSV2 does not support dimensions < 64: %dx%d", w, h));
        return 0;
    }
    if ((w & 1) || (h & 1)) {
        DSV_ERROR(("DSV2 does not support odd dimensions: %dx%d", w, h));
        return 0;
    }
    if ((w * h) >= (4096 * 4096)) {
        DSV_WARNING(("video dimensions %dx%d exceed what DSV2 is designed to handle, expect decoding issues!", w, h));
    }
    if (fmt->filter_strength < DSV_MIN_FILTER_STR || fmt->filter_strength > DSV_MAX_FILTER_STR) {
        /* a bit more serious than the dimensions being large */
        DSV_ERROR(("filter strength was strange: %d, expect decoding issues!.", fmt->filter_strength));
    }
    return 1;
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

                movec_pred(mvs, params, i, j, &px, &py);
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
d28_dec_init(DSV_DECODER *d)
{
    memset(d, 0, sizeof(*d));
}

extern void
d28_dec_free(DSV_DECODER *d)
{
    if (d->ref) {
        img_unref(d->ref);
    }
    if (d->transform_buf) {
        d28_free(d->transform_buf);
        d->transform_buf = NULL;
    }
}

extern DSV_META *
d28_get_metadata(DSV_DECODER *d)
{
    DSV_META *meta;

    meta = (DSV_META *) d28_alloc(sizeof(DSV_META));
    memcpy(meta, &d->vidmeta, sizeof(DSV_META));

    return meta;
}

extern int
d28_dec(DSV_DECODER *d, DSV_BUF *buffer, DSV_FRAME **out, DSV_FNUM *fn)
{
    DSV_BS bs;
    DSV_IMAGE *img;
    DSV_PARAMS *p;
    int i, quant, is_ref, pkt_type, subsamp, do_filter;
    DSV_META *meta = &d->vidmeta;
    DSV_FRAME *residual;
    DSV_MV *mvs = NULL;
    DSV_FNUM fno;
    DSV_FMETA fm;
    int stats[DSV_MAX_STAT];
    DSV_COEFS coefs[3];
    unsigned xf_buf_sz;
    int has_ref;

    *fn = ~(DSV_FNUM) 0;
    
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
                if (decode_meta(d, &bs)) {
                    d->got_metadata = 1;
                    ret = DSV_DEC_GOT_META;
                } else {
                    ret = DSV_DEC_ERROR;
                }
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

    img = (DSV_IMAGE *) d28_alloc(sizeof(DSV_IMAGE));
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
    d->quant = quant;
    p->lossless = (quant == 1);
    if (bs_get_bit(&bs)) {
        p->reserved = bs_get_bits(&bs, 15);
    } else {
        p->reserved = 0;
    }
    bs_align(&bs);
    /* read frame metadata (stability / skip, motion data / adaptive quant) */
    img->blockdata = (uint8_t *) d28_alloc(p->nblocks_h * p->nblocks_v);
    decode_stability_blocks(img, &bs, buffer, has_ref, stats);
    if (has_ref) {
        mvs = (DSV_MV *) d28_alloc(sizeof(DSV_MV) * p->nblocks_h * p->nblocks_v);
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

    /* (re)allocate if image is larger than what we currently have allocated */
    xf_buf_sz = ((meta->width + 2) * (meta->height + 2)) + MAX(meta->width, meta->height);
    if (d->transform_buf_sz < xf_buf_sz) {
        d->transform_buf_sz = xf_buf_sz;
        if (d->transform_buf) {
            d28_free(d->transform_buf);
            d->transform_buf = NULL;
        }
        d->transform_buf = (DSV_SBC *) d28_alloc(d->transform_buf_sz * sizeof(DSV_SBC));
        if (d->transform_buf == NULL) {
            DSV_ERROR(("out of memory"));
        }
    }
    fm.transform_buf = d->transform_buf;

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
