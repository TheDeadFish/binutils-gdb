/* Symbol table definitions for GDB.

   Copyright (C) 1986-2019 Free Software Foundation, Inc.

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

#if !defined (SYMTAB_H)
#define SYMTAB_H 1

#include <array>
#include <vector>
#include <string>
#include "gdbsupport/gdb_vecs.h"
#include "gdbtypes.h"
#include "gdb_obstack.h"
#include "gdb_regex.h"
#include "gdbsupport/enum-flags.h"
#include "gdbsupport/function-view.h"
#include "gdbsupport/gdb_optional.h"
#include "gdbsupport/gdb_string_view.h"
#include "gdbsupport/next-iterator.h"
#include "completer.h"
#include "gdb-demangle.h"

/* Opaque declarations.  */
struct ui_file;
struct frame_info;
struct symbol;
struct obstack;
struct objfile;
struct block;
struct blockvector;
struct axs_value;
struct agent_expr;
struct program_space;
struct language_defn;
struct common_block;
struct obj_section;
struct cmd_list_element;
class probe;
struct lookup_name_info;

/* How to match a lookup name against a symbol search name.  */
enum class symbol_name_match_type
{
  /* Wild matching.  Matches unqualified symbol names in all
     namespace/module/packages, etc.  */
  WILD,

  /* Full matching.  The lookup name indicates a fully-qualified name,
     and only matches symbol search names in the specified
     namespace/module/package.  */
  FULL,

  /* Search name matching.  This is like FULL, but the search name did
     not come from the user; instead it is already a search name
     retrieved from a search_name () call.
     For Ada, this avoids re-encoding an already-encoded search name
     (which would potentially incorrectly lowercase letters in the
     linkage/search name that should remain uppercase).  For C++, it
     avoids trying to demangle a name we already know is
     demangled.  */
  SEARCH_NAME,

  /* Expression matching.  The same as FULL matching in most
     languages.  The same as WILD matching in Ada.  */
  EXPRESSION,
};

/* Hash the given symbol search name according to LANGUAGE's
   rules.  */
extern unsigned int search_name_hash (enum language language,
				      const char *search_name);

/* Ada-specific bits of a lookup_name_info object.  This is lazily
   constructed on demand.  */

class ada_lookup_name_info final
{
 public:
  /* Construct.  */
  explicit ada_lookup_name_info (const lookup_name_info &lookup_name);

  /* Compare SYMBOL_SEARCH_NAME with our lookup name, using MATCH_TYPE
     as name match type.  Returns true if there's a match, false
     otherwise.  If non-NULL, store the matching results in MATCH.  */
  bool matches (const char *symbol_search_name,
		symbol_name_match_type match_type,
		completion_match_result *comp_match_res) const;

  /* The Ada-encoded lookup name.  */
  const std::string &lookup_name () const
  { return m_encoded_name; }

  /* Return true if we're supposed to be doing a wild match look
     up.  */
  bool wild_match_p () const
  { return m_wild_match_p; }

  /* Return true if we're looking up a name inside package
     Standard.  */
  bool standard_p () const
  { return m_standard_p; }

  /* Return true if doing a verbatim match.  */
  bool verbatim_p () const
  { return m_verbatim_p; }

private:
  /* The Ada-encoded lookup name.  */
  std::string m_encoded_name;

  /* Whether the user-provided lookup name was Ada encoded.  If so,
     then return encoded names in the 'matches' method's 'completion
     match result' output.  */
  bool m_encoded_p : 1;

  /* True if really doing wild matching.  Even if the user requests
     wild matching, some cases require full matching.  */
  bool m_wild_match_p : 1;

  /* True if doing a verbatim match.  This is true if the decoded
     version of the symbol name is wrapped in '<'/'>'.  This is an
     escape hatch users can use to look up symbols the Ada encoding
     does not understand.  */
  bool m_verbatim_p : 1;

   /* True if the user specified a symbol name that is inside package
      Standard.  Symbol names inside package Standard are handled
      specially.  We always do a non-wild match of the symbol name
      without the "standard__" prefix, and only search static and
      global symbols.  This was primarily introduced in order to allow
      the user to specifically access the standard exceptions using,
      for instance, Standard.Constraint_Error when Constraint_Error is
      ambiguous (due to the user defining its own Constraint_Error
      entity inside its program).  */
  bool m_standard_p : 1;
};

/* Language-specific bits of a lookup_name_info object, for languages
   that do name searching using demangled names (C++/D/Go).  This is
   lazily constructed on demand.  */

struct demangle_for_lookup_info final
{
public:
  demangle_for_lookup_info (const lookup_name_info &lookup_name,
			    language lang);

  /* The demangled lookup name.  */
  const std::string &lookup_name () const
  { return m_demangled_name; }

private:
  /* The demangled lookup name.  */
  std::string m_demangled_name;
};

/* Object that aggregates all information related to a symbol lookup
   name.  I.e., the name that is matched against the symbol's search
   name.  Caches per-language information so that it doesn't require
   recomputing it for every symbol comparison, like for example the
   Ada encoded name and the symbol's name hash for a given language.
   The object is conceptually immutable once constructed, and thus has
   no setters.  This is to prevent some code path from tweaking some
   property of the lookup name for some local reason and accidentally
   altering the results of any continuing search(es).
   lookup_name_info objects are generally passed around as a const
   reference to reinforce that.  (They're not passed around by value
   because they're not small.)  */
class lookup_name_info final
{
 public:
  /* Create a new object.  */
  lookup_name_info (std::string name,
		    symbol_name_match_type match_type,
		    bool completion_mode = false,
		    bool ignore_parameters = false)
    : m_match_type (match_type),
      m_completion_mode (completion_mode),
      m_ignore_parameters (ignore_parameters),
      m_name (std::move (name))
  {}

  /* Getters.  See description of each corresponding field.  */
  symbol_name_match_type match_type () const { return m_match_type; }
  bool completion_mode () const { return m_completion_mode; }
  const std::string &name () const { return m_name; }
  const bool ignore_parameters () const { return m_ignore_parameters; }

  /* Return a version of this lookup name that is usable with
     comparisons against symbols have no parameter info, such as
     psymbols and GDB index symbols.  */
  lookup_name_info make_ignore_params () const
  {
    return lookup_name_info (m_name, m_match_type, m_completion_mode,
			     true /* ignore params */);
  }

  /* Get the search name hash for searches in language LANG.  */
  unsigned int search_name_hash (language lang) const
  {
    /* Only compute each language's hash once.  */
    if (!m_demangled_hashes_p[lang])
      {
	m_demangled_hashes[lang]
	  = ::search_name_hash (lang, language_lookup_name (lang).c_str ());
	m_demangled_hashes_p[lang] = true;
      }
    return m_demangled_hashes[lang];
  }

  /* Get the search name for searches in language LANG.  */
  const std::string &language_lookup_name (language lang) const
  {
    switch (lang)
      {
      case language_ada:
	return ada ().lookup_name ();
      case language_cplus:
	return cplus ().lookup_name ();
      case language_d:
	return d ().lookup_name ();
      case language_go:
	return go ().lookup_name ();
      default:
	return m_name;
      }
  }

  /* Get the Ada-specific lookup info.  */
  const ada_lookup_name_info &ada () const
  {
    maybe_init (m_ada);
    return *m_ada;
  }

  /* Get the C++-specific lookup info.  */
  const demangle_for_lookup_info &cplus () const
  {
    maybe_init (m_cplus, language_cplus);
    return *m_cplus;
  }

  /* Get the D-specific lookup info.  */
  const demangle_for_lookup_info &d () const
  {
    maybe_init (m_d, language_d);
    return *m_d;
  }

  /* Get the Go-specific lookup info.  */
  const demangle_for_lookup_info &go () const
  {
    maybe_init (m_go, language_go);
    return *m_go;
  }

  /* Get a reference to a lookup_name_info object that matches any
     symbol name.  */
  static const lookup_name_info &match_any ();

private:
  /* Initialize FIELD, if not initialized yet.  */
  template<typename Field, typename... Args>
  void maybe_init (Field &field, Args&&... args) const
  {
    if (!field)
      field.emplace (*this, std::forward<Args> (args)...);
  }

  /* The lookup info as passed to the ctor.  */
  symbol_name_match_type m_match_type;
  bool m_completion_mode;
  bool m_ignore_parameters;
  std::string m_name;

  /* Language-specific info.  These fields are filled lazily the first
     time a lookup is done in the corresponding language.  They're
     mutable because lookup_name_info objects are typically passed
     around by const reference (see intro), and they're conceptually
     "cache" that can always be reconstructed from the non-mutable
     fields.  */
  mutable gdb::optional<ada_lookup_name_info> m_ada;
  mutable gdb::optional<demangle_for_lookup_info> m_cplus;
  mutable gdb::optional<demangle_for_lookup_info> m_d;
  mutable gdb::optional<demangle_for_lookup_info> m_go;

  /* The demangled hashes.  Stored in an array with one entry for each
     possible language.  The second array records whether we've
     already computed the each language's hash.  (These are separate
     arrays instead of a single array of optional<unsigned> to avoid
     alignment padding).  */
  mutable std::array<unsigned int, nr_languages> m_demangled_hashes;
  mutable std::array<bool, nr_languages> m_demangled_hashes_p {};
};

/* Comparison function for completion symbol lookup.

   Returns true if the symbol name matches against LOOKUP_NAME.

   SYMBOL_SEARCH_NAME should be a symbol's "search" name.

   On success and if non-NULL, COMP_MATCH_RES->match is set to point
   to the symbol name as should be presented to the user as a
   completion match list element.  In most languages, this is the same
   as the symbol's search name, but in some, like Ada, the display
   name is dynamically computed within the comparison routine.

   Also, on success and if non-NULL, COMP_MATCH_RES->match_for_lcd
   points the part of SYMBOL_SEARCH_NAME that was considered to match
   LOOKUP_NAME.  E.g., in C++, in linespec/wild mode, if the symbol is
   "foo::function()" and LOOKUP_NAME is "function(", MATCH_FOR_LCD
   points to "function()" inside SYMBOL_SEARCH_NAME.  */
typedef bool (symbol_name_matcher_ftype)
  (const char *symbol_search_name,
   const lookup_name_info &lookup_name,
   completion_match_result *comp_match_res);

/* Some of the structures in this file are space critical.
   The space-critical structures are:

     struct general_symbol_info
     struct symbol
     struct partial_symbol

   These structures are laid out to encourage good packing.
   They use ENUM_BITFIELD and short int fields, and they order the
   structure members so that fields less than a word are next
   to each other so they can be packed together.  */

/* Rearranged: used ENUM_BITFIELD and rearranged field order in
   all the space critical structures (plus struct minimal_symbol).
   Memory usage dropped from 99360768 bytes to 90001408 bytes.
   I measured this with before-and-after tests of
   "HEAD-old-gdb -readnow HEAD-old-gdb" and
   "HEAD-new-gdb -readnow HEAD-old-gdb" on native i686-pc-linux-gnu,
   red hat linux 8, with LD_LIBRARY_PATH=/usr/lib/debug,
   typing "maint space 1" at the first command prompt.

   Here is another measurement (from andrew c):
     # no /usr/lib/debug, just plain glibc, like a normal user
     gdb HEAD-old-gdb
     (gdb) break internal_error
     (gdb) run
     (gdb) maint internal-error
     (gdb) backtrace
     (gdb) maint space 1

   gdb gdb_6_0_branch  2003-08-19  space used: 8896512
   gdb HEAD            2003-08-19  space used: 8904704
   gdb HEAD            2003-08-21  space used: 8396800 (+symtab.h)
   gdb HEAD            2003-08-21  space used: 8265728 (+gdbtypes.h)

   The third line shows the savings from the optimizations in symtab.h.
   The fourth line shows the savings from the optimizations in
   gdbtypes.h.  Both optimizations are in gdb HEAD now.

   --chastain 2003-08-21  */

/* Define a structure for the information that is common to all symbol types,
   including minimal symbols, partial symbols, and full symbols.  In a
   multilanguage environment, some language specific information may need to
   be recorded along with each symbol.  */

/* This structure is space critical.  See space comments at the top.  */

struct general_symbol_info
{
  /* Short version as to when to use which name accessor:
     Use natural_name () to refer to the name of the symbol in the original
     source code.  Use linkage_name () if you want to know what the linker
     thinks the symbol's name is.  Use print_name () for output.  Use
     demangled_name () if you specifically need to know whether natural_name ()
     and linkage_name () are different.  */

  const char *linkage_name () const
  { return name; }

  /* Return SYMBOL's "natural" name, i.e. the name that it was called in
     the original source code.  In languages like C++ where symbols may
     be mangled for ease of manipulation by the linker, this is the
     demangled name.  */
  const char *natural_name () const;

  /* Returns a version of the name of a symbol that is
     suitable for output.  In C++ this is the "demangled" form of the
     name if demangle is on and the "mangled" form of the name if
     demangle is off.  In other languages this is just the symbol name.
     The result should never be NULL.  Don't use this for internal
     purposes (e.g. storing in a hashtable): it's only suitable for output.  */
  const char *print_name () const
  { return demangle ? natural_name () : linkage_name (); }

  /* Return the demangled name for a symbol based on the language for
     that symbol.  If no demangled name exists, return NULL.  */
  const char *demangled_name () const;

  /* Returns the name to be used when sorting and searching symbols.
     In C++, we search for the demangled form of a name,
     and so sort symbols accordingly.  In Ada, however, we search by mangled
     name.  If there is no distinct demangled name, then this
     returns the same value (same pointer) as linkage_name ().  */
  const char *search_name () const;

  /* Name of the symbol.  This is a required field.  Storage for the
     name is allocated on the objfile_obstack for the associated
     objfile.  For languages like C++ that make a distinction between
     the mangled name and demangled name, this is the mangled
     name.  */

  const char *name;

  /* Value of the symbol.  Which member of this union to use, and what
     it means, depends on what kind of symbol this is and its
     SYMBOL_CLASS.  See comments there for more details.  All of these
     are in host byte order (though what they point to might be in
     target byte order, e.g. LOC_CONST_BYTES).  */

  union
  {
    LONGEST ivalue;

    const struct block *block;

    const gdb_byte *bytes;

    CORE_ADDR address;

    /* A common block.  Used with LOC_COMMON_BLOCK.  */

    const struct common_block *common_block;

    /* For opaque typedef struct chain.  */

    struct symbol *chain;
  }
  value;

  /* Since one and only one language can apply, wrap the language specific
     information inside a union.  */

  union
  {
    /* A pointer to an obstack that can be used for storage associated
       with this symbol.  This is only used by Ada, and only when the
       'ada_mangled' field is zero.  */
    struct obstack *obstack;

    /* This is used by languages which wish to store a demangled name.
       currently used by Ada, C++, and Objective C.  */
    const char *demangled_name;
  }
  language_specific;

  /* Record the source code language that applies to this symbol.
     This is used to select one of the fields from the language specific
     union above.  */

  ENUM_BITFIELD(language) language : LANGUAGE_BITS;

  /* This is only used by Ada.  If set, then the 'demangled_name' field
     of language_specific is valid.  Otherwise, the 'obstack' field is
     valid.  */
  unsigned int ada_mangled : 1;

  /* Which section is this symbol in?  This is an index into
     section_offsets for this objfile.  Negative means that the symbol
     does not get relocated relative to a section.  */

  short section;
};

extern void symbol_set_demangled_name (struct general_symbol_info *,
				       const char *,
                                       struct obstack *);

extern const char *symbol_get_demangled_name
  (const struct general_symbol_info *);

extern CORE_ADDR symbol_overlayed_address (CORE_ADDR, struct obj_section *);

/* Return the address of SYM.  The MAYBE_COPIED flag must be set on
   SYM.  If SYM appears in the main program's minimal symbols, then
   that minsym's address is returned; otherwise, SYM's address is
   returned.  This should generally only be used via the
   SYMBOL_VALUE_ADDRESS macro.  */

extern CORE_ADDR get_symbol_address (const struct symbol *sym);

/* Note that these macros only work with symbol, not partial_symbol.  */

#define SYMBOL_VALUE(symbol)		(symbol)->value.ivalue
#define SYMBOL_VALUE_ADDRESS(symbol)			      \
  (((symbol)->maybe_copied) ? get_symbol_address (symbol)     \
   : ((symbol)->value.address))
#define SET_SYMBOL_VALUE_ADDRESS(symbol, new_value)	\
  ((symbol)->value.address = (new_value))
#define SYMBOL_VALUE_BYTES(symbol)	(symbol)->value.bytes
#define SYMBOL_VALUE_COMMON_BLOCK(symbol) (symbol)->value.common_block
#define SYMBOL_BLOCK_VALUE(symbol)	(symbol)->value.block
#define SYMBOL_VALUE_CHAIN(symbol)	(symbol)->value.chain
#define SYMBOL_LANGUAGE(symbol)		(symbol)->language
#define SYMBOL_SECTION(symbol)		(symbol)->section
#define SYMBOL_OBJ_SECTION(objfile, symbol)			\
  (((symbol)->section >= 0)				\
   ? (&(((objfile)->sections)[(symbol)->section]))	\
   : NULL)

/* Initializes the language dependent portion of a symbol
   depending upon the language for the symbol.  */
#define SYMBOL_SET_LANGUAGE(symbol,language,obstack)	\
  (symbol_set_language ((symbol), (language), (obstack)))
extern void symbol_set_language (struct general_symbol_info *symbol,
                                 enum language language,
				 struct obstack *obstack);

/* Set just the linkage name of a symbol; do not try to demangle
   it.  Used for constructs which do not have a mangled name,
   e.g. struct tags.  Unlike SYMBOL_SET_NAMES, linkage_name must
   be terminated and either already on the objfile's obstack or
   permanently allocated.  */
#define SYMBOL_SET_LINKAGE_NAME(symbol,linkage_name) \
  (symbol)->name = (linkage_name)

/* Set the linkage and natural names of a symbol, by demangling
   the linkage name.  If linkage_name may not be nullterminated,
   copy_name must be set to true.  */
#define SYMBOL_SET_NAMES(symbol,linkage_name,copy_name,objfile)	\
  symbol_set_names ((symbol), linkage_name, copy_name, \
		    (objfile)->per_bfd)
extern void symbol_set_names (struct general_symbol_info *symbol,
			      gdb::string_view linkage_name, bool copy_name,
			      struct objfile_per_bfd_storage *per_bfd);

/* Return true if NAME matches the "search" name of SYMBOL, according
   to the symbol's language.  */
#define SYMBOL_MATCHES_SEARCH_NAME(symbol, name)                       \
  symbol_matches_search_name ((symbol), (name))

/* Helper for SYMBOL_MATCHES_SEARCH_NAME that works with both symbols
   and psymbols.  */
extern bool symbol_matches_search_name
  (const struct general_symbol_info *gsymbol,
   const lookup_name_info &name);

/* Compute the hash of the given symbol search name of a symbol of
   language LANGUAGE.  */
extern unsigned int search_name_hash (enum language language,
				      const char *search_name);

/* Classification types for a minimal symbol.  These should be taken as
   "advisory only", since if gdb can't easily figure out a
   classification it simply selects mst_unknown.  It may also have to
   guess when it can't figure out which is a better match between two
   types (mst_data versus mst_bss) for example.  Since the minimal
   symbol info is sometimes derived from the BFD library's view of a
   file, we need to live with what information bfd supplies.  */

enum minimal_symbol_type
{
  mst_unknown = 0,		/* Unknown type, the default */
  mst_text,			/* Generally executable instructions */

  /* A GNU ifunc symbol, in the .text section.  GDB uses to know
     whether the user is setting a breakpoint on a GNU ifunc function,
     and thus GDB needs to actually set the breakpoint on the target
     function.  It is also used to know whether the program stepped
     into an ifunc resolver -- the resolver may get a separate
     symbol/alias under a different name, but it'll have the same
     address as the ifunc symbol.  */
  mst_text_gnu_ifunc,           /* Executable code returning address
				   of executable code */

  /* A GNU ifunc function descriptor symbol, in a data section
     (typically ".opd").  Seen on architectures that use function
     descriptors, like PPC64/ELFv1.  In this case, this symbol's value
     is the address of the descriptor.  There'll be a corresponding
     mst_text_gnu_ifunc synthetic symbol for the text/entry
     address.  */
  mst_data_gnu_ifunc,		/* Executable code returning address
				   of executable code */

  mst_slot_got_plt,		/* GOT entries for .plt sections */
  mst_data,			/* Generally initialized data */
  mst_bss,			/* Generally uninitialized data */
  mst_abs,			/* Generally absolute (nonrelocatable) */
  /* GDB uses mst_solib_trampoline for the start address of a shared
     library trampoline entry.  Breakpoints for shared library functions
     are put there if the shared library is not yet loaded.
     After the shared library is loaded, lookup_minimal_symbol will
     prefer the minimal symbol from the shared library (usually
     a mst_text symbol) over the mst_solib_trampoline symbol, and the
     breakpoints will be moved to their true address in the shared
     library via breakpoint_re_set.  */
  mst_solib_trampoline,		/* Shared library trampoline code */
  /* For the mst_file* types, the names are only guaranteed to be unique
     within a given .o file.  */
  mst_file_text,		/* Static version of mst_text */
  mst_file_data,		/* Static version of mst_data */
  mst_file_bss,			/* Static version of mst_bss */
  nr_minsym_types
};

/* The number of enum minimal_symbol_type values, with some padding for
   reasonable growth.  */
#define MINSYM_TYPE_BITS 4
gdb_static_assert (nr_minsym_types <= (1 << MINSYM_TYPE_BITS));

/* Define a simple structure used to hold some very basic information about
   all defined global symbols (text, data, bss, abs, etc).  The only required
   information is the general_symbol_info.

   In many cases, even if a file was compiled with no special options for
   debugging at all, as long as was not stripped it will contain sufficient
   information to build a useful minimal symbol table using this structure.
   Even when a file contains enough debugging information to build a full
   symbol table, these minimal symbols are still useful for quickly mapping
   between names and addresses, and vice versa.  They are also sometimes
   used to figure out what full symbol table entries need to be read in.  */

struct minimal_symbol : public general_symbol_info
{
  /* Size of this symbol.  dbx_end_psymtab in dbxread.c uses this
     information to calculate the end of the partial symtab based on the
     address of the last symbol plus the size of the last symbol.  */

  unsigned long size;

  /* Which source file is this symbol in?  Only relevant for mst_file_*.  */
  const char *filename;

  /* Classification type for this minimal symbol.  */

  ENUM_BITFIELD(minimal_symbol_type) type : MINSYM_TYPE_BITS;

  /* Non-zero if this symbol was created by gdb.
     Such symbols do not appear in the output of "info var|fun".  */
  unsigned int created_by_gdb : 1;

  /* Two flag bits provided for the use of the target.  */
  unsigned int target_flag_1 : 1;
  unsigned int target_flag_2 : 1;

  /* Nonzero iff the size of the minimal symbol has been set.
     Symbol size information can sometimes not be determined, because
     the object file format may not carry that piece of information.  */
  unsigned int has_size : 1;

  /* For data symbols only, if this is set, then the symbol might be
     subject to copy relocation.  In this case, a minimal symbol
     matching the symbol's linkage name is first looked for in the
     main objfile.  If found, then that address is used; otherwise the
     address in this symbol is used.  */

  unsigned maybe_copied : 1;

  /* Minimal symbols with the same hash key are kept on a linked
     list.  This is the link.  */

  struct minimal_symbol *hash_next;

  /* Minimal symbols are stored in two different hash tables.  This is
     the `next' pointer for the demangled hash table.  */

  struct minimal_symbol *demangled_hash_next;

  /* True if this symbol is of some data type.  */

  bool data_p () const;

  /* True if MSYMBOL is of some text type.  */

  bool text_p () const;
};

/* Return the address of MINSYM, which comes from OBJF.  The
   MAYBE_COPIED flag must be set on MINSYM.  If MINSYM appears in the
   main program's minimal symbols, then that minsym's address is
   returned; otherwise, MINSYM's address is returned.  This should
   generally only be used via the MSYMBOL_VALUE_ADDRESS macro.  */

extern CORE_ADDR get_msymbol_address (struct objfile *objf,
				      const struct minimal_symbol *minsym);

#define MSYMBOL_TARGET_FLAG_1(msymbol)  (msymbol)->target_flag_1
#define MSYMBOL_TARGET_FLAG_2(msymbol)  (msymbol)->target_flag_2
#define MSYMBOL_SIZE(msymbol)		((msymbol)->size + 0)
#define SET_MSYMBOL_SIZE(msymbol, sz)		\
  do						\
    {						\
      (msymbol)->size = sz;			\
      (msymbol)->has_size = 1;			\
    } while (0)
#define MSYMBOL_HAS_SIZE(msymbol)	((msymbol)->has_size + 0)
#define MSYMBOL_TYPE(msymbol)		(msymbol)->type

#define MSYMBOL_VALUE(symbol)		(symbol)->value.ivalue
/* The unrelocated address of the minimal symbol.  */
#define MSYMBOL_VALUE_RAW_ADDRESS(symbol) ((symbol)->value.address + 0)
/* The relocated address of the minimal symbol, using the section
   offsets from OBJFILE.  */
#define MSYMBOL_VALUE_ADDRESS(objfile, symbol)				\
  (((symbol)->maybe_copied) ? get_msymbol_address (objfile, symbol)	\
   : ((symbol)->value.address						\
      + ANOFFSET ((objfile)->section_offsets, ((symbol)->section))))
/* For a bound minsym, we can easily compute the address directly.  */
#define BMSYMBOL_VALUE_ADDRESS(symbol) \
  MSYMBOL_VALUE_ADDRESS ((symbol).objfile, (symbol).minsym)
#define SET_MSYMBOL_VALUE_ADDRESS(symbol, new_value)	\
  ((symbol)->value.address = (new_value))
#define MSYMBOL_VALUE_BYTES(symbol)	(symbol)->value.bytes
#define MSYMBOL_BLOCK_VALUE(symbol)	(symbol)->value.block
#define MSYMBOL_VALUE_CHAIN(symbol)	(symbol)->value.chain
#define MSYMBOL_LANGUAGE(symbol)	(symbol)->language
#define MSYMBOL_SECTION(symbol)		(symbol)->section
#define MSYMBOL_OBJ_SECTION(objfile, symbol)			\
  (((symbol)->section >= 0)				\
   ? (&(((objfile)->sections)[(symbol)->section]))	\
   : NULL)

#include "minsyms.h"



/* Represent one symbol name; a variable, constant, function or typedef.  */

/* Different name domains for symbols.  Looking up a symbol specifies a
   domain and ignores symbol definitions in other name domains.  */

typedef enum domain_enum_tag
{
  /* UNDEF_DOMAIN is used when a domain has not been discovered or
     none of the following apply.  This usually indicates an error either
     in the symbol information or in gdb's handling of symbols.  */

  UNDEF_DOMAIN,

  /* VAR_DOMAIN is the usual domain.  In C, this contains variables,
     function names, typedef names and enum type values.  */

  VAR_DOMAIN,

  /* STRUCT_DOMAIN is used in C to hold struct, union and enum type names.
     Thus, if `struct foo' is used in a C program, it produces a symbol named
     `foo' in the STRUCT_DOMAIN.  */

  STRUCT_DOMAIN,

  /* MODULE_DOMAIN is used in Fortran to hold module type names.  */

  MODULE_DOMAIN,

  /* LABEL_DOMAIN may be used for names of labels (for gotos).  */

  LABEL_DOMAIN,

  /* Fortran common blocks.  Their naming must be separate from VAR_DOMAIN.
     They also always use LOC_COMMON_BLOCK.  */
  COMMON_BLOCK_DOMAIN,

  /* This must remain last.  */
  NR_DOMAINS
} domain_enum;

/* The number of bits in a symbol used to represent the domain.  */

#define SYMBOL_DOMAIN_BITS 3
gdb_static_assert (NR_DOMAINS <= (1 << SYMBOL_DOMAIN_BITS));

extern const char *domain_name (domain_enum);

/* Searching domains, used for `search_symbols'.  Element numbers are
   hardcoded in GDB, check all enum uses before changing it.  */

enum search_domain
{
  /* Everything in VAR_DOMAIN minus FUNCTIONS_DOMAIN and
     TYPES_DOMAIN.  */
  VARIABLES_DOMAIN = 0,

  /* All functions -- for some reason not methods, though.  */
  FUNCTIONS_DOMAIN = 1,

  /* All defined types */
  TYPES_DOMAIN = 2,

  /* All modules.  */
  MODULES_DOMAIN = 3,

  /* Any type.  */
  ALL_DOMAIN = 4
};

extern const char *search_domain_name (enum search_domain);

/* An address-class says where to find the value of a symbol.  */

enum address_class
{
  /* Not used; catches errors.  */

  LOC_UNDEF,

  /* Value is constant int SYMBOL_VALUE, host byteorder.  */

  LOC_CONST,

  /* Value is at fixed address SYMBOL_VALUE_ADDRESS.  */

  LOC_STATIC,

  /* Value is in register.  SYMBOL_VALUE is the register number
     in the original debug format.  SYMBOL_REGISTER_OPS holds a
     function that can be called to transform this into the
     actual register number this represents in a specific target
     architecture (gdbarch).

     For some symbol formats (stabs, for some compilers at least),
     the compiler generates two symbols, an argument and a register.
     In some cases we combine them to a single LOC_REGISTER in symbol
     reading, but currently not for all cases (e.g. it's passed on the
     stack and then loaded into a register).  */

  LOC_REGISTER,

  /* It's an argument; the value is at SYMBOL_VALUE offset in arglist.  */

  LOC_ARG,

  /* Value address is at SYMBOL_VALUE offset in arglist.  */

  LOC_REF_ARG,

  /* Value is in specified register.  Just like LOC_REGISTER except the
     register holds the address of the argument instead of the argument
     itself.  This is currently used for the passing of structs and unions
     on sparc and hppa.  It is also used for call by reference where the
     address is in a register, at least by mipsread.c.  */

  LOC_REGPARM_ADDR,

  /* Value is a local variable at SYMBOL_VALUE offset in stack frame.  */

  LOC_LOCAL,

  /* Value not used; definition in SYMBOL_TYPE.  Symbols in the domain
     STRUCT_DOMAIN all have this class.  */

  LOC_TYPEDEF,

  /* Value is address SYMBOL_VALUE_ADDRESS in the code.  */

  LOC_LABEL,

  /* In a symbol table, value is SYMBOL_BLOCK_VALUE of a `struct block'.
     In a partial symbol table, SYMBOL_VALUE_ADDRESS is the start address
     of the block.  Function names have this class.  */

  LOC_BLOCK,

  /* Value is a constant byte-sequence pointed to by SYMBOL_VALUE_BYTES, in
     target byte order.  */

  LOC_CONST_BYTES,

  /* Value is at fixed address, but the address of the variable has
     to be determined from the minimal symbol table whenever the
     variable is referenced.
     This happens if debugging information for a global symbol is
     emitted and the corresponding minimal symbol is defined
     in another object file or runtime common storage.
     The linker might even remove the minimal symbol if the global
     symbol is never referenced, in which case the symbol remains
     unresolved.
     
     GDB would normally find the symbol in the minimal symbol table if it will
     not find it in the full symbol table.  But a reference to an external
     symbol in a local block shadowing other definition requires full symbol
     without possibly having its address available for LOC_STATIC.  Testcase
     is provided as `gdb.dwarf2/dw2-unresolved.exp'.

     This is also used for thread local storage (TLS) variables.  In this case,
     the address of the TLS variable must be determined when the variable is
     referenced, from the MSYMBOL_VALUE_RAW_ADDRESS, which is the offset
     of the TLS variable in the thread local storage of the shared
     library/object.  */

  LOC_UNRESOLVED,

  /* The variable does not actually exist in the program.
     The value is ignored.  */

  LOC_OPTIMIZED_OUT,

  /* The variable's address is computed by a set of location
     functions (see "struct symbol_computed_ops" below).  */
  LOC_COMPUTED,

  /* The variable uses general_symbol_info->value->common_block field.
     It also always uses COMMON_BLOCK_DOMAIN.  */
  LOC_COMMON_BLOCK,

  /* Not used, just notes the boundary of the enum.  */
  LOC_FINAL_VALUE
};

/* The number of bits needed for values in enum address_class, with some
   padding for reasonable growth, and room for run-time registered address
   classes. See symtab.c:MAX_SYMBOL_IMPLS.
   This is a #define so that we can have a assertion elsewhere to
   verify that we have reserved enough space for synthetic address
   classes.  */
#define SYMBOL_ACLASS_BITS 5
gdb_static_assert (LOC_FINAL_VALUE <= (1 << SYMBOL_ACLASS_BITS));

/* The methods needed to implement LOC_COMPUTED.  These methods can
   use the symbol's .aux_value for additional per-symbol information.

   At present this is only used to implement location expressions.  */

struct symbol_computed_ops
{

  /* Return the value of the variable SYMBOL, relative to the stack
     frame FRAME.  If the variable has been optimized out, return
     zero.

     Iff `read_needs_frame (SYMBOL)' is not SYMBOL_NEEDS_FRAME, then
     FRAME may be zero.  */

  struct value *(*read_variable) (struct symbol * symbol,
				  struct frame_info * frame);

  /* Read variable SYMBOL like read_variable at (callee) FRAME's function
     entry.  SYMBOL should be a function parameter, otherwise
     NO_ENTRY_VALUE_ERROR will be thrown.  */
  struct value *(*read_variable_at_entry) (struct symbol *symbol,
					   struct frame_info *frame);

  /* Find the "symbol_needs_kind" value for the given symbol.  This
     value determines whether reading the symbol needs memory (e.g., a
     global variable), just registers (a thread-local), or a frame (a
     local variable).  */
  enum symbol_needs_kind (*get_symbol_read_needs) (struct symbol * symbol);

  /* Write to STREAM a natural-language description of the location of
     SYMBOL, in the context of ADDR.  */
  void (*describe_location) (struct symbol * symbol, CORE_ADDR addr,
			     struct ui_file * stream);

  /* Non-zero if this symbol's address computation is dependent on PC.  */
  unsigned char location_has_loclist;

  /* Tracepoint support.  Append bytecodes to the tracepoint agent
     expression AX that push the address of the object SYMBOL.  Set
     VALUE appropriately.  Note --- for objects in registers, this
     needn't emit any code; as long as it sets VALUE properly, then
     the caller will generate the right code in the process of
     treating this as an lvalue or rvalue.  */

  void (*tracepoint_var_ref) (struct symbol *symbol, struct agent_expr *ax,
			      struct axs_value *value);

  /* Generate C code to compute the location of SYMBOL.  The C code is
     emitted to STREAM.  GDBARCH is the current architecture and PC is
     the PC at which SYMBOL's location should be evaluated.
     REGISTERS_USED is a vector indexed by register number; the
     generator function should set an element in this vector if the
     corresponding register is needed by the location computation.
     The generated C code must assign the location to a local
     variable; this variable's name is RESULT_NAME.  */

  void (*generate_c_location) (struct symbol *symbol, string_file *stream,
			       struct gdbarch *gdbarch,
			       unsigned char *registers_used,
			       CORE_ADDR pc, const char *result_name);

};

/* The methods needed to implement LOC_BLOCK for inferior functions.
   These methods can use the symbol's .aux_value for additional
   per-symbol information.  */

struct symbol_block_ops
{
  /* Fill in *START and *LENGTH with DWARF block data of function
     FRAMEFUNC valid for inferior context address PC.  Set *LENGTH to
     zero if such location is not valid for PC; *START is left
     uninitialized in such case.  */
  void (*find_frame_base_location) (struct symbol *framefunc, CORE_ADDR pc,
				    const gdb_byte **start, size_t *length);

  /* Return the frame base address.  FRAME is the frame for which we want to
     compute the base address while FRAMEFUNC is the symbol for the
     corresponding function.  Return 0 on failure (FRAMEFUNC may not hold the
     information we need).

     This method is designed to work with static links (nested functions
     handling).  Static links are function properties whose evaluation returns
     the frame base address for the enclosing frame.  However, there are
     multiple definitions for "frame base": the content of the frame base
     register, the CFA as defined by DWARF unwinding information, ...

     So this specific method is supposed to compute the frame base address such
     as for nested functions, the static link computes the same address.  For
     instance, considering DWARF debugging information, the static link is
     computed with DW_AT_static_link and this method must be used to compute
     the corresponding DW_AT_frame_base attribute.  */
  CORE_ADDR (*get_frame_base) (struct symbol *framefunc,
			       struct frame_info *frame);
};

/* Functions used with LOC_REGISTER and LOC_REGPARM_ADDR.  */

struct symbol_register_ops
{
  int (*register_number) (struct symbol *symbol, struct gdbarch *gdbarch);
};

/* Objects of this type are used to find the address class and the
   various computed ops vectors of a symbol.  */

struct symbol_impl
{
  enum address_class aclass;

  /* Used with LOC_COMPUTED.  */
  const struct symbol_computed_ops *ops_computed;

  /* Used with LOC_BLOCK.  */
  const struct symbol_block_ops *ops_block;

  /* Used with LOC_REGISTER and LOC_REGPARM_ADDR.  */
  const struct symbol_register_ops *ops_register;
};

/* struct symbol has some subclasses.  This enum is used to
   differentiate between them.  */

enum symbol_subclass_kind
{
  /* Plain struct symbol.  */
  SYMBOL_NONE,

  /* struct template_symbol.  */
  SYMBOL_TEMPLATE,

  /* struct rust_vtable_symbol.  */
  SYMBOL_RUST_VTABLE
};

/* This structure is space critical.  See space comments at the top.  */

struct symbol : public general_symbol_info, public allocate_on_obstack
{
  symbol ()
    /* Class-initialization of bitfields is only allowed in C++20.  */
    : domain (UNDEF_DOMAIN),
      aclass_index (0),
      is_objfile_owned (0),
      is_argument (0),
      is_inlined (0),
      maybe_copied (0),
      subclass (SYMBOL_NONE)
    {
      /* We can't use an initializer list for members of a base class, and
         general_symbol_info needs to stay a POD type.  */
      name = nullptr;
      value.ivalue = 0;
      language_specific.obstack = nullptr;
      language = language_unknown;
      ada_mangled = 0;
      section = 0;
      /* GCC 4.8.5 (on CentOS 7) does not correctly compile class-
         initialization of unions, so we initialize it manually here.  */
      owner.symtab = nullptr;
    }

  /* Data type of value */

  struct type *type = nullptr;

  /* The owner of this symbol.
     Which one to use is defined by symbol.is_objfile_owned.  */

  union
  {
    /* The symbol table containing this symbol.  This is the file associated
       with LINE.  It can be NULL during symbols read-in but it is never NULL
       during normal operation.  */
    struct symtab *symtab;

    /* For types defined by the architecture.  */
    struct gdbarch *arch;
  } owner;

  /* Domain code.  */

  ENUM_BITFIELD(domain_enum_tag) domain : SYMBOL_DOMAIN_BITS;

  /* Address class.  This holds an index into the 'symbol_impls'
     table.  The actual enum address_class value is stored there,
     alongside any per-class ops vectors.  */

  unsigned int aclass_index : SYMBOL_ACLASS_BITS;

  /* If non-zero then symbol is objfile-owned, use owner.symtab.
       Otherwise symbol is arch-owned, use owner.arch.  */

  unsigned int is_objfile_owned : 1;

  /* Whether this is an argument.  */

  unsigned is_argument : 1;

  /* Whether this is an inlined function (class LOC_BLOCK only).  */
  unsigned is_inlined : 1;

  /* For LOC_STATIC only, if this is set, then the symbol might be
     subject to copy relocation.  In this case, a minimal symbol
     matching the symbol's linkage name is first looked for in the
     main objfile.  If found, then that address is used; otherwise the
     address in this symbol is used.  */

  unsigned maybe_copied : 1;

  /* The concrete type of this symbol.  */

  ENUM_BITFIELD (symbol_subclass_kind) subclass : 2;

  /* Line number of this symbol's definition, except for inlined
     functions.  For an inlined function (class LOC_BLOCK and
     SYMBOL_INLINED set) this is the line number of the function's call
     site.  Inlined function symbols are not definitions, and they are
     never found by symbol table lookup.
     If this symbol is arch-owned, LINE shall be zero.

     FIXME: Should we really make the assumption that nobody will try
     to debug files longer than 64K lines?  What about machine
     generated programs?  */

  unsigned short line = 0;

  /* An arbitrary data pointer, allowing symbol readers to record
     additional information on a per-symbol basis.  Note that this data
     must be allocated using the same obstack as the symbol itself.  */
  /* So far it is only used by:
     LOC_COMPUTED: to find the location information
     LOC_BLOCK (DWARF2 function): information used internally by the
     DWARF 2 code --- specifically, the location expression for the frame
     base for this function.  */
  /* FIXME drow/2003-02-21: For the LOC_BLOCK case, it might be better
     to add a magic symbol to the block containing this information,
     or to have a generic debug info annotation slot for symbols.  */

  void *aux_value = nullptr;

  struct symbol *hash_next = nullptr;
};

/* Several lookup functions return both a symbol and the block in which the
   symbol is found.  This structure is used in these cases.  */

struct block_symbol
{
  /* The symbol that was found, or NULL if no symbol was found.  */
  struct symbol *symbol;

  /* If SYMBOL is not NULL, then this is the block in which the symbol is
     defined.  */
  const struct block *block;
};

extern const struct symbol_impl *symbol_impls;

/* Note: There is no accessor macro for symbol.owner because it is
   "private".  */

#define SYMBOL_DOMAIN(symbol)	(symbol)->domain
#define SYMBOL_IMPL(symbol)		(symbol_impls[(symbol)->aclass_index])
#define SYMBOL_ACLASS_INDEX(symbol)	(symbol)->aclass_index
#define SYMBOL_CLASS(symbol)		(SYMBOL_IMPL (symbol).aclass)
#define SYMBOL_OBJFILE_OWNED(symbol)	((symbol)->is_objfile_owned)
#define SYMBOL_IS_ARGUMENT(symbol)	(symbol)->is_argument
#define SYMBOL_INLINED(symbol)		(symbol)->is_inlined
#define SYMBOL_IS_CPLUS_TEMPLATE_FUNCTION(symbol) \
  (((symbol)->subclass) == SYMBOL_TEMPLATE)
#define SYMBOL_TYPE(symbol)		(symbol)->type
#define SYMBOL_LINE(symbol)		(symbol)->line
#define SYMBOL_COMPUTED_OPS(symbol)	(SYMBOL_IMPL (symbol).ops_computed)
#define SYMBOL_BLOCK_OPS(symbol)	(SYMBOL_IMPL (symbol).ops_block)
#define SYMBOL_REGISTER_OPS(symbol)	(SYMBOL_IMPL (symbol).ops_register)
#define SYMBOL_LOCATION_BATON(symbol)   (symbol)->aux_value

extern int register_symbol_computed_impl (enum address_class,
					  const struct symbol_computed_ops *);

extern int register_symbol_block_impl (enum address_class aclass,
				       const struct symbol_block_ops *ops);

extern int register_symbol_register_impl (enum address_class,
					  const struct symbol_register_ops *);

/* Return the OBJFILE of SYMBOL.
   It is an error to call this if symbol.is_objfile_owned is false, which
   only happens for architecture-provided types.  */

extern struct objfile *symbol_objfile (const struct symbol *symbol);

/* Return the ARCH of SYMBOL.  */

extern struct gdbarch *symbol_arch (const struct symbol *symbol);

/* Return the SYMTAB of SYMBOL.
   It is an error to call this if symbol.is_objfile_owned is false, which
   only happens for architecture-provided types.  */

extern struct symtab *symbol_symtab (const struct symbol *symbol);

/* Set the symtab of SYMBOL to SYMTAB.
   It is an error to call this if symbol.is_objfile_owned is false, which
   only happens for architecture-provided types.  */

extern void symbol_set_symtab (struct symbol *symbol, struct symtab *symtab);

/* An instance of this type is used to represent a C++ template
   function.  A symbol is really of this type iff
   SYMBOL_IS_CPLUS_TEMPLATE_FUNCTION is true.  */

struct template_symbol : public symbol
{
  /* The number of template arguments.  */
  int n_template_arguments = 0;

  /* The template arguments.  This is an array with
     N_TEMPLATE_ARGUMENTS elements.  */
  struct symbol **template_arguments = nullptr;
};

/* A symbol that represents a Rust virtual table object.  */

struct rust_vtable_symbol : public symbol
{
  /* The concrete type for which this vtable was created; that is, in
     "impl Trait for Type", this is "Type".  */
  struct type *concrete_type = nullptr;
};


/* Each item represents a line-->pc (or the reverse) mapping.  This is
   somewhat more wasteful of space than one might wish, but since only
   the files which are actually debugged are read in to core, we don't
   waste much space.  */

struct linetable_entry
{
  int line;
  CORE_ADDR pc;
};

/* The order of entries in the linetable is significant.  They should
   be sorted by increasing values of the pc field.  If there is more than
   one entry for a given pc, then I'm not sure what should happen (and
   I not sure whether we currently handle it the best way).

   Example: a C for statement generally looks like this

   10   0x100   - for the init/test part of a for stmt.
   20   0x200
   30   0x300
   10   0x400   - for the increment part of a for stmt.

   If an entry has a line number of zero, it marks the start of a PC
   range for which no line number information is available.  It is
   acceptable, though wasteful of table space, for such a range to be
   zero length.  */

struct linetable
{
  int nitems;

  /* Actually NITEMS elements.  If you don't like this use of the
     `struct hack', you can shove it up your ANSI (seriously, if the
     committee tells us how to do it, we can probably go along).  */
  struct linetable_entry item[1];
};

/* How to relocate the symbols from each section in a symbol file.
   Each struct contains an array of offsets.
   The ordering and meaning of the offsets is file-type-dependent;
   typically it is indexed by section numbers or symbol types or
   something like that.

   To give us flexibility in changing the internal representation
   of these offsets, the ANOFFSET macro must be used to insert and
   extract offset values in the struct.  */

struct section_offsets
{
  CORE_ADDR offsets[1];		/* As many as needed.  */
};

#define	ANOFFSET(secoff, whichone) \
  ((whichone == -1)			  \
   ? (internal_error (__FILE__, __LINE__, \
		      _("Section index is uninitialized")), -1) \
   : secoff->offsets[whichone])

/* The size of a section_offsets table for N sections.  */
#define SIZEOF_N_SECTION_OFFSETS(n) \
  (sizeof (struct section_offsets) \
   + sizeof (((struct section_offsets *) 0)->offsets) * ((n)-1))

/* Each source file or header is represented by a struct symtab.
   The name "symtab" is historical, another name for it is "filetab".
   These objects are chained through the `next' field.  */

struct symtab
{
  /* Unordered chain of all filetabs in the compunit,  with the exception
     that the "main" source file is the first entry in the list.  */

  struct symtab *next;

  /* Backlink to containing compunit symtab.  */

  struct compunit_symtab *compunit_symtab;

  /* Table mapping core addresses to line numbers for this file.
     Can be NULL if none.  Never shared between different symtabs.  */

  struct linetable *linetable;

  /* Name of this source file.  This pointer is never NULL.  */

  const char *filename;

  /* Language of this source file.  */

  enum language language;

  /* Full name of file as found by searching the source path.
     NULL if not yet known.  */

  char *fullname;
};

#define SYMTAB_COMPUNIT(symtab) ((symtab)->compunit_symtab)
#define SYMTAB_LINETABLE(symtab) ((symtab)->linetable)
#define SYMTAB_LANGUAGE(symtab) ((symtab)->language)
#define SYMTAB_BLOCKVECTOR(symtab) \
  COMPUNIT_BLOCKVECTOR (SYMTAB_COMPUNIT (symtab))
#define SYMTAB_OBJFILE(symtab) \
  COMPUNIT_OBJFILE (SYMTAB_COMPUNIT (symtab))
#define SYMTAB_PSPACE(symtab) (SYMTAB_OBJFILE (symtab)->pspace)
#define SYMTAB_DIRNAME(symtab) \
  COMPUNIT_DIRNAME (SYMTAB_COMPUNIT (symtab))

/* Compunit symtabs contain the actual "symbol table", aka blockvector, as well
   as the list of all source files (what gdb has historically associated with
   the term "symtab").
   Additional information is recorded here that is common to all symtabs in a
   compilation unit (DWARF or otherwise).

   Example:
   For the case of a program built out of these files:

   foo.c
     foo1.h
     foo2.h
   bar.c
     foo1.h
     bar.h

   This is recorded as:

   objfile -> foo.c(cu) -> bar.c(cu) -> NULL
                |            |
                v            v
              foo.c        bar.c
                |            |
                v            v
              foo1.h       foo1.h
                |            |
                v            v
              foo2.h       bar.h
                |            |
                v            v
               NULL         NULL

   where "foo.c(cu)" and "bar.c(cu)" are struct compunit_symtab objects,
   and the files foo.c, etc. are struct symtab objects.  */

struct compunit_symtab
{
  /* Unordered chain of all compunit symtabs of this objfile.  */
  struct compunit_symtab *next;

  /* Object file from which this symtab information was read.  */
  struct objfile *objfile;

  /* Name of the symtab.
     This is *not* intended to be a usable filename, and is
     for debugging purposes only.  */
  const char *name;

  /* Unordered list of file symtabs, except that by convention the "main"
     source file (e.g., .c, .cc) is guaranteed to be first.
     Each symtab is a file, either the "main" source file (e.g., .c, .cc)
     or header (e.g., .h).  */
  struct symtab *filetabs;

  /* Last entry in FILETABS list.
     Subfiles are added to the end of the list so they accumulate in order,
     with the main source subfile living at the front.
     The main reason is so that the main source file symtab is at the head
     of the list, and the rest appear in order for debugging convenience.  */
  struct symtab *last_filetab;

  /* Non-NULL string that identifies the format of the debugging information,
     such as "stabs", "dwarf 1", "dwarf 2", "coff", etc.  This is mostly useful
     for automated testing of gdb but may also be information that is
     useful to the user.  */
  const char *debugformat;

  /* String of producer version information, or NULL if we don't know.  */
  const char *producer;

  /* Directory in which it was compiled, or NULL if we don't know.  */
  const char *dirname;

  /* List of all symbol scope blocks for this symtab.  It is shared among
     all symtabs in a given compilation unit.  */
  const struct blockvector *blockvector;

  /* Section in objfile->section_offsets for the blockvector and
     the linetable.  Probably always SECT_OFF_TEXT.  */
  int block_line_section;

  /* Symtab has been compiled with both optimizations and debug info so that
     GDB may stop skipping prologues as variables locations are valid already
     at function entry points.  */
  unsigned int locations_valid : 1;

  /* DWARF unwinder for this CU is valid even for epilogues (PC at the return
     instruction).  This is supported by GCC since 4.5.0.  */
  unsigned int epilogue_unwind_valid : 1;

  /* struct call_site entries for this compilation unit or NULL.  */
  htab_t call_site_htab;

  /* The macro table for this symtab.  Like the blockvector, this
     is shared between different symtabs in a given compilation unit.
     It's debatable whether it *should* be shared among all the symtabs in
     the given compilation unit, but it currently is.  */
  struct macro_table *macro_table;

  /* If non-NULL, then this points to a NULL-terminated vector of
     included compunits.  When searching the static or global
     block of this compunit, the corresponding block of all
     included compunits will also be searched.  Note that this
     list must be flattened -- the symbol reader is responsible for
     ensuring that this vector contains the transitive closure of all
     included compunits.  */
  struct compunit_symtab **includes;

  /* If this is an included compunit, this points to one includer
     of the table.  This user is considered the canonical compunit
     containing this one.  An included compunit may itself be
     included by another.  */
  struct compunit_symtab *user;
};

#define COMPUNIT_OBJFILE(cust) ((cust)->objfile)
#define COMPUNIT_FILETABS(cust) ((cust)->filetabs)
#define COMPUNIT_DEBUGFORMAT(cust) ((cust)->debugformat)
#define COMPUNIT_PRODUCER(cust) ((cust)->producer)
#define COMPUNIT_DIRNAME(cust) ((cust)->dirname)
#define COMPUNIT_BLOCKVECTOR(cust) ((cust)->blockvector)
#define COMPUNIT_BLOCK_LINE_SECTION(cust) ((cust)->block_line_section)
#define COMPUNIT_LOCATIONS_VALID(cust) ((cust)->locations_valid)
#define COMPUNIT_EPILOGUE_UNWIND_VALID(cust) ((cust)->epilogue_unwind_valid)
#define COMPUNIT_CALL_SITE_HTAB(cust) ((cust)->call_site_htab)
#define COMPUNIT_MACRO_TABLE(cust) ((cust)->macro_table)

/* A range adapter to allowing iterating over all the file tables
   within a compunit.  */

struct compunit_filetabs : public next_adapter<struct symtab>
{
  compunit_filetabs (struct compunit_symtab *cu)
    : next_adapter<struct symtab> (cu->filetabs)
  {
  }
};

/* Return the primary symtab of CUST.  */

extern struct symtab *
  compunit_primary_filetab (const struct compunit_symtab *cust);

/* Return the language of CUST.  */

extern enum language compunit_language (const struct compunit_symtab *cust);



/* The virtual function table is now an array of structures which have the
   form { int16 offset, delta; void *pfn; }. 

   In normal virtual function tables, OFFSET is unused.
   DELTA is the amount which is added to the apparent object's base
   address in order to point to the actual object to which the
   virtual function should be applied.
   PFN is a pointer to the virtual function.

   Note that this macro is g++ specific (FIXME).  */

#define VTBL_FNADDR_OFFSET 2

/* External variables and functions for the objects described above.  */

/* True if we are nested inside psymtab_to_symtab.  */

extern int currently_reading_symtab;

/* symtab.c lookup functions */

extern const char multiple_symbols_ask[];
extern const char multiple_symbols_all[];
extern const char multiple_symbols_cancel[];

const char *multiple_symbols_select_mode (void);

bool symbol_matches_domain (enum language symbol_language,
			    domain_enum symbol_domain,
			    domain_enum domain);

/* lookup a symbol table by source file name.  */

extern struct symtab *lookup_symtab (const char *);

/* An object of this type is passed as the 'is_a_field_of_this'
   argument to lookup_symbol and lookup_symbol_in_language.  */

struct field_of_this_result
{
  /* The type in which the field was found.  If this is NULL then the
     symbol was not found in 'this'.  If non-NULL, then one of the
     other fields will be non-NULL as well.  */

  struct type *type;

  /* If the symbol was found as an ordinary field of 'this', then this
     is non-NULL and points to the particular field.  */

  struct field *field;

  /* If the symbol was found as a function field of 'this', then this
     is non-NULL and points to the particular field.  */

  struct fn_fieldlist *fn_field;
};

/* Find the definition for a specified symbol name NAME
   in domain DOMAIN in language LANGUAGE, visible from lexical block BLOCK
   if non-NULL or from global/static blocks if BLOCK is NULL.
   Returns the struct symbol pointer, or NULL if no symbol is found.
   C++: if IS_A_FIELD_OF_THIS is non-NULL on entry, check to see if
   NAME is a field of the current implied argument `this'.  If so fill in the
   fields of IS_A_FIELD_OF_THIS, otherwise the fields are set to NULL.
   The symbol's section is fixed up if necessary.  */

extern struct block_symbol
  lookup_symbol_in_language (const char *,
			     const struct block *,
			     const domain_enum,
			     enum language,
			     struct field_of_this_result *);

/* Same as lookup_symbol_in_language, but using the current language.  */

extern struct block_symbol lookup_symbol (const char *,
					  const struct block *,
					  const domain_enum,
					  struct field_of_this_result *);

/* Find the definition for a specified symbol search name in domain
   DOMAIN, visible from lexical block BLOCK if non-NULL or from
   global/static blocks if BLOCK is NULL.  The passed-in search name
   should not come from the user; instead it should already be a
   search name as retrieved from a search_name () call.  See definition of
   symbol_name_match_type::SEARCH_NAME.  Returns the struct symbol
   pointer, or NULL if no symbol is found.  The symbol's section is
   fixed up if necessary.  */

extern struct block_symbol lookup_symbol_search_name (const char *search_name,
						      const struct block *block,
						      domain_enum domain);

/* A default version of lookup_symbol_nonlocal for use by languages
   that can't think of anything better to do.
   This implements the C lookup rules.  */

extern struct block_symbol
  basic_lookup_symbol_nonlocal (const struct language_defn *langdef,
				const char *,
				const struct block *,
				const domain_enum);

/* Some helper functions for languages that need to write their own
   lookup_symbol_nonlocal functions.  */

/* Lookup a symbol in the static block associated to BLOCK, if there
   is one; do nothing if BLOCK is NULL or a global block.
   Upon success fixes up the symbol's section if necessary.  */

extern struct block_symbol
  lookup_symbol_in_static_block (const char *name,
				 const struct block *block,
				 const domain_enum domain);

/* Search all static file-level symbols for NAME from DOMAIN.
   Upon success fixes up the symbol's section if necessary.  */

extern struct block_symbol lookup_static_symbol (const char *name,
						 const domain_enum domain);

/* Lookup a symbol in all files' global blocks.

   If BLOCK is non-NULL then it is used for two things:
   1) If a target-specific lookup routine for libraries exists, then use the
      routine for the objfile of BLOCK, and
   2) The objfile of BLOCK is used to assist in determining the search order
      if the target requires it.
      See gdbarch_iterate_over_objfiles_in_search_order.

   Upon success fixes up the symbol's section if necessary.  */

extern struct block_symbol
  lookup_global_symbol (const char *name,
			const struct block *block,
			const domain_enum domain);

/* Lookup a symbol in block BLOCK.
   Upon success fixes up the symbol's section if necessary.  */

extern struct symbol *
  lookup_symbol_in_block (const char *name,
			  symbol_name_match_type match_type,
			  const struct block *block,
			  const domain_enum domain);

/* Look up the `this' symbol for LANG in BLOCK.  Return the symbol if
   found, or NULL if not found.  */

extern struct block_symbol
  lookup_language_this (const struct language_defn *lang,
			const struct block *block);

/* Lookup a [struct, union, enum] by name, within a specified block.  */

extern struct type *lookup_struct (const char *, const struct block *);

extern struct type *lookup_union (const char *, const struct block *);

extern struct type *lookup_enum (const char *, const struct block *);

/* from blockframe.c: */

/* lookup the function symbol corresponding to the address.  The
   return value will not be an inlined function; the containing
   function will be returned instead.  */

extern struct symbol *find_pc_function (CORE_ADDR);

/* lookup the function corresponding to the address and section.  The
   return value will not be an inlined function; the containing
   function will be returned instead.  */

extern struct symbol *find_pc_sect_function (CORE_ADDR, struct obj_section *);

/* lookup the function symbol corresponding to the address and
   section.  The return value will be the closest enclosing function,
   which might be an inline function.  */

extern struct symbol *find_pc_sect_containing_function
  (CORE_ADDR pc, struct obj_section *section);

/* Find the symbol at the given address.  Returns NULL if no symbol
   found.  Only exact matches for ADDRESS are considered.  */

extern struct symbol *find_symbol_at_address (CORE_ADDR);

/* Finds the "function" (text symbol) that is smaller than PC but
   greatest of all of the potential text symbols in SECTION.  Sets
   *NAME and/or *ADDRESS conditionally if that pointer is non-null.
   If ENDADDR is non-null, then set *ENDADDR to be the end of the
   function (exclusive).  If the optional parameter BLOCK is non-null,
   then set *BLOCK to the address of the block corresponding to the
   function symbol, if such a symbol could be found during the lookup;
   nullptr is used as a return value for *BLOCK if no block is found. 
   This function either succeeds or fails (not halfway succeeds).  If
   it succeeds, it sets *NAME, *ADDRESS, and *ENDADDR to real
   information and returns true.  If it fails, it sets *NAME, *ADDRESS
   and *ENDADDR to zero and returns false.
   
   If the function in question occupies non-contiguous ranges,
   *ADDRESS and *ENDADDR are (subject to the conditions noted above) set
   to the start and end of the range in which PC is found.  Thus
   *ADDRESS <= PC < *ENDADDR with no intervening gaps (in which ranges
   from other functions might be found).
   
   This property allows find_pc_partial_function to be used (as it had
   been prior to the introduction of non-contiguous range support) by
   various tdep files for finding a start address and limit address
   for prologue analysis.  This still isn't ideal, however, because we
   probably shouldn't be doing prologue analysis (in which
   instructions are scanned to determine frame size and stack layout)
   for any range that doesn't contain the entry pc.  Moreover, a good
   argument can be made that prologue analysis ought to be performed
   starting from the entry pc even when PC is within some other range.
   This might suggest that *ADDRESS and *ENDADDR ought to be set to the
   limits of the entry pc range, but that will cause the 
   *ADDRESS <= PC < *ENDADDR condition to be violated; many of the
   callers of find_pc_partial_function expect this condition to hold. 

   Callers which require the start and/or end addresses for the range
   containing the entry pc should instead call
   find_function_entry_range_from_pc.  */

extern bool find_pc_partial_function (CORE_ADDR pc, const char **name,
				      CORE_ADDR *address, CORE_ADDR *endaddr,
				      const struct block **block = nullptr);

/* Like find_pc_partial_function, above, but *ADDRESS and *ENDADDR are
   set to start and end addresses of the range containing the entry pc.

   Note that it is not necessarily the case that (for non-NULL ADDRESS
   and ENDADDR arguments) the *ADDRESS <= PC < *ENDADDR condition will
   hold.

   See comment for find_pc_partial_function, above, for further
   explanation.  */

extern bool find_function_entry_range_from_pc (CORE_ADDR pc,
					       const char **name,
					       CORE_ADDR *address,
					       CORE_ADDR *endaddr);

/* Return the type of a function with its first instruction exactly at
   the PC address.  Return NULL otherwise.  */

extern struct type *find_function_type (CORE_ADDR pc);

/* See if we can figure out the function's actual type from the type
   that the resolver returns.  RESOLVER_FUNADDR is the address of the
   ifunc resolver.  */

extern struct type *find_gnu_ifunc_target_type (CORE_ADDR resolver_funaddr);

/* Find the GNU ifunc minimal symbol that matches SYM.  */
extern bound_minimal_symbol find_gnu_ifunc (const symbol *sym);

extern void clear_pc_function_cache (void);

/* Expand symtab containing PC, SECTION if not already expanded.  */

extern void expand_symtab_containing_pc (CORE_ADDR, struct obj_section *);

/* lookup full symbol table by address.  */

extern struct compunit_symtab *find_pc_compunit_symtab (CORE_ADDR);

/* lookup full symbol table by address and section.  */

extern struct compunit_symtab *
  find_pc_sect_compunit_symtab (CORE_ADDR, struct obj_section *);

extern bool find_pc_line_pc_range (CORE_ADDR, CORE_ADDR *, CORE_ADDR *);

extern void reread_symbols (void);

/* Look up a type named NAME in STRUCT_DOMAIN in the current language.
   The type returned must not be opaque -- i.e., must have at least one field
   defined.  */

extern struct type *lookup_transparent_type (const char *);

extern struct type *basic_lookup_transparent_type (const char *);

/* Macro for name of symbol to indicate a file compiled with gcc.  */
#ifndef GCC_COMPILED_FLAG_SYMBOL
#define GCC_COMPILED_FLAG_SYMBOL "gcc_compiled."
#endif

/* Macro for name of symbol to indicate a file compiled with gcc2.  */
#ifndef GCC2_COMPILED_FLAG_SYMBOL
#define GCC2_COMPILED_FLAG_SYMBOL "gcc2_compiled."
#endif

extern bool in_gnu_ifunc_stub (CORE_ADDR pc);

/* Functions for resolving STT_GNU_IFUNC symbols which are implemented only
   for ELF symbol files.  */

struct gnu_ifunc_fns
{
  /* See elf_gnu_ifunc_resolve_addr for its real implementation.  */
  CORE_ADDR (*gnu_ifunc_resolve_addr) (struct gdbarch *gdbarch, CORE_ADDR pc);

  /* See elf_gnu_ifunc_resolve_name for its real implementation.  */
  bool (*gnu_ifunc_resolve_name) (const char *function_name,
				 CORE_ADDR *function_address_p);

  /* See elf_gnu_ifunc_resolver_stop for its real implementation.  */
  void (*gnu_ifunc_resolver_stop) (struct breakpoint *b);

  /* See elf_gnu_ifunc_resolver_return_stop for its real implementation.  */
  void (*gnu_ifunc_resolver_return_stop) (struct breakpoint *b);
};

#define gnu_ifunc_resolve_addr gnu_ifunc_fns_p->gnu_ifunc_resolve_addr
#define gnu_ifunc_resolve_name gnu_ifunc_fns_p->gnu_ifunc_resolve_name
#define gnu_ifunc_resolver_stop gnu_ifunc_fns_p->gnu_ifunc_resolver_stop
#define gnu_ifunc_resolver_return_stop \
  gnu_ifunc_fns_p->gnu_ifunc_resolver_return_stop

extern const struct gnu_ifunc_fns *gnu_ifunc_fns_p;

extern CORE_ADDR find_solib_trampoline_target (struct frame_info *, CORE_ADDR);

struct symtab_and_line
{
  /* The program space of this sal.  */
  struct program_space *pspace = NULL;

  struct symtab *symtab = NULL;
  struct symbol *symbol = NULL;
  struct obj_section *section = NULL;
  struct minimal_symbol *msymbol = NULL;
  /* Line number.  Line numbers start at 1 and proceed through symtab->nlines.
     0 is never a valid line number; it is used to indicate that line number
     information is not available.  */
  int line = 0;

  CORE_ADDR pc = 0;
  CORE_ADDR end = 0;
  bool explicit_pc = false;
  bool explicit_line = false;

  /* The probe associated with this symtab_and_line.  */
  probe *prob = NULL;
  /* If PROBE is not NULL, then this is the objfile in which the probe
     originated.  */
  struct objfile *objfile = NULL;
};



/* Given a pc value, return line number it is in.  Second arg nonzero means
   if pc is on the boundary use the previous statement's line number.  */

extern struct symtab_and_line find_pc_line (CORE_ADDR, int);

/* Same function, but specify a section as well as an address.  */

extern struct symtab_and_line find_pc_sect_line (CORE_ADDR,
						 struct obj_section *, int);

/* Wrapper around find_pc_line to just return the symtab.  */

extern struct symtab *find_pc_line_symtab (CORE_ADDR);

/* Given a symtab and line number, return the pc there.  */

extern bool find_line_pc (struct symtab *, int, CORE_ADDR *);

extern bool find_line_pc_range (struct symtab_and_line, CORE_ADDR *,
				CORE_ADDR *);

extern void resolve_sal_pc (struct symtab_and_line *);

/* solib.c */

extern void clear_solib (void);

/* The reason we're calling into a completion match list collector
   function.  */
enum class complete_symbol_mode
  {
    /* Completing an expression.  */
    EXPRESSION,

    /* Completing a linespec.  */
    LINESPEC,
  };

extern void default_collect_symbol_completion_matches_break_on
  (completion_tracker &tracker,
   complete_symbol_mode mode,
   symbol_name_match_type name_match_type,
   const char *text, const char *word, const char *break_on,
   enum type_code code);
extern void default_collect_symbol_completion_matches
  (completion_tracker &tracker,
   complete_symbol_mode,
   symbol_name_match_type name_match_type,
   const char *,
   const char *,
   enum type_code);
extern void collect_symbol_completion_matches
  (completion_tracker &tracker,
   complete_symbol_mode mode,
   symbol_name_match_type name_match_type,
   const char *, const char *);
extern void collect_symbol_completion_matches_type (completion_tracker &tracker,
						    const char *, const char *,
						    enum type_code);

extern void collect_file_symbol_completion_matches
  (completion_tracker &tracker,
   complete_symbol_mode,
   symbol_name_match_type name_match_type,
   const char *, const char *, const char *);

extern completion_list
  make_source_files_completion_list (const char *, const char *);

/* Return whether SYM is a function/method, as opposed to a data symbol.  */

extern bool symbol_is_function_or_method (symbol *sym);

/* Return whether MSYMBOL is a function/method, as opposed to a data
   symbol */

extern bool symbol_is_function_or_method (minimal_symbol *msymbol);

/* Return whether SYM should be skipped in completion mode MODE.  In
   linespec mode, we're only interested in functions/methods.  */

template<typename Symbol>
static bool
completion_skip_symbol (complete_symbol_mode mode, Symbol *sym)
{
  return (mode == complete_symbol_mode::LINESPEC
	  && !symbol_is_function_or_method (sym));
}

/* symtab.c */

bool matching_obj_sections (struct obj_section *, struct obj_section *);

extern struct symtab *find_line_symtab (struct symtab *, int, int *, bool *);

/* Given a function symbol SYM, find the symtab and line for the start
   of the function.  If FUNFIRSTLINE is true, we want the first line
   of real code inside the function.  */
extern symtab_and_line find_function_start_sal (symbol *sym, bool
						funfirstline);

/* Same, but start with a function address/section instead of a
   symbol.  */
extern symtab_and_line find_function_start_sal (CORE_ADDR func_addr,
						obj_section *section,
						bool funfirstline);

extern void skip_prologue_sal (struct symtab_and_line *);

/* symtab.c */

extern CORE_ADDR skip_prologue_using_sal (struct gdbarch *gdbarch,
					  CORE_ADDR func_addr);

extern struct symbol *fixup_symbol_section (struct symbol *,
					    struct objfile *);

/* If MSYMBOL is an text symbol, look for a function debug symbol with
   the same address.  Returns NULL if not found.  This is necessary in
   case a function is an alias to some other function, because debug
   information is only emitted for the alias target function's
   definition, not for the alias.  */
extern symbol *find_function_alias_target (bound_minimal_symbol msymbol);

/* Symbol searching */
/* Note: struct symbol_search, search_symbols, et.al. are declared here,
   instead of making them local to symtab.c, for gdbtk's sake.  */

/* When using search_symbols, a vector of the following structs is
   returned.  */
struct symbol_search
{
  symbol_search (int block_, struct symbol *symbol_)
    : block (block_),
      symbol (symbol_)
  {
    msymbol.minsym = nullptr;
    msymbol.objfile = nullptr;
  }

  symbol_search (int block_, struct minimal_symbol *minsym,
		 struct objfile *objfile)
    : block (block_),
      symbol (nullptr)
  {
    msymbol.minsym = minsym;
    msymbol.objfile = objfile;
  }

  bool operator< (const symbol_search &other) const
  {
    return compare_search_syms (*this, other) < 0;
  }

  bool operator== (const symbol_search &other) const
  {
    return compare_search_syms (*this, other) == 0;
  }

  /* The block in which the match was found.  Could be, for example,
     STATIC_BLOCK or GLOBAL_BLOCK.  */
  int block;

  /* Information describing what was found.

     If symbol is NOT NULL, then information was found for this match.  */
  struct symbol *symbol;

  /* If msymbol is non-null, then a match was made on something for
     which only minimal_symbols exist.  */
  struct bound_minimal_symbol msymbol;

private:

  static int compare_search_syms (const symbol_search &sym_a,
				  const symbol_search &sym_b);
};

extern std::vector<symbol_search> search_symbols (const char *,
						  enum search_domain,
						  const char *,
						  int,
						  const char **,
						  bool);

/* When searching for Fortran symbols within modules (functions/variables)
   we return a vector of this type.  The first item in the pair is the
   module symbol, and the second item is the symbol for the function or
   variable we found.  */
typedef std::pair<symbol_search, symbol_search> module_symbol_search;

/* Searches the symbols to find function and variables symbols (depending
   on KIND) within Fortran modules.  The MODULE_REGEXP matches against the
   name of the module, REGEXP matches against the name of the symbol within
   the module, and TYPE_REGEXP matches against the type of the symbol
   within the module.  */
extern std::vector<module_symbol_search> search_module_symbols
	(const char *module_regexp, const char *regexp,
	 const char *type_regexp, search_domain kind);

extern bool treg_matches_sym_type_name (const compiled_regex &treg,
					const struct symbol *sym);

/* The name of the ``main'' function.  */
extern const char *main_name ();
extern enum language main_language (void);

/* Lookup symbol NAME from DOMAIN in MAIN_OBJFILE's global or static blocks,
   as specified by BLOCK_INDEX.
   This searches MAIN_OBJFILE as well as any associated separate debug info
   objfiles of MAIN_OBJFILE.
   BLOCK_INDEX can be GLOBAL_BLOCK or STATIC_BLOCK.
   Upon success fixes up the symbol's section if necessary.  */

extern struct block_symbol
  lookup_global_symbol_from_objfile (struct objfile *main_objfile,
				     enum block_enum block_index,
				     const char *name,
				     const domain_enum domain);

/* Return 1 if the supplied producer string matches the ARM RealView
   compiler (armcc).  */
bool producer_is_realview (const char *producer);

void fixup_section (struct general_symbol_info *ginfo,
		    CORE_ADDR addr, struct objfile *objfile);

/* Look up objfile containing BLOCK.  */

struct objfile *lookup_objfile_from_block (const struct block *block);

extern unsigned int symtab_create_debug;

extern unsigned int symbol_lookup_debug;

extern bool basenames_may_differ;

bool compare_filenames_for_search (const char *filename,
				   const char *search_name);

bool compare_glob_filenames_for_search (const char *filename,
					const char *search_name);

bool iterate_over_some_symtabs (const char *name,
				const char *real_path,
				struct compunit_symtab *first,
				struct compunit_symtab *after_last,
				gdb::function_view<bool (symtab *)> callback);

void iterate_over_symtabs (const char *name,
			   gdb::function_view<bool (symtab *)> callback);


std::vector<CORE_ADDR> find_pcs_for_symtab_line
    (struct symtab *symtab, int line, struct linetable_entry **best_entry);

/* Prototype for callbacks for LA_ITERATE_OVER_SYMBOLS.  The callback
   is called once per matching symbol SYM.  The callback should return
   true to indicate that LA_ITERATE_OVER_SYMBOLS should continue
   iterating, or false to indicate that the iteration should end.  */

typedef bool (symbol_found_callback_ftype) (struct block_symbol *bsym);

/* Iterate over the symbols named NAME, matching DOMAIN, in BLOCK.

   For each symbol that matches, CALLBACK is called.  The symbol is
   passed to the callback.

   If CALLBACK returns false, the iteration ends and this function
   returns false.  Otherwise, the search continues, and the function
   eventually returns true.  */

bool iterate_over_symbols (const struct block *block,
			   const lookup_name_info &name,
			   const domain_enum domain,
			   gdb::function_view<symbol_found_callback_ftype> callback);

/* Like iterate_over_symbols, but if all calls to CALLBACK return
   true, then calls CALLBACK one additional time with a block_symbol
   that has a valid block but a NULL symbol.  */

bool iterate_over_symbols_terminated
  (const struct block *block,
   const lookup_name_info &name,
   const domain_enum domain,
   gdb::function_view<symbol_found_callback_ftype> callback);

/* Storage type used by demangle_for_lookup.  demangle_for_lookup
   either returns a const char * pointer that points to either of the
   fields of this type, or a pointer to the input NAME.  This is done
   this way because the underlying functions that demangle_for_lookup
   calls either return a std::string (e.g., cp_canonicalize_string) or
   a malloc'ed buffer (libiberty's demangled), and we want to avoid
   unnecessary reallocation/string copying.  */
class demangle_result_storage
{
public:

  /* Swap the std::string storage with STR, and return a pointer to
     the beginning of the new string.  */
  const char *swap_string (std::string &str)
  {
    std::swap (m_string, str);
    return m_string.c_str ();
  }

  /* Set the malloc storage to now point at PTR.  Any previous malloc
     storage is released.  */
  const char *set_malloc_ptr (char *ptr)
  {
    m_malloc.reset (ptr);
    return ptr;
  }

private:

  /* The storage.  */
  std::string m_string;
  gdb::unique_xmalloc_ptr<char> m_malloc;
};

const char *
  demangle_for_lookup (const char *name, enum language lang,
		       demangle_result_storage &storage);

struct symbol *allocate_symbol (struct objfile *);

void initialize_objfile_symbol (struct symbol *);

struct template_symbol *allocate_template_symbol (struct objfile *);

/* Test to see if the symbol of language SYMBOL_LANGUAGE specified by
   SYMNAME (which is already demangled for C++ symbols) matches
   SYM_TEXT in the first SYM_TEXT_LEN characters.  If so, add it to
   the current completion list.  */
void completion_list_add_name (completion_tracker &tracker,
			       language symbol_language,
			       const char *symname,
			       const lookup_name_info &lookup_name,
			       const char *text, const char *word);

/* A simple symbol searching class.  */

class symbol_searcher
{
public:
  /* Returns the symbols found for the search.  */
  const std::vector<block_symbol> &
  matching_symbols () const
  {
    return m_symbols;
  }

  /* Returns the minimal symbols found for the search.  */
  const std::vector<bound_minimal_symbol> &
  matching_msymbols () const
  {
    return m_minimal_symbols;
  }

  /* Search for all symbols named NAME in LANGUAGE with DOMAIN, restricting
     search to FILE_SYMTABS and SEARCH_PSPACE, both of which may be NULL
     to search all symtabs and program spaces.  */
  void find_all_symbols (const std::string &name,
			 const struct language_defn *language,
			 enum search_domain search_domain,
			 std::vector<symtab *> *search_symtabs,
			 struct program_space *search_pspace);

  /* Reset this object to perform another search.  */
  void reset ()
  {
    m_symbols.clear ();
    m_minimal_symbols.clear ();
  }

private:
  /* Matching debug symbols.  */
  std::vector<block_symbol>  m_symbols;

  /* Matching non-debug symbols.  */
  std::vector<bound_minimal_symbol> m_minimal_symbols;
};

#endif /* !defined(SYMTAB_H) */
