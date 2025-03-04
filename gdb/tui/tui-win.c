/* TUI window generic functions.

   Copyright (C) 1998-2019 Free Software Foundation, Inc.

   Contributed by Hewlett-Packard Company.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* This module contains procedures for handling tui window functions
   like resize, scrolling, scrolling, changing focus, etc.

   Author: Susan B. Macchia  */

#include "defs.h"
#include "command.h"
#include "symtab.h"
#include "breakpoint.h"
#include "frame.h"
#include "cli/cli-cmds.h"
#include "top.h"
#include "source.h"
#include "event-loop.h"
#include "gdbcmd.h"

#include "tui/tui.h"
#include "tui/tui-io.h"
#include "tui/tui-command.h"
#include "tui/tui-data.h"
#include "tui/tui-layout.h"
#include "tui/tui-wingeneral.h"
#include "tui/tui-stack.h"
#include "tui/tui-regs.h"
#include "tui/tui-disasm.h"
#include "tui/tui-source.h"
#include "tui/tui-winsource.h"
#include "tui/tui-win.h"

#include "gdb_curses.h"
#include <ctype.h>
#include "readline/readline.h"
#include "gdbsupport/gdb_string_view.h"

#include <signal.h>

static enum tui_status tui_adjust_win_heights (struct tui_win_info *, 
					       int);
static int new_height_ok (struct tui_win_info *, int);
static void tui_set_tab_width_command (const char *, int);
static void tui_refresh_all_command (const char *, int);
static void tui_all_windows_info (const char *, int);
static void tui_scroll_forward_command (const char *, int);
static void tui_scroll_backward_command (const char *, int);
static void tui_scroll_left_command (const char *, int);
static void tui_scroll_right_command (const char *, int);
static void parse_scrolling_args (const char *, 
				  struct tui_win_info **, 
				  int *);


#define WIN_HEIGHT_USAGE    "Usage: winheight WINDOW-NAME [+ | -] NUM-LINES\n"
#define FOCUS_USAGE         "Usage: focus [WINDOW-NAME | next | prev]\n"

#ifndef ACS_LRCORNER
#  define ACS_LRCORNER '+'
#endif
#ifndef ACS_LLCORNER
#  define ACS_LLCORNER '+'
#endif
#ifndef ACS_ULCORNER
#  define ACS_ULCORNER '+'
#endif
#ifndef ACS_URCORNER
#  define ACS_URCORNER '+'
#endif
#ifndef ACS_HLINE
#  define ACS_HLINE '-'
#endif
#ifndef ACS_VLINE
#  define ACS_VLINE '|'
#endif

/* Possible values for tui-border-kind variable.  */
static const char *const tui_border_kind_enums[] = {
  "space",
  "ascii",
  "acs",
  NULL
};

/* Possible values for tui-border-mode and tui-active-border-mode.  */
static const char *const tui_border_mode_enums[] = {
  "normal",
  "standout",
  "reverse",
  "half",
  "half-standout",
  "bold",
  "bold-standout",
  NULL
};

struct tui_translate
{
  const char *name;
  int value;
};

/* Translation table for border-mode variables.
   The list of values must be terminated by a NULL.
   After the NULL value, an entry defines the default.  */
struct tui_translate tui_border_mode_translate[] = {
  { "normal",		A_NORMAL },
  { "standout",		A_STANDOUT },
  { "reverse",		A_REVERSE },
  { "half",		A_DIM },
  { "half-standout",	A_DIM | A_STANDOUT },
  { "bold",		A_BOLD },
  { "bold-standout",	A_BOLD | A_STANDOUT },
  { 0, 0 },
  { "normal",		A_NORMAL }
};

/* Translation tables for border-kind, one for each border
   character (see wborder, border curses operations).
   -1 is used to indicate the ACS because ACS characters
   are determined at run time by curses (depends on terminal).  */
struct tui_translate tui_border_kind_translate_vline[] = {
  { "space",    ' ' },
  { "ascii",    '|' },
  { "acs",      -1 },
  { 0, 0 },
  { "ascii",    '|' }
};

struct tui_translate tui_border_kind_translate_hline[] = {
  { "space",    ' ' },
  { "ascii",    '-' },
  { "acs",      -1 },
  { 0, 0 },
  { "ascii",    '-' }
};

struct tui_translate tui_border_kind_translate_ulcorner[] = {
  { "space",    ' ' },
  { "ascii",    '+' },
  { "acs",      -1 },
  { 0, 0 },
  { "ascii",    '+' }
};

struct tui_translate tui_border_kind_translate_urcorner[] = {
  { "space",    ' ' },
  { "ascii",    '+' },
  { "acs",      -1 },
  { 0, 0 },
  { "ascii",    '+' }
};

struct tui_translate tui_border_kind_translate_llcorner[] = {
  { "space",    ' ' },
  { "ascii",    '+' },
  { "acs",      -1 },
  { 0, 0 },
  { "ascii",    '+' }
};

struct tui_translate tui_border_kind_translate_lrcorner[] = {
  { "space",    ' ' },
  { "ascii",    '+' },
  { "acs",      -1 },
  { 0, 0 },
  { "ascii",    '+' }
};


/* Tui configuration variables controlled with set/show command.  */
const char *tui_active_border_mode = "bold-standout";
static void
show_tui_active_border_mode (struct ui_file *file,
			     int from_tty,
			     struct cmd_list_element *c, 
			     const char *value)
{
  fprintf_filtered (file, _("\
The attribute mode to use for the active TUI window border is \"%s\".\n"),
		    value);
}

const char *tui_border_mode = "normal";
static void
show_tui_border_mode (struct ui_file *file, 
		      int from_tty,
		      struct cmd_list_element *c, 
		      const char *value)
{
  fprintf_filtered (file, _("\
The attribute mode to use for the TUI window borders is \"%s\".\n"),
		    value);
}

const char *tui_border_kind = "acs";
static void
show_tui_border_kind (struct ui_file *file, 
		      int from_tty,
		      struct cmd_list_element *c, 
		      const char *value)
{
  fprintf_filtered (file, _("The kind of border for TUI windows is \"%s\".\n"),
		    value);
}


/* Tui internal configuration variables.  These variables are updated
   by tui_update_variables to reflect the tui configuration
   variables.  */
chtype tui_border_vline;
chtype tui_border_hline;
chtype tui_border_ulcorner;
chtype tui_border_urcorner;
chtype tui_border_llcorner;
chtype tui_border_lrcorner;

int tui_border_attrs;
int tui_active_border_attrs;

/* Identify the item in the translation table.
   When the item is not recognized, use the default entry.  */
static struct tui_translate *
translate (const char *name, struct tui_translate *table)
{
  while (table->name)
    {
      if (name && strcmp (table->name, name) == 0)
        return table;
      table++;
    }

  /* Not found, return default entry.  */
  table++;
  return table;
}

/* Update the tui internal configuration according to gdb settings.
   Returns 1 if the configuration has changed and the screen should
   be redrawn.  */
int
tui_update_variables (void)
{
  int need_redraw = 0;
  struct tui_translate *entry;

  entry = translate (tui_border_mode, tui_border_mode_translate);
  if (tui_border_attrs != entry->value)
    {
      tui_border_attrs = entry->value;
      need_redraw = 1;
    }
  entry = translate (tui_active_border_mode, tui_border_mode_translate);
  if (tui_active_border_attrs != entry->value)
    {
      tui_active_border_attrs = entry->value;
      need_redraw = 1;
    }

  /* If one corner changes, all characters are changed.
     Only check the first one.  The ACS characters are determined at
     run time by curses terminal management.  */
  entry = translate (tui_border_kind, tui_border_kind_translate_lrcorner);
  if (tui_border_lrcorner != (chtype) entry->value)
    {
      tui_border_lrcorner = (entry->value < 0) ? ACS_LRCORNER : entry->value;
      need_redraw = 1;
    }
  entry = translate (tui_border_kind, tui_border_kind_translate_llcorner);
  tui_border_llcorner = (entry->value < 0) ? ACS_LLCORNER : entry->value;

  entry = translate (tui_border_kind, tui_border_kind_translate_ulcorner);
  tui_border_ulcorner = (entry->value < 0) ? ACS_ULCORNER : entry->value;

  entry = translate (tui_border_kind, tui_border_kind_translate_urcorner);
  tui_border_urcorner = (entry->value < 0) ? ACS_URCORNER : entry->value;

  entry = translate (tui_border_kind, tui_border_kind_translate_hline);
  tui_border_hline = (entry->value < 0) ? ACS_HLINE : entry->value;

  entry = translate (tui_border_kind, tui_border_kind_translate_vline);
  tui_border_vline = (entry->value < 0) ? ACS_VLINE : entry->value;

  return need_redraw;
}

static void
set_tui_cmd (const char *args, int from_tty)
{
}

static void
show_tui_cmd (const char *args, int from_tty)
{
}

static struct cmd_list_element *tuilist;

static void
tui_command (const char *args, int from_tty)
{
  printf_unfiltered (_("\"tui\" must be followed by the name of a "
                     "tui command.\n"));
  help_list (tuilist, "tui ", all_commands, gdb_stdout);
}

struct cmd_list_element **
tui_get_cmd_list (void)
{
  if (tuilist == 0)
    add_prefix_cmd ("tui", class_tui, tui_command,
                    _("Text User Interface commands."),
                    &tuilist, "tui ", 0, &cmdlist);
  return &tuilist;
}

/* The set_func hook of "set tui ..." commands that affect the window
   borders on the TUI display.  */
void
tui_set_var_cmd (const char *null_args,
		 int from_tty, struct cmd_list_element *c)
{
  if (tui_update_variables () && tui_active)
    tui_rehighlight_all ();
}



/* True if TUI resizes should print a message.  This is used by the
   test suite.  */

static bool resize_message;

static void
show_tui_resize_message (struct ui_file *file, int from_tty,
			 struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("TUI resize messaging is %s.\n"), value);
}



/* Generic window name completion function.  Complete window name pointed
   to by TEXT and WORD.  If INCLUDE_NEXT_PREV_P is true then the special
   window names 'next' and 'prev' will also be considered as possible
   completions of the window name.  */

static void
window_name_completer (completion_tracker &tracker,
		       int include_next_prev_p,
		       const char *text, const char *word)
{
  std::vector<const char *> completion_name_vec;

  for (tui_win_info *win_info : all_tui_windows ())
    {
      const char *completion_name = NULL;

      /* We can't focus on an invisible window.  */
      if (!win_info->is_visible ())
	continue;

      completion_name = win_info->name ();
      gdb_assert (completion_name != NULL);
      completion_name_vec.push_back (completion_name);
    }

  /* If no windows are considered visible then the TUI has not yet been
     initialized.  But still "focus src" and "focus cmd" will work because
     invoking the focus command will entail initializing the TUI which sets the
     default layout to SRC_COMMAND.  */
  if (completion_name_vec.empty ())
    {
      completion_name_vec.push_back (SRC_NAME);
      completion_name_vec.push_back (CMD_NAME);
    }

  if (include_next_prev_p)
    {
      completion_name_vec.push_back ("next");
      completion_name_vec.push_back ("prev");
    }


  completion_name_vec.push_back (NULL);
  complete_on_enum (tracker, completion_name_vec.data (), text, word);
}

/* Complete possible window names to focus on.  TEXT is the complete text
   entered so far, WORD is the word currently being completed.  */

static void
focus_completer (struct cmd_list_element *ignore,
		 completion_tracker &tracker,
		 const char *text, const char *word)
{
  window_name_completer (tracker, 1, text, word);
}

/* Complete possible window names for winheight command.  TEXT is the
   complete text entered so far, WORD is the word currently being
   completed.  */

static void
winheight_completer (struct cmd_list_element *ignore,
		     completion_tracker &tracker,
		     const char *text, const char *word)
{
  /* The first word is the window name.  That we can complete.  Subsequent
     words can't be completed.  */
  if (word != text)
    return;

  window_name_completer (tracker, 0, text, word);
}

/* Update gdb's knowledge of the terminal size.  */
void
tui_update_gdb_sizes (void)
{
  int width, height;

  if (tui_active)
    {
      width = TUI_CMD_WIN->width;
      height = TUI_CMD_WIN->height;
    }
  else
    {
      width = tui_term_width ();
      height = tui_term_height ();
    }

  set_screen_width_and_height (width, height);
}


/* Set the logical focus to win_info.  */
void
tui_set_win_focus_to (struct tui_win_info *win_info)
{
  if (win_info != NULL)
    {
      struct tui_win_info *win_with_focus = tui_win_with_focus ();

      tui_unhighlight_win (win_with_focus);
      tui_set_win_with_focus (win_info);
      tui_highlight_win (win_info);
    }
}


void
tui_win_info::forward_scroll (int num_to_scroll)
{
  if (num_to_scroll == 0)
    num_to_scroll = height - 3;

  do_scroll_vertical (num_to_scroll);
}

void
tui_win_info::backward_scroll (int num_to_scroll)
{
  if (num_to_scroll == 0)
    num_to_scroll = height - 3;

  do_scroll_vertical (-num_to_scroll);
}


void
tui_win_info::left_scroll (int num_to_scroll)
{
  if (num_to_scroll == 0)
    num_to_scroll = 1;

  do_scroll_horizontal (num_to_scroll);
}


void
tui_win_info::right_scroll (int num_to_scroll)
{
  if (num_to_scroll == 0)
    num_to_scroll = 1;

  do_scroll_horizontal (-num_to_scroll);
}


void
tui_refresh_all_win (void)
{
  clearok (curscr, TRUE);
  tui_refresh_all ();
}

void
tui_rehighlight_all (void)
{
  for (tui_win_info *win_info : all_tui_windows ())
    win_info->check_and_display_highlight_if_needed ();
}

/* Resize all the windows based on the terminal size.  This function
   gets called from within the readline SIGWINCH handler.  */
void
tui_resize_all (void)
{
  int height_diff, width_diff;
  int screenheight, screenwidth;

  rl_get_screen_size (&screenheight, &screenwidth);
  width_diff = screenwidth - tui_term_width ();
  height_diff = screenheight - tui_term_height ();
  if (height_diff || width_diff)
    {
      enum tui_layout_type cur_layout = tui_current_layout ();
      struct tui_win_info *win_with_focus = tui_win_with_focus ();
      struct tui_win_info *first_win;
      struct tui_win_info *second_win;
      tui_source_window_base *src_win;
      struct tui_locator_window *locator = tui_locator_win_info_ptr ();
      int new_height, split_diff, cmd_split_diff, num_wins_displayed = 2;

#ifdef HAVE_RESIZE_TERM
      resize_term (screenheight, screenwidth);
#endif      
      /* Turn keypad off while we resize.  */
      if (win_with_focus != TUI_CMD_WIN)
	keypad (TUI_CMD_WIN->handle.get (), FALSE);
      tui_update_gdb_sizes ();
      tui_set_term_height_to (screenheight);
      tui_set_term_width_to (screenwidth);
      if (cur_layout == SRC_DISASSEM_COMMAND 
	  || cur_layout == SRC_DATA_COMMAND
	  || cur_layout == DISASSEM_DATA_COMMAND)
	num_wins_displayed++;
      split_diff = height_diff / num_wins_displayed;
      cmd_split_diff = split_diff;
      if (height_diff % num_wins_displayed)
	{
	  if (height_diff < 0)
	    cmd_split_diff--;
	  else
           cmd_split_diff++;
       }
      /* Now adjust each window.  */
      /* erase + clearok are used instead of a straightforward clear as
         AIX 5.3 does not define clear.  */
      erase ();
      clearok (curscr, TRUE);
      switch (cur_layout)
       {
	case SRC_COMMAND:
	case DISASSEM_COMMAND:
	  src_win = *(tui_source_windows ().begin ());
	  /* Check for invalid heights.  */
	  if (height_diff == 0)
	    new_height = src_win->height;
	  else if ((src_win->height + split_diff) >=
		   (screenheight - MIN_CMD_WIN_HEIGHT - 1))
	    new_height = screenheight - MIN_CMD_WIN_HEIGHT - 1;
	  else if ((src_win->height + split_diff) <= 0)
	    new_height = MIN_WIN_HEIGHT;
	  else
	    new_height = src_win->height + split_diff;

	  src_win->resize (new_height, screenwidth, 0, 0);

	  locator->resize (1, screenwidth, 0, new_height);

	  new_height = screenheight - (new_height + 1);
	  TUI_CMD_WIN->resize (new_height, screenwidth,
			       0, locator->origin.y + 1);
	  break;
	default:
	  if (cur_layout == SRC_DISASSEM_COMMAND)
	    {
	      src_win = TUI_SRC_WIN;
	      first_win = src_win;
	      second_win = TUI_DISASM_WIN;
	    }
	  else
	    {
	      first_win = TUI_DATA_WIN;
	      src_win = *(tui_source_windows ().begin ());
	      second_win = src_win;
	    }
	  /* Change the first window's height/width.  */
	  /* Check for invalid heights.  */
	  if (height_diff == 0)
	    new_height = first_win->height;
	  else if ((first_win->height +
		    second_win->height + (split_diff * 2)) >=
		   (screenheight - MIN_CMD_WIN_HEIGHT - 1))
	    new_height = (screenheight - MIN_CMD_WIN_HEIGHT - 1) / 2;
	  else if ((first_win->height + split_diff) <= 0)
	    new_height = MIN_WIN_HEIGHT;
	  else
	    new_height = first_win->height + split_diff;

	  first_win->resize (new_height, screenwidth, 0, 0);

	  /* Change the second window's height/width.  */
	  /* Check for invalid heights.  */
	  if (height_diff == 0)
	    new_height = second_win->height;
	  else if ((first_win->height +
		    second_win->height + (split_diff * 2)) >=
		   (screenheight - MIN_CMD_WIN_HEIGHT - 1))
	    {
	      new_height = screenheight - MIN_CMD_WIN_HEIGHT - 1;
	      if (new_height % 2)
		new_height = (new_height / 2) + 1;
	      else
		new_height /= 2;
	    }
	  else if ((second_win->height + split_diff) <= 0)
	    new_height = MIN_WIN_HEIGHT;
	  else
	    new_height = second_win->height + split_diff;

	  second_win->resize (new_height, screenwidth,
			      0, first_win->height - 1);

	  locator->resize (1, screenwidth,
			   0, second_win->origin.y + new_height);

	  /* Change the command window's height/width.  */
	  new_height = screenheight - (locator->origin.y + 1);
	  TUI_CMD_WIN->resize (new_height, screenwidth,
			       0, locator->origin.y + 1);
	  break;
	}

      tui_delete_invisible_windows ();
      /* Turn keypad back on, unless focus is in the command
	 window.  */
      if (win_with_focus != TUI_CMD_WIN)
	keypad (TUI_CMD_WIN->handle.get (), TRUE);
    }
}

#ifdef SIGWINCH
/* Token for use by TUI's asynchronous SIGWINCH handler.  */
static struct async_signal_handler *tui_sigwinch_token;

/* TUI's SIGWINCH signal handler.  */
static void
tui_sigwinch_handler (int signal)
{
  mark_async_signal_handler (tui_sigwinch_token);
  tui_set_win_resized_to (true);
}

/* Callback for asynchronously resizing TUI following a SIGWINCH signal.  */
static void
tui_async_resize_screen (gdb_client_data arg)
{
  rl_resize_terminal ();

  if (!tui_active)
    {
      int screen_height, screen_width;

      rl_get_screen_size (&screen_height, &screen_width);
      set_screen_width_and_height (screen_width, screen_height);

      /* win_resized is left set so that the next call to tui_enable()
	 resizes the TUI windows.  */
    }
  else
    {
      tui_set_win_resized_to (false);
      tui_resize_all ();
      tui_refresh_all_win ();
      tui_update_gdb_sizes ();
      if (resize_message)
	{
	  static int count;
	  printf_unfiltered ("@@ resize done %d, size = %dx%d\n", count,
			     tui_term_width (), tui_term_height ());
	  ++count;
	}
      tui_redisplay_readline ();
    }
}
#endif

/* Initialize TUI's SIGWINCH signal handler.  Note that the handler is not
   uninstalled when we exit TUI, so the handler should not assume that TUI is
   always active.  */
void
tui_initialize_win (void)
{
#ifdef SIGWINCH
  tui_sigwinch_token
    = create_async_signal_handler (tui_async_resize_screen, NULL);

  {
#ifdef HAVE_SIGACTION
    struct sigaction old_winch;

    memset (&old_winch, 0, sizeof (old_winch));
    old_winch.sa_handler = &tui_sigwinch_handler;
#ifdef SA_RESTART
    old_winch.sa_flags = SA_RESTART;
#endif
    sigaction (SIGWINCH, &old_winch, NULL);
#else
    signal (SIGWINCH, &tui_sigwinch_handler);
#endif
  }
#endif
}


static void
tui_scroll_forward_command (const char *arg, int from_tty)
{
  int num_to_scroll = 1;
  struct tui_win_info *win_to_scroll;

  /* Make sure the curses mode is enabled.  */
  tui_enable ();
  if (arg == NULL)
    parse_scrolling_args (arg, &win_to_scroll, NULL);
  else
    parse_scrolling_args (arg, &win_to_scroll, &num_to_scroll);
  win_to_scroll->forward_scroll (num_to_scroll);
}


static void
tui_scroll_backward_command (const char *arg, int from_tty)
{
  int num_to_scroll = 1;
  struct tui_win_info *win_to_scroll;

  /* Make sure the curses mode is enabled.  */
  tui_enable ();
  if (arg == NULL)
    parse_scrolling_args (arg, &win_to_scroll, NULL);
  else
    parse_scrolling_args (arg, &win_to_scroll, &num_to_scroll);
  win_to_scroll->backward_scroll (num_to_scroll);
}


static void
tui_scroll_left_command (const char *arg, int from_tty)
{
  int num_to_scroll;
  struct tui_win_info *win_to_scroll;

  /* Make sure the curses mode is enabled.  */
  tui_enable ();
  parse_scrolling_args (arg, &win_to_scroll, &num_to_scroll);
  win_to_scroll->left_scroll (num_to_scroll);
}


static void
tui_scroll_right_command (const char *arg, int from_tty)
{
  int num_to_scroll;
  struct tui_win_info *win_to_scroll;

  /* Make sure the curses mode is enabled.  */
  tui_enable ();
  parse_scrolling_args (arg, &win_to_scroll, &num_to_scroll);
  win_to_scroll->right_scroll (num_to_scroll);
}


/* Answer the window represented by name.  */
static struct tui_win_info *
tui_partial_win_by_name (gdb::string_view name)
{
  if (name != NULL)
    {
      for (tui_win_info *item : all_tui_windows ())
	{
	  const char *cur_name = item->name ();

	  if (startswith (cur_name, name))
	    return item;
	}
    }

  return NULL;
}

/* Set focus to the window named by 'arg'.  */
static void
tui_set_focus_command (const char *arg, int from_tty)
{
  tui_enable ();

  if (arg != NULL)
    {
      struct tui_win_info *win_info = NULL;

      if (subset_compare (arg, "next"))
	win_info = tui_next_win (tui_win_with_focus ());
      else if (subset_compare (arg, "prev"))
	win_info = tui_prev_win (tui_win_with_focus ());
      else
	win_info = tui_partial_win_by_name (arg);

      if (win_info == NULL)
	error (_("Unrecognized window name \"%s\""), arg);
      if (!win_info->is_visible ())
	error (_("Window \"%s\" is not visible"), arg);

      tui_set_win_focus_to (win_info);
      keypad (TUI_CMD_WIN->handle.get (), win_info != TUI_CMD_WIN);
      printf_filtered (_("Focus set to %s window.\n"),
		       tui_win_with_focus ()->name ());
    }
  else
    error (_("Incorrect Number of Arguments.\n%s"), FOCUS_USAGE);
}

static void
tui_all_windows_info (const char *arg, int from_tty)
{
  struct tui_win_info *win_with_focus = tui_win_with_focus ();
  struct ui_out *uiout = current_uiout;

  ui_out_emit_table table_emitter (uiout, 3, -1, "tui-windows");
  uiout->table_header (10, ui_left, "name", "Name");
  uiout->table_header (5, ui_right, "lines", "Lines");
  uiout->table_header (10, ui_left, "focus", "Focus");
  uiout->table_body ();

  for (tui_win_info *win_info : all_tui_windows ())
    if (win_info->is_visible ())
      {
	ui_out_emit_tuple tuple_emitter (uiout, nullptr);

	uiout->field_string ("name", win_info->name ());
	uiout->field_signed ("lines", win_info->height);
	if (win_with_focus == win_info)
	  uiout->field_string ("focus", _("(has focus)"));
	else
	  uiout->field_skip ("focus");
	uiout->text ("\n");
      }
}


static void
tui_refresh_all_command (const char *arg, int from_tty)
{
  /* Make sure the curses mode is enabled.  */
  tui_enable ();

  tui_refresh_all_win ();
}

/* The tab width that should be used by the TUI.  */

unsigned int tui_tab_width = DEFAULT_TAB_LEN;

/* The tab width as set by the user.  */

static unsigned int internal_tab_width = DEFAULT_TAB_LEN;

/* After the tab width is set, call this to update the relevant
   windows.  */

static void
update_tab_width ()
{
  for (tui_win_info *win_info : all_tui_windows ())
    {
      if (win_info->is_visible ())
	win_info->update_tab_width ();
    }
}

/* Callback for "set tui tab-width".  */

static void
tui_set_tab_width (const char *ignore,
		   int from_tty, struct cmd_list_element *c)
{
  if (internal_tab_width == 0)
    {
      internal_tab_width = tui_tab_width;
      error (_("Tab width must not be 0"));
    }

  tui_tab_width = internal_tab_width;
  update_tab_width ();
}

/* Callback for "show tui tab-width".  */

static void
tui_show_tab_width (struct ui_file *file, int from_tty,
		    struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (gdb_stdout, _("TUI tab width is %s spaces.\n"), value);

}

/* Set the tab width of the specified window.  */
static void
tui_set_tab_width_command (const char *arg, int from_tty)
{
  /* Make sure the curses mode is enabled.  */
  tui_enable ();
  if (arg != NULL)
    {
      int ts;

      ts = atoi (arg);
      if (ts <= 0)
	warning (_("Tab widths greater than 0 must be specified."));
      else
	{
	  internal_tab_width = ts;
	  tui_tab_width = ts;

	  update_tab_width ();
	}
    }
}


/* Set the height of the specified window.  */
static void
tui_set_win_height_command (const char *arg, int from_tty)
{
  /* Make sure the curses mode is enabled.  */
  tui_enable ();
  if (arg != NULL)
    {
      const char *buf = arg;
      const char *buf_ptr = buf;
      int new_height;
      struct tui_win_info *win_info;

      buf_ptr = strchr (buf_ptr, ' ');
      if (buf_ptr != NULL)
	{
	  /* Validate the window name.  */
	  gdb::string_view wname (buf, buf_ptr - buf);
	  win_info = tui_partial_win_by_name (wname);

	  if (win_info == NULL)
	    error (_("Unrecognized window name \"%s\""), arg);
	  if (!win_info->is_visible ())
	    error (_("Window \"%s\" is not visible"), arg);

	  /* Process the size.  */
	  buf_ptr = skip_spaces (buf_ptr);

	  if (*buf_ptr != '\0')
	    {
	      bool negate = false;
	      bool fixed_size = true;
	      int input_no;;

	      if (*buf_ptr == '+' || *buf_ptr == '-')
		{
		  if (*buf_ptr == '-')
		    negate = true;
		  fixed_size = false;
		  buf_ptr++;
		}
	      input_no = atoi (buf_ptr);
	      if (input_no > 0)
		{
		  if (negate)
		    input_no *= (-1);
		  if (fixed_size)
		    new_height = input_no;
		  else
		    new_height = win_info->height + input_no;

		  /* Now change the window's height, and adjust
		     all other windows around it.  */
		  if (tui_adjust_win_heights (win_info,
					      new_height) == TUI_FAILURE)
		    warning (_("Invalid window height specified.\n%s"),
			     WIN_HEIGHT_USAGE);
		  else
		    tui_update_gdb_sizes ();
		}
	      else
		warning (_("Invalid window height specified.\n%s"),
			 WIN_HEIGHT_USAGE);
	    }
	}
      else
	printf_filtered (WIN_HEIGHT_USAGE);
    }
  else
    printf_filtered (WIN_HEIGHT_USAGE);
}

/* Function to adjust all window heights around the primary.   */
static enum tui_status
tui_adjust_win_heights (struct tui_win_info *primary_win_info,
			int new_height)
{
  enum tui_status status = TUI_FAILURE;

  if (new_height_ok (primary_win_info, new_height))
    {
      status = TUI_SUCCESS;
      if (new_height != primary_win_info->height)
	{
	  int diff;
	  struct tui_win_info *win_info;
	  struct tui_locator_window *locator = tui_locator_win_info_ptr ();
	  enum tui_layout_type cur_layout = tui_current_layout ();
	  int width = tui_term_width ();

	  diff = (new_height - primary_win_info->height) * (-1);
	  if (cur_layout == SRC_COMMAND 
	      || cur_layout == DISASSEM_COMMAND)
	    {
	      struct tui_win_info *src_win_info;

	      primary_win_info->resize (new_height, width,
					0, primary_win_info->origin.y);
	      if (primary_win_info->type == CMD_WIN)
		{
		  win_info = *(tui_source_windows ().begin ());
		  src_win_info = win_info;
		}
	      else
		{
		  win_info = tui_win_list[CMD_WIN];
		  src_win_info = primary_win_info;
		}
	      win_info->resize (win_info->height + diff, width,
				0, win_info->origin.y);
	      TUI_CMD_WIN->origin.y = locator->origin.y + 1;
	      if ((src_win_info->type == SRC_WIN
		   || src_win_info->type == DISASSEM_WIN))
		{
		  tui_source_window_base *src_base
		    = (tui_source_window_base *) src_win_info;
		  if (src_base->content.empty ())
		    src_base->erase_source_content ();
		}
	    }
	  else
	    {
	      struct tui_win_info *first_win;
	      struct tui_source_window_base *second_win;
	      tui_source_window_base *src1;

	      if (cur_layout == SRC_DISASSEM_COMMAND)
		{
		  src1 = TUI_SRC_WIN;
		  first_win = src1;
		  second_win = TUI_DISASM_WIN;
		}
	      else
		{
		  src1 = nullptr;
		  first_win = TUI_DATA_WIN;
		  second_win = *(tui_source_windows ().begin ());
		}
	      if (primary_win_info == TUI_CMD_WIN)
		{ /* Split the change in height across the 1st & 2nd
		     windows, adjusting them as well.  */
		  /* Subtract the locator.  */
		  int first_split_diff = diff / 2;
		  int second_split_diff = first_split_diff;

		  if (diff % 2)
		    {
		      if (first_win->height >
			  second_win->height)
			if (diff < 0)
			  first_split_diff--;
			else
			  first_split_diff++;
		      else
			{
			  if (diff < 0)
			    second_split_diff--;
			  else
			    second_split_diff++;
			}
		    }
		  /* Make sure that the minimum heights are
		     honored.  */
		  while ((first_win->height + first_split_diff) < 3)
		    {
		      first_split_diff++;
		      second_split_diff--;
		    }
		  while ((second_win->height + second_split_diff) < 3)
		    {
		      second_split_diff++;
		      first_split_diff--;
		    }
		  first_win->resize (first_win->height + first_split_diff,
				     width,
				     0, first_win->origin.y);
		  second_win->resize (second_win->height + second_split_diff,
				      width,
				      0, first_win->height - 1);
		  locator->resize (1, width,
				   0, (second_win->origin.y
				       + second_win->height + 1));

		  TUI_CMD_WIN->resize (new_height, width,
				       0, locator->origin.y + 1);
		}
	      else
		{
		  if ((TUI_CMD_WIN->height + diff) < 1)
		    { /* If there is no way to increase the command
			 window take real estate from the 1st or 2nd
			 window.  */
		      if ((TUI_CMD_WIN->height + diff) < 1)
			{
			  int i;

			  for (i = TUI_CMD_WIN->height + diff;
			       (i < 1); i++)
			    if (primary_win_info == first_win)
			      second_win->height--;
			    else
			      first_win->height--;
			}
		    }
		  if (primary_win_info == first_win)
		    first_win->resize (new_height, width, 0, 0);
		  else
		    first_win->resize (first_win->height, width, 0, 0);
		  second_win->origin.y = first_win->height - 1;
		  if (primary_win_info == second_win)
		    second_win->resize (new_height, width,
					0, first_win->height - 1);
		  else
		    second_win->resize (second_win->height, width,
					0, first_win->height - 1);
		  locator->resize (1, width,
				   0, (second_win->origin.y
				       + second_win->height + 1));
		  TUI_CMD_WIN->origin.y = locator->origin.y + 1;
		  if ((TUI_CMD_WIN->height + diff) < 1)
		    TUI_CMD_WIN->resize (1, width, 0, locator->origin.y + 1);
		  else
		    TUI_CMD_WIN->resize (TUI_CMD_WIN->height + diff, width,
					 0, locator->origin.y + 1);
		}
	      if (src1 != nullptr && src1->content.empty ())
		src1->erase_source_content ();
	      if (second_win->content.empty ())
		second_win->erase_source_content ();
	    }
	}
    }

  return status;
}

/* See tui-data.h.  */

int
tui_win_info::max_height () const
{
  return tui_term_height () - 2;
}

static int
new_height_ok (struct tui_win_info *primary_win_info, 
	       int new_height)
{
  int ok = (new_height < tui_term_height ());

  if (ok)
    {
      int diff;
      enum tui_layout_type cur_layout = tui_current_layout ();

      diff = (new_height - primary_win_info->height) * (-1);
      if (cur_layout == SRC_COMMAND || cur_layout == DISASSEM_COMMAND)
	{
	  ok = (new_height <= primary_win_info->max_height ()
		&& new_height >= MIN_CMD_WIN_HEIGHT);
	  if (ok)
	    {			/* Check the total height.  */
	      struct tui_win_info *win_info;

	      if (primary_win_info == TUI_CMD_WIN)
		win_info = *(tui_source_windows ().begin ());
	      else
		win_info = TUI_CMD_WIN;
	      ok = ((new_height +
		     (win_info->height + diff)) <= tui_term_height ());
	    }
	}
      else
	{
	  int cur_total_height, total_height, min_height = 0;
	  struct tui_win_info *first_win;
	  struct tui_win_info *second_win;

	  if (cur_layout == SRC_DISASSEM_COMMAND)
	    {
	      first_win = TUI_SRC_WIN;
	      second_win = TUI_DISASM_WIN;
	    }
	  else
	    {
	      first_win = TUI_DATA_WIN;
	      second_win = *(tui_source_windows ().begin ());
	    }
	  /* We could simply add all the heights to obtain the same
	     result but below is more explicit since we subtract 1 for
	     the line that the first and second windows share, and add
	     one for the locator.  */
	  total_height = cur_total_height =
	    (first_win->height + second_win->height - 1)
	    + TUI_CMD_WIN->height + 1;	/* Locator. */
	  if (primary_win_info == TUI_CMD_WIN)
	    {
	      /* Locator included since first & second win share a line.  */
	      ok = ((first_win->height +
		     second_win->height + diff) >=
		    (MIN_WIN_HEIGHT * 2) 
		    && new_height >= MIN_CMD_WIN_HEIGHT);
	      if (ok)
		{
		  total_height = new_height + 
		    (first_win->height +
		     second_win->height + diff);
		  min_height = MIN_CMD_WIN_HEIGHT;
		}
	    }
	  else
	    {
	      min_height = MIN_WIN_HEIGHT;

	      /* First see if we can increase/decrease the command
	         window.  And make sure that the command window is at
	         least 1 line.  */
	      ok = ((TUI_CMD_WIN->height + diff) > 0);
	      if (!ok)
		{ /* Looks like we have to increase/decrease one of
		     the other windows.  */
		  if (primary_win_info == first_win)
		    ok = (second_win->height + diff) >= min_height;
		  else
		    ok = (first_win->height + diff) >= min_height;
		}
	      if (ok)
		{
		  if (primary_win_info == first_win)
		    total_height = new_height +
		      second_win->height +
		      TUI_CMD_WIN->height + diff;
		  else
		    total_height = new_height +
		      first_win->height +
		      TUI_CMD_WIN->height + diff;
		}
	    }
	  /* Now make sure that the proposed total height doesn't
	     exceed the old total height.  */
	  if (ok)
	    ok = (new_height >= min_height 
		  && total_height <= cur_total_height);
	}
    }

  return ok;
}


static void
parse_scrolling_args (const char *arg, 
		      struct tui_win_info **win_to_scroll,
		      int *num_to_scroll)
{
  if (num_to_scroll)
    *num_to_scroll = 0;
  *win_to_scroll = tui_win_with_focus ();

  /* First set up the default window to scroll, in case there is no
     window name arg.  */
  if (arg != NULL)
    {
      char *buf_ptr;

      /* Process the number of lines to scroll.  */
      std::string copy = arg;
      buf_ptr = &copy[0];
      if (isdigit (*buf_ptr))
	{
	  char *num_str;

	  num_str = buf_ptr;
	  buf_ptr = strchr (buf_ptr, ' ');
	  if (buf_ptr != NULL)
	    {
	      *buf_ptr = '\0';
	      if (num_to_scroll)
		*num_to_scroll = atoi (num_str);
	      buf_ptr++;
	    }
	  else if (num_to_scroll)
	    *num_to_scroll = atoi (num_str);
	}

      /* Process the window name if one is specified.  */
      if (buf_ptr != NULL)
	{
	  const char *wname;

	  wname = skip_spaces (buf_ptr);

	  if (*wname != '\0')
	    {
	      *win_to_scroll = tui_partial_win_by_name (wname);

	      if (*win_to_scroll == NULL)
		error (_("Unrecognized window `%s'"), wname);
	      if (!(*win_to_scroll)->is_visible ())
		error (_("Window is not visible"));
	      else if (*win_to_scroll == TUI_CMD_WIN)
		*win_to_scroll = *(tui_source_windows ().begin ());
	    }
	}
    }
}

/* Function to initialize gdb commands, for tui window
   manipulation.  */

void
_initialize_tui_win (void)
{
  static struct cmd_list_element *tui_setlist;
  static struct cmd_list_element *tui_showlist;
  struct cmd_list_element *cmd;

  /* Define the classes of commands.
     They will appear in the help list in the reverse of this order.  */
  add_prefix_cmd ("tui", class_tui, set_tui_cmd,
                  _("TUI configuration variables."),
		  &tui_setlist, "set tui ",
		  0 /* allow-unknown */, &setlist);
  add_prefix_cmd ("tui", class_tui, show_tui_cmd,
                  _("TUI configuration variables."),
		  &tui_showlist, "show tui ",
		  0 /* allow-unknown */, &showlist);

  add_com ("refresh", class_tui, tui_refresh_all_command,
           _("Refresh the terminal display."));

  cmd = add_com ("tabset", class_tui, tui_set_tab_width_command, _("\
Set the width (in characters) of tab stops.\n\
Usage: tabset N"));
  deprecate_cmd (cmd, "set tui tab-width");

  cmd = add_com ("winheight", class_tui, tui_set_win_height_command, _("\
Set or modify the height of a specified window.\n"
WIN_HEIGHT_USAGE
"Window names are:\n\
   src  : the source window\n\
   cmd  : the command window\n\
   asm  : the disassembly window\n\
   regs : the register display"));
  add_com_alias ("wh", "winheight", class_tui, 0);
  set_cmd_completer (cmd, winheight_completer);
  add_info ("win", tui_all_windows_info,
	    _("List of all displayed windows."));
  cmd = add_com ("focus", class_tui, tui_set_focus_command, _("\
Set focus to named window or next/prev window.\n"
FOCUS_USAGE
"Valid Window names are:\n\
   src  : the source window\n\
   asm  : the disassembly window\n\
   regs : the register display\n\
   cmd  : the command window"));
  add_com_alias ("fs", "focus", class_tui, 0);
  set_cmd_completer (cmd, focus_completer);
  add_com ("+", class_tui, tui_scroll_forward_command, _("\
Scroll window forward.\n\
Usage: + [WIN] [N]"));
  add_com ("-", class_tui, tui_scroll_backward_command, _("\
Scroll window backward.\n\
Usage: - [WIN] [N]"));
  add_com ("<", class_tui, tui_scroll_left_command, _("\
Scroll window text to the left.\n\
Usage: < [WIN] [N]"));
  add_com (">", class_tui, tui_scroll_right_command, _("\
Scroll window text to the right.\n\
Usage: > [WIN] [N]"));

  /* Define the tui control variables.  */
  add_setshow_enum_cmd ("border-kind", no_class, tui_border_kind_enums,
			&tui_border_kind, _("\
Set the kind of border for TUI windows."), _("\
Show the kind of border for TUI windows."), _("\
This variable controls the border of TUI windows:\n\
   space           use a white space\n\
   ascii           use ascii characters + - | for the border\n\
   acs             use the Alternate Character Set"),
			tui_set_var_cmd,
			show_tui_border_kind,
			&tui_setlist, &tui_showlist);

  add_setshow_enum_cmd ("border-mode", no_class, tui_border_mode_enums,
			&tui_border_mode, _("\
Set the attribute mode to use for the TUI window borders."), _("\
Show the attribute mode to use for the TUI window borders."), _("\
This variable controls the attributes to use for the window borders:\n\
   normal          normal display\n\
   standout        use highlight mode of terminal\n\
   reverse         use reverse video mode\n\
   half            use half bright\n\
   half-standout   use half bright and standout mode\n\
   bold            use extra bright or bold\n\
   bold-standout   use extra bright or bold with standout mode"),
			tui_set_var_cmd,
			show_tui_border_mode,
			&tui_setlist, &tui_showlist);

  add_setshow_enum_cmd ("active-border-mode", no_class, tui_border_mode_enums,
			&tui_active_border_mode, _("\
Set the attribute mode to use for the active TUI window border."), _("\
Show the attribute mode to use for the active TUI window border."), _("\
This variable controls the attributes to use for the active window border:\n\
   normal          normal display\n\
   standout        use highlight mode of terminal\n\
   reverse         use reverse video mode\n\
   half            use half bright\n\
   half-standout   use half bright and standout mode\n\
   bold            use extra bright or bold\n\
   bold-standout   use extra bright or bold with standout mode"),
			tui_set_var_cmd,
			show_tui_active_border_mode,
			&tui_setlist, &tui_showlist);

  add_setshow_zuinteger_cmd ("tab-width", no_class,
			     &internal_tab_width, _("\
Set the tab width, in characters, for the TUI."), _("\
Show the tab witdh, in characters, for the TUI."), _("\
This variable controls how many spaces are used to display a tab character."),
			     tui_set_tab_width, tui_show_tab_width,
			     &tui_setlist, &tui_showlist);

  add_setshow_boolean_cmd ("tui-resize-message", class_maintenance,
			   &resize_message, _("\
Set TUI resize messaging."), _("\
Show TUI resize messaging."), _("\
When enabled GDB will print a message when the terminal is resized."),
			   nullptr,
			   show_tui_resize_message,
			   &maintenance_set_cmdlist,
			   &maintenance_show_cmdlist);
}
