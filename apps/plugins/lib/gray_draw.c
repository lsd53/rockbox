/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
* $Id$
*
* Greyscale framework
* Drawing functions for buffered mode
*
* This is a generic framework to use grayscale display within Rockbox
* plugins. It obviously does not work for the player.
*
* Copyright (C) 2004-2005 Jens Arnold
*
* All files in this archive are subject to the GNU General Public License.
* See the file COPYING in the source tree root for full license agreement.
*
* This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
* KIND, either express or implied.
*
****************************************************************************/

#ifndef SIMULATOR /* not for simulator by now */
#include "plugin.h"

#ifdef HAVE_LCD_BITMAP /* and also not for the Player */
#include "gray.h"

/*** low-level drawing functions ***/

static void setpixel(unsigned char *address)
{
    *address = _gray_info.fg_brightness;
}

static void clearpixel(unsigned char *address)
{
    *address = _gray_info.bg_brightness;
}

static void flippixel(unsigned char *address)
{
    *address = _LEVEL_FAC * _gray_info.depth - *address;
}

static void nopixel(unsigned char *address)
{
    (void)address;
}

void (* const _gray_pixelfuncs[8])(unsigned char *address) = {
    flippixel, nopixel, setpixel, setpixel,
    nopixel, clearpixel, nopixel, clearpixel
};

/*** Drawing functions ***/

/* Clear the whole display */
void gray_clear_display(void)
{
    int brightness = (_gray_info.drawmode & DRMODE_INVERSEVID) ?
                     _gray_info.fg_brightness : _gray_info.bg_brightness;

    _gray_rb->memset(_gray_info.cur_buffer, brightness,
                     MULU16(_gray_info.width, _gray_info.height));
}

/* Set a single pixel */
void gray_drawpixel(int x, int y)
{
    if (((unsigned)x < (unsigned)_gray_info.width)
        && ((unsigned)y < (unsigned)_gray_info.height))
        _gray_pixelfuncs[_gray_info.drawmode](&_gray_info.cur_buffer[MULU16(x,
                                               _gray_info.height) + y]);
}

/* Draw a line */
void gray_drawline(int x1, int y1, int x2, int y2)
{
    int numpixels;
    int i;
    int deltax, deltay;
    int d, dinc1, dinc2;
    int x, xinc1, xinc2;
    int y, yinc1, yinc2;
    void (*pfunc)(unsigned char *address) = _gray_pixelfuncs[_gray_info.drawmode];

    deltax = abs(x2 - x1);
    deltay = abs(y2 - y1);
    xinc2 = 1;
    yinc2 = 1;

    if (deltax >= deltay)
    {
        numpixels = deltax;
        d = 2 * deltay - deltax;
        dinc1 = deltay * 2;
        dinc2 = (deltay - deltax) * 2;
        xinc1 = 1;
        yinc1 = 0;
    }
    else
    {
        numpixels = deltay;
        d = 2 * deltax - deltay;
        dinc1 = deltax * 2;
        dinc2 = (deltax - deltay) * 2;
        xinc1 = 0;
        yinc1 = 1;
    }
    numpixels++; /* include endpoints */

    if (x1 > x2)
    {
        xinc1 = -xinc1;
        xinc2 = -xinc2;
    }

    if (y1 > y2)
    {
        yinc1 = -yinc1;
        yinc2 = -yinc2;
    }

    x = x1;
    y = y1;

    for (i = 0; i < numpixels; i++)
    {
        if (((unsigned)x < (unsigned)_gray_info.width) 
            && ((unsigned)y < (unsigned)_gray_info.height))
            pfunc(&_gray_info.cur_buffer[MULU16(x, _gray_info.height) + y]);

        if (d < 0)
        {
            d += dinc1;
            x += xinc1;
            y += yinc1;
        }
        else
        {
            d += dinc2;
            x += xinc2;
            y += yinc2;
        }
    }
}

/* Draw a horizontal line (optimised) */
void gray_hline(int x1, int x2, int y)
{
    int x;
    unsigned char *dst, *dst_end;
    void (*pfunc)(unsigned char *address);

    /* direction flip */
    if (x2 < x1)
    {
        x = x1;
        x1 = x2;
        x2 = x;
    }
    
    /* nothing to draw? */
    if (((unsigned)y >= (unsigned)_gray_info.height) 
        || (x1 >= _gray_info.width) || (x2 < 0))
        return;  
    
    /* clipping */
    if (x1 < 0)
        x1 = 0;
    if (x2 >= _gray_info.width)
        x2 = _gray_info.width - 1;
        
    pfunc = _gray_pixelfuncs[_gray_info.drawmode];
    dst   = &_gray_info.cur_buffer[MULU16(x1, _gray_info.height) + y];

    dst_end = dst + MULU16(x2 - x1, _gray_info.height);
    do
    {
        pfunc(dst);
        dst += _gray_info.height;
    }
    while (dst <= dst_end);
}

/* Draw a vertical line (optimised) */
void gray_vline(int x, int y1, int y2)
{
    int y, bits;
    unsigned char *dst;
    bool fillopt;
    void (*pfunc)(unsigned char *address);

    /* direction flip */
    if (y2 < y1)
    {
        y = y1;
        y1 = y2;
        y2 = y;
    }

    /* nothing to draw? */
    if (((unsigned)x >= (unsigned)_gray_info.width) 
        || (y1 >= _gray_info.height) || (y2 < 0))
        return;  
    
    /* clipping */
    if (y1 < 0)
        y1 = 0;
    if (y2 >= _gray_info.height)
        y2 = _gray_info.height - 1;

    bits = _gray_info.fg_brightness;
    fillopt = (_gray_info.drawmode & DRMODE_INVERSEVID) ?
              (_gray_info.drawmode & DRMODE_BG) :
              (_gray_info.drawmode & DRMODE_FG);
    if (fillopt &&(_gray_info.drawmode & DRMODE_INVERSEVID))
        bits = _gray_info.bg_brightness;

    pfunc = _gray_pixelfuncs[_gray_info.drawmode];
    dst   = &_gray_info.cur_buffer[MULU16(x, _gray_info.height) + y1];

    if (fillopt)
        _gray_rb->memset(dst, bits, y2 - y1 + 1);
    else
    {
        unsigned char *dst_end = dst + y2 - y1;
        do
            pfunc(dst++);
        while (dst <= dst_end);
    }
}

/* Draw a rectangular box */
void gray_drawrect(int x, int y, int width, int height)
{
    if ((width <= 0) || (height <= 0))
        return;

    int x2 = x + width - 1;
    int y2 = y + height - 1;

    gray_vline(x, y, y2);
    gray_vline(x2, y, y2);
    gray_hline(x, x2, y);
    gray_hline(x, x2, y2);
}

/* Fill a rectangular area */
void gray_fillrect(int x, int y, int width, int height)
{
    int bits;
    unsigned char *dst, *dst_end;
    bool fillopt;
    void (*pfunc)(unsigned char *address);

    /* nothing to draw? */
    if ((width <= 0) || (height <= 0) || (x >= _gray_info.width) 
        || (y >= _gray_info.height) || (x + width <= 0) || (y + height <= 0))
        return;

    /* clipping */
    if (x < 0)
    {
        width += x;
        x = 0;
    }
    if (y < 0)
    {
        height += y;
        y = 0;
    }
    if (x + width > _gray_info.width)
        width = _gray_info.width - x;
    if (y + height > _gray_info.height)
        height = _gray_info.height - y;
    
    bits = _gray_info.fg_brightness;
    fillopt = (_gray_info.drawmode & DRMODE_INVERSEVID) ?
              (_gray_info.drawmode & DRMODE_BG) :
              (_gray_info.drawmode & DRMODE_FG);
    if (fillopt &&(_gray_info.drawmode & DRMODE_INVERSEVID))
        bits = _gray_info.bg_brightness;

    pfunc = _gray_pixelfuncs[_gray_info.drawmode];
    dst   = &_gray_info.cur_buffer[MULU16(x, _gray_info.height) + y];
    dst_end = dst + MULU16(width, _gray_info.height);

    do
    {
        if (fillopt)
            _gray_rb->memset(dst, bits, height);
        else
        {
            unsigned char *dst_col = dst;
            unsigned char *col_end = dst_col + height;
            
            do
                pfunc(dst_col++);
            while (dst_col < col_end);
        }
        dst += _gray_info.height;
    }
    while (dst < dst_end);
}

/* Draw a filled triangle */
void gray_filltriangle(int x1, int y1, int x2, int y2, int x3, int y3)
{
    int x, y;
    long fp_y1, fp_y2, fp_dy1, fp_dy2;

    /* sort vertices by increasing x value */
    if (x1 > x3)
    {
        if (x2 < x3)       /* x2 < x3 < x1 */
        {
            x = x1; x1 = x2; x2 = x3; x3 = x;
            y = y1; y1 = y2; y2 = y3; y3 = y;
        }
        else if (x2 > x1)  /* x3 < x1 < x2 */
        {
            x = x1; x1 = x3; x3 = x2; x2 = x;
            y = y1; y1 = y3; y3 = y2; y2 = y;
        }
        else               /* x3 <= x2 <= x1 */
        {
            x = x1; x1 = x3; x3 = x;
            y = y1; y1 = y3; y3 = y;
        }
    }
    else
    {
        if (x2 < x1)       /* x2 < x1 <= x3 */
        {
            x = x1; x1 = x2; x2 = x;
            y = y1; y1 = y2; y2 = y;
        }
        else if (x2 > x3)  /* x1 <= x3 < x2 */
        {
            x = x2; x2 = x3; x3 = x;
            y = y2; y2 = y3; y3 = y;
        }
        /* else already sorted */
    }

    if (x1 < x3)  /* draw */
    {
        fp_dy1 = ((y3 - y1) << 16) / (x3 - x1);
        fp_y1  = (y1 << 16) + (1<<15) + (fp_dy1 >> 1);

        if (x1 < x2)  /* first part */
        {   
            fp_dy2 = ((y2 - y1) << 16) / (x2 - x1);
            fp_y2  = (y1 << 16) + (1<<15) + (fp_dy2 >> 1);
            for (x = x1; x < x2; x++)
            {
                gray_vline(x, fp_y1 >> 16, fp_y2 >> 16);
                fp_y1 += fp_dy1;
                fp_y2 += fp_dy2;
            }
        }
        if (x2 < x3)  /* second part */
        {
            fp_dy2 = ((y3 - y2) << 16) / (x3 - x2);
            fp_y2 = (y2 << 16) + (1<<15) + (fp_dy2 >> 1);
            for (x = x2; x < x3; x++)
            {
                gray_vline(x, fp_y1 >> 16, fp_y2 >> 16);
                fp_y1 += fp_dy1;
                fp_y2 += fp_dy2;
            }
        }
    }
}

/* About Rockbox' internal monochrome bitmap format:
 *
 * A bitmap contains one bit for every pixel that defines if that pixel is
 * foreground (1) or background (0). Bits within a byte are arranged
 * vertically, LSB at top.
 * The bytes are stored in row-major order, with byte 0 being top left,
 * byte 1 2nd from left etc. The first row of bytes defines pixel rows
 * 0..7, the second row defines pixel row 8..15 etc.
 *
 * This is similar to the internal lcd hw format. */

/* Draw a partial monochrome bitmap */
void gray_mono_bitmap_part(const unsigned char *src, int src_x, int src_y,
                           int stride, int x, int y, int width, int height)
{
    const unsigned char *src_end;
    unsigned char *dst, *dst_end;
    void (*fgfunc)(unsigned char *address);
    void (*bgfunc)(unsigned char *address);

    /* nothing to draw? */
    if ((width <= 0) || (height <= 0) || (x >= _gray_info.width) 
        || (y >= _gray_info.height) || (x + width <= 0) || (y + height <= 0))
        return;
        
    /* clipping */
    if (x < 0)
    {
        width += x;
        src_x -= x;
        x = 0;
    }
    if (y < 0)
    {
        height += y;
        src_y -= y;
        y = 0;
    }
    if (x + width > _gray_info.width)
        width = _gray_info.width - x;
    if (y + height > _gray_info.height)
        height = _gray_info.height - y;

    src    += MULU16(stride, src_y >> 3) + src_x; /* move starting point */
    src_y  &= 7;
    src_end = src + width;

    dst    = &_gray_info.cur_buffer[MULU16(x, _gray_info.height) + y];
    fgfunc = _gray_pixelfuncs[_gray_info.drawmode];
    bgfunc = _gray_pixelfuncs[_gray_info.drawmode ^ DRMODE_INVERSEVID];
    
    do
    {
        const unsigned char *src_col = src++;
        unsigned char *dst_col = dst;
        unsigned char data = *src_col >> src_y;
        int numbits = 8 - src_y;
        
        dst_end = dst_col + height;
        do
        {
            if (data & 0x01)
                fgfunc(dst_col++);
            else
                bgfunc(dst_col++);

            data >>= 1;
            if (--numbits == 0)
            {
                src_col += stride;
                data = *src_col;
                numbits = 8;
            }
        }
        while (dst_col < dst_end);
        
        dst += _gray_info.height;
    }
    while (src < src_end);
}

/* Draw a full monochrome bitmap */
void gray_mono_bitmap(const unsigned char *src, int x, int y, int width, int height)
{
    gray_mono_bitmap_part(src, 0, 0, width, x, y, width, height);
}

/* Draw a partial greyscale bitmap, canonical format */
void gray_gray_bitmap_part(const unsigned char *src, int src_x, int src_y,
                           int stride, int x, int y, int width, int height)
{
    const unsigned char *src_end;
    unsigned char *dst, *dst_end;

    /* nothing to draw? */
    if ((width <= 0) || (height <= 0) || (x >= _gray_info.width)
        || (y >= _gray_info.height) || (x + width <= 0) || (y + height <= 0))
        return;
        
    /* clipping */
    if (x < 0)
    {
        width += x;
        src_x -= x;
        x = 0;
    }
    if (y < 0)
    {
        height += y;
        src_y -= y;
        y = 0;
    }
    if (x + width > _gray_info.width)
        width = _gray_info.width - x;
    if (y + height > _gray_info.height)
        height = _gray_info.height - y;

    src    += MULU16(stride, src_y) + src_x; /* move starting point */
    src_end = src + width;
    dst     = &_gray_info.cur_buffer[MULU16(x, _gray_info.height) + y];

    do
    {
        const unsigned char *src_col = src++;
        unsigned char *dst_col = dst;

        dst_end = dst_col + height;
        do
        {
            unsigned data = MULU16(_LEVEL_FAC * _gray_info.depth, *src_col) + 127;
            *dst_col++ = (data + (data >> 8)) >> 8; /* approx. data / 255 */
            src_col += stride;
        }
        while (dst_col < dst_end);

        dst += _gray_info.height;
    }
    while (src < src_end);
}

/* Draw a full greyscale bitmap, canonical format */
void gray_gray_bitmap(const unsigned char *src, int x, int y, int width,
                      int height)
{
    gray_gray_bitmap_part(src, 0, 0, width, x, y, width, height);
}

/* Put a string at a given pixel position, skipping first ofs pixel columns */
void gray_putsxyofs(int x, int y, int ofs, const unsigned char *str)
{
    int ch;
    struct font* pf = font_get(_gray_info.curfont);

    while ((ch = *str++) != '\0' && x < _gray_info.width)
    {
        int width;
        const unsigned char *bits;

        /* check input range */
        if (ch < pf->firstchar || ch >= pf->firstchar+pf->size)
            ch = pf->defaultchar;
        ch -= pf->firstchar;

        /* get proportional width and glyph bits */
        width = pf->width ? pf->width[ch] : pf->maxwidth;

        if (ofs > width)
        {
            ofs -= width;
            continue;
        }

        bits = pf->bits + (pf->offset ?
               pf->offset[ch] : ((pf->height + 7) / 8 * pf->maxwidth * ch));

        gray_mono_bitmap_part(bits, ofs, 0, width, x, y, width - ofs, pf->height);
        
        x += width - ofs;
        ofs = 0;
    }
}

/* Put a string at a given pixel position */
void gray_putsxy(int x, int y, const unsigned char *str)
{
    gray_putsxyofs(x, y, 0, str);
}

/*** Unbuffered drawing functions ***/

/* Clear the greyscale display (sets all pixels to white) */
void gray_ub_clear_display(void)
{
    _gray_rb->memset(_gray_info.plane_data, 0, MULU16(_gray_info.depth,
                     _gray_info.plane_size));
}

/* Write a pixel block, defined by their brightnesses in a greymap.
   Address is the byte in the first bitplane, src is the greymap start address,
   stride is the increment for the greymap to get to the next pixel, mask
   determines which pixels of the destination block are changed. For "0" bits,
   the src address is not incremented! */
static void _writearray(unsigned char *address, const unsigned char *src,
                        int stride, unsigned mask)
{
#if (CONFIG_CPU == SH7034) && (LCD_DEPTH == 1)
    unsigned long pat_stack[8];
    unsigned long *pat_ptr = &pat_stack[8];
    unsigned char *end_addr;

    /* precalculate the bit patterns with random shifts 
       for all 8 pixels and put them on an extra "stack" */
    asm (
        "mov     #8,r3       \n"  /* loop count in r3: 8 pixels */
        "mov     %7,r2       \n"  /* copy mask -- gcc bug workaround */

    ".wa_loop:               \n"  /** load pattern for pixel **/
        "mov     #0,r0       \n"  /* pattern for skipped pixel must be 0 */
        "shlr    r2          \n"  /* shift out lsb of mask */
        "bf      .wa_skip    \n"  /* skip this pixel */

        "mov.b   @%2,r0      \n"  /* load src byte */
        "extu.b  r0,r0       \n"  /* extend unsigned */
        "mulu    %4,r0       \n"  /* macl = byte * depth; */
        "sts     macl,r1     \n"  /* r1 = macl; */
        "add     #127,r1     \n"  /* byte += 127; */
        "mov     r1,r0       \n"
        "shlr8   r1          \n"
        "add     r1,r0       \n"  /* byte += byte >> 8; */
        "shlr8   r0          \n"  /* byte >>= 8; */
        "shll2   r0          \n"
        "mov.l   @(r0,%5),r4 \n"  /* r4 = bitpattern[byte]; */

        "mov     #75,r0      \n"
        "mulu    r0,%0       \n"  /* multiply by 75 */
        "sts     macl,%0     \n"
        "add     #74,%0      \n"  /* add another 74 */
        /* Since the lower bits are not very random: */
        "swap.b  %0,r1       \n"  /* get bits 8..15 (need max. 5) */
        "and     %6,r1       \n"  /* mask out unneeded bits */

        "cmp/hs  %4,r1       \n"  /* random >= depth ? */
        "bf      .wa_ntrim   \n"
        "sub     %4,r1       \n"  /* yes: random -= depth; */
    ".wa_ntrim:              \n"

        "mov.l   .ashlsi3,r0 \n"  /** rotate pattern **/
        "jsr     @r0         \n"  /* r4 -> r0, shift left by r5 */
        "mov     r1,r5       \n"

        "mov     %4,r5       \n"
        "sub     r1,r5       \n"  /* r5 = depth - r1 */
        "mov.l   .lshrsi3,r1 \n"
        "jsr     @r1         \n"  /* r4 -> r0, shift right by r5 */
        "mov     r0,r1       \n"  /* store previous result in r1 */

        "or      r1,r0       \n"  /* rotated_pattern = r0 | r1 */

    ".wa_skip:               \n"
        "mov.l   r0,@-%1     \n"  /* push on pattern stack */

        "add     %3,%2       \n"  /* src += stride; */
        "add     #-1,r3      \n"  /* decrease loop count */
        "cmp/pl  r3          \n"  /* loop count > 0? */
        "bt      .wa_loop    \n"  /* yes: loop */
        : /* outputs */
        /* %0, in & out */ "+r"(_gray_random_buffer),
        /* %1, in & out */ "+r"(pat_ptr)
        : /* inputs */
        /* %2 */ "r"(src),
        /* %3 */ "r"(stride),
        /* %4 */ "r"(_gray_info.depth),
        /* %5 */ "r"(_gray_info.bitpattern),
        /* %6 */ "r"(_gray_info.randmask),
        /* %7 */ "r"(mask)
        : /* clobbers */
        "r0", "r1", "r2", "r3", "r4", "r5", "macl", "pr"
    );

    end_addr = address + MULU16(_gray_info.depth, _gray_info.plane_size);

    /* set the bits for all 8 pixels in all bytes according to the
     * precalculated patterns on the pattern stack */
    asm (
        "mov.l   @%3+,r1     \n"  /* pop all 8 patterns */
        "mov.l   @%3+,r2     \n"
        "mov.l   @%3+,r3     \n"
        "mov.l   @%3+,r8     \n"
        "mov.l   @%3+,r9     \n"
        "mov.l   @%3+,r10    \n"
        "mov.l   @%3+,r11    \n"
        "mov.l   @%3+,r12    \n"

        "not     %4,%4       \n"  /* "set" mask -> "keep" mask */
        "extu.b  %4,%4       \n"  /* mask out high bits */
        "tst     %4,%4       \n"  /* nothing to keep? */
        "bt      .wa_sloop   \n"  /* yes: jump to short loop */

    ".wa_floop:              \n"  /** full loop (there are bits to keep)**/
        "shlr    r1          \n"  /* rotate lsb of pattern 1 to t bit */
        "rotcl   r0          \n"  /* rotate t bit into r0 */
        "shlr    r2          \n"
        "rotcl   r0          \n"
        "shlr    r3          \n"
        "rotcl   r0          \n"
        "shlr    r8          \n"
        "rotcl   r0          \n"
        "shlr    r9          \n"
        "rotcl   r0          \n"
        "shlr    r10         \n"
        "rotcl   r0          \n"
        "shlr    r11         \n"
        "rotcl   r0          \n"
        "shlr    r12         \n"
        "mov.b   @%0,%3      \n"  /* read old value */
        "rotcl   r0          \n"
        "and     %4,%3       \n"  /* mask out unneeded bits */
        "or      r0,%3       \n"  /* set new bits */
        "mov.b   %3,@%0      \n"  /* store value to bitplane */
        "add     %2,%0       \n"  /* advance to next bitplane */
        "cmp/hi  %0,%1       \n"  /* last bitplane done? */
        "bt      .wa_floop   \n"  /* no: loop */

        "bra     .wa_end     \n"
        "nop                 \n"

    ".wa_sloop:              \n"  /** short loop (nothing to keep) **/
        "shlr    r1          \n"  /* rotate lsb of pattern 1 to t bit */
        "rotcl   r0          \n"  /* rotate t bit into r0 */
        "shlr    r2          \n"
        "rotcl   r0          \n"
        "shlr    r3          \n"
        "rotcl   r0          \n"
        "shlr    r8          \n"
        "rotcl   r0          \n"
        "shlr    r9          \n"
        "rotcl   r0          \n"
        "shlr    r10         \n"
        "rotcl   r0          \n"
        "shlr    r11         \n"
        "rotcl   r0          \n"
        "shlr    r12         \n"
        "rotcl   r0          \n"
        "mov.b   r0,@%0      \n"  /* store byte to bitplane */
        "add     %2,%0       \n"  /* advance to next bitplane */
        "cmp/hi  %0,%1       \n"  /* last bitplane done? */
        "bt      .wa_sloop   \n"  /* no: loop */

    ".wa_end:                \n"
        : /* outputs */
        : /* inputs */
        /* %0 */ "r"(address),
        /* %1 */ "r"(end_addr),
        /* %2 */ "r"(_gray_info.plane_size),
        /* %3 */ "r"(pat_ptr),
        /* %4 */ "r"(mask)
        : /* clobbers */
        "r0", "r1", "r2", "r3", "r8", "r9", "r10", "r11", "r12"
    );
#endif
}

#if CONFIG_CPU == SH7034
/* References to C library routines used in _writearray() */
asm (
    ".align  2           \n"
".ashlsi3:               \n"  /* C library routine: */
    ".long   ___ashlsi3  \n"  /* shift r4 left by r5, return in r0 */
".lshrsi3:               \n"  /* C library routine: */
    ".long   ___lshrsi3  \n"  /* shift r4 right by r5, return in r0 */
    /* both routines preserve r4, destroy r5 and take ~16 cycles */
);
#endif

/* Draw a partial greyscale bitmap, canonical format */
void gray_ub_gray_bitmap_part(const unsigned char *src, int src_x, int src_y,
                              int stride, int x, int y, int width, int height)
{
    int shift, ny;
    unsigned char *dst, *dst_end;
    unsigned mask, mask_bottom;

    /* nothing to draw? */
    if ((width <= 0) || (height <= 0) || (x >= _gray_info.width)
        || (y >= _gray_info.height) || (x + width <= 0) || (y + height <= 0))
        return;
        
    /* clipping */
    if (x < 0)
    {
        width += x;
        src_x -= x;
        x = 0;
    }
    if (y < 0)
    {
        height += y;
        src_y -= y;
        y = 0;
    }
    if (x + width > _gray_info.width)
        width = _gray_info.width - x;
    if (y + height > _gray_info.height)
        height = _gray_info.height - y;

    shift = y & (_PBLOCK-1);
    src += MULU16(stride, src_y) + src_x - MULU16(stride, shift);
    dst  = _gray_info.plane_data + x
           + MULU16(_gray_info.width, y >> _PBLOCK_EXP);
    ny   = height - 1 + shift;

    mask = 0xFFu << shift;   /* ATTN LCD_DEPTH == 2 */
    mask_bottom = 0xFFu >> (~ny & (_PBLOCK-1));

    for (; ny >= _PBLOCK; ny -= _PBLOCK)
    {
        const unsigned char *src_row = src;
        unsigned char *dst_row = dst;

        dst_end = dst_row + width;
        do
            _writearray(dst_row++, src_row++, stride, mask);
        while (dst_row < dst_end);

        src += stride << _PBLOCK_EXP;
        dst += _gray_info.width;
        mask = 0xFFu;
    }
    mask &= mask_bottom;
    dst_end = dst + width;
    do
        _writearray(dst++, src++, stride, mask);
    while (dst < dst_end);
}

/* Draw a full greyscale bitmap, canonical format */
void gray_ub_gray_bitmap(const unsigned char *src, int x, int y, int width,
                         int height)
{
    gray_ub_gray_bitmap_part(src, 0, 0, width, x, y, width, height);
}


#endif /* HAVE_LCD_BITMAP */
#endif /* !SIMULATOR */

