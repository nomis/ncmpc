/* 
 * $Id$
 *
 * (c) 2004 by Kalle Wallin <kaw@linux.se>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <ncurses.h>

#include "config.h"
#include "ncmpc.h"
#include "options.h"
#include "support.h"
#include "mpdclient.h"
#include "strfsong.h"
#include "command.h"
#include "screen.h"
#include "screen_utils.h"

#define MAX_SONG_LENGTH 512

static list_window_t *lw = NULL;

static void 
playlist_changed_callback(mpdclient_t *c, int event, gpointer data)
{
  D("screen_play.c> playlist_callback() [%d]\n", event);
  switch(event)
    {
    case PLAYLIST_EVENT_DELETE:
      break;
    case PLAYLIST_EVENT_MOVE:
      lw->selected = *((int *) data);
      break;
    default:
      break;
    }
  /* make shure the playlist is repainted */
  lw->clear = 1;
  lw->repaint = 1;
  list_window_check_selected(lw, c->playlist.length);
}

static char *
list_callback(int index, int *highlight, void *data)
{
  static char songname[MAX_SONG_LENGTH];
  mpdclient_t *c = (mpdclient_t *) data;
  mpd_Song *song;

  *highlight = 0;
  if( (song=playlist_get_song(c, index)) == NULL )
    {
      return NULL;
    }

  if( c->song && song->id==c->song->id && !IS_STOPPED(c->status->state) )
    {
      *highlight = 1;
    }
  strfsong(songname, MAX_SONG_LENGTH, LIST_FORMAT, song);
  return songname;
}

static int
center_playing_item(screen_t *screen, mpdclient_t *c)
{
  int length = c->playlist.length;
  int offset = lw->selected-lw->start;
  int index;
  
  if( !lw || !c->song || length<lw->rows || IS_STOPPED(c->status->state) )
    return 0;

  /* try to center the song that are playing */
  index = playlist_get_index(c, c->song);
  D("Autocenter song id:%d pos:%d index:%d\n", c->song->id,c->song->pos,index);
  lw->start = index-(lw->rows/2);
  if( lw->start+lw->rows > length )
    lw->start = length-lw->rows;
  if( lw->start<0 )
    lw->start=0;

  /* make sure the cursor is in the window */
  lw->selected = lw->start+offset;
  list_window_check_selected(lw, length);

  lw->clear = 1;
  lw->repaint = 1;

  return 0;
}

static int
handle_save_playlist(screen_t *screen, mpdclient_t *c, char *name)
{
  gchar *filename;
  gint error;

  if( name==NULL )
    {
      /* query the user for a filename */
      filename=screen_getstr(screen->status_window.w, _("Save playlist as: "));
      filename=trim(filename);
    }
  else
    {
      filename=g_strdup(name);
    }
  if( filename==NULL || filename[0]=='\0' )
    return -1;
  /* send save command to mpd */
  D("Saving playlist as \'%s \'...\n", filename);
  if( (error=mpdclient_cmd_save_playlist(c, filename)) )
    {
      gint code = GET_ACK_ERROR_CODE(error);

      if( code == MPD_ACK_ERROR_EXIST )
	{
	  char buf[256];
	  int key;

	  snprintf(buf, 256, _("Replace %s [%s/%s] ? "), filename, YES, NO);
	  key = tolower(screen_getch(screen->status_window.w, buf));
	  if( key == YES[0] )
	    {
	      char *filename_utf8 = locale_to_utf8(filename);
	      
	      if( mpdclient_cmd_delete_playlist(c, filename_utf8) )
		{
		  g_free(filename);
		  g_free(filename_utf8);
		  return -1;
		}
	      g_free(filename_utf8);
	      error = handle_save_playlist(screen, c, filename);
	      g_free(filename);
	      return error;
	    }	  
	}
      g_free(filename);
      return -1;
    }
  /* success */
  screen_status_printf(_("Saved %s"), filename);
  g_free(filename);
  return 0;
}

static void
play_init(WINDOW *w, int cols, int rows)
{
  lw = list_window_init(w, cols, rows);
}

static void
play_open(screen_t *screen, mpdclient_t *c)
{
  static gboolean install_cb = TRUE;

  if( install_cb )
    {
      mpdclient_install_playlist_callback(c, playlist_changed_callback);
      install_cb = FALSE;
    }
}

static void
play_resize(int cols, int rows)
{
  lw->cols = cols;
  lw->rows = rows;
}


static void
play_exit(void)
{
  list_window_free(lw);
}

static char *
play_title(char *str, size_t size)
{
  if( strcmp(options.host, "localhost") == 0 )
    return _("Playlist");
  
  snprintf(str, size, _("Playlist on %s"), options.host);

  return str;
}

static void
play_paint(screen_t *screen, mpdclient_t *c)
{ 
  lw->clear = 1;

  list_window_paint(lw, list_callback, (void *) c);
  wnoutrefresh(lw->w);
}

static void
play_update(screen_t *screen, mpdclient_t *c)
{
  if( options.auto_center )
    {
      static int prev_song_id = 0;
      
      if( c->song && prev_song_id != c->song->id )	
	{
	  center_playing_item(screen, c);
	  prev_song_id = c->song->id;
	}
    }

  if( c->playlist.updated )
    {
      if( lw->selected >= c->playlist.length )
	lw->selected = c->playlist.length-1;
      if( lw->start    >= c->playlist.length )
	list_window_reset(lw);

      play_paint(screen, c);
      c->playlist.updated = FALSE;
    }
  else if( lw->repaint || 1)
    {
      list_window_paint(lw, list_callback, (void *) c);
      wnoutrefresh(lw->w);
      lw->repaint = 0;
    }
}

static int
play_cmd(screen_t *screen, mpdclient_t *c, command_t cmd)
{
  switch(cmd)
    {
    case CMD_PLAY:
      mpdclient_cmd_play(c, lw->selected);
      return 1;
    case CMD_DELETE:
      mpdclient_cmd_delete(c, lw->selected);
      return 1;
    case CMD_SAVE_PLAYLIST:
      handle_save_playlist(screen, c, NULL);
      return 1;
    case CMD_SCREEN_UPDATE:
      center_playing_item(screen, c);
      return 1;
    case CMD_LIST_MOVE_UP:
      mpdclient_cmd_move(c, lw->selected, lw->selected-1);
      return 1;
    case CMD_LIST_MOVE_DOWN:
      mpdclient_cmd_move(c, lw->selected, lw->selected+1);
      return 1;
    case CMD_LIST_FIND:
    case CMD_LIST_RFIND:
    case CMD_LIST_FIND_NEXT:
    case CMD_LIST_RFIND_NEXT:
      return screen_find(screen, c, 
			 lw, c->playlist.length,
			 cmd, list_callback);
    default:
      break;
    }
  return list_window_cmd(lw, c->playlist.length, cmd) ;
}



static list_window_t *
play_lw(void)
{
  return lw;
}


screen_functions_t *
get_screen_playlist(void)
{
  static screen_functions_t functions;

  memset(&functions, 0, sizeof(screen_functions_t));
  functions.init   = play_init;
  functions.exit   = play_exit;
  functions.open   = play_open;
  functions.close  = NULL;
  functions.resize = play_resize;
  functions.paint  = play_paint;
  functions.update = play_update;
  functions.cmd    = play_cmd;
  functions.get_lw = play_lw;
  functions.get_title = play_title;

  return &functions;
}
