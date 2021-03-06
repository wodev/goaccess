/**
 * ui.c -- various curses interfaces
 * Copyright (C) 2009-2014 by Gerardo Orellana <goaccess@prosoftcorp.com>
 * GoAccess - An Ncurses apache weblog analyzer & interactive viewer
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * A copy of the GNU General Public License is attached to this
 * source distribution for its full text.
 *
 * Visit http://goaccess.prosoftcorp.com for new releases.
 */

#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#define STDIN_FILENO  0
#ifndef _BSD_SOURCE
#define _BSD_SOURCE     /* include stuff from 4.3 BSD */
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <pthread.h>
#include <ctype.h>

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

#ifdef HAVE_LIBTOKYOCABINET
#include "tcabdb.h"
#else
#include "gkhash.h"
#endif

#include "color.h"
#include "error.h"
#include "gmenu.h"
#include "goaccess.h"
#include "util.h"
#include "xmalloc.h"

/* *INDENT-OFF* */
static GOutput outputting[] = {
  {VISITORS        , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 1 , 1} ,
  {REQUESTS        , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0} ,
  {REQUESTS_STATIC , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0} ,
  {NOT_FOUND       , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0} ,
  {HOSTS           , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 1 , 0} ,
  {OS              , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 1 , 1} ,
  {BROWSERS        , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 1 , 1} ,
  {VISIT_TIMES     , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 1 , 1} ,
  {VIRTUAL_HOSTS   , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 0 , 0} ,
  {REFERRERS       , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 0 , 0} ,
  {REFERRING_SITES , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 0 , 0} ,
  {KEYPHRASES      , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 0 , 0} ,
#ifdef HAVE_LIBGEOIP
  {GEO_LOCATION    , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 0 , 0} ,
#endif
  {STATUS_CODES    , 1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 , 0 , 1 , 0 , 0} ,
};
/* *INDENT-ON* */

static char *date_format = NULL;
static char *log_format = NULL;
static char *time_format = NULL;

typedef struct Field_
{
  const char *field;
  /* char due to log, bw, log_file */
  char *value;
  GColors *(*colorlbl) (void);
  GColors *(*colorval) (void);
  short oneliner;
} Field;

/* Determine which metrics to output given a module */
GOutput *
output_lookup (GModule module)
{
  int i, num_panels = ARRAY_SIZE (outputting);

  for (i = 0; i < num_panels; i++) {
    if (outputting[i].module == module)
      return &outputting[i];
  }
  return NULL;
}

/* initialize curses colors */
void
init_colors (int force)
{
  /* use default foreground/background colors */
  use_default_colors ();
  /* first set a default normal color */
  set_normal_color ();
  /* then parse custom colors and initialize them */
  set_colors (force);
}

/* creation - ncurses' window handling */
void
set_input_opts (void)
{
  initscr ();
  clear ();
  noecho ();
  halfdelay (10);
  nonl ();
  intrflush (stdscr, FALSE);
  keypad (stdscr, TRUE);
  if (curs_set (0) == ERR)
    LOG_DEBUG (("Unable to change cursor: %s\n", strerror (errno)));

  if (conf.mouse_support)
    mousemask (BUTTON1_CLICKED, NULL);
}

/* delete ncurses window handling */
void
close_win (WINDOW * w)
{
  if (w == NULL)
    return;
  wclear (w);
  wrefresh (w);
  delwin (w);
}

void
generate_time (void)
{
  timestamp = time (NULL);
  now_tm = localtime (&timestamp);
}

void
end_spinner (void)
{
  pthread_mutex_lock (&parsing_spinner->mutex);
  parsing_spinner->state = SPN_END;
  pthread_mutex_unlock (&parsing_spinner->mutex);
}

void
init_windows (WINDOW ** header_win, WINDOW ** main_win)
{
  GColors *color = get_color (COLOR_BG);
  int row = 0, col = 0;

  /* init standard screen */
  getmaxyx (stdscr, row, col);
  if (row < MIN_HEIGHT || col < MIN_WIDTH)
    FATAL ("Minimum screen size - 0 columns by 7 lines");

  /* init header screen */
  *header_win = newwin (6, col, 0, 0);
  keypad (*header_win, TRUE);
  if (*header_win == NULL)
    FATAL ("Unable to allocate memory for header_win.");

  /* init main screen */
  *main_win = newwin (row - 8, col, 7, 0);
  keypad (*main_win, TRUE);
  if (*main_win == NULL)
    FATAL ("Unable to allocate memory for main_win.");

  /* background colors */
  wbkgd (*main_win, COLOR_PAIR (color->pair->idx));
  wbkgd (*header_win, COLOR_PAIR (color->pair->idx));
  wbkgd (stdscr, COLOR_PAIR (color->pair->idx));
}

#pragma GCC diagnostic ignored "-Wformat-nonliteral"
/* draw a generic header */
void
draw_header (WINDOW * win, const char *s, const char *fmt, int y, int x, int w,
             GColors * (*func) (void))
{
  GColors *color = (*func) ();
  char *buf;

  buf = xmalloc (snprintf (NULL, 0, fmt, s) + 1);
  sprintf (buf, fmt, s);

  wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
  mvwhline (win, y, x, ' ', w);
  mvwaddnstr (win, y, x, buf, w);
  wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));

  free (buf);
}

#pragma GCC diagnostic warning "-Wformat-nonliteral"

/* determine the actual size of the main window */
void
term_size (WINDOW * main_win)
{
  getmaxyx (stdscr, term_h, term_w);

  real_size_y = term_h - (MAX_HEIGHT_HEADER + MAX_HEIGHT_FOOTER);
  wresize (main_win, real_size_y, term_w);
  wmove (main_win, real_size_y, 0);
}

const char *
module_to_label (GModule module)
{
  static const char *modules[] = {
    VISIT_LABEL,
    REQUE_LABEL,
    STATI_LABEL,
    FOUND_LABEL,
    HOSTS_LABEL,
    OPERA_LABEL,
    BROWS_LABEL,
    VTIME_LABEL,
    VHOST_LABEL,
    REFER_LABEL,
    SITES_LABEL,
    KEYPH_LABEL,
#ifdef HAVE_LIBGEOIP
    GEOLO_LABEL,
#endif
    CODES_LABEL,
  };

  return modules[module];
}

const char *
module_to_id (GModule module)
{
  static const char *modules[] = {
    VISIT_ID,
    REQUE_ID,
    STATI_ID,
    FOUND_ID,
    HOSTS_ID,
    OPERA_ID,
    BROWS_ID,
    VTIME_ID,
    VHOST_ID,
    REFER_ID,
    SITES_ID,
    KEYPH_ID,
#ifdef HAVE_LIBGEOIP
    GEOLO_ID,
#endif
    CODES_ID,
  };

  return modules[module];
}

const char *
module_to_head (GModule module)
{
  static const char *modules[] = {
    VISIT_HEAD,
    REQUE_HEAD,
    STATI_HEAD,
    FOUND_HEAD,
    HOSTS_HEAD,
    OPERA_HEAD,
    BROWS_HEAD,
    VTIME_HEAD,
    VHOST_HEAD,
    REFER_HEAD,
    SITES_HEAD,
    KEYPH_HEAD,
#ifdef HAVE_LIBGEOIP
    GEOLO_HEAD,
#endif
    CODES_HEAD,
  };

  return modules[module];
}

const char *
module_to_desc (GModule module)
{
  static const char *modules[] = {
    VISIT_DESC,
    REQUE_DESC,
    STATI_DESC,
    FOUND_DESC,
    HOSTS_DESC,
    OPERA_DESC,
    BROWS_DESC,
    VTIME_DESC,
    VHOST_DESC,
    REFER_DESC,
    SITES_DESC,
    KEYPH_DESC,
#ifdef HAVE_LIBGEOIP
    GEOLO_DESC,
#endif
    CODES_DESC,
  };

  return modules[module];
}

/* rerender header window to reflect active module */
void
update_active_module (WINDOW * header_win, GModule current)
{
  GColors *color = get_color (COLOR_ACTIVE_LABEL);
  const char *module = module_to_label (current);
  int col = getmaxx (stdscr);

  char *lbl = xmalloc (snprintf (NULL, 0, "[Active Panel: %s]", module) + 1);
  sprintf (lbl, "[Active Panel: %s]", module);

  wmove (header_win, 0, 30);

  wattron (header_win, color->attr | COLOR_PAIR (color->pair->idx));
  mvwprintw (header_win, 0, col - strlen (lbl) - 1, "%s", lbl);
  wattroff (header_win, color->attr | COLOR_PAIR (color->pair->idx));

  wrefresh (header_win);

  free (lbl);
}

static void
render_overall_field (WINDOW * win, const char *s, int y, int x,
                      GColors * color)
{
  wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
  mvwprintw (win, y, x, "%s", s);
  wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
}

static void
render_overall_value (WINDOW * win, const char *s, int y, int x,
                      GColors * color)
{
  wattron (win, color->attr | COLOR_PAIR (color->pair->idx));
  mvwprintw (win, y, x, "%s", s);
  wattroff (win, color->attr | COLOR_PAIR (color->pair->idx));
}

static char *
get_str_excluded_ips (GLog * logger)
{
  return int2str (logger->excluded_ip, 0);
}

static char *
get_str_failed_reqs (GLog * logger)
{
  return int2str (logger->invalid, 0);
}

static char *
get_str_processed_reqs (GLog * logger)
{
  return int2str (logger->processed, 0);
}

static char *
get_str_valid_reqs (GLog * logger)
{
  return int2str (logger->valid, 0);
}

static char *
get_str_notfound_reqs (void)
{
  return int2str (ht_get_size_datamap (NOT_FOUND), 0);
}

static char *
get_str_ref_reqs (void)
{
  return int2str (ht_get_size_datamap (REFERRERS), 0);
}

static char *
get_str_reqs (void)
{
  return int2str (ht_get_size_datamap (REQUESTS), 0);
}

static char *
get_str_static_reqs (void)
{
  return int2str (ht_get_size_datamap (REQUESTS_STATIC), 0);
}

static char *
get_str_visitors (void)
{
  return int2str (ht_get_size_uniqmap (VISITORS), 0);
}

static char *
get_str_proctime (void)
{
  return int2str (((long long) end_proc - start_proc), 0);
}

static char *
get_str_filesize (GLog * logger, const char *ifile)
{
  if (!logger->piping && ifile != NULL)
    return filesize_str (file_size (ifile));
  else
    return alloc_string ("N/A");
}

static char *
get_str_logfile (GLog * logger, const char *ifile)
{
  if (!logger->piping && ifile != NULL)
    return alloc_string (ifile);
  else
    return alloc_string ("STDIN");
}

static char *
get_str_bandwidth (GLog * logger)
{
  return filesize_str ((float) logger->resp_size);
}

/* render overall statistics */
static void
render_overall_statistics (WINDOW * win, Field fields[], size_t n)
{
  GColors *color = NULL;
  int x_field = 2, x_value = 0;
  size_t i, j, k, max_field = 0, max_value = 0, mod_val, y;

  for (i = 0, k = 0, y = 2; i < n; i++) {
    /* new line every OVERALL_NUM_COLS */
    mod_val = k % OVERALL_NUM_COLS;

    /* reset position & length and increment row */
    if (k > 0 && mod_val == 0) {
      max_value = max_field = 0;
      x_value = x_field = 2;
      y++;
    }

    /* x pos = max length of field */
    x_field += max_field;

    color = (*fields[i].colorlbl) ();
    render_overall_field (win, fields[i].field, y, x_field, color);

    /* get max length of field in the same column */
    max_field = 0;
    for (j = 0; j < n; j++) {
      size_t len = strlen (fields[j].field);
      if (j % OVERALL_NUM_COLS == mod_val && len > max_field &&
          !fields[j].oneliner)
        max_field = len;
    }
    /* get max length of value in the same column */
    max_value = 0;
    for (j = 0; j < n; j++) {
      size_t len = strlen (fields[j].value);
      if (j % OVERALL_NUM_COLS == mod_val && len > max_value &&
          !fields[j].oneliner)
        max_value = len;
    }

    /* spacers */
    x_value = max_field + x_field + 1;
    max_field += max_value + 2;

    color = (*fields[i].colorval) ();
    render_overall_value (win, fields[i].value, y, x_value, color);
    k += fields[i].oneliner ? OVERALL_NUM_COLS : 1;
  }
}

void
display_general (WINDOW * win, char *ifile, GLog * logger)
{
  GColors *(*colorlbl) (void) = color_overall_lbls;
  GColors *(*colorpth) (void) = color_overall_path;
  GColors *(*colorval) (void) = color_overall_vals;

  int col = getmaxx (stdscr);
  size_t n, i;

  /* *INDENT-OFF* */
  Field fields[] = {
    {T_REQUESTS   , get_str_processed_reqs (logger)  , colorlbl , colorval , 0},
    {T_UNIQUE_VIS , get_str_visitors ()              , colorlbl , colorval , 0},
    {T_UNIQUE_FIL , get_str_reqs ()                  , colorlbl , colorval , 0},
    {T_REFERRER   , get_str_ref_reqs ()              , colorlbl , colorval , 0},
    {T_VALID      , get_str_valid_reqs (logger)      , colorlbl , colorval , 0},
    {T_GEN_TIME   , get_str_proctime ()              , colorlbl , colorval , 0},
    {T_STATIC_FIL , get_str_static_reqs ()           , colorlbl , colorval , 0},
    {T_LOG        , get_str_filesize (logger, ifile) , colorlbl , colorval , 0},
    {T_FAILED     , get_str_failed_reqs (logger)     , colorlbl , colorval , 0},
    {T_EXCLUDE_IP , get_str_excluded_ips (logger)    , colorlbl , colorval , 0},
    {T_UNIQUE404  , get_str_notfound_reqs ()         , colorlbl , colorval , 0},
    {T_BW         , get_str_bandwidth (logger)       , colorlbl , colorval , 0},
    {T_LOG_PATH   , get_str_logfile (logger, ifile)  , colorlbl , colorpth , 1}
  };
  /* *INDENT-ON* */

  werase (win);
  draw_header (win, T_DASH " - " T_HEAD, " %s", 0, 0, col, color_panel_header);

  n = ARRAY_SIZE (fields);
  render_overall_statistics (win, fields, n);

  for (i = 0; i < n; i++) {
    free (fields[i].value);
  }
}

/* implement basic frame work to build a field input */
char *
input_string (WINDOW * win, int pos_y, int pos_x, size_t max_width,
              const char *str, int enable_case, int *toggle_case)
{
  char *s = xmalloc (max_width + 1), *tmp;
  size_t i, c, pos = 0, x = 0, quit = 1, len = 0, size_x = 0, size_y = 0;

  getmaxyx (win, size_y, size_x);
  size_x -= 4;

  /* are we setting a default string */
  if (str) {
    len = MIN (max_width, strlen (str));
    memcpy (s, str, len);
    s[len] = '\0';

    x = pos = 0;
    /* is the default str length greater than input field? */
    if (strlen (s) > size_x) {
      tmp = xstrdup (&s[0]);
      tmp[size_x] = '\0';
      mvwprintw (win, pos_y, pos_x, "%s", tmp);
      free (tmp);
    } else {
      mvwprintw (win, pos_y, pos_x, "%s", s);
    }
  } else {
    s[0] = '\0';
  }

  if (enable_case)
    mvwprintw (win, size_y - 2, 1, " %s", CSENSITIVE);

  wmove (win, pos_y, pos_x + x);
  wrefresh (win);

  curs_set (1);
  while (quit) {
    c = wgetch (stdscr);
    switch (c) {
    case 1:    /* ^a   */
    case 262:  /* HOME */
      pos = x = 0;
      break;
    case 5:
    case 360:  /* END of line */
      if (strlen (s) > size_x) {
        x = size_x;
        pos = strlen (s) - size_x;
      } else {
        pos = 0;
        x = strlen (s);
      }
      break;
    case 7:    /* ^g  */
    case 27:   /* ESC */
      pos = x = 0;
      if (str && *str == '\0')
        s[0] = '\0';
      quit = 0;
      break;
    case 9:    /* TAB   */
      if (!enable_case)
        break;
      *toggle_case = *toggle_case == 0 ? 1 : 0;
      if (*toggle_case)
        mvwprintw (win, size_y - 2, 1, " %s", CISENSITIVE);
      else if (!*toggle_case)
        mvwprintw (win, size_y - 2, 1, " %s", CSENSITIVE);
      break;
    case 21:   /* ^u */
      s[0] = '\0';
      pos = x = 0;
      break;
    case 8:    /* xterm-256color */
    case 127:
    case KEY_BACKSPACE:
      if (pos + x > 0) {
        memmove (&s[(pos + x) - 1], &s[pos + x], (max_width - (pos + x)) + 1);
        if (pos <= 0)
          x--;
        else
          pos--;
      }
      break;
    case KEY_LEFT:
      if (x > 0)
        x--;
      else if (pos > 0)
        pos--;
      break;
    case KEY_RIGHT:
      if ((x + pos) < strlen (s)) {
        if (x < size_x)
          x++;
        else
          pos++;
      }
      break;
    case 0x0a:
    case 0x0d:
    case KEY_ENTER:
      quit = 0;
      break;
    default:
      if (strlen (s) == max_width)
        break;
      if (!isprint (c))
        break;

      if (strlen (s) == pos) {
        s[pos + x] = c;
        s[pos + x + 1] = '\0';
        waddch (win, c);
      } else {
        memmove (&s[pos + x + 1], &s[pos + x], strlen (&s[pos + x]) + 1);
        s[pos + x] = c;
      }
      if ((x + pos) < max_width) {
        if (x < size_x)
          x++;
        else
          pos++;
      }
    }
    tmp = xstrdup (&s[pos > 0 ? pos : 0]);
    tmp[MIN (strlen (tmp), size_x)] = '\0';
    for (i = strlen (tmp); i < size_x; i++)
      mvwprintw (win, pos_y, pos_x + i, "%s", " ");
    mvwprintw (win, pos_y, pos_x, "%s", tmp);
    free (tmp);

    wmove (win, pos_y, pos_x + x);
    wrefresh (win);
  }
  curs_set (0);
  return s;
}

static int
fill_host_agents_gmenu (void *val, void *user_data)
{
  GMenu *menu = user_data;
  char *agent = ht_get_host_agent_val ((*(int *) val));

  if (agent == NULL)
    return 1;

  menu->items[menu->size].name = agent;
  menu->items[menu->size].checked = 0;
  menu->size++;

  return 0;
}

static void
load_host_agents_gmenu (void *list, void *user_data, int count)
{
  GSLList *lst = list;
  GMenu *menu = user_data;

  menu->items = (GItem *) xcalloc (count, sizeof (GItem));
  list_foreach (lst, fill_host_agents_gmenu, menu);
}

#ifdef TCB_BTREE
int
set_host_agents (const char *addr, void (*func) (void *, void *, int),
                 void *arr)
{
  TCLIST *tclist;
  GSLList *list;
  int key, count = 0;

  key = ht_get_keymap (HOSTS, addr);
  if (key == 0)
    return 1;

  tclist = ht_get_host_agent_tclist (HOSTS, key);
  if (!tclist)
    return 1;

  list = tclist_to_gsllist (tclist);
  if ((count = list_count (list)) == 0) {
    free (list);
    return 1;
  }

  func (list, arr, count);

  list_remove_nodes (list);
  tclistdel (tclist);

  return 0;
}
#endif

#ifndef TCB_BTREE
int
set_host_agents (const char *addr, void (*func) (void *, void *, int),
                 void *arr)
{
  GSLList *list;
  int data_nkey, count = 0;

  data_nkey = ht_get_keymap (HOSTS, addr);
  if (data_nkey == 0)
    return 1;

  list = ht_get_host_agent_list (HOSTS, data_nkey);
  if (!list)
    return 1;

  if ((count = list_count (list)) == 0) {
    free (list);
    return 1;
  }

  func (list, arr, count);

#ifdef TCB_MEMHASH
  free (list);
#endif

  return 0;
}
#endif

/* render a list of agents if available */
void
load_agent_list (WINDOW * main_win, char *addr)
{
  GMenu *menu;
  WINDOW *win;

  char buf[256];
  int c, quit = 1, i;
  int y, x, list_h, list_w, menu_w, menu_h;

  if (!conf.list_agents)
    return;

  getmaxyx (stdscr, y, x);
  list_h = y / 2;       /* list window - height */
  list_w = x - 4;       /* list window - width */
  menu_h = list_h - AGENTS_MENU_Y - 1;  /* menu window - height */
  menu_w = list_w - AGENTS_MENU_X - AGENTS_MENU_X;      /* menu window - width */

  win = newwin (list_h, list_w, (y - list_h) / 2, (x - list_w) / 2);
  keypad (win, TRUE);
  wborder (win, '|', '|', '-', '-', '+', '+', '+', '+');

  /* create a new instance of GMenu and make it selectable */
  menu = new_gmenu (win, menu_h, menu_w, AGENTS_MENU_Y, AGENTS_MENU_X);
  if (set_host_agents (addr, load_host_agents_gmenu, menu) == 1)
    goto out;

  post_gmenu (menu);
  snprintf (buf, sizeof buf, "User Agents for %s", addr);
  draw_header (win, buf, " %s", 1, 1, list_w - 2, color_panel_header);
  mvwprintw (win, 2, 2, "[UP/DOWN] to scroll - [q] to close window");
  wrefresh (win);

  while (quit) {
    c = wgetch (stdscr);
    switch (c) {
    case KEY_DOWN:
      gmenu_driver (menu, REQ_DOWN);
      break;
    case KEY_UP:
      gmenu_driver (menu, REQ_UP);
      break;
    case KEY_RESIZE:
    case 'q':
      quit = 0;
      break;
    }
    wrefresh (win);
  }

  touchwin (main_win);
  close_win (win);
  wrefresh (main_win);

out:

  /* clean stuff up */
  for (i = 0; i < menu->size; ++i)
    free (menu->items[i].name);
  if (menu->items)
    free (menu->items);
  free (menu);
}

/* render processing spinner */
static void
ui_spinner (void *ptr_data)
{
  GSpinner *sp = (GSpinner *) ptr_data;
  GColors *color = NULL;

  static char const spin_chars[] = "/-\\|";
  char buf[SPIN_LBL];
  int i = 0;
  long long tdiff = 0, psec = 0;
  time_t begin;

  if (sp->curses)
    color = (*sp->color) ();

  time (&begin);
  while (1) {
    pthread_mutex_lock (&sp->mutex);
    if (sp->state == SPN_END)
      break;

    setlocale (LC_NUMERIC, "");
    if (conf.no_progress) {
      snprintf (buf, sizeof buf, SPIN_FMT, sp->label);
    } else {
      tdiff = (long long) (time (NULL) - begin);
      psec = tdiff >= 1 ? *(sp->processed) / tdiff : 0;
      snprintf (buf, sizeof buf, SPIN_FMTM, sp->label, *(sp->processed), psec);
    }
    setlocale (LC_NUMERIC, "POSIX");

    if (sp->curses) {
      /* CURSES */
      draw_header (sp->win, buf, " %s", sp->y, sp->x, sp->w, sp->color);
      /* caret */
      wattron (sp->win, COLOR_PAIR (color->pair->idx));
      mvwaddch (sp->win, sp->y, sp->spin_x, spin_chars[i++ & 3]);
      wattroff (sp->win, COLOR_PAIR (color->pair->idx));
      wrefresh (sp->win);
    } else if (!conf.no_progress) {
      /* STDOUT */
      fprintf (stderr, "%s\r", buf);
    }

    pthread_mutex_unlock (&sp->mutex);
    usleep (100000);
  }
  sp = NULL;
  free (sp);
}

/* create spinner's thread */
void
ui_spinner_create (GSpinner * spinner)
{
  pthread_create (&(spinner->thread), NULL, (void *) &ui_spinner, spinner);
  pthread_detach (spinner->thread);
}

/* allocate memory and initialize data */
void
set_curses_spinner (GSpinner * spinner)
{
  int y, x;
  if (spinner == NULL)
    return;

  getmaxyx (stdscr, y, x);

  spinner->color = color_progress;
  spinner->curses = 1;
  spinner->win = stdscr;
  spinner->x = 0;
  spinner->w = x;
  spinner->spin_x = x - 2;
  spinner->y = y - 1;
}

/* allocate memory and initialize data */
GSpinner *
new_gspinner (void)
{
  GSpinner *spinner;

  spinner = xcalloc (1, sizeof (GSpinner));
  spinner->label = "Parsing...";
  spinner->state = SPN_RUN;
  spinner->curses = 0;
  if (conf.load_from_disk)
    conf.no_progress = 1;

  if (pthread_mutex_init (&(spinner->mutex), NULL))
    FATAL ("Failed init thread mutex");

  return spinner;
}

static void
clear_confdlg_status_bar (WINDOW * win)
{
  draw_header (win, "", "%s", 3, 2, CONF_MENU_W, color_default);
}

/* render config log date/format dialog */
int
render_confdlg (GLog * logger, GSpinner * spinner)
{
  GMenu *menu;
  WINDOW *win;

  char *cstm_log, *cstm_date, *cstm_time;
  int c, quit = 1, invalid = 1, y, x, h = CONF_WIN_H, w = CONF_WIN_W;
  int w2 = w - 2;
  size_t i, n, sel;

  /* conf dialog menu options */
  const char *choices[] = {
    "NCSA Combined Log Format",
    "NCSA Combined Log Format with Virtual Host",
    "Common Log Format (CLF)",
    "Common Log Format (CLF) with Virtual Host",
    "W3C",
    "CloudFront (Download Distribution)",
    "Google Cloud Storage",
    "AWS Elastic Load Balancing (HTTP/S)"
  };
  n = ARRAY_SIZE (choices);
  getmaxyx (stdscr, y, x);

  win = newwin (h, w, (y - h) / 2, (x - w) / 2);
  keypad (win, TRUE);
  wborder (win, '|', '|', '-', '-', '+', '+', '+', '+');

  /* create a new instance of GMenu and make it selectable */
  menu = new_gmenu (win, CONF_MENU_H, CONF_MENU_W, CONF_MENU_Y, CONF_MENU_X);
  menu->size = n;
  menu->selectable = 1;

  /* add items to GMenu */
  menu->items = (GItem *) xcalloc (n, sizeof (GItem));
  for (i = 0; i < n; ++i) {
    menu->items[i].name = alloc_string (choices[i]);
    sel = get_selected_format_idx ();
    menu->items[i].checked = sel == i ? 1 : 0;
  }
  post_gmenu (menu);

  draw_header (win, "Log Format Configuration", " %s", 1, 1, w2,
               color_panel_header);
  mvwprintw (win, 2, 2, "[SPACE] to toggle - [ENTER] to proceed");

  /* set log format from goaccessrc if available */
  draw_header (win, "Log Format - [c] to add/edit format", " %s", 11, 1, w2,
               color_panel_header);
  if (conf.log_format) {
    log_format = escape_str (conf.log_format);
    mvwprintw (win, 12, 2, "%.*s", CONF_MENU_W, log_format);
    if (conf.log_format)
      free (conf.log_format);
  }

  /* set date format from goaccessrc if available */
  draw_header (win, "Date Format - [d] to add/edit format", " %s", 14, 1, w2,
               color_panel_header);
  if (conf.date_format) {
    date_format = escape_str (conf.date_format);
    mvwprintw (win, 15, 2, "%.*s", CONF_MENU_W, date_format);
    if (conf.date_format)
      free (conf.date_format);
  }

  /* set time format from goaccessrc if available */
  draw_header (win, "Time Format - [t] to add/edit format", " %s", 17, 1, w2,
               color_panel_header);
  if (conf.time_format) {
    time_format = escape_str (conf.time_format);
    mvwprintw (win, 18, 2, "%.*s", CONF_MENU_W, time_format);
    if (conf.time_format)
      free (conf.time_format);
  }

  wrefresh (win);
  while (quit) {
    c = wgetch (stdscr);
    switch (c) {
    case KEY_DOWN:
      gmenu_driver (menu, REQ_DOWN);
      clear_confdlg_status_bar (win);
      break;
    case KEY_UP:
      gmenu_driver (menu, REQ_UP);
      clear_confdlg_status_bar (win);
      break;
    case 32:   /* space */
      gmenu_driver (menu, REQ_SEL);

      if (time_format)
        free (time_format);
      if (date_format)
        free (date_format);
      if (log_format)
        free (log_format);

      for (i = 0; i < n; ++i) {
        if (menu->items[i].checked != 1)
          continue;

        date_format = get_selected_date_str (i);
        log_format = get_selected_format_str (i);
        time_format = get_selected_time_str (i);

        mvwprintw (win, 12, 1, " %s", log_format);
        mvwprintw (win, 15, 1, " %s", date_format);
        mvwprintw (win, 18, 1, " %s", time_format);
        break;
      }
      break;
    case 99:   /* c */
      /* clear top status bar */
      clear_confdlg_status_bar (win);
      wmove (win, 12, 2);

      /* get input string */
      cstm_log = input_string (win, 12, 2, 70, log_format, 0, 0);
      if (cstm_log != NULL && *cstm_log != '\0') {
        if (log_format)
          free (log_format);

        log_format = alloc_string (cstm_log);
        free (cstm_log);
      }
      /* did not set an input string */
      else {
        if (cstm_log)
          free (cstm_log);
        if (log_format) {
          free (log_format);
          log_format = NULL;
        }
      }
      break;
    case 100:  /* d */
      /* clear top status bar */
      clear_confdlg_status_bar (win);
      wmove (win, 15, 0);

      /* get input string */
      cstm_date = input_string (win, 15, 2, 14, date_format, 0, 0);
      if (cstm_date != NULL && *cstm_date != '\0') {
        if (date_format)
          free (date_format);

        date_format = alloc_string (cstm_date);
        free (cstm_date);
      }
      /* did not set an input string */
      else {
        if (cstm_date)
          free (cstm_date);
        if (date_format) {
          free (date_format);
          date_format = NULL;
        }
      }
      break;
    case 116:  /* t */
      /* clear top status bar */
      clear_confdlg_status_bar (win);
      wmove (win, 15, 0);

      /* get input string */
      cstm_time = input_string (win, 18, 2, 14, time_format, 0, 0);
      if (cstm_time != NULL && *cstm_time != '\0') {
        if (time_format)
          free (time_format);

        time_format = alloc_string (cstm_time);
        free (cstm_time);
      }
      /* did not set an input string */
      else {
        if (cstm_time)
          free (cstm_time);
        if (time_format) {
          free (time_format);
          time_format = NULL;
        }
      }
      break;
    case 274:  /* F10 */
    case 0x0a:
    case 0x0d:
    case KEY_ENTER:
      /* display status bar error messages */
      if (time_format == NULL)
        draw_header (win, "Select a time format.", " %s", 3, 2, CONF_MENU_W,
                     color_error);
      if (date_format == NULL)
        draw_header (win, "Select a date format.", " %s", 3, 2, CONF_MENU_W,
                     color_error);
      if (log_format == NULL)
        draw_header (win, "Select a log format.", " %s", 3, 2, CONF_MENU_W,
                     color_error);

      if (date_format && log_format && time_format) {
        conf.time_format = unescape_str (time_format);
        conf.date_format = unescape_str (date_format);
        conf.log_format = unescape_str (log_format);

        /* test log against selected settings */
        if (test_format (logger)) {
          invalid = 1;
          draw_header (win, "No valid hits.", " %s", 3, 2, CONF_MENU_W,
                       color_error);

          free (conf.log_format);
          free (conf.date_format);
          free (conf.time_format);
        }
        /* valid data, reset logger & start parsing */
        else {
          reset_struct (logger);
          /* start spinner thread */
          spinner->win = win;
          spinner->y = 3;
          spinner->x = 2;
          spinner->spin_x = CONF_MENU_W;
          spinner->w = CONF_MENU_W;
          spinner->color = color_progress;
          ui_spinner_create (spinner);

          invalid = 0;
          quit = 0;
        }
      }
      break;
    case KEY_RESIZE:
    case 'q':
      quit = 0;
      break;
    }
    pthread_mutex_lock (&spinner->mutex);
    wrefresh (win);
    pthread_mutex_unlock (&spinner->mutex);
  }
  /* clean stuff up */
  for (i = 0; i < n; ++i)
    free (menu->items[i].name);
  free (menu->items);
  free (menu);

  return invalid ? 1 : 0;
}

/* get selected scheme */
static void
scheme_chosen (const char *name)
{
  int force = 0;

  free_color_lists ();
  if (strcmp ("Green/Original", name) == 0) {
    conf.color_scheme = STD_GREEN;
    force = 1;
  } else if (strcmp ("Monochrome/Default", name) == 0) {
    conf.color_scheme = MONOCHROME;
    force = 1;
  } else if (strcmp ("Custom Scheme", name) == 0) {
    force = 0;
  }
  init_colors (force);
}

/* render schemes dialog */
void
load_schemes_win (WINDOW * main_win)
{
  GMenu *menu;
  WINDOW *win;
  int c, quit = 1;
  size_t i, n;
  int y, x, h = SCHEME_WIN_H, w = SCHEME_WIN_W;
  int w2 = w - 2;

  /* ###NOTE: 'Custom Scheme' needs to go at the end */
  const char *choices[] = {
    "Monochrome/Default",
    "Green/Original",
    "Custom Scheme"
  };

  n = ARRAY_SIZE (choices);
  getmaxyx (stdscr, y, x);

  win = newwin (h, w, (y - h) / 2, (x - w) / 2);
  keypad (win, TRUE);
  wborder (win, '|', '|', '-', '-', '+', '+', '+', '+');

  /* create a new instance of GMenu and make it selectable */
  menu =
    new_gmenu (win, SCHEME_MENU_H, SCHEME_MENU_W, SCHEME_MENU_Y, SCHEME_MENU_X);
  /* remove custom color option if no custom scheme used */
  menu->size = conf.color_idx == 0 ? n - 1 : n;

  /* add items to GMenu */
  menu->items = (GItem *) xcalloc (n, sizeof (GItem));
  for (i = 0; i < n; ++i) {
    menu->items[i].name = alloc_string (choices[i]);
    menu->items[i].checked = 0;
  }
  post_gmenu (menu);

  draw_header (win, "Scheme Configuration", " %s", 1, 1, w2,
               color_panel_header);
  mvwprintw (win, 2, 2, "[ENTER] to switch scheme");

  wrefresh (win);
  while (quit) {
    c = wgetch (stdscr);
    switch (c) {
    case KEY_DOWN:
      gmenu_driver (menu, REQ_DOWN);
      break;
    case KEY_UP:
      gmenu_driver (menu, REQ_UP);
      break;
    case 32:
    case 0x0a:
    case 0x0d:
    case KEY_ENTER:
      gmenu_driver (menu, REQ_SEL);
      for (i = 0; i < n; ++i) {
        if (menu->items[i].checked != 1)
          continue;
        scheme_chosen (choices[i]);
        break;
      }
      quit = 0;
      break;
    case KEY_RESIZE:
    case 'q':
      quit = 0;
      break;
    }
    wrefresh (win);
  }
  /* clean stuff up */
  for (i = 0; i < n; ++i)
    free (menu->items[i].name);
  free (menu->items);
  free (menu);

  touchwin (main_win);
  close_win (win);
  wrefresh (main_win);
}

/* render sort dialog */
void
load_sort_win (WINDOW * main_win, GModule module, GSort * sort)
{
  GMenu *menu;
  WINDOW *win;
  GSortField opts[SORT_MAX_OPTS];

  int c, quit = 1;
  int i = 0, k, n = 0;
  int y, x, h = SORT_WIN_H, w = SORT_WIN_W;
  int w2 = w - 2;

  getmaxyx (stdscr, y, x);

  /* determine amount of sort choices */
  for (i = 0, k = 0; -1 != sort_choices[module][i]; i++) {
    GSortField field = sort_choices[module][i];
    if (SORT_BY_CUMTS == field && !conf.serve_usecs)
      continue;
    else if (SORT_BY_MAXTS == field && !conf.serve_usecs)
      continue;
    else if (SORT_BY_AVGTS == field && !conf.serve_usecs)
      continue;
    else if (SORT_BY_BW == field && !conf.bandwidth)
      continue;
    else if (SORT_BY_PROT == field && !conf.append_protocol)
      continue;
    else if (SORT_BY_MTHD == field && !conf.append_method)
      continue;
    opts[k++] = field;
    n++;
  }

  win = newwin (h, w, (y - h) / 2, (x - w) / 2);
  keypad (win, TRUE);
  wborder (win, '|', '|', '-', '-', '+', '+', '+', '+');

  /* create a new instance of GMenu and make it selectable */
  menu = new_gmenu (win, SORT_MENU_H, SORT_MENU_W, SORT_MENU_Y, SORT_MENU_X);
  menu->size = n;
  menu->selectable = 1;

  /* add items to GMenu */
  menu->items = (GItem *) xcalloc (n, sizeof (GItem));

  /* set choices, checked option and index */
  for (i = 0; i < n; ++i) {
    GSortField field = sort_choices[module][opts[i]];
    if (SORT_BY_HITS == field) {
      menu->items[i].name = alloc_string ("Hits");
      if (sort->field == SORT_BY_HITS) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    } else if (SORT_BY_VISITORS == field) {
      menu->items[i].name = alloc_string ("Visitors");
      if (sort->field == SORT_BY_VISITORS) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    } else if (SORT_BY_DATA == field) {
      menu->items[i].name = alloc_string ("Data");
      if (sort->field == SORT_BY_DATA) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    } else if (SORT_BY_BW == field) {
      menu->items[i].name = alloc_string ("Bandwidth");
      if (sort->field == SORT_BY_BW) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    } else if (SORT_BY_AVGTS == field) {
      menu->items[i].name = alloc_string ("Avg. Time Served");
      if (sort->field == SORT_BY_AVGTS) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    } else if (SORT_BY_CUMTS == field) {
      menu->items[i].name = alloc_string ("Cum. Time Served");
      if (sort->field == SORT_BY_CUMTS) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    } else if (SORT_BY_MAXTS == field) {
      menu->items[i].name = alloc_string ("Max. Time Served");
      if (sort->field == SORT_BY_MAXTS) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    } else if (SORT_BY_PROT == field) {
      menu->items[i].name = alloc_string ("Protocol");
      if (sort->field == SORT_BY_PROT) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    } else if (SORT_BY_MTHD == field) {
      menu->items[i].name = alloc_string ("Method");
      if (sort->field == SORT_BY_MTHD) {
        menu->items[i].checked = 1;
        menu->idx = i;
      }
    }
  }
  post_gmenu (menu);

  draw_header (win, "Sort active module by", " %s", 1, 1, w2,
               color_panel_header);
  mvwprintw (win, 2, 2, "[ENTER] to select field - [TAB] sort");
  if (sort->sort == SORT_ASC)
    mvwprintw (win, SORT_WIN_H - 2, 1, " %s", SORT_ASC_SEL);
  else
    mvwprintw (win, SORT_WIN_H - 2, 1, " %s", SORT_DESC_SEL);

  wrefresh (win);
  while (quit) {
    c = wgetch (stdscr);
    switch (c) {
    case KEY_DOWN:
      gmenu_driver (menu, REQ_DOWN);
      break;
    case KEY_UP:
      gmenu_driver (menu, REQ_UP);
      break;
    case 9:    /* TAB */
      if (sort->sort == SORT_ASC) {
        /* ascending */
        sort->sort = SORT_DESC;
        mvwprintw (win, SORT_WIN_H - 2, 1, " %s", SORT_DESC_SEL);
      } else {
        /* descending */
        sort->sort = SORT_ASC;
        mvwprintw (win, SORT_WIN_H - 2, 1, " %s", SORT_ASC_SEL);
      }
      break;
    case 32:
    case 0x0a:
    case 0x0d:
    case KEY_ENTER:
      gmenu_driver (menu, REQ_SEL);
      for (i = 0; i < n; ++i) {
        if (menu->items[i].checked != 1)
          continue;
        if (strcmp ("Hits", menu->items[i].name) == 0)
          sort->field = SORT_BY_HITS;
        else if (strcmp ("Visitors", menu->items[i].name) == 0)
          sort->field = SORT_BY_VISITORS;
        else if (strcmp ("Data", menu->items[i].name) == 0)
          sort->field = SORT_BY_DATA;
        else if (strcmp ("Bandwidth", menu->items[i].name) == 0)
          sort->field = SORT_BY_BW;
        else if (strcmp ("Avg. Time Served", menu->items[i].name) == 0)
          sort->field = SORT_BY_AVGTS;
        else if (strcmp ("Cum. Time Served", menu->items[i].name) == 0)
          sort->field = SORT_BY_CUMTS;
        else if (strcmp ("Max. Time Served", menu->items[i].name) == 0)
          sort->field = SORT_BY_MAXTS;
        else if (strcmp ("Protocol", menu->items[i].name) == 0)
          sort->field = SORT_BY_PROT;
        else if (strcmp ("Method", menu->items[i].name) == 0)
          sort->field = SORT_BY_MTHD;
        quit = 0;
        break;
      }
      break;
    case KEY_RESIZE:
    case 'q':
      quit = 0;
      break;
    }
    wrefresh (win);
  }

  /* clean stuff up */
  for (i = 0; i < n; ++i)
    free (menu->items[i].name);
  free (menu->items);
  free (menu);

  touchwin (main_win);
  close_win (win);
  wrefresh (main_win);
}

static const char *help_main[] = {
  "Copyright (C) 2009-2015",
  "by Gerardo Orellana <hello@goaccess.io>",
  "http://goaccess.io",
  "Released under the GNU GPL. See `man` page for more details",
  "",
  "GoAccess is an open source real-time web log analyzer and",
  "interactive viewer that runs in a terminal in *nix systems.",
  "It provides fast and valuable HTTP statistics for system",
  "administrators that require a visual server report on the",
  "fly.",
  "",
  "The data collected based on the parsing of the log is",
  "divided into different modules. Modules are automatically",
  "generated and presented to the user.",
  "",
  "The main dashboard displays general statistics, top",
  "visitors, requests, browsers, operating systems,",
  "hosts, etc.",
  "",
  "The user can make use of the following keys:",
  " ^F1^  or ^h^    Main help",
  " ^F5^            Redraw [main window]",
  " ^q^             Quit the program, current window or module",
  " ^o^ or ^ENTER^  Expand selected module",
  " ^[Shift]0-9^    Set selected module to active",
  " ^Up^ arrow      Scroll up main dashboard",
  " ^Down^ arrow    Scroll down main dashboard",
  " ^j^             Scroll down within expanded module",
  " ^k^             Scroll up within expanded module",
  " ^c^             Set or change scheme color",
  " ^CTRL^ + ^f^    Scroll forward one screen within",
  "                 active module",
  " ^CTRL^ + ^b^    Scroll backward one screen within",
  "                 active module",
  " ^TAB^           Iterate modules (forward)",
  " ^SHIFT^ + ^TAB^ Iterate modules (backward)",
  " ^s^             Sort options for current module",
  " ^/^             Search across all modules",
  " ^n^             Find position of the next occurrence",
  " ^g^             Move to the first item or top of screen",
  " ^G^             Move to the last item or bottom of screen",
  "",
  "Examples can be found by running `man goaccess`.",
  "",
  "If you believe you have found a bug, please drop me",
  "an email with details.",
  "",
  "If you have a medium or high traffic website, it",
  "would be interesting to hear your experience with",
  "GoAccess, such as generating time, visitors or hits.",
  "",
  "Feedback? Just shoot me an email to:",
  "hello@goaccess.io",
};

/* render help dialog */
void
load_help_popup (WINDOW * main_win)
{
  int c, quit = 1;
  size_t i, n;
  int y, x, h = HELP_WIN_HEIGHT, w = HELP_WIN_WIDTH;
  int w2 = w - 2;
  WINDOW *win;
  GMenu *menu;

  n = ARRAY_SIZE (help_main);
  getmaxyx (stdscr, y, x);

  win = newwin (h, w, (y - h) / 2, (x - w) / 2);
  keypad (win, TRUE);
  wborder (win, '|', '|', '-', '-', '+', '+', '+', '+');

  /* create a new instance of GMenu and make it selectable */
  menu =
    new_gmenu (win, HELP_MENU_HEIGHT, HELP_MENU_WIDTH, HELP_MENU_Y,
               HELP_MENU_X);
  menu->size = n;

  /* add items to GMenu */
  menu->items = (GItem *) xcalloc (n, sizeof (GItem));
  for (i = 0; i < n; ++i) {
    menu->items[i].name = alloc_string (help_main[i]);
    menu->items[i].checked = 0;
  }
  post_gmenu (menu);

  draw_header (win, "GoAccess Quick Help", " %s", 1, 1, w2, color_panel_header);
  mvwprintw (win, 2, 2, "[UP/DOWN] to scroll - [q] to quit");

  wrefresh (win);
  while (quit) {
    c = wgetch (stdscr);
    switch (c) {
    case KEY_DOWN:
      gmenu_driver (menu, REQ_DOWN);
      break;
    case KEY_UP:
      gmenu_driver (menu, REQ_UP);
      break;
    case KEY_RESIZE:
    case 'q':
      quit = 0;
      break;
    }
    wrefresh (win);
  }
  /* clean stuff up */
  for (i = 0; i < n; ++i)
    free (menu->items[i].name);
  free (menu->items);
  free (menu);

  touchwin (main_win);
  close_win (win);
  wrefresh (main_win);
}
