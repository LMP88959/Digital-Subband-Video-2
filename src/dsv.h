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

#ifndef _DSV_H_
#define _DSV_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>

/* configurable */
#define DSV_PORTABLE 1

/* B.1 Packet Header */
#define DSV_FOURCC_0     'D'
#define DSV_FOURCC_1     'S'
#define DSV_FOURCC_2     'V'
#define DSV_FOURCC_3     '2'
#define DSV_VERSION_MINOR 7
#define DSV_VERSION_BUILD 4

/* B.1.1 Packet Type */
#define DSV_PT_META 0x00
#define DSV_PT_PIC  0x04
#define DSV_PT_EOS  0x10
#define DSV_MAKE_PT(is_ref, has_ref) (DSV_PT_PIC | ((is_ref) << 1) | (has_ref))

#define DSV_PT_IS_PIC(x)   ((x) & DSV_PT_PIC)
#define DSV_PT_IS_REF(x)  (((x) & 0x6) == 0x6)
#define DSV_PT_HAS_REF(x)  ((x) & 0x1)

#define DSV_PACKET_HDR_SIZE (4 + 1 + 1 + 4 + 4)
#define DSV_PACKET_TYPE_OFFSET 5
#define DSV_PACKET_PREV_OFFSET 6
#define DSV_PACKET_NEXT_OFFSET 10

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
} DSV_META;

typedef struct {
    uint8_t *data;
    int len;
    int format;
    int stride;
    int w, h;
} DSV_PLANE;

/* subband coefs */
typedef int32_t DSV_SBC;
typedef struct {
    DSV_SBC *data;
    int width;
    int height;
} DSV_COEFS;

typedef struct {
    uint8_t *alloc;

    DSV_PLANE planes[3];

    int refcount;

    int format;
    int width;
    int height;

    int border;
} DSV_FRAME;

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
#define DSV_MV_BIT_NOXMITY  5 /* don't transmit luma residual */
#define DSV_MV_BIT_NOXMITC  6 /* don't transmit chroma residual */
#define DSV_MV_BIT_SIMCMPLX 7

#define DSV_MV_IS_INTRA(mv)     ((mv)->flags & (1 << DSV_MV_BIT_INTRA))
#define DSV_MV_IS_EPRM(mv)      ((mv)->flags & (1 << DSV_MV_BIT_EPRM))
#define DSV_MV_IS_MAINTAIN(mv)  ((mv)->flags & (1 << DSV_MV_BIT_MAINTAIN))
#define DSV_MV_IS_SKIP(mv)      ((mv)->flags & (1 << DSV_MV_BIT_SKIP))
#define DSV_MV_IS_RINGING(mv)   ((mv)->flags & (1 << DSV_MV_BIT_RINGING))
#define DSV_MV_IS_NOXMITY(mv)   ((mv)->flags & (1 << DSV_MV_BIT_NOXMITY))
#define DSV_MV_IS_NOXMITC(mv)   ((mv)->flags & (1 << DSV_MV_BIT_NOXMITC))
#define DSV_MV_IS_SIMCMPLX(mv)  ((mv)->flags & (1 << DSV_MV_BIT_SIMCMPLX))

#define DSV_BIT_SET(v, b, on) ((v) &= ~(1 << (b)), (v) |= ((on) << (b)))
#define DSV_MV_SET_INTRA(mv, b)     (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_INTRA, b))
#define DSV_MV_SET_EPRM(mv, b)      (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_EPRM, b))
#define DSV_MV_SET_MAINTAIN(mv, b)  (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_MAINTAIN, b))
#define DSV_MV_SET_SKIP(mv, b)      (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_SKIP, b))
#define DSV_MV_SET_RINGING(mv, b)   (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_RINGING, b))
#define DSV_MV_SET_NOXMITY(mv, b)   (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_NOXMITY, b))
#define DSV_MV_SET_NOXMITC(mv, b)   (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_NOXMITC, b))
#define DSV_MV_SET_SIMCMPLX(mv, b)  (DSV_BIT_SET((mv)->flags, DSV_MV_BIT_SIMCMPLX, b))
    uint32_t flags;
    uint16_t err;
#define DSV_SRC_DC_PRED 0x100
    uint16_t dc;
    uint8_t submask;
} DSV_MV;

#define DSV_GET_LINE(p, y) ((p)->data + (y) * (p)->stride)
#define DSV_GET_XY(p, x, y) ((p)->data + (x) + (y) * (p)->stride)

#define DSV_MAX_QP_BITS 12
#define DSV_MAX_QP ((1 << DSV_MAX_QP_BITS) - 1)

extern void dsv_mk_coefs(DSV_COEFS *frame, int format, int width, int height);

extern DSV_FRAME *dsv_mk_frame(int format, int width, int height, int border);
extern DSV_FRAME *dsv_load_planar_frame(int format, void *data, int width, int height);

extern DSV_FRAME *dsv_frame_ref_inc(DSV_FRAME *frame);
extern void dsv_frame_ref_dec(DSV_FRAME *frame);

extern void dsv_frame_copy(DSV_FRAME *dst, DSV_FRAME *src);
extern void dsv_ds2x_frame_luma(DSV_FRAME *dest, DSV_FRAME *src);

extern DSV_FRAME *dsv_clone_frame(DSV_FRAME *f, int border);
extern DSV_FRAME *dsv_extend_frame(DSV_FRAME *frame);
extern DSV_FRAME *dsv_extend_frame_luma(DSV_FRAME *frame);

/* fills plane struct at (x, y), no error/bounds checking */
extern void dsv_plane_xy(DSV_FRAME *f, DSV_PLANE *out, int c, int x, int y);

typedef struct {
    DSV_META *vidmeta;

    int effort;
    int do_psy;

    int is_ref;
    int has_ref;

    /* block sizes */
    int blk_w;
    int blk_h;
    /* number of blocks horizontally and vertically in the image */
    int nblocks_h;
    int nblocks_v;

    int temporal_mc; /* temporal motion compensation state */
    int lossless;
} DSV_PARAMS;

typedef struct {
    uint8_t *data;
    unsigned len;
} DSV_BUF;

extern void dsv_mk_buf(DSV_BUF *buf, int size);
extern void dsv_buf_free(DSV_BUF *buffer);

extern int dsv_yuv_write(FILE *out, int fno, DSV_PLANE *p);
extern int dsv_yuv_write_seq(FILE *out, DSV_PLANE *p);
extern int dsv_yuv_read(FILE *in, int fno, uint8_t *o, int w, int h, int subsamp);
extern int dsv_yuv_read_seq(FILE *in, uint8_t *o, int w, int h, int subsamp);

#ifndef DSV_MEMORY_STATS
#define DSV_MEMORY_STATS 1
#endif

extern void *dsv_alloc(int size);
extern void dsv_free(void *ptr);

extern void dsv_memory_report(void);

#define DSV_LEVEL_NONE    0
#define DSV_LEVEL_ERROR   1
#define DSV_LEVEL_WARNING 2
#define DSV_LEVEL_INFO    3
#define DSV_LEVEL_DEBUG   4

extern char *dsv_lvlname[DSV_LEVEL_DEBUG + 1];

#define DSV_LOG_LVL(level, x) \
    do { if (level <= dsv_get_log_level()) { \
      printf("[DSV][%s] ", dsv_lvlname[level]); \
      printf("%s: %s(%d): ", __FILE__,  __FUNCTION__, __LINE__); \
      printf x; \
      printf("\n"); \
    }} while(0)\

#define DSV_ERROR(x)   DSV_LOG_LVL(DSV_LEVEL_ERROR, x)
#define DSV_WARNING(x) DSV_LOG_LVL(DSV_LEVEL_WARNING, x)
#define DSV_INFO(x)    DSV_LOG_LVL(DSV_LEVEL_INFO, x)
#define DSV_DEBUG(x)   DSV_LOG_LVL(DSV_LEVEL_DEBUG, x)

#if 1
#define DSV_ASSERT(x) do {                  \
    if (!(x)) {                             \
        DSV_ERROR(("assert: " #x));         \
        exit(-1);                           \
    }                                       \
} while(0)
#else
#define DSV_ASSERT(x)
#endif
extern void dsv_set_log_level(int level);
extern int dsv_get_log_level(void);

#ifdef __cplusplus
}
#endif

#endif
