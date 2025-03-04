/* General window behavior.

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

#include "defs.h"
#include "tui/tui.h"
#include "tui/tui-data.h"
#include "tui/tui-wingeneral.h"
#include "tui/tui-win.h"
#include "tui/tui-stack.h"

#include "gdb_curses.h"

/* See tui-data.h.  */

void
tui_gen_win_info::refresh_window ()
{
  if (handle != NULL)
    wrefresh (handle.get ());
}

/* Draw a border arround the window.  */
static void
box_win (struct tui_win_info *win_info, 
	 bool highlight_flag)
{
  WINDOW *win;
  int attrs;

  win = win_info->handle.get ();
  if (highlight_flag)
    attrs = tui_active_border_attrs;
  else
    attrs = tui_border_attrs;

  wattron (win, attrs);
#ifdef HAVE_WBORDER
  wborder (win, tui_border_vline, tui_border_vline,
	   tui_border_hline, tui_border_hline,
	   tui_border_ulcorner, tui_border_urcorner,
	   tui_border_llcorner, tui_border_lrcorner);
#else
  box (win, tui_border_vline, tui_border_hline);
#endif
  if (!win_info->title.empty ())
    {
      /* Emit "+-TITLE-+" -- so 2 characters on the right and 2 on
	 the left.  */
      int max_len = win_info->width - 2 - 2;

      if (win_info->title.size () <= max_len)
	mvwaddstr (win, 0, 3, win_info->title.c_str ());
      else
	{
	  std::string truncated
	    = "..." + win_info->title.substr (win_info->title.size ()
					      - max_len + 3);
	  mvwaddstr (win, 0, 3, truncated.c_str ());
	}
    }
  wattroff (win, attrs);
}


void
tui_unhighlight_win (struct tui_win_info *win_info)
{
  if (win_info != NULL 
      && win_info->can_box ()
      && win_info->handle != NULL)
    {
      box_win (win_info, false);
      win_info->refresh_window ();
      win_info->set_highlight (false);
    }
}


void
tui_highlight_win (struct tui_win_info *win_info)
{
  if (win_info != NULL
      && win_info->can_box ()
      && win_info->handle != NULL)
    {
      box_win (win_info, true);
      win_info->refresh_window ();
      win_info->set_highlight (true);
    }
}

void
tui_win_info::check_and_display_highlight_if_needed ()
{
  if (can_box ())
    {
      if (is_highlighted)
	tui_highlight_win (this);
      else
	tui_unhighlight_win (this);
    }
}


void
tui_gen_win_info::make_window ()
{
  handle.reset (newwin (height, width, origin.y, origin.x));
  if (handle != NULL)
    scrollok (handle.get (), TRUE);
}

void
tui_win_info::make_window ()
{
  tui_gen_win_info::make_window ();
  if (handle != NULL && can_box ())
    box_win (this, false);
}

/* We can't really make windows visible, or invisible.  So we have to
   delete the entire window when making it visible, and create it
   again when making it visible.  */
void
tui_gen_win_info::make_visible (bool visible)
{
  if (is_visible () == visible)
    return;

  if (visible)
    make_window ();
  else
    handle.reset (nullptr);
}

/* See tui-wingeneral.h.  */

void
tui_make_all_invisible (void)
{
  for (tui_win_info *win_info : all_tui_windows ())
    win_info->make_visible (false);
}

/* Function to refresh all the windows currently displayed.  */

void
tui_refresh_all ()
{
  struct tui_locator_window *locator = tui_locator_win_info_ptr ();

  for (tui_win_info *win_info : all_tui_windows ())
    {
      if (win_info->is_visible ())
	win_info->refresh_window ();
    }
  if (locator->is_visible ())
    locator->refresh_window ();
}
