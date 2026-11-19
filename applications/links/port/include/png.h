#ifndef LINKS_IPO_PNG_H
#define LINKS_IPO_PNG_H

#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PNG_LIBPNG_VER_STRING "1.6.40"
#define PNG_LIBPNG_VER        10640

#define PNG_USER_MEM_SUPPORTED 1
#define PNG_READ_SUPPORTED     1

#define PNG_COLOR_MASK_PALETTE 1
#define PNG_COLOR_MASK_COLOR   2
#define PNG_COLOR_MASK_ALPHA   4

#define PNG_COLOR_TYPE_GRAY       0
#define PNG_COLOR_TYPE_PALETTE    (PNG_COLOR_MASK_COLOR | PNG_COLOR_MASK_PALETTE)
#define PNG_COLOR_TYPE_RGB        (PNG_COLOR_MASK_COLOR)
#define PNG_COLOR_TYPE_RGB_ALPHA  (PNG_COLOR_MASK_COLOR | PNG_COLOR_MASK_ALPHA)
#define PNG_COLOR_TYPE_GRAY_ALPHA (PNG_COLOR_MASK_ALPHA)
#define PNG_COLOR_TYPE_RGBA       PNG_COLOR_TYPE_RGB_ALPHA
#define PNG_COLOR_TYPE_GA         PNG_COLOR_TYPE_GRAY_ALPHA

#define PNG_INFO_tRNS         0x0010u
#define PNG_INFO_gAMA         0x0001u
#define PNG_INFO_sRGB         0x0800u

#define PNG_INTERLACE_NONE    0
#define PNG_INTERLACE_ADAM7   1

typedef uint32_t png_uint_32;
typedef int32_t  png_int_32;
typedef uint8_t  png_byte;
typedef size_t   png_size_t;
typedef char     png_char;
typedef const char *png_const_charp;
typedef char    *png_charp;
typedef void    *png_voidp;
typedef png_byte *png_bytep;
typedef const png_byte *png_const_bytep;
typedef png_byte **png_bytepp;

typedef struct png_struct_def png_struct;
typedef png_struct *png_structp;
typedef png_struct **png_structpp;
typedef struct png_info_def png_info;
typedef png_info *png_infop;
typedef png_info **png_infopp;

typedef void (*png_error_ptr)(png_structp, png_const_charp);
typedef void (*png_rw_ptr)(png_structp, png_bytep, png_size_t);
typedef void *(*png_malloc_ptr)(png_structp, png_size_t);
typedef void (*png_free_ptr)(png_structp, png_voidp);
typedef void (*png_progressive_info_ptr)(png_structp, png_infop);
typedef void (*png_progressive_row_ptr)(png_structp, png_bytep, png_uint_32, int);
typedef void (*png_progressive_end_ptr)(png_structp, png_infop);

struct png_struct_def;
struct png_info_def;

jmp_buf *ipo_png_jmpbuf_ptr(png_structp png_ptr);
#define png_jmpbuf(png_ptr) (*ipo_png_jmpbuf_ptr(png_ptr))

void png_longjmp(png_structp png_ptr, int val);
void png_error(png_structp png_ptr, png_const_charp error_message);

png_structp png_create_read_struct(png_const_charp user_png_ver, png_voidp error_ptr,
                                   png_error_ptr error_fn, png_error_ptr warn_fn);
png_structp png_create_read_struct_2(png_const_charp user_png_ver, png_voidp error_ptr,
                                     png_error_ptr error_fn, png_error_ptr warn_fn,
                                     png_voidp mem_ptr, png_malloc_ptr malloc_fn,
                                     png_free_ptr free_fn);
png_infop png_create_info_struct(png_structp png_ptr);
void png_destroy_read_struct(png_structpp png_ptr_ptr, png_infopp info_ptr_ptr,
                             png_infopp end_info_ptr_ptr);

void png_set_read_fn(png_structp png_ptr, png_voidp io_ptr, png_rw_ptr read_data_fn);
png_voidp png_get_io_ptr(png_structp png_ptr);

void png_set_progressive_read_fn(png_structp png_ptr, png_voidp progressive_ptr,
                                 png_progressive_info_ptr info_fn,
                                 png_progressive_row_ptr row_fn,
                                 png_progressive_end_ptr end_fn);
png_voidp png_get_progressive_ptr(png_structp png_ptr);
void png_process_data(png_structp png_ptr, png_infop info_ptr,
                      png_bytep buffer, png_size_t buffer_size);
void png_progressive_combine_row(png_structp png_ptr, png_bytep old_row,
                                 png_const_bytep new_row);

void png_read_info(png_structp png_ptr, png_infop info_ptr);
void png_read_update_info(png_structp png_ptr, png_infop info_ptr);
void png_read_rows(png_structp png_ptr, png_bytepp row, png_bytepp display_row,
                   png_uint_32 num_rows);
void png_read_end(png_structp png_ptr, png_infop info_ptr);

png_uint_32 png_get_image_width(png_structp png_ptr, png_infop info_ptr);
png_uint_32 png_get_image_height(png_structp png_ptr, png_infop info_ptr);
png_byte png_get_color_type(png_structp png_ptr, png_infop info_ptr);
png_byte png_get_bit_depth(png_structp png_ptr, png_infop info_ptr);
png_byte png_get_interlace_type(png_structp png_ptr, png_infop info_ptr);
png_uint_32 png_get_valid(png_structp png_ptr, png_infop info_ptr, png_uint_32 flag);
png_uint_32 png_get_gAMA(png_structp png_ptr, png_infop info_ptr, double *file_gamma);
png_uint_32 png_get_sRGB(png_structp png_ptr, png_infop info_ptr, int *file_srgb_intent);

void png_set_gamma(png_structp png_ptr, double screen_gamma, double override_file_gamma);
void png_set_expand(png_structp png_ptr);
void png_set_strip_16(png_structp png_ptr);
void png_set_strip_alpha(png_structp png_ptr);
void png_set_rgb_to_gray(png_structp png_ptr, int error_action,
                         double red_weight, double green_weight);
void png_set_gray_to_rgb(png_structp png_ptr);
void png_set_swap(png_structp png_ptr);
int  png_set_interlace_handling(png_structp png_ptr);
png_const_charp png_get_libpng_ver(png_structp png_ptr);

#ifdef __cplusplus
}
#endif

#endif /* LINKS_IPO_PNG_H */
