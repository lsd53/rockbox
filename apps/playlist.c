/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by wavey@wavey.org
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "playlist.h"
#include <file.h>
#include "sprintf.h"
#include "debug.h"
#include "mpeg.h"
#include "lcd.h"
#include "kernel.h"
#include "settings.h"
#include "status.h"
#include "applimits.h"
#ifdef HAVE_LCD_BITMAP
#include "icons.h"
#include "widgets.h"
#endif

struct playlist_info playlist;

#define PLAYLIST_BUFFER_SIZE (AVERAGE_FILENAME_LENGTH*MAX_FILES_IN_DIR)

unsigned char playlist_buffer[PLAYLIST_BUFFER_SIZE];
static int playlist_end_pos = 0;

char now_playing[MAX_PATH+1];

void playlist_clear(void)
{
    playlist_end_pos = 0;
    playlist_buffer[0] = 0;
}

int playlist_add(char *filename)
{
    int len = strlen(filename);

    if(len+2 > PLAYLIST_BUFFER_SIZE - playlist_end_pos)
        return -1;

    strcpy(&playlist_buffer[playlist_end_pos], filename);
    playlist_end_pos += len;
    playlist_buffer[playlist_end_pos++] = '\n';
    playlist_buffer[playlist_end_pos] = '\0';
    return 0;
}

char* playlist_next(int steps, int* index)
{
    int seek;
    int max;
    int fd;
    int i;
    char *buf;
    char dir_buf[MAX_PATH+1];
    char *dir_end;

    if(abs(steps) > playlist.amount)
        /* prevent madness when all files are empty/bad */
        return NULL;

    playlist.index = (playlist.index+steps) % playlist.amount;
    while ( playlist.index < 0 ) {
        if ( global_settings.loop_playlist )
            playlist.index += playlist.amount;
        else
            playlist.index = 0;
    }
    seek = playlist.indices[playlist.index];

    if(playlist.in_ram)
    {
        buf = playlist_buffer + seek;
        max = playlist_end_pos - seek;
    }
    else
    {
        fd = open(playlist.filename, O_RDONLY);
        if(-1 != fd)
        {
            buf = playlist_buffer;
            lseek(fd, seek, SEEK_SET);
            max = read(fd, buf, MAX_PATH);
            close(fd);
        }
        else
            return NULL;
    }

    if (index)
        *index = playlist.index;

    /* Zero-terminate the file name */
    seek=0;
    while((buf[seek] != '\n') &&
          (buf[seek] != '\r') &&
          (seek < max))
        seek++;

    /* Now work back killing white space */
    while((buf[seek-1] == ' ') || 
          (buf[seek-1] == '\t'))
        seek--;

    buf[seek]=0;
      
    /* replace backslashes with forward slashes */
    for ( i=0; i<seek; i++ )
        if ( buf[i] == '\\' )
            buf[i] = '/';

    if('/' == buf[0]) {
        strcpy(now_playing, &buf[0]);
        return now_playing;
    }
    else {
        strncpy(dir_buf, playlist.filename, playlist.dirlen-1);
        dir_buf[playlist.dirlen-1] = 0;

        /* handle dos style drive letter */
        if ( ':' == buf[1] ) {
            strcpy(now_playing, &buf[2]);
            return now_playing;
        }
        else if ( '.' == buf[0] && '.' == buf[1] && '/' == buf[2] ) {
            /* handle relative paths */
            seek=3;
            while(buf[seek] == '.' &&
                  buf[seek+1] == '.' &&
                  buf[seek+2] == '/')
                seek += 3;
            for (i=0; i<seek/3; i++) {
                dir_end = strrchr(dir_buf, '/');
                if (dir_end)
                    *dir_end = '\0';
                else
                    break;
            }
            snprintf(now_playing, MAX_PATH+1, "%s/%s", dir_buf, &buf[seek]);
            return now_playing;
        }
        else if ( '.' == buf[0] && '/' == buf[1] ) {
            snprintf(now_playing, MAX_PATH+1, "%s/%s", dir_buf, &buf[2]);
            return now_playing;
        }
        else {
            snprintf(now_playing, MAX_PATH+1, "%s/%s", dir_buf, buf);
            return now_playing;
        }
    }
}

/*
 * This function is called to start playback of a given playlist.  This
 * playlist may be stored in RAM (when using full-dir playback).
 *
 * Return: the new index (possibly different due to shuffle)
 */
int play_list(char *dir,         /* "current directory" */
              char *file,        /* playlist */
              int start_index,   /* index in the playlist */
              bool shuffled_index, /* if TRUE the specified index is for the
                                       playlist AFTER the shuffle */
              int start_offset,  /* offset in the file */
              int random_seed )  /* used for shuffling */
{
    char *sep="";
    int dirlen;
    empty_playlist();

    playlist.index = start_index;

#ifdef HAVE_LCD_BITMAP
    if(global_settings.statusbar)
        lcd_setmargins(0, STATUSBAR_HEIGHT);
    else
        lcd_setmargins(0, 0);
#endif

    /* If file is NULL, the list is in RAM */
    if(file) {
        lcd_clear_display();
        lcd_puts(0,0,"Loading...");
        status_draw();
        lcd_update();
        playlist.in_ram = false;
    } else {
        /* Assign a dummy filename */
        file = "";
        playlist.in_ram = true;
    }

    dirlen = strlen(dir);

    /* If the dir does not end in trailing slash, we use a separator.
       Otherwise we don't. */
    if('/' != dir[dirlen-1]) {
        sep="/";
        dirlen++;
    }
    
    playlist.dirlen = dirlen;

    snprintf(playlist.filename, sizeof(playlist.filename),
             "%s%s%s", 
             dir, sep, file);

    /* add track indices to playlist data structure */
    add_indices_to_playlist();
    
    if(global_settings.playlist_shuffle) {
        if(!playlist.in_ram) {
            lcd_puts(0,0,"Shuffling...");
            status_draw();
            lcd_update();
            randomise_playlist( random_seed );
        }
        else {
            int i;

            /* store the seek position before the shuffle */
            int seek_pos = playlist.indices[start_index];

            /* now shuffle around the indices */
            randomise_playlist( random_seed );

            if(!shuffled_index) {
                /* The given index was for the unshuffled list, so we need
                   to figure out the index AFTER the shuffle has been made.
                   We scan for the seek position we remmber from before. */

                for(i=0; i< playlist.amount; i++) {
                    if(seek_pos == playlist.indices[i]) {
                        /* here's the start song! yiepee! */
                        playlist.index = i;
                        break; /* now stop searching */
                    }
                }
                /* if we for any reason wouldn't find the index again, it
                   won't set the index again and we should start at index 0
                   instead */
            }
        }
    }

    if(!playlist.in_ram) {
        lcd_puts(0,0,"Playing...  ");
        status_draw();
        lcd_update();
    }
    /* also make the first song get playing */
    mpeg_play(start_offset);

    return playlist.index;
}

/*
 * remove any filename and indices associated with the playlist
 */
void empty_playlist(void)
{
    playlist.filename[0] = '\0';
    playlist.index = 0;
    playlist.amount = 0;
}

/*
 * calculate track offsets within a playlist file
 */
void add_indices_to_playlist(void)
{
    int nread;
    int fd = -1;
    int i = 0;
    int store_index = 0;
    int count = 0;
    int next_tick = current_tick + HZ;

    unsigned char *p = playlist_buffer;
    char line[16];

    if(!playlist.in_ram) {
        fd = open(playlist.filename, O_RDONLY);
        if(-1 == fd)
            return; /* failure */
    }

    store_index = 1;

    while(1)
    {
        if(playlist.in_ram) {
            nread = playlist_end_pos;
        } else {
            nread = read(fd, playlist_buffer, PLAYLIST_BUFFER_SIZE);
            /* Terminate on EOF */
            if(nread <= 0)
                break;
        }
        
        p = playlist_buffer;

        for(count=0; count < nread; count++,p++) {

            /* Are we on a new line? */
            if((*p == '\n') || (*p == '\r'))
            {
                store_index = 1;
            } 
            else if((*p == '#') && store_index)
            {
                /* If the first character on a new line is a hash
                   sign, we treat it as a comment. So called winamp
                   style playlist. */
                store_index = 0;
            }
            else if(store_index) 
            {
                
                /* Store a new entry */
                playlist.indices[ playlist.amount ] = i+count;
                playlist.amount++;
                if ( playlist.amount >= MAX_PLAYLIST_SIZE ) {
                    if(!playlist.in_ram)
                        close(fd);

                    lcd_clear_display();
                    lcd_puts(0,0,"Playlist");
                    lcd_puts(0,1,"buffer full");
                    lcd_update();
                    sleep(HZ*2);
                    lcd_clear_display();

                    return;
                }

                store_index = 0;
                /* Update the screen if it takes very long */
                if(!playlist.in_ram) {
                    if ( current_tick >= next_tick ) {
                        next_tick = current_tick + HZ;
                        snprintf(line, sizeof line, "%d files",
                                 playlist.amount);
                        lcd_puts(0,1,line);
                        status_draw();
                        lcd_update();
                    }
                }
            }
        }

        i+= count;

        if(playlist.in_ram)
            break;
    }
    if(!playlist.in_ram) {
        snprintf(line, sizeof line, "%d files", playlist.amount);
        lcd_puts(0,1,line);
        status_draw();
        lcd_update();
        close(fd);
    }
}

/*
 * randomly rearrange the array of indices for the playlist
 */
void randomise_playlist( unsigned int seed )
{
    int count;
    int candidate;
    int store;
    
    /* seed with the given seed */
    srand( seed );

    /* randomise entire indices list */
    for(count = playlist.amount - 1; count >= 0; count--)
    {
        /* the rand is from 0 to RAND_MAX, so adjust to our value range */
        candidate = rand() % (count + 1);

        /* now swap the values at the 'count' and 'candidate' positions */
        store = playlist.indices[candidate];
        playlist.indices[candidate] = playlist.indices[count];
        playlist.indices[count] = store;
    }
}

static int compare(const void* p1, const void* p2)
{
    int* e1 = (int*) p1;
    int* e2 = (int*) p2;

    return *e1 - *e2;
}

/*
 * Sort the array of indices for the playlist. If start_current is true then
 * set the index to the new index of the current song.
 */
void sort_playlist(bool start_current)
{
    int i;
    int current = playlist.indices[playlist.index];

    if (playlist.amount > 0)
    {
        qsort(&playlist.indices, playlist.amount, sizeof(playlist.indices[0]), compare);
    }

    if (start_current)
    {
        /* Set the index to the current song */
        for (i=0; i<playlist.amount; i++)
        {
            if (playlist.indices[i] == current)
            {
                playlist.index = i;
                break;
            }
        }
    }
}

/* -----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../firmware/rockbox-mode.el")
 * end:
 */
