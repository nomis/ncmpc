/* ncmpc (Ncurses MPD Client)
 * (c) 2004-2010 The Music Player Daemon Project
 * Project homepage: http://musicpd.org
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
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "screen_artist.h"
#include "screen_interface.h"
#include "screen_status.h"
#include "screen_find.h"
#include "screen_browser.h"
#include "screen.h"
#include "i18n.h"
#include "charset.h"
#include "mpdclient.h"
#include "screen_browser.h"
#include "filelist.h"
#include "options.h"

#include <assert.h>
#include <string.h>
#include <glib.h>

#define BUFSIZE 1024

typedef enum { LIST_ARTISTS, LIST_ALBUMS, LIST_SONGS } artist_mode_t;

static artist_mode_t mode = LIST_ARTISTS;
static GPtrArray *artist_list, *album_list;
static char *artist = NULL;
static char *album  = NULL;
static char ALL_TRACKS[] = "";

static struct screen_browser browser;

static gint
compare_utf8(gconstpointer s1, gconstpointer s2)
{
	const char *const*t1 = s1, *const*t2 = s2;

	char *key1 = g_utf8_collate_key(*t1,-1);
	char *key2 = g_utf8_collate_key(*t2,-1);
	int n = strcmp(key1,key2);
	g_free(key1);
	g_free(key2);
	return n;
}

/* list_window callback */
static const char *
screen_artist_lw_callback(unsigned idx, void *data)
{
	GPtrArray *list = data;

	if (mode == LIST_ALBUMS) {
		if (idx == 0)
			return "..";
		else if (idx == list->len + 1)
			return _("All tracks");

		--idx;
	}

	assert(idx < list->len);

	char *str_utf8 = g_ptr_array_index(list, idx);
	assert(str_utf8 != NULL);

	char *str = utf8_to_locale(str_utf8);

	static char buf[BUFSIZE];
	g_strlcpy(buf, str, sizeof(buf));
	g_free(str);

	return buf;
}

static void
screen_artist_paint(void);

static void
artist_repaint(void)
{
	screen_artist_paint();
	wrefresh(browser.lw->w);
}

static void
string_array_free(GPtrArray *array)
{
	for (unsigned i = 0; i < array->len; ++i) {
		char *value = g_ptr_array_index(array, i);
		g_free(value);
	}

	g_ptr_array_free(array, TRUE);
}

static void
free_lists(void)
{
	if (artist_list != NULL) {
		string_array_free(artist_list);
		artist_list = NULL;
	}

	if (album_list != NULL) {
		string_array_free(album_list);
		album_list = NULL;
	}

	if (browser.filelist) {
		filelist_free(browser.filelist);
		browser.filelist = NULL;
	}
}

static void
recv_tag_values(struct mpd_connection *connection, enum mpd_tag_type tag,
		GPtrArray *list)
{
	struct mpd_pair *pair;

	while ((pair = mpd_recv_pair_tag(connection, tag)) != NULL) {
		g_ptr_array_add(list, g_strdup(pair->value));
		mpd_return_pair(connection, pair);
	}
}

static void
load_artist_list(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);

	assert(mode == LIST_ARTISTS);
	assert(artist == NULL);
	assert(album == NULL);
	assert(artist_list == NULL);
	assert(album_list == NULL);
	assert(browser.filelist == NULL);

	artist_list = g_ptr_array_new();

	if (connection != NULL) {
		mpd_search_db_tags(connection, MPD_TAG_ARTIST);
		mpd_search_commit(connection);
		recv_tag_values(connection, MPD_TAG_ARTIST, artist_list);

		mpdclient_finish_command(c);
	}

	/* sort list */
	g_ptr_array_sort(artist_list, compare_utf8);
	list_window_set_length(browser.lw, artist_list->len);
}

static void
load_album_list(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);

	assert(mode == LIST_ALBUMS);
	assert(artist != NULL);
	assert(album == NULL);
	assert(album_list == NULL);
	assert(browser.filelist == NULL);

	album_list = g_ptr_array_new();

	if (connection != NULL) {
		mpd_search_db_tags(connection, MPD_TAG_ALBUM);
		mpd_search_add_tag_constraint(connection,
					      MPD_OPERATOR_DEFAULT,
					      MPD_TAG_ARTIST, artist);
		mpd_search_commit(connection);

		recv_tag_values(connection, MPD_TAG_ALBUM, album_list);

		mpdclient_finish_command(c);
	}

	/* sort list */
	g_ptr_array_sort(album_list, compare_utf8);

	list_window_set_length(browser.lw, album_list->len + 2);
}

static void
load_song_list(struct mpdclient *c)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);

	assert(mode == LIST_SONGS);
	assert(artist != NULL);
	assert(album != NULL);
	assert(browser.filelist == NULL);

	browser.filelist = filelist_new();
	/* add a dummy entry for ".." */
	filelist_append(browser.filelist, NULL);

	if (connection != NULL) {
		mpd_search_db_songs(connection, true);
		mpd_search_add_tag_constraint(connection, MPD_OPERATOR_DEFAULT,
					      MPD_TAG_ARTIST, artist);
		if (album != ALL_TRACKS)
			mpd_search_add_tag_constraint(connection, MPD_OPERATOR_DEFAULT,
						      MPD_TAG_ALBUM, album);
		mpd_search_commit(connection);

		filelist_recv(browser.filelist, connection);

		mpdclient_finish_command(c);
	}

	/* fix highlights */
	screen_browser_sync_highlights(browser.filelist, &c->playlist);
	list_window_set_length(browser.lw, filelist_length(browser.filelist));
}

static void
free_state(void)
{
	g_free(artist);
	if (album != ALL_TRACKS)
		g_free(album);
	artist = NULL;
	album = NULL;

	free_lists();
}

static void
open_artist_list(struct mpdclient *c)
{
	free_state();

	mode = LIST_ARTISTS;
	load_artist_list(c);
}

static void
open_album_list(struct mpdclient *c, char *_artist)
{
	assert(_artist != NULL);

	free_state();

	mode = LIST_ALBUMS;
	artist = _artist;
	load_album_list(c);
}

static void
open_song_list(struct mpdclient *c, char *_artist, char *_album)
{
	assert(_artist != NULL);
	assert(_album != NULL);

	free_state();

	mode = LIST_SONGS;
	artist = _artist;
	album = _album;
	load_song_list(c);
}

static void
reload_lists(struct mpdclient *c)
{
	free_lists();

	switch (mode) {
	case LIST_ARTISTS:
		load_artist_list(c);
		break;

	case LIST_ALBUMS:
		load_album_list(c);
		break;

	case LIST_SONGS:
		load_song_list(c);
		break;
	}
}

static void
screen_artist_init(WINDOW *w, int cols, int rows)
{
	browser.lw = list_window_init(w, cols, rows);
	browser.song_format = options.list_format;
	artist = NULL;
	album = NULL;
}

static void
screen_artist_quit(void)
{
	free_state();
	list_window_free(browser.lw);
}

static void
screen_artist_open(struct mpdclient *c)
{
	if (artist_list == NULL && album_list == NULL &&
	    browser.filelist == NULL)
		reload_lists(c);
}

static void
screen_artist_resize(int cols, int rows)
{
	list_window_resize(browser.lw, cols, rows);
}

/**
 * Paint one item in the artist list.
 */
static void
paint_artist_callback(WINDOW *w, unsigned i,
		      gcc_unused unsigned y, unsigned width,
		      bool selected, const void *data)
{
	const GPtrArray *list = data;
	char *p = utf8_to_locale(g_ptr_array_index(list, i));

	screen_browser_paint_directory(w, width, selected, p);
	g_free(p);
}

/**
 * Paint one item in the album list.  There are two virtual items
 * inserted: at the beginning, there's the special item ".." to go to
 * the parent directory, and at the end, there's the item "All tracks"
 * to view the tracks of all albums.
 */
static void
paint_album_callback(WINDOW *w, unsigned i,
		     gcc_unused unsigned y, unsigned width,
		     bool selected, const void *data)
{
	const GPtrArray *list = data;
	const char *p;
	char *q = NULL;

	if (i == 0)
		p = "..";
	else if (i == list->len + 1)
		p = _("All tracks");
	else
		p = q = utf8_to_locale(g_ptr_array_index(list, i - 1));

	screen_browser_paint_directory(w, width, selected, p);
	g_free(q);
}

static void
screen_artist_paint(void)
{
	if (browser.filelist) {
		screen_browser_paint(&browser);
	} else if (album_list != NULL)
		list_window_paint2(browser.lw,
				   paint_album_callback, album_list);
	else if (artist_list != NULL)
		list_window_paint2(browser.lw,
				   paint_artist_callback, artist_list);
	else {
		wmove(browser.lw->w, 0, 0);
		wclrtobot(browser.lw->w);
	}
}

static const char *
screen_artist_get_title(char *str, size_t size)
{
	switch(mode) {
		char *s1, *s2;

	case LIST_ARTISTS:
		g_snprintf(str, size, _("All artists"));
		break;

	case LIST_ALBUMS:
		s1 = utf8_to_locale(artist);
		g_snprintf(str, size, _("Albums of artist: %s"), s1);
		g_free(s1);
		break;

	case LIST_SONGS:
		s1 = utf8_to_locale(artist);

		if (album == ALL_TRACKS)
			g_snprintf(str, size,
				   _("All tracks of artist: %s"), s1);
		else if (*album != 0) {
			s2 = utf8_to_locale(album);
			g_snprintf(str, size, _("Album: %s - %s"), s1, s2);
			g_free(s2);
		} else
			g_snprintf(str, size,
				   _("Tracks of no album of artist: %s"), s1);
		g_free(s1);
		break;
	}

	return str;
}

static void
screen_artist_update(struct mpdclient *c)
{
	if (browser.filelist == NULL)
		return;

	if (c->events & MPD_IDLE_DATABASE)
		/* the db has changed -> update the list */
		reload_lists(c);

	if (c->events & (MPD_IDLE_DATABASE | MPD_IDLE_QUEUE))
		screen_browser_sync_highlights(browser.filelist, &c->playlist);

	if (c->events & (MPD_IDLE_DATABASE
#ifndef NCMPC_MINI
			 | MPD_IDLE_QUEUE
#endif
			 ))
		artist_repaint();
}

/* add_query - Add all songs satisfying specified criteria.
   _artist is actually only used in the ALBUM case to distinguish albums with
   the same name from different artists. */
static void
add_query(struct mpdclient *c, enum mpd_tag_type table, const char *_filter,
	  const char *_artist)
{
	struct mpd_connection *connection = mpdclient_get_connection(c);

	assert(_filter != NULL);

	if (connection == NULL)
		return;

	char *str = utf8_to_locale(_filter);
	if (table == MPD_TAG_ALBUM)
		screen_status_printf(_("Adding album %s..."), str);
	else
		screen_status_printf(_("Adding %s..."), str);
	g_free(str);

	mpd_search_db_songs(connection, true);
	mpd_search_add_tag_constraint(connection, MPD_OPERATOR_DEFAULT,
				      table, _filter);
	if (table == MPD_TAG_ALBUM)
		mpd_search_add_tag_constraint(connection, MPD_OPERATOR_DEFAULT,
					      MPD_TAG_ARTIST, _artist);
	mpd_search_commit(connection);

	struct filelist *addlist = filelist_new_recv(connection);

	if (mpdclient_finish_command(c))
		mpdclient_filelist_add_all(c, addlist);

	filelist_free(addlist);
}

static int
screen_artist_lw_cmd(struct mpdclient *c, command_t cmd)
{
	switch (mode) {
	case LIST_ARTISTS:
	case LIST_ALBUMS:
		return list_window_cmd(browser.lw, cmd);

	case LIST_SONGS:
		return browser_cmd(&browser, c, cmd);
	}

	assert(0);
	return 0;
}

static int
string_array_find(GPtrArray *array, const char *value)
{
	guint i;

	for (i = 0; i < array->len; ++i)
		if (strcmp((const char*)g_ptr_array_index(array, i),
			   value) == 0)
			return i;

	return -1;
}

static bool
screen_artist_cmd(struct mpdclient *c, command_t cmd)
{
	switch(cmd) {
		struct list_window_range range;
		char *selected;
		char *old;
		char *old_ptr;
		int idx;

	case CMD_PLAY:
		switch (mode) {
		case LIST_ARTISTS:
			if (browser.lw->selected >= artist_list->len)
				return true;

			selected = g_ptr_array_index(artist_list,
						     browser.lw->selected);
			open_album_list(c, g_strdup(selected));
			list_window_reset(browser.lw);

			artist_repaint();
			return true;

		case LIST_ALBUMS:
			if (browser.lw->selected == 0) {
				/* handle ".." */
				old = g_strdup(artist);

				open_artist_list(c);
				list_window_reset(browser.lw);
				/* restore previous list window state */
				idx = string_array_find(artist_list, old);
				g_free(old);

				if (idx >= 0) {
					list_window_set_cursor(browser.lw, idx);
					list_window_center(browser.lw, idx);
				}
			} else if (browser.lw->selected == album_list->len + 1) {
				/* handle "show all" */
				open_song_list(c, g_strdup(artist), ALL_TRACKS);
				list_window_reset(browser.lw);
			} else {
				/* select album */
				selected = g_ptr_array_index(album_list,
							     browser.lw->selected - 1);
				open_song_list(c, g_strdup(artist), g_strdup(selected));
				list_window_reset(browser.lw);
			}

			artist_repaint();
			return true;

		case LIST_SONGS:
			if (browser.lw->selected == 0) {
				/* handle ".." */
				old = g_strdup(album);
				old_ptr = album;

				open_album_list(c, g_strdup(artist));
				list_window_reset(browser.lw);
				/* restore previous list window state */
				idx = old_ptr == ALL_TRACKS
					? (int)album_list->len
					: string_array_find(album_list, old);
				g_free(old);

				if (idx >= 0) {
					++idx;
					list_window_set_cursor(browser.lw, idx);
					list_window_center(browser.lw, idx);
				}

				artist_repaint();
				return true;
			}
			break;
		}
		break;

		/* FIXME? CMD_GO_* handling duplicates code from CMD_PLAY */

	case CMD_GO_PARENT_DIRECTORY:
		switch (mode) {
		case LIST_ARTISTS:
			break;

		case LIST_ALBUMS:
			old = g_strdup(artist);

			open_artist_list(c);
			list_window_reset(browser.lw);
			/* restore previous list window state */
			idx = string_array_find(artist_list, old);
			g_free(old);

			if (idx >= 0) {
				list_window_set_cursor(browser.lw, idx);
				list_window_center(browser.lw, idx);
			}
			break;

		case LIST_SONGS:
			old = g_strdup(album);
			old_ptr = album;

			open_album_list(c, g_strdup(artist));
			list_window_reset(browser.lw);
			/* restore previous list window state */
			idx = old_ptr == ALL_TRACKS
				? (int)album_list->len
				: string_array_find(album_list, old);
			g_free(old);

			if (idx >= 0) {
				++idx;
				list_window_set_cursor(browser.lw, idx);
				list_window_center(browser.lw, idx);
			}
			break;
		}

		artist_repaint();
		break;

	case CMD_GO_ROOT_DIRECTORY:
		switch (mode) {
		case LIST_ARTISTS:
			break;

		case LIST_ALBUMS:
		case LIST_SONGS:
			open_artist_list(c);
			list_window_reset(browser.lw);
			/* restore first list window state (pop while returning true) */
			/* XXX */
			break;
		}

		artist_repaint();
		break;

	case CMD_SELECT:
	case CMD_ADD:
		switch(mode) {
		case LIST_ARTISTS:
			if (browser.lw->selected >= artist_list->len)
				return true;

			list_window_get_range(browser.lw, &range);
			for (unsigned i = range.start; i < range.end; ++i) {
				selected = g_ptr_array_index(artist_list, i);
				add_query(c, MPD_TAG_ARTIST, selected, NULL);
				cmd = CMD_LIST_NEXT; /* continue and select next item... */
			}
			break;

		case LIST_ALBUMS:
			list_window_get_range(browser.lw, &range);
			for (unsigned i = range.start; i < range.end; ++i) {
				if(i == album_list->len + 1)
					add_query(c, MPD_TAG_ARTIST, artist, NULL);
				else if (i > 0)
				{
					selected = g_ptr_array_index(album_list,
								     browser.lw->selected - 1);
					add_query(c, MPD_TAG_ALBUM, selected, artist);
					cmd = CMD_LIST_NEXT; /* continue and select next item... */
				}
			}
			break;

		case LIST_SONGS:
			/* handled by browser_cmd() */
			break;
		}
		break;

		/* continue and update... */
	case CMD_SCREEN_UPDATE:
		reload_lists(c);
		return false;

	case CMD_LIST_FIND:
	case CMD_LIST_RFIND:
	case CMD_LIST_FIND_NEXT:
	case CMD_LIST_RFIND_NEXT:
		switch (mode) {
		case LIST_ARTISTS:
			screen_find(browser.lw, cmd,
				    screen_artist_lw_callback, artist_list);
			artist_repaint();
			return true;

		case LIST_ALBUMS:
			screen_find(browser.lw, cmd,
				    screen_artist_lw_callback, album_list);
			artist_repaint();
			return true;

		case LIST_SONGS:
			/* handled by browser_cmd() */
			break;
		}
	case CMD_LIST_JUMP:
		switch (mode) {
		case LIST_ARTISTS:
			screen_jump(browser.lw, screen_artist_lw_callback,
				    paint_artist_callback, artist_list);
			artist_repaint();
			return true;

		case LIST_ALBUMS:
			screen_jump(browser.lw, screen_artist_lw_callback,
				    paint_album_callback, album_list);
			artist_repaint();
			return true;

		case LIST_SONGS:
			/* handled by browser_cmd() */
			break;
		}

		break;

	default:
		break;
	}

	if (screen_artist_lw_cmd(c, cmd)) {
		if (screen_is_visible(&screen_artist))
			artist_repaint();
		return true;
	}

	return false;
}

const struct screen_functions screen_artist = {
	.init = screen_artist_init,
	.exit = screen_artist_quit,
	.open = screen_artist_open,
	.resize = screen_artist_resize,
	.paint = screen_artist_paint,
	.update = screen_artist_update,
	.cmd = screen_artist_cmd,
	.get_title = screen_artist_get_title,
};
