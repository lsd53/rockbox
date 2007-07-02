/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006 by Michael Sevakis
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdlib.h>
#include "system.h"
#include "kernel.h"
#include "logf.h"
#include "audio.h"
#include "sound.h"

/* peaks */
#ifdef HAVE_RECORDING
static unsigned long *rec_peak_addr;
static int rec_peak_left, rec_peak_right;
#endif

/** DMA **/
#ifdef CPU_PP502x
#define FIFO_FREE_COUNT ((IISFIFO_CFG & 0x3f000000) >> 24)
#elif CONFIG_CPU == PP5002
#define FIFO_FREE_COUNT ((IISFIFO_CFG & 0x7800000) >> 23)
#endif

/****************************************************************************
 ** Playback DMA transfer
 **/
static int pcm_freq = HW_SAMPR_DEFAULT; /* 44.1 is default */

/* NOTE: The order of these two variables is important if you use the iPod
   assembler optimised fiq handler, so don't change it. */
unsigned short* p IBSS_ATTR;
size_t p_size IBSS_ATTR;

/* ASM optimised FIQ handler. GCC fails to make use of the fact that FIQ mode
   has registers r8-r14 banked, and so does not need to be saved. This routine
   uses only these registers, and so will never touch the stack unless it
   actually needs to do so when calling pcm_callback_for_more. C version is
   still included below for reference.
 */
#if 1
void fiq(void) ICODE_ATTR __attribute__((naked));
void fiq(void)
{
    /* r12 contains IISCONFIG address (set in crt0.S to minimise code in actual
     * FIQ handler. r11 contains address of p (also set in crt0.S). Most other
     * addresses we need are generated by using offsets with these two.
     * r12 + 0x40 is IISFIFO_WR, and r12 + 0x0c is IISFIFO_CFG.
     * r8 and r9 contains local copies of p_size and p respectively.
     * r10 is a working register.
     */
    asm volatile (
#if CONFIG_CPU == PP5002
        "ldr r10, =0xcf001040 \n\t" /* Some magic from iPodLinux */
        "ldr r10, [r10]       \n\t"
        "ldr r10, [r12, #0x1c]\n\t"
        "bic r10, r10, #0x200 \n\t" /* clear interrupt */
        "str r10, [r12, #0x1c]\n\t"
#else
        "ldr r10, [r12]       \n\t"
        "bic r10, r10, #0x2   \n\t" /* clear interrupt */
        "str r10, [r12]       \n\t"
#endif
        "ldr r8, [r11, #4]    \n\t" /* r8 = p_size */
        "ldr r9, [r11]        \n\t" /* r9 = p */
    ".loop:                   \n\t"
        "cmp r8, #0           \n\t" /* is p_size 0? */
        "beq .more_data       \n\t" /* if so, ask pcmbuf for more data */
    ".fifo_loop:              \n\t"
#if CONFIG_CPU == PP5002
        "ldr r10, [r12, #0x1c]\n\t" /* read IISFIFO_CFG to check FIFO status */
        "and r10, r10, #0x7800000\n\t"
        "cmp r10, #0x800000    \n\t"
#else
        "ldr r10, [r12, #0x0c]\n\t" /* read IISFIFO_CFG to check FIFO status */
        "and r10, r10, #0x3f0000\n\t"
        "cmp r10, #0x10000    \n\t"
#endif
        "bls .fifo_full       \n\t" /* FIFO full, exit */
        "ldr r10, [r9], #4    \n\t" /* load two samples */
#ifdef HAVE_AS3514
        "str r10, [r12, #0x40]\n\t" /* write them */
#else
        "mov r10, r10, ror #16\n\t" /* put left sample at the top bits */
        "str r10, [r12, #0x40]\n\t" /* write top sample, lower sample ignored */
        "mov r10, r10, lsl #16\n\t" /* shift lower sample up */
        "str r10, [r12, #0x40]\n\t" /* then write it */
#endif
        "subs r8, r8, #4      \n\t" /* check if we have more samples */
        "bne .fifo_loop       \n\t" /* yes, continue */
    ".more_data:              \n\t"
        "stmdb sp!, { r0-r3, r12, lr}\n\t" /* stack scratch regs and lr */
        "mov r0, r11          \n\t" /* r0 = &p */
        "add r1, r11, #4      \n\t" /* r1 = &p_size */
        "str r9, [r0]         \n\t" /* save internal copies of variables back */
        "str r8, [r1]         \n\t"
        "ldr r2, =pcm_callback_for_more\n\t"
        "ldr r2, [r2]         \n\t" /* get callback address */
        "cmp r2, #0           \n\t" /* check for null pointer */
        "movne lr, pc         \n\t" /* call pcm_callback_for_more */
        "bxne r2              \n\t"
        "ldmia sp!, { r0-r3, r12, lr}\n\t"
        "ldr r8, [r11, #4]    \n\t" /* reload p_size and p */
        "ldr r9, [r11]        \n\t"
        "cmp r8, #0           \n\t" /* did we actually get more data? */
        "bne .loop            \n\t" /* yes, continue to try feeding FIFO */
    ".dma_stop:               \n\t" /* no more data, do dma_stop() and exit */
        "ldr r10, =pcm_playing\n\t"
        "strb r8, [r10]       \n\t" /* pcm_playing = false (r8=0, look above) */
        "ldr r10, =pcm_paused \n\t"
        "strb r8, [r10]       \n\t" /* pcm_paused = false (r8=0, look above) */
        "ldr r10, [r12]       \n\t"
#if CONFIG_CPU == PP5002
        "bic r10, r10, #0x4\n\t" /* disable playback FIFO */
        "str r10, [r12]       \n\t"
        "ldr r10, [r12, #0x1c]  \n\t"
        "bic r10, r10, #0x200   \n\t" /* clear interrupt */
        "str r10, [r12, #0x1c]  \n\t"
#else
        "bic r10, r10, #0x20000002\n\t" /* disable playback FIFO and IRQ */
        "str r10, [r12]       \n\t"
#endif
        "mrs r10, cpsr        \n\t"
        "orr r10, r10, #0x40  \n\t" /* disable FIQ */
        "msr cpsr_c, r10      \n\t"
    ".exit:                   \n\t"
        "str r8, [r11, #4]    \n\t"
        "str r9, [r11]        \n\t"
        "subs pc, lr, #4      \n\t" /* FIQ specific return sequence */
    ".fifo_full:              \n\t" /* enable IRQ and exit */
#if CONFIG_CPU == PP5002
        "ldr r10, [r12, #0x1c]\n\t"
        "orr r10, r10, #0x200 \n\t" /* set interrupt */
        "str r10, [r12, #0x1c]\n\t"
#else
        "ldr r10, [r12]       \n\t"
        "orr r10, r10, #0x2   \n\t" /* set interrupt */
        "str r10, [r12]       \n\t"
#endif
        "b .exit              \n\t"
    );
}
#else /* C version for reference */
void fiq(void) ICODE_ATTR __attribute__ ((interrupt ("FIQ")));
void fiq(void)
{
    /* Clear interrupt */
#ifdef CPU_PP502x
    IISCONFIG &= ~(1 << 1);
#elif CONFIG_CPU == PP5002
    inl(0xcf001040);
    IISFIFO_CFG &= ~(1<<9);
#endif

    do {
        while (p_size) {
            if (FIFO_FREE_COUNT < 2) {
                /* Enable interrupt */
#ifdef CPU_PP502x
                IISCONFIG |= (1 << 1);
#elif CONFIG_CPU == PP5002
                IISFIFO_CFG |= (1<<9);
#endif
                return;
            }

#ifdef HAVE_AS3514
            IISFIFO_WR = *(int32_t *)p;
            p += 2;
#else
            IISFIFO_WR = (*(p++))<<16;
            IISFIFO_WR = (*(p++))<<16;
#endif
            p_size-=4;
        }

        /* p is empty, get some more data */
        if (pcm_callback_for_more) {
            pcm_callback_for_more((unsigned char**)&p,&p_size);
        }
    } while (p_size);

    /* No more data, so disable the FIFO/FIQ */
    pcm_play_dma_stop();
}
#endif /* ASM / C selection */

void pcm_play_dma_start(const void *addr, size_t size)
{
    p=(unsigned short*)addr;
    p_size=size;

    pcm_playing = true;

#ifdef CPU_PP502x
    CPU_INT_PRIORITY |= I2S_MASK;   /* FIQ priority for I2S */
    CPU_INT_EN = I2S_MASK;          /* Enable I2S interrupt */
#else
    /* setup I2S interrupt for FIQ */
    outl(inl(0xcf00102c) | DMA_OUT_MASK, 0xcf00102c);
    outl(DMA_OUT_MASK, 0xcf001024);
#endif

    /* Clear the FIQ disable bit in cpsr_c */
    set_fiq_handler(fiq);
    enable_fiq();

    /* Enable playback FIFO */
#ifdef CPU_PP502x
    IISCONFIG |= (1 << 29);
#elif CONFIG_CPU == PP5002
    IISCONFIG |= 0x4;
#endif

    /* Fill the FIFO - we assume there are enough bytes in the pcm buffer to
       fill the 32-byte FIFO. */
    while (p_size > 0) {
        if (FIFO_FREE_COUNT < 2) {
            /* Enable interrupt */
#ifdef CPU_PP502x
            IISCONFIG |= (1 << 1);
#elif CONFIG_CPU == PP5002
            IISFIFO_CFG |= (1<<9);
#endif
            return;
        }

#ifdef HAVE_AS3514
        IISFIFO_WR = *(int32_t *)p;
        p += 2;
#else
        IISFIFO_WR = (*(p++))<<16;
        IISFIFO_WR = (*(p++))<<16;
#endif
        p_size-=4;
    }
}

/* Stops the DMA transfer and interrupt */
void pcm_play_dma_stop(void)
{
    pcm_playing = false;
    pcm_paused = false;

#ifdef CPU_PP502x
    /* Disable playback FIFO and interrupt */
    IISCONFIG &= ~((1 << 29) | (1 << 1));
#elif CONFIG_CPU == PP5002

    /* Disable playback FIFO */
    IISCONFIG &= ~0x4;

    /* Disable the interrupt */
    IISFIFO_CFG &= ~(1<<9);
#endif

    disable_fiq();
}

void pcm_play_pause_pause(void)
{
#ifdef CPU_PP502x
    /* Disable playback FIFO and interrupt */
    IISCONFIG &= ~((1 << 29) | (1 << 1));
#elif CONFIG_CPU == PP5002
    /* Disable the interrupt */
    IISFIFO_CFG &= ~(1<<9);
    /* Disable playback FIFO */
    IISCONFIG &= ~0x4;
#endif
    disable_fiq();
}

void pcm_play_pause_unpause(void)
{
    /* Enable the FIFO and fill it */

    set_fiq_handler(fiq);
    enable_fiq();

    /* Enable playback FIFO */
#ifdef CPU_PP502x
    IISCONFIG |= (1 << 29);
#elif CONFIG_CPU == PP5002
    IISCONFIG |= 0x4;
#endif

    /* Fill the FIFO - we assume there are enough bytes in the
       pcm buffer to fill the 32-byte FIFO. */
    while (p_size > 0) {
        if (FIFO_FREE_COUNT < 2) {
            /* Enable interrupt */
#ifdef CPU_PP502x
            IISCONFIG |= (1 << 1);
#elif CONFIG_CPU == PP5002
            IISFIFO_CFG |= (1<<9);
#endif
            return;
        }

#ifdef HAVE_AS3514
        IISFIFO_WR = *(int32_t *)p;
        p += 2;
#else
        IISFIFO_WR = (*(p++))<<16;
        IISFIFO_WR = (*(p++))<<16;
#endif
        p_size-=4;
    }
}

void pcm_set_frequency(unsigned int frequency)
{
    (void)frequency;
    pcm_freq = HW_SAMPR_DEFAULT;
}

size_t pcm_get_bytes_waiting(void)
{
    return p_size;
}

void pcm_init(void)
{
    pcm_playing = false;
    pcm_paused = false;
    pcm_callback_for_more = NULL;

    /* Initialize default register values. */
    audiohw_init();

    /* Power on */
    audiohw_enable_output(true);

    /* Unmute the master channel (DAC should be at zero point now). */
    audiohw_mute(false);

    /* Call pcm_play_dma_stop to initialize everything. */
    pcm_play_dma_stop();
}

void pcm_postinit(void)
{
    audiohw_postinit();
}

/****************************************************************************
 ** Recording DMA transfer
 **/
#ifdef HAVE_RECORDING

#ifdef HAVE_AS3514
void fiq_record(void) ICODE_ATTR __attribute__((naked));
void fiq_record(void)
{
    register pcm_more_callback_type2 more_ready;
    register int32_t value1, value2;

    asm volatile ("stmfd sp!, {r0-r7, ip, lr} \n");   /* Store context */

    IISCONFIG &= ~(1 << 0);

    if (audio_channels == 2) {
        /* RX is stereo */
        while (p_size > 0) {
            if (FIFO_FREE_COUNT < 8) {
                /* enable interrupt */
                IISCONFIG |= (1 << 0);
                goto fiq_record_exit;
            }

            /* Discard every other sample since ADC clock is 1/2 LRCK */
            value1 = IISFIFO_RD;
            value2 = IISFIFO_RD;

            *(int32_t *)p = value1;
            p += 2;
            p_size -= 4;

            /* TODO: Figure out how to do IIS loopback */
            if (audio_output_source != AUDIO_SRC_PLAYBACK) {
                IISFIFO_WR = value1;
                IISFIFO_WR = value1;
            }
        }
    }
    else {
        /* RX is left channel mono */
        while (p_size > 0) {
            if (FIFO_FREE_COUNT < 8) {
                /* enable interrupt */
                IISCONFIG |= (1 << 0);
                goto fiq_record_exit;
            }

            /* Discard every other sample since ADC clock is 1/2 LRCK */
            value1 = IISFIFO_RD;
            value2 = IISFIFO_RD;
            *p++ = value1;
            *p++ = value1;
            p_size -= 4;

            if (audio_output_source != AUDIO_SRC_PLAYBACK) {
                value1 = *((int32_t *)p - 1);
                IISFIFO_WR = value1;
                IISFIFO_WR = value1;
            }
        }
    }

    more_ready = pcm_callback_more_ready;

    if (more_ready == NULL || more_ready(0) < 0) {
        /* Finished recording */
        pcm_rec_dma_stop();
    }

fiq_record_exit:
    asm volatile("ldmfd sp!, {r0-r7, ip, lr} \n"   /* Restore context */
                 "subs  pc, lr, #4           \n"); /* Return from FIQ */
}

#else
static short peak_l, peak_r IBSS_ATTR;

void fiq_record(void) ICODE_ATTR __attribute__ ((interrupt ("FIQ")));
void fiq_record(void)
{
    short value;
    pcm_more_callback_type2 more_ready;
    int status = 0;

    /* Clear interrupt */
#ifdef CPU_PP502x
    IISCONFIG &= ~(1 << 0);
#elif CONFIG_CPU == PP5002
    /* TODO */
#endif

    while (p_size > 0) {
        if (FIFO_FREE_COUNT < 2) {
            /* enable interrupt */
#ifdef CPU_PP502x
            IISCONFIG |= (1 << 0);
#elif CONFIG_CPU == PP5002
            /* TODO */
#endif
            return;
        }

        value = (unsigned short)(IISFIFO_RD >> 16);
        if (value > peak_l) peak_l = value;
        else if (-value > peak_l) peak_l = -value;
        *(p++) = value;

        value = (unsigned short)(IISFIFO_RD >> 16);
        if (value > peak_r) peak_r = value;
        else if (-value > peak_r) peak_r = -value;
        *(p++) = value;

        p_size -= 4;

        /* If we have filled the current chunk, start a new one */
        if (p_size == 0) {
            rec_peak_left = peak_l;
            rec_peak_right = peak_r;
            peak_l = peak_r = 0;
        }
    }

    more_ready = pcm_callback_more_ready;

    if (more_ready != NULL && more_ready(status) >= 0)
        return;

    /* Finished recording */
    pcm_rec_dma_stop();
}

#endif /* HAVE_AS3514 */

/* Continue transferring data in */
void pcm_record_more(void *start, size_t size)
{
    rec_peak_addr = start; /* Start peaking at dest */
    p             = start; /* Start of RX buffer    */
    p_size        = size;  /* Bytes to transfer     */
#ifdef CPU_PP502x
    IISCONFIG |= (1 << 0);
#elif CONFIG_CPU == PP5002
    /* TODO */
#endif
}

void pcm_rec_dma_stop(void)
{
    logf("pcm_rec_dma_stop");

    disable_fiq();

    /* clear interrupt, disable fifo */
    IISCONFIG &= ~((1 << 28) | (1 << 0));

    /* clear rx fifo */
    IISFIFO_CFG |= (1 << 12);

    pcm_recording = false;
}

void pcm_rec_dma_start(void *addr, size_t size)
{
    logf("pcm_rec_dma_start");

    pcm_recording = true;

#ifndef HAVE_AS3514
    peak_l = peak_r = 0;
#endif

    p_size = size;
    p = addr;

    /* setup FIQ */
    CPU_INT_PRIORITY |= I2S_MASK;
    CPU_INT_EN = I2S_MASK;

    /* interrupt on full fifo, enable record fifo */
    IISCONFIG |= (1 << 28) | (1 << 0);

    set_fiq_handler(fiq_record);
    enable_fiq();
}

void pcm_close_recording(void)
{
    logf("pcm_close_recording");
    pcm_rec_dma_stop();
} /* pcm_close_recording */

void pcm_init_recording(void)
{
    logf("pcm_init_recording");

    pcm_recording           = false;
    pcm_callback_more_ready = NULL;

#ifdef CPU_PP502x
#if defined(IPOD_COLOR) || defined (IPOD_4G)
    /* The usual magic from IPL - I'm guessing this configures the headphone
       socket to be input or output - in this case, input. */
    GPIOI_OUTPUT_VAL &= ~0x40;
    GPIOA_OUTPUT_VAL &= ~0x4;
#endif
    /* Setup the recording FIQ handler */
    set_fiq_handler(fiq_record);
#endif

    pcm_rec_dma_stop();
} /* pcm_init */

void pcm_calculate_rec_peaks(int *left, int *right)
{
#ifdef HAVE_AS3514
    if (pcm_recording)
    {
        unsigned long *start = rec_peak_addr;
        unsigned long *end = (unsigned long *)p;

        if (start < end)
        {
            unsigned long *addr = start;
            long peak_l   = 0, peak_r   = 0;
            long peaksq_l = 0, peaksq_r = 0;

            do
            {
                long value = *addr;
                long ch, chsq;

                ch = (int16_t)value;
                chsq = ch*ch;
                if (chsq > peaksq_l)
                    peak_l = ch, peaksq_l = chsq;

                ch = value >> 16;
                chsq = ch*ch;
                if (chsq > peaksq_r)
                    peak_r = ch, peaksq_r = chsq;

                addr += 4;
            }
            while (addr < end);

            if (start == rec_peak_addr)
                rec_peak_addr = end;

            rec_peak_left  = abs(peak_l);
            rec_peak_right = abs(peak_r);
        }
    }
    else
    {
        rec_peak_left = rec_peak_right = 0;
    }
#endif /* HAVE_AS3514 */

    if (left)
        *left = rec_peak_left;

    if (right)
        *right = rec_peak_right;
}
#endif /* HAVE_RECORDING */

/*
 * This function goes directly into the DMA buffer to calculate the left and
 * right peak values. To avoid missing peaks it tries to look forward two full
 * peek periods (2/HZ sec, 100% overlap), although it's always possible that
 * the entire period will not be visible. To reduce CPU load it only looks at
 * every third sample, and this can be reduced even further if needed (even
 * every tenth sample would still be pretty accurate).
 */

/* Check for a peak every PEAK_STRIDE samples */
#define PEAK_STRIDE   3
/* Up to 1/50th of a second of audio for peak calculation */
/* This should use NATIVE_FREQUENCY, or eventually an adjustable freq. value */
#define PEAK_SAMPLES  (44100/50)
void pcm_calculate_peaks(int *left, int *right)
{
    short *addr;
    short *end;
    {
        size_t samples = p_size / 4;
        addr = p;

        if (samples > PEAK_SAMPLES)
            samples = PEAK_SAMPLES - (PEAK_STRIDE - 1);
        else
            samples -= MIN(PEAK_STRIDE - 1, samples);

        end = &addr[samples * 2];
    }

    if (left && right) {
        int left_peak = 0, right_peak = 0;

        while (addr < end) {
            int value;
            if ((value = addr [0]) > left_peak)
                left_peak = value;
            else if (-value > left_peak)
                left_peak = -value;

            if ((value = addr [PEAK_STRIDE | 1]) > right_peak)
                right_peak = value;
            else if (-value > right_peak)
                right_peak = -value;

            addr = &addr[PEAK_STRIDE * 2];
        }

        *left = left_peak;
        *right = right_peak;
    }
    else if (left || right) {
        int peak_value = 0, value;

        if (right)
            addr += (PEAK_STRIDE | 1);

        while (addr < end) {
            if ((value = addr [0]) > peak_value)
                peak_value = value;
            else if (-value > peak_value)
                peak_value = -value;

            addr += PEAK_STRIDE * 2;
        }

        if (left)
            *left = peak_value;
        else
            *right = peak_value;
    }
}
