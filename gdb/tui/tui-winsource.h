/* TUI display source/assembly window.

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

#ifndef TUI_TUI_WINSOURCE_H
#define TUI_TUI_WINSOURCE_H

#include "tui/tui-data.h"
#include "symtab.h"

/* Flags to tell what kind of breakpoint is at current line.  */
enum tui_bp_flag
{
  TUI_BP_ENABLED = 0x01,
  TUI_BP_DISABLED = 0x02,
  TUI_BP_HIT = 0x04,
  TUI_BP_CONDITIONAL = 0x08,
  TUI_BP_HARDWARE = 0x10
};

DEF_ENUM_FLAGS_TYPE (enum tui_bp_flag, tui_bp_flags);

/* Position of breakpoint markers in the exec info string.  */
#define TUI_BP_HIT_POS      0
#define TUI_BP_BREAK_POS    1
#define TUI_EXEC_POS        2
#define TUI_EXECINFO_SIZE   4

/* Elements in the Source/Disassembly Window.  */
struct tui_source_element
{
  tui_source_element ()
  {
    line_or_addr.loa = LOA_LINE;
    line_or_addr.u.line_no = 0;
  }

  DISABLE_COPY_AND_ASSIGN (tui_source_element);

  tui_source_element (tui_source_element &&other)
    : line (std::move (other.line)),
      line_or_addr (other.line_or_addr),
      is_exec_point (other.is_exec_point),
      break_mode (other.break_mode)
  {
  }

  std::string line;
  struct tui_line_or_address line_or_addr;
  bool is_exec_point = false;
  tui_bp_flags break_mode = 0;
};


/* The base class for all source-like windows, namely the source and
   disassembly windows.  */

struct tui_source_window_base : public tui_win_info
{
protected:
  explicit tui_source_window_base (enum tui_win_type type);
  ~tui_source_window_base ();

  DISABLE_COPY_AND_ASSIGN (tui_source_window_base);

  void do_scroll_horizontal (int num_to_scroll) override;

  /* Erase the content and display STRING.  */
  void do_erase_source_content (const char *string);

  void rerender () override;

  virtual enum tui_status set_contents
    (struct gdbarch *gdbarch,
     struct symtab *s,
     struct tui_line_or_address line_or_addr) = 0;

public:

  /* Refill the source window's source cache and update it.  If this
     is a disassembly window, then just update it.  */
  void refill ();

  /* Set the location of the execution point.  */
  void set_is_exec_point_at (struct tui_line_or_address l);

  void update_tab_width () override;

  virtual bool location_matches_p (struct bp_location *loc, int line_no) = 0;

  void update_exec_info ();

  /* Update the window to display the given location.  Does nothing if
     the location is already displayed.  */
  virtual void maybe_update (struct frame_info *fi, symtab_and_line sal,
			     int line_no, CORE_ADDR addr) = 0;

  void update_source_window_as_is  (struct gdbarch *gdbarch,
				    struct symtab *s,
				    struct tui_line_or_address line_or_addr);
  void update_source_window (struct gdbarch *gdbarch,
			     struct symtab *s,
			     struct tui_line_or_address line_or_addr);

  /* Scan the source window and the breakpoints to update the
     break_mode information for each line.  Returns true if something
     changed and the execution window must be refreshed.  See
     tui_update_all_breakpoint_info for a description of
     BEING_DELETED.  */
  bool update_breakpoint_info (struct breakpoint *being_deleted,
			       bool current_only);

  /* Erase the source content.  */
  virtual void erase_source_content () = 0;

  /* Used for horizontal scroll.  */
  int horizontal_offset = 0;
  struct tui_line_or_address start_line_or_addr;

  /* Architecture associated with code at this location.  */
  struct gdbarch *gdbarch = nullptr;

  std::vector<tui_source_element> content;

private:

  void show_source_content ();

  /* Called when the user "set style enabled" setting is changed.  */
  void style_changed ();

  /* A token used to register and unregister an observer.  */
  gdb::observers::token m_observable;
};


/* A wrapper for a TUI window iterator that only iterates over source
   windows.  */

struct tui_source_window_iterator
{
public:

  typedef tui_source_window_iterator self_type;
  typedef struct tui_source_window_base *value_type;
  typedef struct tui_source_window_base *&reference;
  typedef struct tui_source_window_base **pointer;
  typedef std::forward_iterator_tag iterator_category;
  typedef int difference_type;

  explicit tui_source_window_iterator (bool dummy)
    : m_iter (SRC_WIN)
  {
    advance ();
  }

  tui_source_window_iterator ()
    : m_iter (tui_win_type (DISASSEM_WIN + 1))
  {
  }

  bool operator!= (const self_type &other) const
  {
    return m_iter != other.m_iter;
  }

  value_type operator* () const
  {
    return (value_type) *m_iter;
  }

  self_type &operator++ ()
  {
    ++m_iter;
    advance ();
    return *this;
  }

private:

  void advance ()
  {
    tui_window_iterator end;
    while (m_iter != end && *m_iter == nullptr)
      ++m_iter;
  }

  tui_window_iterator m_iter;
};

/* A range adapter for source windows.  */

struct tui_source_windows
{
  tui_source_window_iterator begin () const
  {
    return tui_source_window_iterator (true);
  }

  tui_source_window_iterator end () const
  {
    return tui_source_window_iterator ();
  }
};

/* Update the execution windows to show the active breakpoints.  This
   is called whenever a breakpoint is inserted, removed or has its
   state changed.  Normally BEING_DELETED is nullptr; if not nullptr,
   it indicates a breakpoint that is in the process of being deleted,
   and which should therefore be ignored by the update.  This is done
   because the relevant observer is notified before the breakpoint is
   removed from the list of breakpoints.  */
extern void tui_update_all_breakpoint_info (struct breakpoint *being_deleted);

/* Function to display the "main" routine.  */
extern void tui_display_main (void);
extern void tui_update_source_windows_with_addr (struct gdbarch *, CORE_ADDR);
extern void tui_update_source_windows_with_line (struct symtab *, 
						 int);

/* Extract some source text from PTR.  LINE_NO is the line number.  If
   it is positive, it is printed at the start of the line.  FIRST_COL
   is the first column to extract, and LINE_WIDTH is the number of
   characters to display.  Returns a string holding the desired text.
   PTR is updated to point to the start of the next line.  */

extern std::string tui_copy_source_line (const char **ptr,
					 int line_no, int first_col,
					 int line_width);

/* Constant definitions. */
#define SCROLL_THRESHOLD 2	/* Threshold for lazy scroll.  */

#endif /* TUI_TUI_WINSOURCE_H */
