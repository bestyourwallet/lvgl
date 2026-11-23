/**
 * @file lv_png.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "../../../lvgl.h"
#if LV_USE_PNG

#include "lv_png.h"
#include "lodepng.h"
#include <stdlib.h>
#include <string.h>
#if LV_USE_FS_FATFS && defined(LVGL_UI) && !defined(TREZOR_EMULATOR)
#include "ff.h"
#define LV_PNG_HEADER_CACHE_ENABLE 1
#else
#define LV_PNG_HEADER_CACHE_ENABLE 0
#endif

/*********************
 *      DEFINES
 *********************/
#ifdef LODEPNG_NO_COMPILE_ALLOCATORS
#define m_free  lodepng_free
#else
#define m_free lv_mem_free
#endif


/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static lv_res_t decoder_info(struct _lv_img_decoder_t * decoder, const void * src, lv_img_header_t * header);
static lv_res_t decoder_open(lv_img_decoder_t * dec, lv_img_decoder_dsc_t * dsc);
static void decoder_close(lv_img_decoder_t * dec, lv_img_decoder_dsc_t * dsc);
static void convert_color_depth(uint8_t * img, uint32_t px_cnt);

/**********************
 *  STATIC VARIABLES
 **********************/
#if LV_PNG_HEADER_CACHE_ENABLE
#define PNG_HDR_CACHE_SIZE 256U
#define PNG_HDR_CACHE_MASK (PNG_HDR_CACHE_SIZE - 1U)
#define PNG_HDR_PATH_MAX   104U

typedef struct {
    uint32_t  gen;
    lv_coord_t w;
    lv_coord_t h;
    char      path[PNG_HDR_PATH_MAX];
} png_hdr_cache_entry_t;

static png_hdr_cache_entry_t png_hdr_cache[PNG_HDR_CACHE_SIZE] __attribute__((section(".exram")));
static uint32_t png_hdr_cache_inited = 0;

static uint32_t png_hdr_hash(const char * s)
{
    uint32_t h = 2166136261U;
    while(*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619U;
    }
    return h;
}

static uint32_t png_hdr_cache_gen(const char * fn)
{
    if(fn[0] != LV_FS_FATFS_LETTER) return 0;
    const char * path = fn + 1;
    if(*path == ':') path++;
    return ff_ro_cache_get_gen(path);
}

static bool png_hdr_cache_get(const char * fn, lv_coord_t * w, lv_coord_t * h)
{
    if(!png_hdr_cache_inited) return false;
    uint32_t gen = png_hdr_cache_gen(fn);
    if(gen == 0) return false;
    uint32_t hash = png_hdr_hash(fn);
    for(uint32_t i = 0; i < 8U; i++) {
        const png_hdr_cache_entry_t * e = &png_hdr_cache[(hash + i) & PNG_HDR_CACHE_MASK];
        if(e->gen != gen) return false;
        if(strcmp(e->path, fn) == 0) {
            *w = e->w;
            *h = e->h;
            return true;
        }
    }
    return false;
}

static void png_hdr_cache_put(const char * fn, lv_coord_t w, lv_coord_t h)
{
    size_t len = strlen(fn);
    if(len == 0 || len >= PNG_HDR_PATH_MAX) return;
    uint32_t gen = png_hdr_cache_gen(fn);
    if(gen == 0) return;
    if(!png_hdr_cache_inited) {
        memset(png_hdr_cache, 0, sizeof(png_hdr_cache));
        png_hdr_cache_inited = 1;
    }
    uint32_t hash = png_hdr_hash(fn);
    uint32_t slot = hash & PNG_HDR_CACHE_MASK;
    for(uint32_t i = 0; i < 8U; i++) {
        uint32_t idx = (hash + i) & PNG_HDR_CACHE_MASK;
        if(png_hdr_cache[idx].gen != gen || strcmp(png_hdr_cache[idx].path, fn) == 0) {
            slot = idx;
            break;
        }
    }
    png_hdr_cache[slot].gen = gen;
    png_hdr_cache[slot].w = w;
    png_hdr_cache[slot].h = h;
    memcpy(png_hdr_cache[slot].path, fn, len + 1U);
}
#endif

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Register the PNG decoder functions in LVGL
 */
void lv_png_init(void)
{
    lv_img_decoder_t * dec = lv_img_decoder_create();
    lv_img_decoder_set_info_cb(dec, decoder_info);
    lv_img_decoder_set_open_cb(dec, decoder_open);
    lv_img_decoder_set_close_cb(dec, decoder_close);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Get info about a PNG image
 * @param src can be file name or pointer to a C array
 * @param header store the info here
 * @return LV_RES_OK: no error; LV_RES_INV: can't get the info
 */
static lv_res_t decoder_info(struct _lv_img_decoder_t * decoder, const void * src, lv_img_header_t * header)
{
    (void) decoder; /*Unused*/
    lv_img_src_t src_type = lv_img_src_get_type(src);          /*Get the source type*/

    /*If it's a PNG file...*/
    if(src_type == LV_IMG_SRC_FILE) {
        const char * fn = src;
        if(strcmp(lv_fs_get_ext(fn), "png") == 0) {              /*Check the extension*/
#if LV_PNG_HEADER_CACHE_ENABLE
            lv_coord_t cached_w = 0, cached_h = 0;
            if(png_hdr_cache_get(fn, &cached_w, &cached_h)) {
                header->always_zero = 0;
                header->cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
                header->w = cached_w;
                header->h = cached_h;
                return LV_RES_OK;
            }
#endif
            /* Read the width and height from the file. They have a constant location:
             * [16..23]: width
             * [24..27]: height
             */
            uint32_t size[2];
            lv_fs_file_t f;
            lv_fs_res_t res = lv_fs_open(&f, fn, LV_FS_MODE_RD);
            if(res != LV_FS_RES_OK) return LV_RES_INV;

            lv_fs_seek(&f, 16, LV_FS_SEEK_SET);

            uint32_t rn;
            lv_fs_read(&f, &size, 8, &rn);
            lv_fs_close(&f);

            if(rn != 8) return LV_RES_INV;

            /*Save the data in the header*/
            header->always_zero = 0;
            header->cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
            /*The width and height are stored in Big endian format so convert them to little endian*/
            header->w = (lv_coord_t)((size[0] & 0xff000000) >> 24) + ((size[0] & 0x00ff0000) >> 8);
            header->h = (lv_coord_t)((size[1] & 0xff000000) >> 24) + ((size[1] & 0x00ff0000) >> 8);
#if LV_PNG_HEADER_CACHE_ENABLE
            png_hdr_cache_put(fn, header->w, header->h);
#endif

            return LV_RES_OK;
        }
    }
    /*If it's a PNG file in a  C array...*/
    else if(src_type == LV_IMG_SRC_VARIABLE) {
        const lv_img_dsc_t * img_dsc = src;
        const uint32_t data_size = img_dsc->data_size;
        const uint8_t magic[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
        if(data_size < sizeof(magic)) return LV_RES_INV;
        if(memcmp(magic, img_dsc->data, sizeof(magic))) return LV_RES_INV;
        header->always_zero = 0;
        header->cf = img_dsc->header.cf;       /*Save the color format*/
        header->w = img_dsc->header.w;         /*Save the color width*/
        header->h = img_dsc->header.h;         /*Save the color height*/
        return LV_RES_OK;
    }

    return LV_RES_INV;         /*If didn't succeeded earlier then it's an error*/
}


/**
 * Open a PNG image and return the decided image
 * @param src can be file name or pointer to a C array
 * @param style style of the image object (unused now but certain formats might use it)
 * @return pointer to the decoded image or `LV_IMG_DECODER_OPEN_FAIL` if failed
 */
static lv_res_t decoder_open(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{

    (void) decoder; /*Unused*/
    uint32_t error;                 /*For the return values of PNG decoder functions*/

    uint8_t * img_data = NULL;

    /*If it's a PNG file...*/
    if(dsc->src_type == LV_IMG_SRC_FILE) {
        const char * fn = dsc->src;
        if(strcmp(lv_fs_get_ext(fn), "png") == 0) {              /*Check the extension*/

            /*Load the PNG file into buffer. It's still compressed (not decoded)*/
            unsigned char * png_data;      /*Pointer to the loaded data. Same as the original file just loaded into the RAM*/
            size_t png_data_size;          /*Size of `png_data` in bytes*/

            error = lodepng_load_file(&png_data, &png_data_size, fn);   /*Load the file*/
            if(error) {
                LV_LOG_WARN("error %u: %s\n", error, lodepng_error_text(error));
                return LV_RES_INV;
            }

            /*Decode the PNG image*/
            unsigned png_width;             /*Will be the width of the decoded image*/
            unsigned png_height;            /*Will be the width of the decoded image*/

            /*Decode the loaded image in ARGB8888 */
            error = lodepng_decode32(&img_data, &png_width, &png_height, png_data, png_data_size);
            m_free(png_data); /*Free the loaded file*/
            if(error) {
                if(img_data != NULL) {
                    lv_mem_free(img_data);
                }
                LV_LOG_WARN("error %u: %s\n", error, lodepng_error_text(error));
                return LV_RES_INV;
            }

            /*Convert the image to the system's color depth*/
            convert_color_depth(img_data,  png_width * png_height);
            dsc->img_data = img_data;
            return LV_RES_OK;     /*The image is fully decoded. Return with its pointer*/
        }
    }
    /*If it's a PNG file in a  C array...*/
    else if(dsc->src_type == LV_IMG_SRC_VARIABLE) {
        const lv_img_dsc_t * img_dsc = dsc->src;
        unsigned png_width;             /*No used, just required by he decoder*/
        unsigned png_height;            /*No used, just required by he decoder*/

        /*Decode the image in ARGB8888 */
        error = lodepng_decode32(&img_data, &png_width, &png_height, img_dsc->data, img_dsc->data_size);

        if(error) {
            if(img_data != NULL) {
                lv_mem_free(img_data);
            }
            return LV_RES_INV;
        }

        /*Convert the image to the system's color depth*/
        convert_color_depth(img_data,  png_width * png_height);

        dsc->img_data = img_data;
        return LV_RES_OK;     /*Return with its pointer*/
    }

    return LV_RES_INV;    /*If not returned earlier then it failed*/
}

/**
 * Free the allocated resources
 */
static void decoder_close(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{
    LV_UNUSED(decoder); /*Unused*/
    if(dsc->img_data) {
        m_free((uint8_t *)dsc->img_data);
        dsc->img_data = NULL;
    }
}

/**
 * If the display is not in 32 bit format (ARGB888) then covert the image to the current color depth
 * @param img the ARGB888 image
 * @param px_cnt number of pixels in `img`
 */
static void convert_color_depth(uint8_t * img, uint32_t px_cnt)
{
#if LV_COLOR_DEPTH == 32
    lv_color32_t * img_argb = (lv_color32_t *)img;
    lv_color_t c;
    lv_color_t * img_c = (lv_color_t *) img;
    uint32_t i;
    for(i = 0; i < px_cnt; i++) {
        c = lv_color_make(img_argb[i].ch.red, img_argb[i].ch.green, img_argb[i].ch.blue);
        img_c[i].ch.red = c.ch.blue;
        img_c[i].ch.blue = c.ch.red;
    }
#elif LV_COLOR_DEPTH == 16
    lv_color32_t * img_argb = (lv_color32_t *)img;
    lv_color_t c;
    uint32_t i;
    for(i = 0; i < px_cnt; i++) {
        c = lv_color_make(img_argb[i].ch.blue, img_argb[i].ch.green, img_argb[i].ch.red);
        img[i * 3 + 2] = img_argb[i].ch.alpha;
        img[i * 3 + 1] = c.full >> 8;
        img[i * 3 + 0] = c.full & 0xFF;
    }
#elif LV_COLOR_DEPTH == 8
    lv_color32_t * img_argb = (lv_color32_t *)img;
    lv_color_t c;
    uint32_t i;
    for(i = 0; i < px_cnt; i++) {
        c = lv_color_make(img_argb[i].ch.red, img_argb[i].ch.green, img_argb[i].ch.blue);
        img[i * 2 + 1] = img_argb[i].ch.alpha;
        img[i * 2 + 0] = c.full;
    }
#elif LV_COLOR_DEPTH == 1
    lv_color32_t * img_argb = (lv_color32_t *)img;
    uint8_t b;
    uint32_t i;
    for(i = 0; i < px_cnt; i++) {
        b = img_argb[i].ch.red | img_argb[i].ch.green | img_argb[i].ch.blue;
        img[i * 2 + 1] = img_argb[i].ch.alpha;
        img[i * 2 + 0] = b > 128 ? 1 : 0;
    }
#endif
}

#endif /*LV_USE_PNG*/


