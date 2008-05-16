/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2008 by Catalin Patulea
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef IPC_H
#define IPC_H

/* Inter-Processor Communication */

/* Meant to be included by both DSP and ARM code. */
#ifdef __GNUC__
/* aligned(2) is VERY IMPORTANT. It ensures gcc generates code with "STRH"
   instead of with "STRB". STRB in the DSP memory range is broken because
   the HPI is in 16-bit mode. */
#define PACKED __attribute__((packed)) __attribute__((aligned (2)))
#else
#define PACKED
#endif

#define PCM_SIZE 32768 /* bytes */

struct sdram_buffer {
    unsigned long addr;
    unsigned short bytes;
} PACKED;

#define SDRAM_BUFFERS 4

struct ipc_message {
    unsigned short msg;
    union {
#define MSG_INIT 1
        struct msg_init {
            unsigned short sdem_addrl;
            unsigned short sdem_addrh;
        } init PACKED;
#define MSG_DEBUGF 2
        struct {
            short buffer[80];
        } debugf PACKED;
#define MSG_REFILL 3
        struct {
            unsigned short topbottom; /* byte offset to unlocked half-buffer */

            unsigned short _DMA_TRG;
            unsigned short _SDEM_ADDRH;
            unsigned short _SDEM_ADDRL;
            unsigned short _DSP_ADDRH;
            unsigned short _DSP_ADDRL;
            unsigned short _DMA_SIZE;
        } refill PACKED;
    } payload PACKED;
} PACKED;
#endif
