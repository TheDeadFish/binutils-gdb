/* C language support routines for GDB, the GNU debugger.

   Copyright (C) 1992-2019 Free Software Foundation, Inc.

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
#include "symtab.h"
#include "gdbtypes.h"
#include "expression.h"
#include "parser-defs.h"
#include "language.h"
#include "varobj.h"
#include "c-lang.h"
#include "c-support.h"
#include "valprint.h"
#include "macroscope.h"
#include "charset.h"
#include "demangle.h"
#include "cp-abi.h"
#include "cp-support.h"
#include "gdb_obstack.h"
#include <ctype.h>
#include "gdbcore.h"
#include "gdbarch.h"

/* Given a C string type, STR_TYPE, return the corresponding target
   character set name.  */

static const char *
charset_for_string_type (c_string_type str_type, struct gdbarch *gdbarch)
{
  switch (str_type & ~C_CHAR)
    {
    case C_STRING:
      return target_charset (gdbarch);
    case C_WIDE_STRING:
      return target_wide_charset (gdbarch);
    case C_STRING_16:
      /* FIXME: UTF-16 is not always correct.  */
      if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
	return "UTF-16BE";
      else
	return "UTF-16LE";
    case C_STRING_32:
      /* FIXME: UTF-32 is not always correct.  */
      if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
	return "UTF-32BE";
      else
	return "UTF-32LE";
    }
  internal_error (__FILE__, __LINE__, _("unhandled c_string_type"));
}

/* Classify ELTTYPE according to what kind of character it is.  Return
   the enum constant representing the character type.  Also set
   *ENCODING to the name of the character set to use when converting
   characters of this type in target BYTE_ORDER to the host character
   set.  */

static c_string_type
classify_type (struct type *elttype, struct gdbarch *gdbarch,
	       const char **encoding)
{
  c_string_type result;

  /* We loop because ELTTYPE may be a typedef, and we want to
     successively peel each typedef until we reach a type we
     understand.  We don't use CHECK_TYPEDEF because that will strip
     all typedefs at once -- but in C, wchar_t is itself a typedef, so
     that would do the wrong thing.  */
  while (elttype)
    {
      const char *name = TYPE_NAME (elttype);

      if (TYPE_CODE (elttype) == TYPE_CODE_CHAR || !name)
	{
	  result = C_CHAR;
	  goto done;
	}

      if (!strcmp (name, "wchar_t"))
	{
	  result = C_WIDE_CHAR;
	  goto done;
	}

      if (!strcmp (name, "char16_t"))
	{
	  result = C_CHAR_16;
	  goto done;
	}

      if (!strcmp (name, "char32_t"))
	{
	  result = C_CHAR_32;
	  goto done;
	}

      if (TYPE_CODE (elttype) != TYPE_CODE_TYPEDEF)
	break;

      /* Call for side effects.  */
      check_typedef (elttype);

      if (TYPE_TARGET_TYPE (elttype))
	elttype = TYPE_TARGET_TYPE (elttype);
      else
	{
	  /* Perhaps check_typedef did not update the target type.  In
	     this case, force the lookup again and hope it works out.
	     It never will for C, but it might for C++.  */
	  elttype = check_typedef (elttype);
	}
    }

  /* Punt.  */
  result = C_CHAR;

 done:
  if (encoding)
    *encoding = charset_for_string_type (result, gdbarch);

  return result;
}

/* Print the character C on STREAM as part of the contents of a
   literal string whose delimiter is QUOTER.  Note that that format
   for printing characters and strings is language specific.  */

void
c_emit_char (int c, struct type *type,
	     struct ui_file *stream, int quoter)
{
  const char *encoding;

  classify_type (type, get_type_arch (type), &encoding);
  generic_emit_char (c, type, stream, quoter, encoding);
}

void
c_printchar (int c, struct type *type, struct ui_file *stream)
{
  c_string_type str_type;

  str_type = classify_type (type, get_type_arch (type), NULL);
  switch (str_type)
    {
    case C_CHAR:
      break;
    case C_WIDE_CHAR:
      fputc_filtered ('L', stream);
      break;
    case C_CHAR_16:
      fputc_filtered ('u', stream);
      break;
    case C_CHAR_32:
      fputc_filtered ('U', stream);
      break;
    }

  fputc_filtered ('\'', stream);
  LA_EMIT_CHAR (c, type, stream, '\'');
  fputc_filtered ('\'', stream);
}

/* Print the character string STRING, printing at most LENGTH
   characters.  LENGTH is -1 if the string is nul terminated.  Each
   character is WIDTH bytes long.  Printing stops early if the number
   hits print_max; repeat counts are printed as appropriate.  Print
   ellipses at the end if we had to stop before printing LENGTH
   characters, or if FORCE_ELLIPSES.  */

void
c_printstr (struct ui_file *stream, struct type *type, 
	    const gdb_byte *string, unsigned int length, 
	    const char *user_encoding, int force_ellipses,
	    const struct value_print_options *options)
{
  c_string_type str_type;
  const char *type_encoding;
  const char *encoding;

  str_type = (classify_type (type, get_type_arch (type), &type_encoding)
	      & ~C_CHAR);
  switch (str_type)
    {
    case C_STRING:
      break;
    case C_WIDE_STRING:
      fputs_filtered ("L", stream);
      break;
    case C_STRING_16:
      fputs_filtered ("u", stream);
      break;
    case C_STRING_32:
      fputs_filtered ("U", stream);
      break;
    }

  encoding = (user_encoding && *user_encoding) ? user_encoding : type_encoding;

  generic_printstr (stream, type, string, length, encoding, force_ellipses,
		    '"', 1, options);
}

/* Obtain a C string from the inferior storing it in a newly allocated
   buffer in BUFFER, which should be freed by the caller.  If the in-
   and out-parameter *LENGTH is specified at -1, the string is read
   until a null character of the appropriate width is found, otherwise
   the string is read to the length of characters specified.  The size
   of a character is determined by the length of the target type of
   the pointer or array.

   If VALUE is an array with a known length, and *LENGTH is -1,
   the function will not read past the end of the array.  However, any
   declared size of the array is ignored if *LENGTH > 0.

   On completion, *LENGTH will be set to the size of the string read in
   characters.  (If a length of -1 is specified, the length returned
   will not include the null character).  CHARSET is always set to the
   target charset.  */

void
c_get_string (struct value *value, gdb::unique_xmalloc_ptr<gdb_byte> *buffer,
	      int *length, struct type **char_type,
	      const char **charset)
{
  int err, width;
  unsigned int fetchlimit;
  struct type *type = check_typedef (value_type (value));
  struct type *element_type = TYPE_TARGET_TYPE (type);
  int req_length = *length;
  enum bfd_endian byte_order
    = type_byte_order (type);

  if (element_type == NULL)
    goto error;

  if (TYPE_CODE (type) == TYPE_CODE_ARRAY)
    {
      /* If we know the size of the array, we can use it as a limit on
	 the number of characters to be fetched.  */
      if (TYPE_NFIELDS (type) == 1
	  && TYPE_CODE (TYPE_FIELD_TYPE (type, 0)) == TYPE_CODE_RANGE)
	{
	  LONGEST low_bound, high_bound;

	  get_discrete_bounds (TYPE_FIELD_TYPE (type, 0),
			       &low_bound, &high_bound);
	  fetchlimit = high_bound - low_bound + 1;
	}
      else
	fetchlimit = UINT_MAX;
    }
  else if (TYPE_CODE (type) == TYPE_CODE_PTR)
    fetchlimit = UINT_MAX;
  else
    /* We work only with arrays and pointers.  */
    goto error;

  if (! c_textual_element_type (element_type, 0))
    goto error;
  classify_type (element_type, get_type_arch (element_type), charset);
  width = TYPE_LENGTH (element_type);

  /* If the string lives in GDB's memory instead of the inferior's,
     then we just need to copy it to BUFFER.  Also, since such strings
     are arrays with known size, FETCHLIMIT will hold the size of the
     array.

     An array is assumed to live in GDB's memory, so we take this path
     here.

     However, it's possible for the caller to request more array
     elements than apparently exist -- this can happen when using the
     C struct hack.  So, only do this if either no length was
     specified, or the length is within the existing bounds.  This
     avoids running off the end of the value's contents.  */
  if ((VALUE_LVAL (value) == not_lval
       || VALUE_LVAL (value) == lval_internalvar
       || TYPE_CODE (type) == TYPE_CODE_ARRAY)
      && fetchlimit != UINT_MAX
      && (*length < 0 || *length <= fetchlimit))
    {
      int i;
      const gdb_byte *contents = value_contents (value);

      /* If a length is specified, use that.  */
      if (*length >= 0)
	i  = *length;
      else
 	/* Otherwise, look for a null character.  */
 	for (i = 0; i < fetchlimit; i++)
	  if (extract_unsigned_integer (contents + i * width,
					width, byte_order) == 0)
 	    break;
  
      /* I is now either a user-defined length, the number of non-null
 	 characters, or FETCHLIMIT.  */
      *length = i * width;
      buffer->reset ((gdb_byte *) xmalloc (*length));
      memcpy (buffer->get (), contents, *length);
      err = 0;
    }
  else
    {
      /* value_as_address does not return an address for an array when
	 c_style_arrays is false, so we handle that specially
	 here.  */
      CORE_ADDR addr;
      if (TYPE_CODE (type) == TYPE_CODE_ARRAY)
	{
	  if (VALUE_LVAL (value) != lval_memory)
	    error (_("Attempt to take address of value "
		     "not located in memory."));
	  addr = value_address (value);
	}
      else
	addr = value_as_address (value);

      /* Prior to the fix for PR 16196 read_string would ignore fetchlimit
	 if length > 0.  The old "broken" behaviour is the behaviour we want:
	 The caller may want to fetch 100 bytes from a variable length array
	 implemented using the common idiom of having an array of length 1 at
	 the end of a struct.  In this case we want to ignore the declared
	 size of the array.  However, it's counterintuitive to implement that
	 behaviour in read_string: what does fetchlimit otherwise mean if
	 length > 0.  Therefore we implement the behaviour we want here:
	 If *length > 0, don't specify a fetchlimit.  This preserves the
	 previous behaviour.  We could move this check above where we know
	 whether the array is declared with a fixed size, but we only want
	 to apply this behaviour when calling read_string.  PR 16286.  */
      if (*length > 0)
	fetchlimit = UINT_MAX;

      err = read_string (addr, *length, width, fetchlimit,
			 byte_order, buffer, length);
      if (err != 0)
	memory_error (TARGET_XFER_E_IO, addr);
    }

  /* If the LENGTH is specified at -1, we want to return the string
     length up to the terminating null character.  If an actual length
     was specified, we want to return the length of exactly what was
     read.  */
  if (req_length == -1)
    /* If the last character is null, subtract it from LENGTH.  */
    if (*length > 0
	&& extract_unsigned_integer (buffer->get () + *length - width,
				     width, byte_order) == 0)
      *length -= width;
  
  /* The read_string function will return the number of bytes read.
     If length returned from read_string was > 0, return the number of
     characters read by dividing the number of bytes by width.  */
  if (*length != 0)
     *length = *length / width;

  *char_type = element_type;

  return;

 error:
  {
    std::string type_str = type_to_string (type);
    if (!type_str.empty ())
      {
	error (_("Trying to read string with inappropriate type `%s'."),
	       type_str.c_str ());
      }
    else
      error (_("Trying to read string with inappropriate type."));
  }
}


/* Evaluating C and C++ expressions.  */

/* Convert a UCN.  The digits of the UCN start at P and extend no
   farther than LIMIT.  DEST_CHARSET is the name of the character set
   into which the UCN should be converted.  The results are written to
   OUTPUT.  LENGTH is the maximum length of the UCN, either 4 or 8.
   Returns a pointer to just after the final digit of the UCN.  */

static char *
convert_ucn (char *p, char *limit, const char *dest_charset,
	     struct obstack *output, int length)
{
  unsigned long result = 0;
  gdb_byte data[4];
  int i;

  for (i = 0; i < length && p < limit && ISXDIGIT (*p); ++i, ++p)
    result = (result << 4) + host_hex_value (*p);

  for (i = 3; i >= 0; --i)
    {
      data[i] = result & 0xff;
      result >>= 8;
    }

  convert_between_encodings ("UTF-32BE", dest_charset, data,
			     4, 4, output, translit_none);

  return p;
}

/* Emit a character, VALUE, which was specified numerically, to
   OUTPUT.  TYPE is the target character type.  */

static void
emit_numeric_character (struct type *type, unsigned long value,
			struct obstack *output)
{
  gdb_byte *buffer;

  buffer = (gdb_byte *) alloca (TYPE_LENGTH (type));
  pack_long (buffer, type, value);
  obstack_grow (output, buffer, TYPE_LENGTH (type));
}

/* Convert an octal escape sequence.  TYPE is the target character
   type.  The digits of the escape sequence begin at P and extend no
   farther than LIMIT.  The result is written to OUTPUT.  Returns a
   pointer to just after the final digit of the escape sequence.  */

static char *
convert_octal (struct type *type, char *p, 
	       char *limit, struct obstack *output)
{
  int i;
  unsigned long value = 0;

  for (i = 0;
       i < 3 && p < limit && ISDIGIT (*p) && *p != '8' && *p != '9';
       ++i)
    {
      value = 8 * value + host_hex_value (*p);
      ++p;
    }

  emit_numeric_character (type, value, output);

  return p;
}

/* Convert a hex escape sequence.  TYPE is the target character type.
   The digits of the escape sequence begin at P and extend no farther
   than LIMIT.  The result is written to OUTPUT.  Returns a pointer to
   just after the final digit of the escape sequence.  */

static char *
convert_hex (struct type *type, char *p,
	     char *limit, struct obstack *output)
{
  unsigned long value = 0;

  while (p < limit && ISXDIGIT (*p))
    {
      value = 16 * value + host_hex_value (*p);
      ++p;
    }

  emit_numeric_character (type, value, output);

  return p;
}

#define ADVANCE					\
  do {						\
    ++p;					\
    if (p == limit)				\
      error (_("Malformed escape sequence"));	\
  } while (0)

/* Convert an escape sequence to a target format.  TYPE is the target
   character type to use, and DEST_CHARSET is the name of the target
   character set.  The backslash of the escape sequence is at *P, and
   the escape sequence will not extend past LIMIT.  The results are
   written to OUTPUT.  Returns a pointer to just past the final
   character of the escape sequence.  */

static char *
convert_escape (struct type *type, const char *dest_charset,
		char *p, char *limit, struct obstack *output)
{
  /* Skip the backslash.  */
  ADVANCE;

  switch (*p)
    {
    case '\\':
      obstack_1grow (output, '\\');
      ++p;
      break;

    case 'x':
      ADVANCE;
      if (!ISXDIGIT (*p))
	error (_("\\x used with no following hex digits."));
      p = convert_hex (type, p, limit, output);
      break;

    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
      p = convert_octal (type, p, limit, output);
      break;

    case 'u':
    case 'U':
      {
	int length = *p == 'u' ? 4 : 8;

	ADVANCE;
	if (!ISXDIGIT (*p))
	  error (_("\\u used with no following hex digits"));
	p = convert_ucn (p, limit, dest_charset, output, length);
      }
    }

  return p;
}

/* Given a single string from a (C-specific) OP_STRING list, convert
   it to a target string, handling escape sequences specially.  The
   output is written to OUTPUT.  DATA is the input string, which has
   length LEN.  DEST_CHARSET is the name of the target character set,
   and TYPE is the type of target character to use.  */

static void
parse_one_string (struct obstack *output, char *data, int len,
		  const char *dest_charset, struct type *type)
{
  char *limit;

  limit = data + len;

  while (data < limit)
    {
      char *p = data;

      /* Look for next escape, or the end of the input.  */
      while (p < limit && *p != '\\')
	++p;
      /* If we saw a run of characters, convert them all.  */
      if (p > data)
	convert_between_encodings (host_charset (), dest_charset,
				   (gdb_byte *) data, p - data, 1,
				   output, translit_none);
      /* If we saw an escape, convert it.  */
      if (p < limit)
	p = convert_escape (type, dest_charset, p, limit, output);
      data = p;
    }
}

/* Expression evaluator for the C language family.  Most operations
   are delegated to evaluate_subexp_standard; see that function for a
   description of the arguments.  */

struct value *
evaluate_subexp_c (struct type *expect_type, struct expression *exp,
		   int *pos, enum noside noside)
{
  enum exp_opcode op = exp->elts[*pos].opcode;

  switch (op)
    {
    case OP_STRING:
      {
	int oplen, limit;
	struct type *type;
	struct value *result;
	c_string_type dest_type;
	const char *dest_charset;
	int satisfy_expected = 0;

	auto_obstack output;

	++*pos;
	oplen = longest_to_int (exp->elts[*pos].longconst);

	++*pos;
	limit = *pos + BYTES_TO_EXP_ELEM (oplen + 1);
	dest_type = ((enum c_string_type_values)
		     longest_to_int (exp->elts[*pos].longconst));
	switch (dest_type & ~C_CHAR)
	  {
	  case C_STRING:
	    type = language_string_char_type (exp->language_defn,
					      exp->gdbarch);
	    break;
	  case C_WIDE_STRING:
	    type = lookup_typename (exp->language_defn, exp->gdbarch,
				    "wchar_t", NULL, 0);
	    break;
	  case C_STRING_16:
	    type = lookup_typename (exp->language_defn, exp->gdbarch,
				    "char16_t", NULL, 0);
	    break;
	  case C_STRING_32:
	    type = lookup_typename (exp->language_defn, exp->gdbarch,
				    "char32_t", NULL, 0);
	    break;
	  default:
	    internal_error (__FILE__, __LINE__, _("unhandled c_string_type"));
	  }

	/* Ensure TYPE_LENGTH is valid for TYPE.  */
	check_typedef (type);

	/* If the caller expects an array of some integral type,
	   satisfy them.  If something odder is expected, rely on the
	   caller to cast.  */
	if (expect_type && TYPE_CODE (expect_type) == TYPE_CODE_ARRAY)
	  {
	    struct type *element_type
	      = check_typedef (TYPE_TARGET_TYPE (expect_type));

	    if (TYPE_CODE (element_type) == TYPE_CODE_INT
		|| TYPE_CODE (element_type) == TYPE_CODE_CHAR)
	      {
		type = element_type;
		satisfy_expected = 1;
	      }
	  }

	dest_charset = charset_for_string_type (dest_type, exp->gdbarch);

	++*pos;
	while (*pos < limit)
	  {
	    int len;

	    len = longest_to_int (exp->elts[*pos].longconst);

	    ++*pos;
	    if (noside != EVAL_SKIP)
	      parse_one_string (&output, &exp->elts[*pos].string, len,
				dest_charset, type);
	    *pos += BYTES_TO_EXP_ELEM (len);
	  }

	/* Skip the trailing length and opcode.  */
	*pos += 2;

	if (noside == EVAL_SKIP)
	  {
	    /* Return a dummy value of the appropriate type.  */
	    if (expect_type != NULL)
	      result = allocate_value (expect_type);
	    else if ((dest_type & C_CHAR) != 0)
	      result = allocate_value (type);
	    else
	      result = value_cstring ("", 0, type);
	    return result;
	  }

	if ((dest_type & C_CHAR) != 0)
	  {
	    LONGEST value;

	    if (obstack_object_size (&output) != TYPE_LENGTH (type))
	      error (_("Could not convert character "
		       "constant to target character set"));
	    value = unpack_long (type, (gdb_byte *) obstack_base (&output));
	    result = value_from_longest (type, value);
	  }
	else
	  {
	    int i;

	    /* Write the terminating character.  */
	    for (i = 0; i < TYPE_LENGTH (type); ++i)
	      obstack_1grow (&output, 0);

	    if (satisfy_expected)
	      {
		LONGEST low_bound, high_bound;
		int element_size = TYPE_LENGTH (type);

		if (get_discrete_bounds (TYPE_INDEX_TYPE (expect_type),
					 &low_bound, &high_bound) < 0)
		  {
		    low_bound = 0;
		    high_bound = (TYPE_LENGTH (expect_type) / element_size) - 1;
		  }
		if (obstack_object_size (&output) / element_size
		    > (high_bound - low_bound + 1))
		  error (_("Too many array elements"));

		result = allocate_value (expect_type);
		memcpy (value_contents_raw (result), obstack_base (&output),
			obstack_object_size (&output));
	      }
	    else
	      result = value_cstring ((const char *) obstack_base (&output),
				      obstack_object_size (&output),
				      type);
	  }
	return result;
      }
      break;

    default:
      break;
    }
  return evaluate_subexp_standard (expect_type, exp, pos, noside);
}

/* la_watch_location_expression for C.  */

gdb::unique_xmalloc_ptr<char>
c_watch_location_expression (struct type *type, CORE_ADDR addr)
{
  type = check_typedef (TYPE_TARGET_TYPE (check_typedef (type)));
  std::string name = type_to_string (type);
  return gdb::unique_xmalloc_ptr<char>
    (xstrprintf ("* (%s *) %s", name.c_str (), core_addr_to_string (addr)));
}

/* See c-lang.h.  */

bool
c_is_string_type_p (struct type *type)
{
  type = check_typedef (type);
  while (TYPE_CODE (type) == TYPE_CODE_REF)
    {
      type = TYPE_TARGET_TYPE (type);
      type = check_typedef (type);
    }

  switch (TYPE_CODE (type))
    {
    case TYPE_CODE_ARRAY:
      {
	/* See if target type looks like a string.  */
	struct type *array_target_type = TYPE_TARGET_TYPE (type);
	return (TYPE_LENGTH (type) > 0
		&& TYPE_LENGTH (array_target_type) > 0
		&& c_textual_element_type (array_target_type, 0));
      }
    case TYPE_CODE_STRING:
      return true;
    case TYPE_CODE_PTR:
      {
	struct type *element_type = TYPE_TARGET_TYPE (type);
	return c_textual_element_type (element_type, 0);
      }
    default:
      break;
    }

  return false;
}


/* Table mapping opcodes into strings for printing operators
   and precedences of the operators.  */

const struct op_print c_op_print_tab[] =
{
  {",", BINOP_COMMA, PREC_COMMA, 0},
  {"=", BINOP_ASSIGN, PREC_ASSIGN, 1},
  {"||", BINOP_LOGICAL_OR, PREC_LOGICAL_OR, 0},
  {"&&", BINOP_LOGICAL_AND, PREC_LOGICAL_AND, 0},
  {"|", BINOP_BITWISE_IOR, PREC_BITWISE_IOR, 0},
  {"^", BINOP_BITWISE_XOR, PREC_BITWISE_XOR, 0},
  {"&", BINOP_BITWISE_AND, PREC_BITWISE_AND, 0},
  {"==", BINOP_EQUAL, PREC_EQUAL, 0},
  {"!=", BINOP_NOTEQUAL, PREC_EQUAL, 0},
  {"<=", BINOP_LEQ, PREC_ORDER, 0},
  {">=", BINOP_GEQ, PREC_ORDER, 0},
  {">", BINOP_GTR, PREC_ORDER, 0},
  {"<", BINOP_LESS, PREC_ORDER, 0},
  {">>", BINOP_RSH, PREC_SHIFT, 0},
  {"<<", BINOP_LSH, PREC_SHIFT, 0},
  {"+", BINOP_ADD, PREC_ADD, 0},
  {"-", BINOP_SUB, PREC_ADD, 0},
  {"*", BINOP_MUL, PREC_MUL, 0},
  {"/", BINOP_DIV, PREC_MUL, 0},
  {"%", BINOP_REM, PREC_MUL, 0},
  {"@", BINOP_REPEAT, PREC_REPEAT, 0},
  {"+", UNOP_PLUS, PREC_PREFIX, 0},
  {"-", UNOP_NEG, PREC_PREFIX, 0},
  {"!", UNOP_LOGICAL_NOT, PREC_PREFIX, 0},
  {"~", UNOP_COMPLEMENT, PREC_PREFIX, 0},
  {"*", UNOP_IND, PREC_PREFIX, 0},
  {"&", UNOP_ADDR, PREC_PREFIX, 0},
  {"sizeof ", UNOP_SIZEOF, PREC_PREFIX, 0},
  {"alignof ", UNOP_ALIGNOF, PREC_PREFIX, 0},
  {"++", UNOP_PREINCREMENT, PREC_PREFIX, 0},
  {"--", UNOP_PREDECREMENT, PREC_PREFIX, 0},
  {NULL, OP_NULL, PREC_PREFIX, 0}
};

enum c_primitive_types {
  c_primitive_type_int,
  c_primitive_type_long,
  c_primitive_type_short,
  c_primitive_type_char,
  c_primitive_type_float,
  c_primitive_type_double,
  c_primitive_type_void,
  c_primitive_type_long_long,
  c_primitive_type_signed_char,
  c_primitive_type_unsigned_char,
  c_primitive_type_unsigned_short,
  c_primitive_type_unsigned_int,
  c_primitive_type_unsigned_long,
  c_primitive_type_unsigned_long_long,
  c_primitive_type_long_double,
  c_primitive_type_complex,
  c_primitive_type_double_complex,
  c_primitive_type_decfloat,
  c_primitive_type_decdouble,
  c_primitive_type_declong,
  nr_c_primitive_types
};

void
c_language_arch_info (struct gdbarch *gdbarch,
		      struct language_arch_info *lai)
{
  const struct builtin_type *builtin = builtin_type (gdbarch);

  lai->string_char_type = builtin->builtin_char;
  lai->primitive_type_vector
    = GDBARCH_OBSTACK_CALLOC (gdbarch, nr_c_primitive_types + 1,
			      struct type *);
  lai->primitive_type_vector [c_primitive_type_int] = builtin->builtin_int;
  lai->primitive_type_vector [c_primitive_type_long] = builtin->builtin_long;
  lai->primitive_type_vector [c_primitive_type_short] = builtin->builtin_short;
  lai->primitive_type_vector [c_primitive_type_char] = builtin->builtin_char;
  lai->primitive_type_vector [c_primitive_type_float] = builtin->builtin_float;
  lai->primitive_type_vector [c_primitive_type_double] = builtin->builtin_double;
  lai->primitive_type_vector [c_primitive_type_void] = builtin->builtin_void;
  lai->primitive_type_vector [c_primitive_type_long_long] = builtin->builtin_long_long;
  lai->primitive_type_vector [c_primitive_type_signed_char] = builtin->builtin_signed_char;
  lai->primitive_type_vector [c_primitive_type_unsigned_char] = builtin->builtin_unsigned_char;
  lai->primitive_type_vector [c_primitive_type_unsigned_short] = builtin->builtin_unsigned_short;
  lai->primitive_type_vector [c_primitive_type_unsigned_int] = builtin->builtin_unsigned_int;
  lai->primitive_type_vector [c_primitive_type_unsigned_long] = builtin->builtin_unsigned_long;
  lai->primitive_type_vector [c_primitive_type_unsigned_long_long] = builtin->builtin_unsigned_long_long;
  lai->primitive_type_vector [c_primitive_type_long_double] = builtin->builtin_long_double;
  lai->primitive_type_vector [c_primitive_type_complex] = builtin->builtin_complex;
  lai->primitive_type_vector [c_primitive_type_double_complex] = builtin->builtin_double_complex;
  lai->primitive_type_vector [c_primitive_type_decfloat] = builtin->builtin_decfloat;
  lai->primitive_type_vector [c_primitive_type_decdouble] = builtin->builtin_decdouble;
  lai->primitive_type_vector [c_primitive_type_declong] = builtin->builtin_declong;

  lai->bool_type_default = builtin->builtin_int;
}

const struct exp_descriptor exp_descriptor_c = 
{
  print_subexp_standard,
  operator_length_standard,
  operator_check_standard,
  op_name_standard,
  dump_subexp_body_standard,
  evaluate_subexp_c
};

static const char *c_extensions[] =
{
  ".c", NULL
};

extern const struct language_defn c_language_defn =
{
  "c",				/* Language name */
  "C",
  language_c,
  range_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  c_extensions,
  &exp_descriptor_c,
  c_parse,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  default_read_var_value,	/* la_read_var_value */
  NULL,				/* Language specific skip_trampoline */
  NULL,				/* name_of_this */
  true,				/* la_store_sym_names_in_linkage_form_p */
  basic_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  basic_lookup_transparent_type,/* lookup_transparent_type */
  NULL,				/* Language specific symbol demangler */
  NULL,
  NULL,				/* Language specific
				   class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_collect_symbol_completion_matches,
  c_language_arch_info,
  default_print_array_index,
  default_pass_by_reference,
  c_watch_location_expression,
  NULL,				/* la_get_symbol_name_matcher */
  iterate_over_symbols,
  default_search_name_hash,
  &c_varobj_ops,
  c_get_compile_context,
  c_compute_program,
  c_is_string_type_p,
  "{...}"			/* la_struct_too_deep_ellipsis */
};

enum cplus_primitive_types {
  cplus_primitive_type_int,
  cplus_primitive_type_long,
  cplus_primitive_type_short,
  cplus_primitive_type_char,
  cplus_primitive_type_float,
  cplus_primitive_type_double,
  cplus_primitive_type_void,
  cplus_primitive_type_long_long,
  cplus_primitive_type_signed_char,
  cplus_primitive_type_unsigned_char,
  cplus_primitive_type_unsigned_short,
  cplus_primitive_type_unsigned_int,
  cplus_primitive_type_unsigned_long,
  cplus_primitive_type_unsigned_long_long,
  cplus_primitive_type_long_double,
  cplus_primitive_type_complex,
  cplus_primitive_type_double_complex,
  cplus_primitive_type_bool,
  cplus_primitive_type_decfloat,
  cplus_primitive_type_decdouble,
  cplus_primitive_type_declong,
  cplus_primitive_type_char16_t,
  cplus_primitive_type_char32_t,
  cplus_primitive_type_wchar_t,
  nr_cplus_primitive_types
};

static void
cplus_language_arch_info (struct gdbarch *gdbarch,
			  struct language_arch_info *lai)
{
  const struct builtin_type *builtin = builtin_type (gdbarch);

  lai->string_char_type = builtin->builtin_char;
  lai->primitive_type_vector
    = GDBARCH_OBSTACK_CALLOC (gdbarch, nr_cplus_primitive_types + 1,
			      struct type *);
  lai->primitive_type_vector [cplus_primitive_type_int]
    = builtin->builtin_int;
  lai->primitive_type_vector [cplus_primitive_type_long]
    = builtin->builtin_long;
  lai->primitive_type_vector [cplus_primitive_type_short]
    = builtin->builtin_short;
  lai->primitive_type_vector [cplus_primitive_type_char]
    = builtin->builtin_char;
  lai->primitive_type_vector [cplus_primitive_type_float]
    = builtin->builtin_float;
  lai->primitive_type_vector [cplus_primitive_type_double]
    = builtin->builtin_double;
  lai->primitive_type_vector [cplus_primitive_type_void]
    = builtin->builtin_void;
  lai->primitive_type_vector [cplus_primitive_type_long_long]
    = builtin->builtin_long_long;
  lai->primitive_type_vector [cplus_primitive_type_signed_char]
    = builtin->builtin_signed_char;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_char]
    = builtin->builtin_unsigned_char;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_short]
    = builtin->builtin_unsigned_short;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_int]
    = builtin->builtin_unsigned_int;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_long]
    = builtin->builtin_unsigned_long;
  lai->primitive_type_vector [cplus_primitive_type_unsigned_long_long]
    = builtin->builtin_unsigned_long_long;
  lai->primitive_type_vector [cplus_primitive_type_long_double]
    = builtin->builtin_long_double;
  lai->primitive_type_vector [cplus_primitive_type_complex]
    = builtin->builtin_complex;
  lai->primitive_type_vector [cplus_primitive_type_double_complex]
    = builtin->builtin_double_complex;
  lai->primitive_type_vector [cplus_primitive_type_bool]
    = builtin->builtin_bool;
  lai->primitive_type_vector [cplus_primitive_type_decfloat]
    = builtin->builtin_decfloat;
  lai->primitive_type_vector [cplus_primitive_type_decdouble]
    = builtin->builtin_decdouble;
  lai->primitive_type_vector [cplus_primitive_type_declong]
    = builtin->builtin_declong;
  lai->primitive_type_vector [cplus_primitive_type_char16_t]
    = builtin->builtin_char16;
  lai->primitive_type_vector [cplus_primitive_type_char32_t]
    = builtin->builtin_char32;
  lai->primitive_type_vector [cplus_primitive_type_wchar_t]
    = builtin->builtin_wchar;

  lai->bool_type_symbol = "bool";
  lai->bool_type_default = builtin->builtin_bool;
}

static const char *cplus_extensions[] =
{
  ".C", ".cc", ".cp", ".cpp", ".cxx", ".c++", NULL
};

extern const struct language_defn cplus_language_defn =
{
  "c++",			/* Language name */
  "C++",
  language_cplus,
  range_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  cplus_extensions,
  &exp_descriptor_c,
  c_parse,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  default_read_var_value,	/* la_read_var_value */
  cplus_skip_trampoline,	/* Language specific skip_trampoline */
  "this",                       /* name_of_this */
  false,			/* la_store_sym_names_in_linkage_form_p */
  cp_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  cp_lookup_transparent_type,   /* lookup_transparent_type */
  gdb_demangle,			/* Language specific symbol demangler */
  gdb_sniff_from_mangled_name,
  cp_class_name_from_physname,  /* Language specific
				   class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_collect_symbol_completion_matches,
  cplus_language_arch_info,
  default_print_array_index,
  cp_pass_by_reference,
  c_watch_location_expression,
  cp_get_symbol_name_matcher,
  iterate_over_symbols,
  cp_search_name_hash,
  &cplus_varobj_ops,
  cplus_get_compile_context,
  cplus_compute_program,
  c_is_string_type_p,
  "{...}"			/* la_struct_too_deep_ellipsis */
};

static const char *asm_extensions[] =
{
  ".s", ".sx", ".S", NULL
};

extern const struct language_defn asm_language_defn =
{
  "asm",			/* Language name */
  "assembly",
  language_asm,
  range_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  asm_extensions,
  &exp_descriptor_c,
  c_parse,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  default_read_var_value,	/* la_read_var_value */
  NULL,				/* Language specific skip_trampoline */
  NULL,				/* name_of_this */
  true,				/* la_store_sym_names_in_linkage_form_p */
  basic_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  basic_lookup_transparent_type,/* lookup_transparent_type */
  NULL,				/* Language specific symbol demangler */
  NULL,
  NULL,				/* Language specific
				   class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_collect_symbol_completion_matches,
  c_language_arch_info, 	/* FIXME: la_language_arch_info.  */
  default_print_array_index,
  default_pass_by_reference,
  c_watch_location_expression,
  NULL,				/* la_get_symbol_name_matcher */
  iterate_over_symbols,
  default_search_name_hash,
  &default_varobj_ops,
  NULL,
  NULL,
  c_is_string_type_p,
  "{...}"			/* la_struct_too_deep_ellipsis */
};

/* The following language_defn does not represent a real language.
   It just provides a minimal support a-la-C that should allow users
   to do some simple operations when debugging applications that use
   a language currently not supported by GDB.  */

extern const struct language_defn minimal_language_defn =
{
  "minimal",			/* Language name */
  "Minimal",
  language_minimal,
  range_check_off,
  case_sensitive_on,
  array_row_major,
  macro_expansion_c,
  NULL,
  &exp_descriptor_c,
  c_parse,
  null_post_parser,
  c_printchar,			/* Print a character constant */
  c_printstr,			/* Function to print string constant */
  c_emit_char,			/* Print a single char */
  c_print_type,			/* Print a type using appropriate syntax */
  c_print_typedef,		/* Print a typedef using appropriate syntax */
  c_val_print,			/* Print a value using appropriate syntax */
  c_value_print,		/* Print a top-level value */
  default_read_var_value,	/* la_read_var_value */
  NULL,				/* Language specific skip_trampoline */
  NULL,				/* name_of_this */
  true,				/* la_store_sym_names_in_linkage_form_p */
  basic_lookup_symbol_nonlocal,	/* lookup_symbol_nonlocal */
  basic_lookup_transparent_type,/* lookup_transparent_type */
  NULL,				/* Language specific symbol demangler */
  NULL,
  NULL,				/* Language specific
				   class_name_from_physname */
  c_op_print_tab,		/* expression operators for printing */
  1,				/* c-style arrays */
  0,				/* String lower bound */
  default_word_break_characters,
  default_collect_symbol_completion_matches,
  c_language_arch_info,
  default_print_array_index,
  default_pass_by_reference,
  c_watch_location_expression,
  NULL,				/* la_get_symbol_name_matcher */
  iterate_over_symbols,
  default_search_name_hash,
  &default_varobj_ops,
  NULL,
  NULL,
  c_is_string_type_p,
  "{...}"			/* la_struct_too_deep_ellipsis */
};
