/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
* $Id$
*
* Grayscale framework & demo plugin
*
* Copyright (C) 2004 Jens Arnold
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

/******************************* Globals ***********************************/

static struct plugin_api* rb; /* global api struct pointer */
static char pbuf[32];         /* global printf buffer */
static unsigned char *gbuf;
static unsigned int gbuf_size = 0;

/*********************** Begin grayscale framework *************************/

/* This is a generic framework to use grayscale display within rockbox
 * plugins. It obviously does not work for the player.
 *
 * If you want to use grayscale display within a plugin, copy this section
 * (up to "End grayscale framework") into your source and you are able to use
 * it. For detailed documentation look at the head of each public function.
 *
 * It requires a global Rockbox api pointer in "rb" and uses the rockbox
 * timer api so you cannot use that timer for other  purposes while
 * displaying grayscale.
 *
 * The framework consists of 3 sections:
 *
 * - internal core functions and definitions
 * - public core functions
 * - public optional functions
 *
 * Usually you will use functions from the latter two sections in your code.
 * You can cut out functions from the third section that you do not need in
 * order to not waste space. Don't forget to cut the prototype as well.
 */

/**** internal core functions and definitions ****/

/* You do not want to touch these if you don't know exactly what you're
 * doing. */

#define GRAY_RUNNING          0x0001  /* grayscale overlay is running */
#define GRAY_DEFERRED_UPDATE  0x0002  /* lcd_update() requested */

/* unsigned 16 bit multiplication (a single instruction on the SH) */
#define MULU16(a, b) (((unsigned short) (a)) * ((unsigned short) (b)))

typedef struct
{
    int x;
    int by;        /* 8-pixel units */
    int width;
    int height;
    int bheight;   /* 8-pixel units */
    int plane_size;
    int depth;     /* number_of_bitplanes  = (number_of_grayscales - 1) */
    int cur_plane; /* for the timer isr */
    unsigned long randmask; /* mask for random value in graypixel() */
    unsigned long flags;    /* various flags, see #defines */
    unsigned char *data;    /* pointer to start of bitplane data */
    unsigned long *bitpattern; /* pointer to start of pattern table */
} tGraybuf;

static tGraybuf *graybuf = NULL;
static short gray_random_buffer;

/** prototypes **/

void gray_timer_isr(void);
void graypixel(int x, int y, unsigned long pattern);
void grayblock(int x, int by, unsigned char* src, int stride);
void grayinvertmasked(int x, int by, unsigned char mask);

/** implementation **/

/* timer interrupt handler: display next bitplane */
void gray_timer_isr(void)
{
    rb->lcd_blit(graybuf->data + MULU16(graybuf->plane_size, graybuf->cur_plane),
                 graybuf->x, graybuf->by, graybuf->width, graybuf->bheight,
                 graybuf->width);

    if (++graybuf->cur_plane >= graybuf->depth)
        graybuf->cur_plane = 0;

    if (graybuf->flags & GRAY_DEFERRED_UPDATE)  /* lcd_update() requested? */
    {
        int x1 = MAX(graybuf->x, 0);
        int x2 = MIN(graybuf->x + graybuf->width, LCD_WIDTH);
        int y1 = MAX(graybuf->by << 3, 0);
        int y2 = MIN((graybuf->by + graybuf->bheight) << 3, LCD_HEIGHT);

        if (y1 > 0)  /* refresh part above overlay, full width */
            rb->lcd_update_rect(0, 0, LCD_WIDTH, y1);

        if (y2 < LCD_HEIGHT) /* refresh part below overlay, full width */
            rb->lcd_update_rect(0, y2, LCD_WIDTH, LCD_HEIGHT - y2);

        if (x1 > 0) /* refresh part to the left of overlay */
            rb->lcd_update_rect(0, y1, x1, y2 - y1);

        if (x2 < LCD_WIDTH) /* refresh part to the right of overlay */
            rb->lcd_update_rect(x2, y1, LCD_WIDTH - x2, y2 - y1);

        graybuf->flags &= ~GRAY_DEFERRED_UPDATE; /* clear request */
    }
}

/* Set a pixel to a specific bit pattern
 * This is the fundamental graphics primitive, asm optimized */
void graypixel(int x, int y, unsigned long pattern)
{
    register long address, mask, random;

    /* Some (pseudo-)random function must be used here to shift the bit
     * pattern randomly, otherwise you would get flicker and/or moire.
     * Since rand() is relatively slow, I've implemented a simple, but very
     * fast pseudo-random generator based on linear congruency in assembler.
     * It delivers 16 pseudo-random bits in each iteration. */

    /* simple but fast pseudo-random generator */
    asm(
        "mov.w   @%1,%0          \n"  /* load last value */
        "mov     #75,r1          \n"
        "mulu    %0,r1           \n"  /* multiply by 75 */
        "sts     macl,%0         \n"  /* get result */
        "add     #74,%0          \n"  /* add another 74 */
        "mov.w   %0,@%1          \n"  /* store new value */
        /* Since the lower bits are not very random: */
        "shlr8   %0              \n"  /* get bits 8..15 (need max. 5) */
        "and     %2,%0           \n"  /* mask out unneeded bits */
        : /* outputs */
        /* %0 */ "=&r"(random)
        : /* inputs */
        /* %1 */ "r"(&gray_random_buffer),
        /* %2 */ "r"(graybuf->randmask)
        : /* clobbers */
        "r1","macl"
    );

    /* precalculate mask and byte address in first bitplane */
    asm(
        "mov     %3,%0           \n"  /* take y as base for address offset */
        "shlr2   %0              \n"  /* shift right by 3 (= divide by 8) */
        "shlr    %0              \n"
        "mulu    %0,%2           \n"  /* multiply with width */
        "and     #7,%3           \n"  /* get lower 3 bits of y */
        "sts     macl,%0         \n"  /* get mulu result */
        "add     %4,%0           \n"  /* add base + x to get final address */

        "mov     %3,%1           \n"  /* move lower 3 bits of y out of r0 */
        "mova    .pp_table,%3    \n"  /* get address of mask table in r0 */
        "bra     .pp_end         \n"  /* skip the table */
        "mov.b   @(%3,%1),%1     \n"  /* get entry from mask table */
        
        ".align  2               \n"
    ".pp_table:                  \n"  /* mask table */
        ".byte   0x01            \n"
        ".byte   0x02            \n"
        ".byte   0x04            \n"
        ".byte   0x08            \n"
        ".byte   0x10            \n"
        ".byte   0x20            \n"
        ".byte   0x40            \n"
        ".byte   0x80            \n"

    ".pp_end:                    \n"
        : /* outputs */
        /* %0 */ "=&r"(address),    
        /* %1 */ "=&r"(mask)
        : /* inputs */
        /* %2 */ "r"(graybuf->width),
        /* %3 = r0 */ "z"(y),
        /* %4 */ "r"(graybuf->data + x)
        : /* clobbers */
        "macl"
    );

    /* the hard part: set bits in all bitplanes according to pattern */
    asm(
        "cmp/hs  %1,%5           \n"  /* random >= depth ? */
        "bf      .p_ntrim        \n"
        "sub     %1,%5           \n"  /* yes: random -= depth */
        /* it's sufficient to do this once, since the mask guarantees
         * random < 2 * depth */
    ".p_ntrim:                   \n"
    
        /* calculate some addresses */
        "mulu    %4,%1           \n"  /* end address offset */
        "not     %3,r1           \n"  /* get inverse mask (for "and") */
        "sts     macl,%1         \n"  /* result of mulu */
        "mulu    %4,%5           \n"  /* address offset of <random>'th plane */
        "add     %2,%1           \n"  /* end offset -> end address */
        "sts     macl,%5         \n"  /* result of mulu */
        "add     %2,%5           \n"  /* address of <random>'th plane */
        "bra     .p_start1       \n"
        "mov     %5,r2           \n"  /* copy address */

        /* first loop: set bits from <random>'th bitplane to last */
    ".p_loop1:                   \n"
        "mov.b   @r2,r3          \n"  /* get data byte */
        "shlr    %0              \n"  /* shift bit mask, sets t bit */
        "and     r1,r3           \n"  /* reset bit (-> "white") */
        "bf      .p_white1       \n"  /* t=0? -> "white" bit */
        "or      %3,r3           \n"  /* set bit ("black" bit) */
    ".p_white1:                  \n"
        "mov.b   r3,@r2          \n"  /* store data byte */
        "add     %4,r2           \n"  /* advance address to next bitplane */
    ".p_start1:                  \n"
        "cmp/hi  r2,%1           \n"  /* address < end address ? */
        "bt      .p_loop1        \n"
        
        "bra     .p_start2       \n"
        "nop                     \n"

        /* second loop: set bits from first to <random-1>'th bitplane
         * Bit setting works the other way round here to equalize average
         * execution times for bright and dark pixels */
    ".p_loop2:                   \n"
        "mov.b   @%2,r3          \n"  /* get data byte */
        "shlr    %0              \n"  /* shift bit mask, sets t bit */
        "or      %3,r3           \n"  /* set bit (-> "black") */
        "bt      .p_black2       \n"  /* t=1? -> "black" bit */
        "and     r1,r3           \n"  /* reset bit ("white" bit) */
    ".p_black2:                  \n"
        "mov.b   r3,@%2          \n"  /* store data byte */
        "add     %4,%2           \n"  /* advance address to next bitplane */
    ".p_start2:                  \n"
        "cmp/hi  %2,%5           \n"  /* address < <random>'th address ? */
        "bt      .p_loop2        \n"
        : /* outputs */
        : /* inputs */
        /* %0 */ "r"(pattern),
        /* %1 */ "r"(graybuf->depth),
        /* %2 */ "r"(address),
        /* %3 */ "r"(mask),
        /* %4 */ "r"(graybuf->plane_size),
        /* %5 */ "r"(random)
        : /* clobbers */
        "r1", "r2", "r3", "macl"
    );
}

/* Set 8 pixels to specific gray values at once, asm optimized
 * This greatly enhances performance of gray_fillrect() and gray_drawgraymap()
 * for larger rectangles and graymaps */
void grayblock(int x, int by, unsigned char* src, int stride)
{
    /* precalculate the bit patterns with random shifts (same RNG as graypixel,
     * see there for an explanation) for all 8 pixels and put them on the 
     * stack (!) */
    asm(
        "mova    .gb_reload,r0   \n"  /* set default loopback address */
        "tst     %1,%1           \n"  /* stride == 0 ? */
        "bf      .gb_needreload  \n"  /* no: keep that address */
        "mova    .gb_reuse,r0    \n"  /* yes: set shortcut (no reload) */
    ".gb_needreload:             \n"
        "mov     r0,r2           \n"  /* loopback address to r2 */
        "mov     #7,r3           \n"  /* loop count in r3: 8 pixels */

        ".align  2               \n"  /** load pattern for pixel **/
    ".gb_reload:                 \n"
        "mov.b   @%0,r0          \n"  /* load src byte */
        "extu.b  r0,r0           \n"  /* extend unsigned */
        "mulu    %2,r0           \n"  /* macl = byte * depth; */
        "add     %1,%0           \n"  /* src += stride; */
        "sts     macl,r4         \n"  /* r4 = macl; */
        "add     r4,r0           \n"  /* byte += r4; */
        "shlr8   r0              \n"  /* byte >>= 8; */
        "shll2   r0              \n"
        "mov.l   @(r0,%3),r4     \n"  /* r4 = bitpattern[byte]; */

        ".align  2               \n"  /** RNG **/
    ".gb_reuse:                  \n"
        "mov.w   @%4,r1          \n"  /* load last value */
        "mov     #75,r0          \n"
        "mulu    r0,r1           \n"  /* multiply by 75 */
        "sts     macl,r1         \n"
        "add     #74,r1          \n"  /* add another 74 */
        "mov.w   r1,@%4          \n"  /* store new value */
        /* Since the lower bits are not very random: */
        "shlr8   r1              \n"  /* get bits 8..15 (need max. 5) */
        "and     %5,r1           \n"  /* mask out unneeded bits */

        "cmp/hs  %2,r1           \n"  /* random >= depth ? */
        "bf      .gb_ntrim       \n"
        "sub     %2,r1           \n"  /* yes: random -= depth; */
    ".gb_ntrim:                  \n"

        "mov.l   .ashlsi3,r0     \n"  /** rotate pattern **/
        "jsr     @r0             \n"  /* shift r4 left by r1 */
        "mov     r1,r5           \n"

        "mov     %2,r5           \n"
        "sub     r1,r5           \n"  /* r5 = depth - r1 */
        "mov     r0,r1           \n"  /* last result stored in r1 */
        "mov.l   .lshrsi3,r0     \n"
        "jsr     @r0             \n"  /* shift r4 right by r5 */
        "nop                     \n"

        "or      r1,r0           \n"  /* rotated_pattern = r0 | r1 */
        "mov.l   r0,@-r15        \n"  /* push pattern */
        
        "cmp/pl  r3              \n"  /* loop count > 0? */
        "bf      .gb_patdone     \n"  /* no: done */

        "jmp     @r2             \n"  /* yes: loop */
        "add     #-1,r3          \n"  /* decrease loop count */

        ".align  2               \n"
    ".ashlsi3:                   \n"  /* C library routine: */
        ".long   ___ashlsi3      \n"  /* shift r4 left by r5, return in r0 */
    ".lshrsi3:                   \n"  /* C library routine: */
        ".long   ___lshrsi3      \n"  /* shift r4 right by r5, return in r0 */
        /* both routines preserve r4, destroy r5 and take ~16 cycles */

    ".gb_patdone:                \n"
        : /* outputs */
        : /* inputs */
        /* %0 */ "r"(src),
        /* %1 */ "r"(stride),
        /* %2 */ "r"(graybuf->depth),
        /* %3 */ "r"(graybuf->bitpattern),
        /* %4 */ "r"(&gray_random_buffer),
        /* %5 */ "r"(graybuf->randmask)
        : /* clobbers */
        "r0", "r1", "r2", "r3", "r4", "r5", "macl"
    );

    /* calculate start address in first bitplane and end address */
    register unsigned char *address = graybuf->data + x
                                    + MULU16(graybuf->width, by);
    register unsigned char *end_addr = address
                           + MULU16(graybuf->depth, graybuf->plane_size);

    /* set the bits for all 8 pixels in all bytes according to the
     * precalculated patterns on the stack */
    asm (
        "mov.l   @r15+,r1        \n"  /* pop all 8 patterns */
        "mov.l   @r15+,r2        \n"
        "mov.l   @r15+,r3        \n"
        "mov.l   @r15+,r4        \n"
        "mov.l   @r15+,r5        \n"
        "mov.l   @r15+,r6        \n"
        "mov.l   @r15+,r7        \n"
        "mov.l   @r15+,r8        \n"

    ".gb_loop:                   \n"  /* loop for all bitplanes */
        "shlr    r1              \n"  /* rotate lsb of pattern 1 to t bit */
        "rotcl   r0              \n"  /* rotate t bit into r0 */
        "shlr    r2              \n"
        "rotcl   r0              \n"
        "shlr    r3              \n"
        "rotcl   r0              \n"
        "shlr    r4              \n"
        "rotcl   r0              \n"
        "shlr    r5              \n"
        "rotcl   r0              \n"
        "shlr    r6              \n"
        "rotcl   r0              \n"
        "shlr    r7              \n"
        "rotcl   r0              \n"
        "shlr    r8              \n"
        "rotcl   r0              \n"
        "mov.b   r0,@%0          \n"  /* store byte to bitplane */
        "add     %2,%0           \n"  /* advance to next bitplane */
        "cmp/hi  %0,%1           \n"  /* last bitplane done? */
        "bt      .gb_loop        \n"  /* no: loop */
        : /* outputs */
        : /* inputs */
        /* %0 */ "r"(address),
        /* %1 */ "r"(end_addr),
        /* %2 */ "r"(graybuf->plane_size)
        : /* clobbers */
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8"
    );
}

/* Invert the bits for 1-8 pixels within the buffer */
void grayinvertmasked(int x, int by, unsigned char mask)
{
    asm(
        "mulu    %4,%5           \n"  /* width * by (offset of row) */
        "mov     #0,r1           \n"  /* current_plane = 0 */
        "sts     macl,r2         \n"  /* get mulu result */
        "add     r2,%1           \n"  /* -> address in 1st bitplane */
        
    ".i_loop:                    \n"
        "mov.b   @%1,r2          \n"  /* get data byte */
        "add     #1,r1           \n"  /* current_plane++; */
        "xor     %2,r2           \n"  /* invert bits */
        "mov.b   r2,@%1          \n"  /* store data byte */
        "add     %3,%1           \n"  /* advance address to next bitplane */
        "cmp/hi  r1,%0           \n"  /* current_plane < depth ? */
        "bt      .i_loop         \n"
        : /* outputs */
        : /* inputs */
        /* %0 */ "r"(graybuf->depth),
        /* %1 */ "r"(graybuf->data + x),
        /* %2 */ "r"(mask),
        /* %3 */ "r"(graybuf->plane_size),
        /* %4 */ "r"(graybuf->width),
        /* %5 */ "r"(by)
        : /* clobbers */
        "r1", "r2", "macl"
    );
}

/*** public core functions ***/

/** prototypes **/

int gray_init_buffer(unsigned char *gbuf, int gbuf_size, int width,
                     int bheight, int depth);
void gray_release_buffer(void);
void gray_position_display(int x, int by);
void gray_show_display(bool enable);

/** implementation **/

/* Prepare the grayscale display buffer
 *
 * arguments:
 *   gbuf      = pointer to the memory area to use (e.g. plugin buffer)
 *   gbuf_size = max usable size of the buffer
 *   width     = width in pixels  (1..112)
 *   bheight   = height in 8-pixel units  (1..8)
 *   depth     = desired number of shades - 1  (1..32)
 *
 * result:
 *   = depth  if there was enough memory
 *   < depth  if there wasn't enough memory. The number of displayable
 *            shades is smaller than desired, but it still works
 *   = 0      if there wasn't even enough memory for 1 bitplane (black & white)
 *
 * You can request any depth from 1 to 32, not just powers of 2. The routine
 * performs "graceful degradation" if the memory is not sufficient for the
 * desired depth. As long as there is at least enough memory for 1 bitplane,
 * it creates as many bitplanes as fit into memory, although 1 bitplane will
 * only deliver black & white display.
 *
 * The total memory needed can be calculated as follows:
 *   total_mem =
 *     sizeof(tGraymap)      (= 48 bytes currently)
 *   + sizeof(long)          (=  4 bytes)
 *   + (width * bheight + sizeof(long)) * depth
 *   + 0..3                  (longword alignment of grayscale display buffer)
 */
int gray_init_buffer(unsigned char *gbuf, int gbuf_size, int width,
                     int bheight, int depth)
{
    int possible_depth, plane_size;
    int i, j;

    if ((unsigned) width > LCD_WIDTH 
        || (unsigned) bheight > (LCD_HEIGHT >> 3) 
        || depth < 1)
        return 0;

    while ((unsigned long)gbuf & 3)  /* the buffer has to be long aligned */
    { 
        gbuf++;
        gbuf_size--;
    }

    plane_size = width * bheight;
    possible_depth = (gbuf_size - sizeof(tGraybuf) - sizeof(unsigned long))
                     / (plane_size + sizeof(unsigned long));

    if (possible_depth < 1)
        return 0;

    depth = MIN(depth, 32);
    depth = MIN(depth, possible_depth);

    graybuf = (tGraybuf *) gbuf; /* global pointer to buffer structure */

    graybuf->x = 0;
    graybuf->by = 0;
    graybuf->width = width;
    graybuf->height = bheight << 3;
    graybuf->bheight = bheight;
    graybuf->plane_size = plane_size;
    graybuf->depth = depth;
    graybuf->cur_plane = 0;
    graybuf->flags = 0;
    graybuf->data = gbuf + sizeof(tGraybuf);
    graybuf->bitpattern = (unsigned long *) (graybuf->data 
                           + depth * plane_size);
    
    i = depth;
    j = 8;
    while (i != 0)
    {
        i >>= 1;
        j--;
    }
    graybuf->randmask = 0xFF >> j;

    /* initial state is all white */
    rb->memset(graybuf->data, 0, depth * plane_size);
    
    /* Precalculate the bit patterns for all possible pixel values */
    for (i = 0; i <= depth; i++)
    {
        unsigned long pattern = 0;
        int value = 0;

        for (j = 0; j < depth; j++)
        {
            pattern <<= 1;
            value += i;

            if (value >= depth)
                value -= depth;   /* "white" bit */
            else
                pattern |= 1;     /* "black" bit */
        }  
        /* now the lower <depth> bits contain the pattern */

        graybuf->bitpattern[i] = pattern;
    }
    
    return depth;
}

/* Release the grayscale display buffer
 *
 * Switches the grayscale overlay off at first if it is still running,
 * then sets the pointer to NULL.
 * DO CALL either this function or at least gray_show_display(false)
 * before you exit, otherwise nasty things may happen.
 */
void gray_release_buffer(void)
{
    gray_show_display(false);
    graybuf = NULL;
}

/* Set position of the top left corner of the grayscale overlay
 *
 * arguments:
 *   x  = left margin in pixels
 *   by = top margin in 8-pixel units
 *
 * You may set this in a way that the overlay spills across the right or
 * bottom display border. In this case it will simply be clipped by the 
 * LCD controller. You can even set negative values, this will clip at the
 * left or top border. I did not test it, but the limits may be +127 / -128
 *
 * If you use this while the grayscale overlay is running, the now-freed area
 * will be restored.
 */
void gray_position_display(int x, int by)
{
    if (graybuf == NULL)
        return;

    graybuf->x = x;
    graybuf->by = by;
    
    if (graybuf->flags & GRAY_RUNNING)
        graybuf->flags |= GRAY_DEFERRED_UPDATE;
}

/* Switch the grayscale overlay on or off
 *
 * arguments:
 *   enable = true:  the grayscale overlay is switched on if initialized
 *          = false: the grayscale overlay is switched off and the regular lcd
 *                   content is restored
 *
 * DO NOT call lcd_update() or any other api function that directly accesses
 * the lcd while the grayscale overlay is running! If you need to do
 * lcd_update() to update something outside the grayscale overlay area, use 
 * gray_deferred_update() instead.
 *
 * Other functions to avoid are:
 *   lcd_blit() (obviously), lcd_update_rect(), lcd_set_contrast(),
 *   lcd_set_invert_display(), lcd_set_flip(), lcd_roll()
 *
 * The grayscale display consumes ~50 % CPU power (for a full screen overlay,
 * less if the overlay is smaller) when switched on. You can switch the overlay
 * on and off as many times as you want.
 */
void gray_show_display(bool enable)
{
    if (graybuf == NULL)
        return;

    if (enable)
    {
        graybuf->flags |= GRAY_RUNNING;
        rb->plugin_register_timer(FREQ / 67, 1, gray_timer_isr);
    }
    else
    {
        rb->plugin_unregister_timer();
        graybuf->flags &= ~GRAY_RUNNING;
        rb->lcd_update(); /* restore whatever there was before */
    }
}

/*** public optional functions ***/

/* Here are the various graphics primitives. Cut out functions you do not
 * need in order to keep plugin code size down.
 */

/** prototypes **/

/* functions affecting the whole display */
void gray_clear_display(void);
void gray_black_display(void);
void gray_deferred_update(void);

/* scrolling functions */
void gray_scroll_left(int count, bool black_border);
void gray_scroll_right(int count, bool black_border);
void gray_scroll_up8(bool black_border);
void gray_scroll_down8(bool black_border);
void gray_scroll_up1(bool black_border);
void gray_scroll_down1(bool black_border);

/* pixel functions */
void gray_drawpixel(int x, int y, int brightness);
void gray_invertpixel(int x, int y);

/* line functions */
void gray_drawline(int x1, int y1, int x2, int y2, int brightness);
void gray_invertline(int x1, int y1, int x2, int y2);

/* rectangle functions */
void gray_drawrect(int x1, int y1, int x2, int y2, int brightness);
void gray_fillrect(int x1, int y1, int x2, int y2, int brightness);
void gray_invertrect(int x1, int y1, int x2, int y2);

/* bitmap functions */
void gray_drawgraymap(unsigned char *src, int x, int y, int nx, int ny,
                      int stride);
void gray_drawbitmap(unsigned char *src, int x, int y, int nx, int ny,
                     int stride, bool draw_bg, int fg_brightness,
                     int bg_brightness);

/** implementation **/

/* Clear the grayscale display (sets all pixels to white)
 */
void gray_clear_display(void)
{
    if (graybuf == NULL)
        return;

    rb->memset(graybuf->data, 0, MULU16(graybuf->depth, graybuf->plane_size));
}

/* Set the grayscale display to all black
 */
void gray_black_display(void)
{
    if (graybuf == NULL)
        return;

    rb->memset(graybuf->data, 0xFF, MULU16(graybuf->depth, graybuf->plane_size));
}

/* Do a lcd_update() to show changes done by rb->lcd_xxx() functions (in areas
 * of the screen not covered by the grayscale overlay). If the grayscale 
 * overlay is running, the update will be done in the next call of the
 * interrupt routine, otherwise it will be performed right away. See also
 * comment for the gray_show_display() function.
 */
void gray_deferred_update(void)
{
    if (graybuf != NULL && (graybuf->flags & GRAY_RUNNING))
        graybuf->flags |= GRAY_DEFERRED_UPDATE;
    else
        rb->lcd_update();
}

/* Scroll the whole grayscale buffer left by <count> pixels
 *
 * black_border determines if the pixels scrolled in at the right are black
 * or white 
 */
void gray_scroll_left(int count, bool black_border)
{
    int by, d;
    unsigned char *ptr;
    unsigned char filler;

    if (graybuf == NULL || (unsigned) count >= (unsigned) graybuf->width)
        return;
        
    if (black_border)
        filler = 0xFF;
    else
        filler = 0;

    /* Scroll row by row to minimize flicker (byte rows = 8 pixels each) */
    for (by = 0; by < graybuf->bheight; by++)
    {
        ptr = graybuf->data + MULU16(graybuf->width, by);
        for (d = 0; d < graybuf->depth; d++)
        {   
            if (count & 1)  /* odd count: scroll byte-wise */
                asm volatile (
                ".sl_loop1:                   \n"
                    "mov.b   @%0+,r1          \n"
                    "mov.b   r1,@(%2,%0)      \n"
                    "cmp/hi  %0,%1            \n"
                    "bt      .sl_loop1        \n"
                    : /* outputs */
                    : /* inputs */
                    /* %0 */ "r"(ptr + count),
                    /* %1 */ "r"(ptr + graybuf->width),
                    /* %2 */ "z"(-count - 1)
                    : /* clobbers */
                    "r1"
                );
            else           /* even count: scroll word-wise */
                asm volatile (
                ".sl_loop2:                   \n"
                    "mov.w   @%0+,r1          \n"
                    "mov.w   r1,@(%2,%0)      \n"
                    "cmp/hi  %0,%1            \n"
                    "bt      .sl_loop2        \n"
                    : /* outputs */
                    : /* inputs */
                    /* %0 */ "r"(ptr + count),
                    /* %1 */ "r"(ptr + graybuf->width),
                    /* %2 */ "z"(-count - 2)
                    : /* clobbers */
                    "r1"
                );

            rb->memset(ptr + graybuf->width - count, filler, count);
            ptr += graybuf->plane_size;
        }
    }
}

/* Scroll the whole grayscale buffer right by <count> pixels
 *
 * black_border determines if the pixels scrolled in at the left are black
 * or white
 */
void gray_scroll_right(int count, bool black_border)
{
    int by, d;
    unsigned char *ptr;
    unsigned char filler;

    if (graybuf == NULL || (unsigned) count >= (unsigned) graybuf->width)
        return;

    if (black_border)
        filler = 0xFF;
    else
        filler = 0;

    /* Scroll row by row to minimize flicker (byte rows = 8 pixels each) */
    for (by = 0; by < graybuf->bheight; by++)
    {
        ptr = graybuf->data + MULU16(graybuf->width, by);
        for (d = 0; d < graybuf->depth; d++)
        {
            if (count & 1)  /* odd count: scroll byte-wise */
                asm volatile (
                ".sr_loop1:                   \n"
                    "mov.b   @(%2,%0),r1      \n"
                    "mov.b   r1,@-%0          \n"
                    "cmp/hs  %1,%0            \n"
                    "bt      .sr_loop1        \n"
                    : /* outputs */
                    : /* inputs */
                    /* %0 */ "r"(ptr + graybuf->width),
                    /* %1 */ "r"(ptr + count),
                    /* %2 */ "z"(-count - 1)
                    : /* clobbers */
                    "r1"
                );
            else            /* even count: scroll word-wise */
                asm volatile (
                ".sr_loop2:                   \n"
                    "mov.w   @(%2,%0),r1      \n"
                    "mov.w   r1,@-%0          \n"
                    "cmp/hs  %1,%0            \n"
                    "bt      .sr_loop2        \n"
                    : /* outputs */
                    : /* inputs */
                    /* %0 */ "r"(ptr + graybuf->width),
                    /* %1 */ "r"(ptr + count),
                    /* %2 */ "z"(-count - 2)
                    : /* clobbers */
                    "r1"
                );

            rb->memset(ptr, filler, count);
            ptr += graybuf->plane_size;
        }
    }
}

/* Scroll the whole grayscale buffer up by 8 pixels
 *
 * black_border determines if the pixels scrolled in at the bottom are black
 * or white
 */
void gray_scroll_up8(bool black_border)
{
    int by, d;
    unsigned char *ptr;
    unsigned char filler;

    if (graybuf == NULL)
        return;
        
    if (black_border)
        filler = 0xFF;
    else
        filler = 0;

    /* Scroll row by row to minimize flicker (byte rows = 8 pixels each) */
    for (by = 1; by < graybuf->bheight; by++)
    {
        ptr = graybuf->data + MULU16(graybuf->width, by);
        for (d = 0; d < graybuf->depth; d++)
        {
            rb->memcpy(ptr - graybuf->width, ptr, graybuf->width);
            ptr += graybuf->plane_size;
        }
    }
    /* fill last row */
    ptr = graybuf->data + graybuf->plane_size - graybuf->width;
    for (d = 0; d < graybuf->depth; d++) 
    {
        rb->memset(ptr, filler, graybuf->width);
        ptr += graybuf->plane_size;
    }
}

/* Scroll the whole grayscale buffer down by 8 pixels
 *
 * black_border determines if the pixels scrolled in at the top are black
 * or white
 */
void gray_scroll_down8(bool black_border)
{
    int by, d;
    unsigned char *ptr;
    unsigned char filler;

    if (graybuf == NULL)
        return;
        
    if (black_border)
        filler = 0xFF;
    else
        filler = 0;

    /* Scroll row by row to minimize flicker (byte rows = 8 pixels each) */
    for (by = graybuf->bheight - 1; by > 0; by--)
    {
        ptr = graybuf->data + MULU16(graybuf->width, by);
        for (d = 0; d < graybuf->depth; d++)
        {
            rb->memcpy(ptr, ptr - graybuf->width, graybuf->width);
            ptr += graybuf->plane_size;
        }
    }
    /* fill first row */
    ptr = graybuf->data;
    for (d = 0; d < graybuf->depth; d++) 
    {
        rb->memset(ptr, filler, graybuf->width);
        ptr += graybuf->plane_size;
    }
}

/* Scroll the whole grayscale buffer up by 1 pixel
 *
 * black_border determines if the pixels scrolled in at the bottom are black
 * or white
 *
 * Scrolling up/down pixel-wise is significantly slower than scrolling
 * left/right or scrolling up/down byte-wise because it involves bit
 * shifting. That's why it is asm optimized.
 */
void gray_scroll_up1(bool black_border)
{
    int filler;

    if (graybuf == NULL)
        return;

    if (black_border)
        filler = 1;
    else
        filler = 0;

    /* scroll column by column to minimize flicker */
    asm(
        "mov     #0,r6           \n"  /* x = 0; */

    ".su_cloop:                  \n"  /* repeat for every column */
        "mov     %1,r7           \n"  /* get start address */
        "mov     #0,r1           \n"  /* current_plane = 0 */
            
    ".su_oloop:                  \n"  /* repeat for every bitplane */
        "mov     r7,r2           \n"  /* get start address */
        "mov     #0,r3           \n"  /* current_row = 0 */
        "mov     %5,r5           \n"  /* get filler bit (bit 0) */

    ".su_iloop:                  \n"  /* repeat for all rows */
        "sub     %2,r2           \n"  /* address -= width; */
        "mov.b   @r2,r4          \n"  /* get new byte */
        "shll8   r5              \n"  /* shift old lsb to bit 8 */
        "extu.b  r4,r4           \n"  /* extend byte unsigned */
        "or      r5,r4           \n"  /* merge old lsb */
        "shlr    r4              \n"  /* shift right */
        "movt    r5              \n"  /* save new lsb */
        "mov.b   r4,@r2          \n"  /* store byte */
        "add     #1,r3           \n"  /* current_row++; */
        "cmp/hi  r3,%3           \n"  /* cuurent_row < bheight ? */
        "bt      .su_iloop       \n"
            
        "add     %4,r7           \n"  /* start_address += plane_size; */
        "add     #1,r1           \n"  /* current_plane++; */
        "cmp/hi  r1,%0           \n"  /* current_plane < depth ? */
        "bt      .su_oloop       \n"

        "add     #1,%1           \n"  /* start_address++; */
        "add     #1,r6           \n"  /* x++; */
        "cmp/hi  r6,%2           \n"  /* x < width ? */
        "bt      .su_cloop       \n"
        : /* outputs */
        : /* inputs */
        /* %0 */ "r"(graybuf->depth),
        /* %1 */ "r"(graybuf->data + graybuf->plane_size),
        /* %2 */ "r"(graybuf->width),
        /* %3 */ "r"(graybuf->bheight),
        /* %4 */ "r"(graybuf->plane_size),
        /* %5 */ "r"(filler)
        : /* clobbers */
        "r1", "r2", "r3", "r4", "r5", "r6", "r7"
    );
}

/* Scroll the whole grayscale buffer down by 1 pixel
 *
 * black_border determines if the pixels scrolled in at the top are black
 * or white
 *
 * Scrolling up/down pixel-wise is significantly slower than scrolling
 * left/right or scrolling up/down byte-wise because it involves bit
 * shifting. That's why it is asm optimized. Scrolling down is a bit
 * faster than scrolling up, though.
 */
void gray_scroll_down1(bool black_border)
{
    int filler;

    if (graybuf == NULL)
        return;

    if (black_border)
        filler = -1; /* sets bit 31 */
    else
        filler = 0;

    /* scroll column by column to minimize flicker */
    asm(
        "mov     #0,r5           \n"  /* x = 0; */

    ".sd_cloop:                  \n"  /* repeat for every column */
        "mov     %1,r6           \n"  /* get start address */
        "mov     #0,r1           \n"  /* current_plane = 0 */
            
    ".sd_oloop:                  \n"  /* repeat for every bitplane */
        "mov     r6,r2           \n"  /* get start address */
        "mov     #0,r3           \n"  /* current_row = 0 */
        "mov     %5,r4           \n"  /* get filler bit (bit 31) */

    ".sd_iloop:                  \n"  /* repeat for all rows */
        "shll    r4              \n"  /* get old msb (again) */
        /* This is possible because the sh1 loads byte data sign-extended,
         * so the upper 25 bits of the register are all identical */
        "mov.b   @r2,r4          \n"  /* get new byte */
        "add     #1,r3           \n"  /* current_row++; */
        "rotcl   r4              \n"  /* rotate left, merges previous msb */
        "mov.b   r4,@r2          \n"  /* store byte */
        "add     %2,r2           \n"  /* address += width; */
        "cmp/hi  r3,%3           \n"  /* cuurent_row < bheight ? */
        "bt      .sd_iloop       \n"
            
        "add     %4,r6           \n"  /* start_address += plane_size; */
        "add     #1,r1           \n"  /* current_plane++; */
        "cmp/hi  r1,%0           \n"  /* current_plane < depth ? */
        "bt      .sd_oloop       \n"

        "add     #1,%1           \n"  /* start_address++; */
        "add     #1,r5           \n"  /* x++ */
        "cmp/hi  r5,%2           \n"  /* x < width ? */
        "bt      .sd_cloop       \n"
        : /* outputs */
        : /* inputs */
        /* %0 */ "r"(graybuf->depth),
        /* %1 */ "r"(graybuf->data),
        /* %2 */ "r"(graybuf->width),
        /* %3 */ "r"(graybuf->bheight),
        /* %4 */ "r"(graybuf->plane_size),
        /* %5 */ "r"(filler)
        : /* clobbers */
        "r1", "r2", "r3", "r4", "r5", "r6"
    );
}

/* Set a pixel to a specific gray value
 *
 * brightness is 0..255 (black to white) regardless of real bit depth
 */
void gray_drawpixel(int x, int y, int brightness)
{
    if (graybuf == NULL 
        || (unsigned) x >= (unsigned) graybuf->width
        || (unsigned) y >= (unsigned) graybuf->height
        || (unsigned) brightness > 255)
        return;

    graypixel(x, y, graybuf->bitpattern[MULU16(brightness,
                    graybuf->depth + 1) >> 8]);
}

/* Invert a pixel
 *
 * The bit pattern for that pixel in the buffer is inverted, so white becomes
 * black, light gray becomes dark gray etc.
 */
void gray_invertpixel(int x, int y)
{
    if (graybuf == NULL 
        || (unsigned) x >= (unsigned) graybuf->width
        || (unsigned) y >= (unsigned) graybuf->height)
        return;

    grayinvertmasked(x, (y >> 3), 1 << (y & 7));
}

/* Draw a line from (x1, y1) to (x2, y2) with a specific gray value
 *
 * brightness is 0..255 (black to white) regardless of real bit depth
 */
void gray_drawline(int x1, int y1, int x2, int y2, int brightness)
{
    int numpixels;
    int i;
    int deltax, deltay;
    int d, dinc1, dinc2;
    int x, xinc1, xinc2;
    int y, yinc1, yinc2;
    unsigned long pattern;

    if (graybuf == NULL 
        || (unsigned) x1 >= (unsigned) graybuf->width
        || (unsigned) y1 >= (unsigned) graybuf->height
        || (unsigned) x2 >= (unsigned) graybuf->width
        || (unsigned) y2 >= (unsigned) graybuf->height
        || (unsigned) brightness > 255)
        return;

    pattern = graybuf->bitpattern[MULU16(brightness, graybuf->depth + 1) >> 8];

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

    for (i=0; i<numpixels; i++)
    {
        graypixel(x, y, pattern);

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

/* Invert a line from (x1, y1) to (x2, y2)
 *
 * The bit patterns for the pixels of the line are inverted, so white becomes
 * black, light gray becomes dark gray etc.
 */
void gray_invertline(int x1, int y1, int x2, int y2)
{
    int numpixels;
    int i;
    int deltax, deltay;
    int d, dinc1, dinc2;
    int x, xinc1, xinc2;
    int y, yinc1, yinc2;

    if (graybuf == NULL 
        || (unsigned) x1 >= (unsigned) graybuf->width
        || (unsigned) y1 >= (unsigned) graybuf->height
        || (unsigned) x2 >= (unsigned) graybuf->width
        || (unsigned) y2 >= (unsigned) graybuf->height)
        return;

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

    for (i=0; i<numpixels; i++)
    {
        grayinvertmasked(x, (y >> 3), 1 << (y & 7));

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

/* Draw a (hollow) rectangle with a specific gray value,
 * corners are (x1, y1) and (x2, y2)
 *
 * brightness is 0..255 (black to white) regardless of real bit depth
 */
void gray_drawrect(int x1, int y1, int x2, int y2, int brightness)
{
    int x, y;
    unsigned long pattern;
    unsigned char srcpixel;

    if (graybuf == NULL 
        || (unsigned) x1 >= (unsigned) graybuf->width
        || (unsigned) y1 >= (unsigned) graybuf->height
        || (unsigned) x2 >= (unsigned) graybuf->width
        || (unsigned) y2 >= (unsigned) graybuf->height
        || (unsigned) brightness > 255)
        return;

    if (y1 > y2)
    {
        y = y1;
        y1 = y2;
        y2 = y;
    }
    if (x1 > x2)
    {
        x = x1;
        x1 = x2; 
        x2 = x;
    }

    pattern = graybuf->bitpattern[MULU16(brightness, graybuf->depth + 1) >> 8];
    srcpixel = brightness;

    for (x = x1 + 1; x < x2; x++)
    {
        graypixel(x, y1, pattern);
        graypixel(x, y2, pattern);
    }
    for (y = y1; y <= y2; )
    {
        if (!(y & 7) && (y2 - y >= 7))
        /* current row byte aligned in fb & at least 8 rows left */
        {
            /* shortcut: draw all 8 rows at once: 2..3 times faster */
            grayblock(x1, y >> 3, &srcpixel, 0);
            grayblock(x2, y >> 3, &srcpixel, 0);
            y += 8;
        }
        else
        {
            graypixel(x1, y, pattern);
            graypixel(x2, y, pattern);
            y++;
        }
    }
}

/* Fill a rectangle with a specific gray value 
 * corners are (x1, y1) and (x2, y2)
 *
 * brightness is 0..255 (black to white) regardless of real bit depth
 */
void gray_fillrect(int x1, int y1, int x2, int y2, int brightness)
{
    int x, y;
    unsigned long pattern;
    unsigned char srcpixel;

    if (graybuf == NULL 
        || (unsigned) x1 >= (unsigned) graybuf->width
        || (unsigned) y1 >= (unsigned) graybuf->height
        || (unsigned) x2 >= (unsigned) graybuf->width
        || (unsigned) y2 >= (unsigned) graybuf->height
        || (unsigned) brightness > 255)
        return;
        
    if (y1 > y2)
    {
        y = y1;
        y1 = y2;
        y2 = y;
    }
    if (x1 > x2)
    {
        x = x1;
        x1 = x2; 
        x2 = x;
    }

    pattern = graybuf->bitpattern[MULU16(brightness, graybuf->depth + 1) >> 8];
    srcpixel = brightness;

    for (y = y1; y <= y2; )
    {
        if (!(y & 7) && (y2 - y >= 7))
        /* current row byte aligned in fb & at least 8 rows left */
        {
            for (x = x1; x <= x2; x++)
            {
                /* shortcut: draw all 8 rows at once: 2..3 times faster */
                grayblock(x, y >> 3, &srcpixel, 0);
            }
            y += 8;
        }
        else
        {
            for (x = x1; x <= x2; x++)
            {
                graypixel(x, y, pattern);
            }
            y++;
        }
    }
}

/* Invert a (solid) rectangle, corners are (x1, y1) and (x2, y2)
 *
 * The bit patterns for all pixels of the rectangle are inverted, so white
 * becomes black, light gray becomes dark gray etc. This is the fastest of
 * all gray_xxxrect() functions! Perfectly suited for cursors.
 */
void gray_invertrect(int x1, int y1, int x2, int y2)
{
    int x, yb, yb1, yb2;
    unsigned char mask;

    if (graybuf == NULL 
        || (unsigned) x1 >= (unsigned) graybuf->width
        || (unsigned) y1 >= (unsigned) graybuf->height
        || (unsigned) x2 >= (unsigned) graybuf->width
        || (unsigned) y2 >= (unsigned) graybuf->height)
        return;

    if (y1 > y2)
    {
        yb = y1;
        y1 = y2;
        y2 = yb;
    }
    if (x1 > x2)
    {
        x = x1;
        x1 = x2; 
        x2 = x;
    }
    
    yb1 = y1 >> 3;
    yb2 = y2 >> 3;

    if (yb1 == yb2)
    {
        mask = 0xFF << (y1 & 7);
        mask &= 0xFF >> (7 - (y2 & 7));

        for (x = x1; x <= x2; x++)
            grayinvertmasked(x, yb1, mask);
    }
    else
    {
        mask = 0xFF << (y1 & 7);

        for (x = x1; x <= x2; x++)
            grayinvertmasked(x, yb1, mask);
            
        for (yb = yb1 + 1; yb < yb2; yb++)
        {
            for (x = x1; x <= x2; x++)
                grayinvertmasked(x, yb, 0xFF);
        }
        
        mask = 0xFF >> (7 - (y2 & 7));

        for (x = x1; x <= x2; x++)
            grayinvertmasked(x, yb2, mask);
    }
}

/* Copy a grayscale bitmap into the display
 * 
 * A grayscale bitmap contains one byte for every pixel that defines the
 * brightness of the pixel (0..255). Bytes are read in row-major order.
 * The <stride> parameter is useful if you want to show only a part of a
 * bitmap. It should always be set to the "row length" of the bitmap, so
 * for displaying the whole bitmap, nx == stride.
 */
void gray_drawgraymap(unsigned char *src, int x, int y, int nx, int ny,
                      int stride)
{
    int xi, yi;
    unsigned char *row;

    if (graybuf == NULL 
        || (unsigned) x >= (unsigned) graybuf->width
        || (unsigned) y >= (unsigned) graybuf->height)
        return;
    
    if ((y + ny) >= graybuf->height) /* clip bottom */
        ny = graybuf->height - y;

    if ((x + nx) >= graybuf->width) /* clip right */
        nx = graybuf->width - x;

    for (yi = y; yi < y + ny; )
    {
    	row = src;

        if (!(yi & 7) && (y + ny - yi > 7))
        /* current row byte aligned in fb & at least 8 rows left */
        {
            for (xi = x; xi < x + nx; xi++)
            {
                /* shortcut: draw all 8 rows at once: 2..3 times faster */
                grayblock(xi, yi >> 3, row++, stride);
            }
            yi += 8;
    	    src += stride << 3;
        }
        else
        {
            for (xi = x; xi < x + nx; xi++)
            {
                graypixel(xi, yi, graybuf->bitpattern[MULU16(*row++,
                                  graybuf->depth + 1) >> 8]);
            }
            yi++;
    	    src += stride;
        }
    }
}

/* Display a bitmap with specific foreground and background gray values
 *
 * This (now) uses the same bitmap format as the core b&w graphics routines,
 * so you can use bmp2rb to generate bitmaps for use with this function as
 * well.
 *
 * A bitmap contains one bit for every pixel that defines if that pixel is
 * foreground (1) or background (0). Bits within a byte are arranged
 * vertically, LSB at top.
 * The bytes are stored in row-major order, with byte 0 being top left,
 * byte 1 2nd from left etc. The first row of bytes defines pixel rows
 * 0..7, the second row defines pixel row 8..15 etc.
 *
 * The <stride> parameter is useful if you want to show only a part of a
 * bitmap. It should always be set to the "row length" of the bitmap.
 *
 * If draw_bg is false, only foreground pixels are drawn, so the background
 * is transparent. In this case bg_brightness is ignored.
 */
void gray_drawbitmap(unsigned char *src, int x, int y, int nx, int ny,
                     int stride, bool draw_bg, int fg_brightness,
                     int bg_brightness)
{
    int xi, dy;
    int bits = 0;  /* Have to initialize to prevent warning */
    unsigned long fg_pattern, bg_pattern;
    unsigned char *col;

    if (graybuf == NULL
        || (unsigned) x >= (unsigned) graybuf->width
        || (unsigned) y >= (unsigned) graybuf->height
        || (unsigned) fg_brightness > 255
        || (unsigned) bg_brightness > 255)
        return;

    if ((y + ny) >= graybuf->height) /* clip bottom */
        ny = graybuf->height - y;

    if ((x + nx) >= graybuf->width) /* clip right */
        nx = graybuf->width - x;

    fg_pattern = graybuf->bitpattern[MULU16(fg_brightness,
                                     graybuf->depth + 1) >> 8];

    bg_pattern = graybuf->bitpattern[MULU16(bg_brightness,
                                     graybuf->depth + 1) >> 8];

    for (xi = x; xi < x + nx; xi++)
    {
        col = src++;
        for (dy = 0; dy < ny; dy++)
        {
            if (!(dy & 7))   /* get next 8 bits */
            {
                bits = (int)(*col);
                col += stride;
            }

            if (bits & 0x01)
                graypixel(xi, y + dy, fg_pattern);
            else
                if (draw_bg)
                    graypixel(xi, y + dy, bg_pattern);

            bits >>= 1;
        }
    }
}

/*********************** end grayscale framework ***************************/

/**************************** main function ********************************/

/* this is only a demo of what the framework can do */
int main(void)
{
    int shades, time;
    int x, y, i;
    int button, scroll_amount;
    bool black_border;

    static unsigned char rockbox[] = {
    /* ...........................................
     * .####...###...###..#...#.####...###..#...#.
     * .#...#.#...#.#...#.#..#..#...#.#...#..#.#..
     * .####..#...#.#.....###...####..#...#...#...
     * .#..#..#...#.#...#.#..#..#...#.#...#..#.#..
     * .#...#..###...###..#...#.####...###..#...#.
     * ...........................................
     * 43 x 7 pixel, 1 bpp
     */
       0x00, 0x3E, 0x0A, 0x0A, 0x1A, 0x24, 0x00, 0x1C, 0x22, 0x22,
       0x22, 0x1C, 0x00, 0x1C, 0x22, 0x22, 0x22, 0x14, 0x00, 0x3E,
       0x08, 0x08, 0x14, 0x22, 0x00, 0x3E, 0x2A, 0x2A, 0x2A, 0x14,
       0x00, 0x1C, 0x22, 0x22, 0x22, 0x1C, 0x00, 0x22, 0x14, 0x08,
       0x14, 0x22, 0x00
    };
    
    static unsigned char showing[] = {
    /* .......................................
     * ..####.#...#..###..#...#.#.#...#..####.
     * .#.....#...#.#...#.#...#.#.##..#.#.....
     * ..###..#####.#...#.#.#.#.#.#.#.#.#..##.
     * .....#.#...#.#...#.#.#.#.#.#..##.#...#.
     * .####..#...#..###...#.#..#.#...#..####.
     * .......................................
     * 39 x 7 pixel, 1 bpp
     */
       0x00, 0x24, 0x2A, 0x2A, 0x2A, 0x12, 0x00, 0x3E, 0x08, 0x08,
       0x08, 0x3E, 0x00, 0x1C, 0x22, 0x22, 0x22, 0x1C, 0x00, 0x1E,
       0x20, 0x18, 0x20, 0x1E, 0x00, 0x3E, 0x00, 0x3E, 0x04, 0x08,
       0x10, 0x3E, 0x00, 0x1C, 0x22, 0x22, 0x2A, 0x3A, 0x00
    };
    
    static unsigned char grayscale_gray[] = {
    /* .......................................................
     * ..####.####...###..#...#..####..###...###..#.....#####.
     * .#.....#...#.#...#.#...#.#.....#...#.#...#.#.....#.....
     * .#..##.####..#####..#.#...###..#.....#####.#.....####..
     * .#...#.#..#..#...#...#.......#.#...#.#...#.#.....#.....
     * ..####.#...#.#...#...#...####...###..#...#.#####.#####.
     * .......................................................
     * 55 x 7 pixel, 8 bpp
     */
       110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,
       110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,
       110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,
       110,110,110,110,110,110,110,
       120,120, 20, 20, 20, 20,120,222,222,222,222,120,120,120, 24, 24,
        24,120,120,226,120,120,120,226,120,120, 28, 28, 28, 28,120,120,
       230,230,230,120,120,120, 32, 32, 32,120,120,234,120,120,120,120,
       120, 36, 36, 36, 36, 36,120,
       130, 20,130,130,130,130,130,222,130,130,130,222,130, 24,130,130,
       130, 24,130,226,130,130,130,226,130, 28,130,130,130,130,130,230,
       130,130,130,230,130, 32,130,130,130, 32,130,234,130,130,130,130,
       130, 36,130,130,130,130,130,
       140, 20,140,140, 20, 20,140,222,222,222,222,140,140, 24, 24, 24,
        24, 24,140,140,226,140,226,140,140,140, 28, 28, 28,140,140,230,
       140,140,140,140,140, 32, 32, 32, 32, 32,140,234,140,140,140,140,
       140, 36, 36, 36, 36,140,140,
       130, 20,130,130,130, 20,130,222,130,130,222,130,130, 24,130,130,
       130, 24,130,130,130,226,130,130,130,130,130,130,130, 28,130,230,
       130,130,130,230,130, 32,130,130,130, 32,130,234,130,130,130,130,
       130, 36,130,130,130,130,130,
       120,120, 20, 20, 20, 20,120,222,120,120,120,222,120, 24,120,120,
       120, 24,120,120,120,226,120,120,120, 28, 28, 28, 28,120,120,120,
       230,230,230,120,120, 32,120,120,120, 32,120,234,234,234,234,234,
       120, 36, 36, 36, 36, 36,120,
       110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,
       110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,
       110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,110,
       110,110,110,110,110,110,110
    };

    if (rb->global_settings->backlight_timeout > 0)
        rb->backlight_set_timeout(1); /* keep the light on */

    rb->lcd_setfont(FONT_SYSFIXED);   /* select default font */

    /* get the remainder of the plugin buffer */
    gbuf = (unsigned char *) rb->plugin_get_buffer(&gbuf_size);

    /* initialize the grayscale buffer:
     * 112 pixels wide, 7 rows (56 pixels) high, (try to) reserve
     * 32 bitplanes for 33 shades of gray. (uses 25268 bytes)*/
    shades = gray_init_buffer(gbuf, gbuf_size, 112, 7, 32) + 1;

    /* place grayscale overlay 1 row down */
    gray_position_display(0, 1);

    rb->snprintf(pbuf, sizeof(pbuf), "Shades: %d", shades);
    rb->lcd_puts(0, 0, pbuf);
    rb->lcd_update();

    gray_show_display(true);  /* switch on grayscale overlay */

    time = *rb->current_tick; /* start time measurement */

    gray_fillrect(0, 0, 111, 55, 150); /* fill everything with gray 150 */

    /* draw a dark gray line star background */
    for (y = 0; y < 56; y += 8)        /* horizontal part */
    {
    	gray_drawline(0, y, 111, 55 - y, 80); /* gray lines */
    }
    for (x = 10; x < 112; x += 10)     /* vertical part */
    {
    	gray_drawline(x, 0, 111 - x, 55, 80); /* gray lines */
    }

    gray_drawrect(0, 0, 111, 55, 0);   /* black border */

    /* draw gray tones */
    for (i = 0; i < 86; i++)           
    {
    	x = 13 + i;
    	gray_fillrect(x, 6, x, 49, 3 * i); /* gray rectangles */
    }

    gray_invertrect(13, 29, 98, 49);   /* invert rectangle (lower half) */
    gray_invertline(13, 27, 98, 27);   /* invert a line */
    
    /* show bitmaps (1 bit and 8 bit) */
    gray_drawbitmap(rockbox, 14, 13, 43, 7, 43, true, 255, 100);   /* opaque */
    gray_drawbitmap(showing, 58, 13, 39, 7, 39, false, 0, 0); /* transparent */
    gray_drawgraymap(grayscale_gray, 28, 35, 55, 7, 55);

    time = *rb->current_tick - time;  /* end time measurement */

    rb->snprintf(pbuf, sizeof(pbuf), "Shades: %d, %d.%02ds", shades,
                 time / 100, time % 100);
    rb->lcd_puts(0, 0, pbuf);
    gray_deferred_update();             /* schedule an lcd_update() */

    /* drawing is now finished, play around with scrolling 
     * until you press OFF or connect USB
     */
    while (true)
    {
        scroll_amount = 1;
        black_border = false;

        button = rb->button_get(true);

        if (button == SYS_USB_CONNECTED)
        {
            gray_release_buffer(); /* switch off overlay and deinitialize */
            /* restore normal backlight setting */
            rb->backlight_set_timeout(rb->global_settings->backlight_timeout);
            return PLUGIN_USB_CONNECTED;
        }

        if (button & BUTTON_ON)
            black_border = true;
            
        if (button & BUTTON_REPEAT)
            scroll_amount = 4;

        switch(button & ~(BUTTON_ON | BUTTON_REPEAT))
        {
            case BUTTON_LEFT:

                gray_scroll_left(scroll_amount, black_border); /* scroll left */
                break;

            case BUTTON_RIGHT:

                gray_scroll_right(scroll_amount, black_border); /* scroll right */
                break;

            case BUTTON_UP:

                gray_scroll_up1(black_border); /* scroll up by 1 pixel */
                break;

            case BUTTON_DOWN:

                gray_scroll_down1(black_border); /* scroll down by 1 pixel */
                break;

            case BUTTON_OFF:

                gray_release_buffer(); /* switch off overlay and deinitialize */
                /* restore normal backlight setting */
                rb->backlight_set_timeout(rb->global_settings->backlight_timeout);
                return PLUGIN_OK;
        }
    }
}

/*************************** Plugin entry point ****************************/

enum plugin_status plugin_start(struct plugin_api* api, void* parameter)
{
    int ret;
    /* this macro should be called as the first thing you do in the plugin.
    it test that the api version and model the plugin was compiled for
    matches the machine it is running on */
    TEST_PLUGIN_API(api);

    rb = api; // copy to global api pointer
    (void)parameter;
    
    ret = main();

    if (ret == PLUGIN_USB_CONNECTED)
        rb->usb_screen();
    return ret;
}

#endif // #ifdef HAVE_LCD_BITMAP
#endif // #ifndef SIMULATOR

