/* Perform non-arithmetic operations on values, for GDB.

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

#include "defs.h"
#include "symtab.h"
#include "gdbtypes.h"
#include "value.h"
#include "frame.h"
#include "inferior.h"
#include "gdbcore.h"
#include "target.h"
#include "demangle.h"
#include "language.h"
#include "gdbcmd.h"
#include "regcache.h"
#include "cp-abi.h"
#include "block.h"
#include "infcall.h"
#include "dictionary.h"
#include "cp-support.h"
#include "target-float.h"
#include "tracepoint.h"
#include "observable.h"
#include "objfiles.h"
#include "extension.h"
#include "gdbtypes.h"
#include "gdbsupport/byte-vector.h"

/* Local functions.  */

static int typecmp (int staticp, int varargs, int nargs,
		    struct field t1[], struct value *t2[]);

static struct value *search_struct_field (const char *, struct value *, 
					  struct type *, int);

static struct value *search_struct_method (const char *, struct value **,
					   struct value **,
					   LONGEST, int *, struct type *);

static int find_oload_champ_namespace (gdb::array_view<value *> args,
				       const char *, const char *,
				       std::vector<symbol *> *oload_syms,
				       badness_vector *,
				       const int no_adl);

static int find_oload_champ_namespace_loop (gdb::array_view<value *> args,
					    const char *, const char *,
					    int, std::vector<symbol *> *oload_syms,
					    badness_vector *, int *,
					    const int no_adl);

static int find_oload_champ (gdb::array_view<value *> args,
			     size_t num_fns,
			     fn_field *methods,
			     xmethod_worker_up *xmethods,
			     symbol **functions,
			     badness_vector *oload_champ_bv);

static int oload_method_static_p (struct fn_field *, int);

enum oload_classification { STANDARD, NON_STANDARD, INCOMPATIBLE };

static enum oload_classification classify_oload_match
  (const badness_vector &, int, int);

static struct value *value_struct_elt_for_reference (struct type *,
						     int, struct type *,
						     const char *,
						     struct type *,
						     int, enum noside);

static struct value *value_namespace_elt (const struct type *,
					  const char *, int , enum noside);

static struct value *value_maybe_namespace_elt (const struct type *,
						const char *, int,
						enum noside);

static CORE_ADDR allocate_space_in_inferior (int);

static struct value *cast_into_complex (struct type *, struct value *);

bool overload_resolution = false;
static void
show_overload_resolution (struct ui_file *file, int from_tty,
			  struct cmd_list_element *c, 
			  const char *value)
{
  fprintf_filtered (file, _("Overload resolution in evaluating "
			    "C++ functions is %s.\n"),
		    value);
}

/* Find the address of function name NAME in the inferior.  If OBJF_P
   is non-NULL, *OBJF_P will be set to the OBJFILE where the function
   is defined.  */

struct value *
find_function_in_inferior (const char *name, struct objfile **objf_p)
{
  struct block_symbol sym;

  sym = lookup_symbol (name, 0, VAR_DOMAIN, 0);
  if (sym.symbol != NULL)
    {
      if (SYMBOL_CLASS (sym.symbol) != LOC_BLOCK)
	{
	  error (_("\"%s\" exists in this program but is not a function."),
		 name);
	}

      if (objf_p)
	*objf_p = symbol_objfile (sym.symbol);

      return value_of_variable (sym.symbol, sym.block);
    }
  else
    {
      struct bound_minimal_symbol msymbol = 
	lookup_bound_minimal_symbol (name);

      if (msymbol.minsym != NULL)
	{
	  struct objfile *objfile = msymbol.objfile;
	  struct gdbarch *gdbarch = get_objfile_arch (objfile);

	  struct type *type;
	  CORE_ADDR maddr;
	  type = lookup_pointer_type (builtin_type (gdbarch)->builtin_char);
	  type = lookup_function_type (type);
	  type = lookup_pointer_type (type);
	  maddr = BMSYMBOL_VALUE_ADDRESS (msymbol);

	  if (objf_p)
	    *objf_p = objfile;

	  return value_from_pointer (type, maddr);
	}
      else
	{
	  if (!target_has_execution)
	    error (_("evaluation of this expression "
		     "requires the target program to be active"));
	  else
	    error (_("evaluation of this expression requires the "
		     "program to have a function \"%s\"."),
		   name);
	}
    }
}

/* Allocate NBYTES of space in the inferior using the inferior's
   malloc and return a value that is a pointer to the allocated
   space.  */

struct value *
value_allocate_space_in_inferior (int len)
{
  struct objfile *objf;
  struct value *val = find_function_in_inferior ("malloc", &objf);
  struct gdbarch *gdbarch = get_objfile_arch (objf);
  struct value *blocklen;

  blocklen = value_from_longest (builtin_type (gdbarch)->builtin_int, len);
  val = call_function_by_hand (val, NULL, blocklen);
  if (value_logical_not (val))
    {
      if (!target_has_execution)
	error (_("No memory available to program now: "
		 "you need to start the target first"));
      else
	error (_("No memory available to program: call to malloc failed"));
    }
  return val;
}

static CORE_ADDR
allocate_space_in_inferior (int len)
{
  return value_as_long (value_allocate_space_in_inferior (len));
}

/* Cast struct value VAL to type TYPE and return as a value.
   Both type and val must be of TYPE_CODE_STRUCT or TYPE_CODE_UNION
   for this to work.  Typedef to one of the codes is permitted.
   Returns NULL if the cast is neither an upcast nor a downcast.  */

static struct value *
value_cast_structs (struct type *type, struct value *v2)
{
  struct type *t1;
  struct type *t2;
  struct value *v;

  gdb_assert (type != NULL && v2 != NULL);

  t1 = check_typedef (type);
  t2 = check_typedef (value_type (v2));

  /* Check preconditions.  */
  gdb_assert ((TYPE_CODE (t1) == TYPE_CODE_STRUCT
	       || TYPE_CODE (t1) == TYPE_CODE_UNION)
	      && !!"Precondition is that type is of STRUCT or UNION kind.");
  gdb_assert ((TYPE_CODE (t2) == TYPE_CODE_STRUCT
	       || TYPE_CODE (t2) == TYPE_CODE_UNION)
	      && !!"Precondition is that value is of STRUCT or UNION kind");

  if (TYPE_NAME (t1) != NULL
      && TYPE_NAME (t2) != NULL
      && !strcmp (TYPE_NAME (t1), TYPE_NAME (t2)))
    return NULL;

  /* Upcasting: look in the type of the source to see if it contains the
     type of the target as a superclass.  If so, we'll need to
     offset the pointer rather than just change its type.  */
  if (TYPE_NAME (t1) != NULL)
    {
      v = search_struct_field (TYPE_NAME (t1),
			       v2, t2, 1);
      if (v)
	return v;
    }

  /* Downcasting: look in the type of the target to see if it contains the
     type of the source as a superclass.  If so, we'll need to
     offset the pointer rather than just change its type.  */
  if (TYPE_NAME (t2) != NULL)
    {
      /* Try downcasting using the run-time type of the value.  */
      int full, using_enc;
      LONGEST top;
      struct type *real_type;

      real_type = value_rtti_type (v2, &full, &top, &using_enc);
      if (real_type)
	{
	  v = value_full_object (v2, real_type, full, top, using_enc);
	  v = value_at_lazy (real_type, value_address (v));
	  real_type = value_type (v);

	  /* We might be trying to cast to the outermost enclosing
	     type, in which case search_struct_field won't work.  */
	  if (TYPE_NAME (real_type) != NULL
	      && !strcmp (TYPE_NAME (real_type), TYPE_NAME (t1)))
	    return v;

	  v = search_struct_field (TYPE_NAME (t2), v, real_type, 1);
	  if (v)
	    return v;
	}

      /* Try downcasting using information from the destination type
	 T2.  This wouldn't work properly for classes with virtual
	 bases, but those were handled above.  */
      v = search_struct_field (TYPE_NAME (t2),
			       value_zero (t1, not_lval), t1, 1);
      if (v)
	{
	  /* Downcasting is possible (t1 is superclass of v2).  */
	  CORE_ADDR addr2 = value_address (v2);

	  addr2 -= value_address (v) + value_embedded_offset (v);
	  return value_at (type, addr2);
	}
    }

  return NULL;
}

/* Cast one pointer or reference type to another.  Both TYPE and
   the type of ARG2 should be pointer types, or else both should be
   reference types.  If SUBCLASS_CHECK is non-zero, this will force a
   check to see whether TYPE is a superclass of ARG2's type.  If
   SUBCLASS_CHECK is zero, then the subclass check is done only when
   ARG2 is itself non-zero.  Returns the new pointer or reference.  */

struct value *
value_cast_pointers (struct type *type, struct value *arg2,
		     int subclass_check)
{
  struct type *type1 = check_typedef (type);
  struct type *type2 = check_typedef (value_type (arg2));
  struct type *t1 = check_typedef (TYPE_TARGET_TYPE (type1));
  struct type *t2 = check_typedef (TYPE_TARGET_TYPE (type2));

  if (TYPE_CODE (t1) == TYPE_CODE_STRUCT
      && TYPE_CODE (t2) == TYPE_CODE_STRUCT
      && (subclass_check || !value_logical_not (arg2)))
    {
      struct value *v2;

      if (TYPE_IS_REFERENCE (type2))
	v2 = coerce_ref (arg2);
      else
	v2 = value_ind (arg2);
      gdb_assert (TYPE_CODE (check_typedef (value_type (v2)))
		  == TYPE_CODE_STRUCT && !!"Why did coercion fail?");
      v2 = value_cast_structs (t1, v2);
      /* At this point we have what we can have, un-dereference if needed.  */
      if (v2)
	{
	  struct value *v = value_addr (v2);

	  deprecated_set_value_type (v, type);
	  return v;
	}
    }

  /* No superclass found, just change the pointer type.  */
  arg2 = value_copy (arg2);
  deprecated_set_value_type (arg2, type);
  set_value_enclosing_type (arg2, type);
  set_value_pointed_to_offset (arg2, 0);	/* pai: chk_val */
  return arg2;
}

/* Cast value ARG2 to type TYPE and return as a value.
   More general than a C cast: accepts any two types of the same length,
   and if ARG2 is an lvalue it can be cast into anything at all.  */
/* In C++, casts may change pointer or object representations.  */

struct value *
value_cast (struct type *type, struct value *arg2)
{
  enum type_code code1;
  enum type_code code2;
  int scalar;
  struct type *type2;

  int convert_to_boolean = 0;

  if (value_type (arg2) == type)
    return arg2;

  /* Check if we are casting struct reference to struct reference.  */
  if (TYPE_IS_REFERENCE (check_typedef (type)))
    {
      /* We dereference type; then we recurse and finally
         we generate value of the given reference.  Nothing wrong with 
	 that.  */
      struct type *t1 = check_typedef (type);
      struct type *dereftype = check_typedef (TYPE_TARGET_TYPE (t1));
      struct value *val = value_cast (dereftype, arg2);

      return value_ref (val, TYPE_CODE (t1));
    }

  if (TYPE_IS_REFERENCE (check_typedef (value_type (arg2))))
    /* We deref the value and then do the cast.  */
    return value_cast (type, coerce_ref (arg2)); 

  /* Strip typedefs / resolve stubs in order to get at the type's
     code/length, but remember the original type, to use as the
     resulting type of the cast, in case it was a typedef.  */
  struct type *to_type = type;

  type = check_typedef (type);
  code1 = TYPE_CODE (type);
  arg2 = coerce_ref (arg2);
  type2 = check_typedef (value_type (arg2));

  /* You can't cast to a reference type.  See value_cast_pointers
     instead.  */
  gdb_assert (!TYPE_IS_REFERENCE (type));

  /* A cast to an undetermined-length array_type, such as 
     (TYPE [])OBJECT, is treated like a cast to (TYPE [N])OBJECT,
     where N is sizeof(OBJECT)/sizeof(TYPE).  */
  if (code1 == TYPE_CODE_ARRAY)
    {
      struct type *element_type = TYPE_TARGET_TYPE (type);
      unsigned element_length = TYPE_LENGTH (check_typedef (element_type));

      if (element_length > 0 && TYPE_ARRAY_UPPER_BOUND_IS_UNDEFINED (type))
	{
	  struct type *range_type = TYPE_INDEX_TYPE (type);
	  int val_length = TYPE_LENGTH (type2);
	  LONGEST low_bound, high_bound, new_length;

	  if (get_discrete_bounds (range_type, &low_bound, &high_bound) < 0)
	    low_bound = 0, high_bound = 0;
	  new_length = val_length / element_length;
	  if (val_length % element_length != 0)
	    warning (_("array element type size does not "
		       "divide object size in cast"));
	  /* FIXME-type-allocation: need a way to free this type when
	     we are done with it.  */
	  range_type = create_static_range_type (NULL,
						 TYPE_TARGET_TYPE (range_type),
						 low_bound,
						 new_length + low_bound - 1);
	  deprecated_set_value_type (arg2, 
				     create_array_type (NULL,
							element_type, 
							range_type));
	  return arg2;
	}
    }

  if (current_language->c_style_arrays
      && TYPE_CODE (type2) == TYPE_CODE_ARRAY
      && !TYPE_VECTOR (type2))
    arg2 = value_coerce_array (arg2);

  if (TYPE_CODE (type2) == TYPE_CODE_FUNC)
    arg2 = value_coerce_function (arg2);

  type2 = check_typedef (value_type (arg2));
  code2 = TYPE_CODE (type2);

  if (code1 == TYPE_CODE_COMPLEX)
    return cast_into_complex (to_type, arg2);
  if (code1 == TYPE_CODE_BOOL)
    {
      code1 = TYPE_CODE_INT;
      convert_to_boolean = 1;
    }
  if (code1 == TYPE_CODE_CHAR)
    code1 = TYPE_CODE_INT;
  if (code2 == TYPE_CODE_BOOL || code2 == TYPE_CODE_CHAR)
    code2 = TYPE_CODE_INT;

  scalar = (code2 == TYPE_CODE_INT || code2 == TYPE_CODE_FLT
	    || code2 == TYPE_CODE_DECFLOAT || code2 == TYPE_CODE_ENUM
	    || code2 == TYPE_CODE_RANGE);

  if ((code1 == TYPE_CODE_STRUCT || code1 == TYPE_CODE_UNION)
      && (code2 == TYPE_CODE_STRUCT || code2 == TYPE_CODE_UNION)
      && TYPE_NAME (type) != 0)
    {
      struct value *v = value_cast_structs (to_type, arg2);

      if (v)
	return v;
    }

  if (is_floating_type (type) && scalar)
    {
      if (is_floating_value (arg2))
	{
	  struct value *v = allocate_value (to_type);
	  target_float_convert (value_contents (arg2), type2,
				value_contents_raw (v), type);
	  return v;
	}

      /* The only option left is an integral type.  */
      if (TYPE_UNSIGNED (type2))
	return value_from_ulongest (to_type, value_as_long (arg2));
      else
	return value_from_longest (to_type, value_as_long (arg2));
    }
  else if ((code1 == TYPE_CODE_INT || code1 == TYPE_CODE_ENUM
	    || code1 == TYPE_CODE_RANGE)
	   && (scalar || code2 == TYPE_CODE_PTR
	       || code2 == TYPE_CODE_MEMBERPTR))
    {
      LONGEST longest;

      /* When we cast pointers to integers, we mustn't use
         gdbarch_pointer_to_address to find the address the pointer
         represents, as value_as_long would.  GDB should evaluate
         expressions just as the compiler would --- and the compiler
         sees a cast as a simple reinterpretation of the pointer's
         bits.  */
      if (code2 == TYPE_CODE_PTR)
        longest = extract_unsigned_integer
		    (value_contents (arg2), TYPE_LENGTH (type2),
		     type_byte_order (type2));
      else
        longest = value_as_long (arg2);
      return value_from_longest (to_type, convert_to_boolean ?
				 (LONGEST) (longest ? 1 : 0) : longest);
    }
  else if (code1 == TYPE_CODE_PTR && (code2 == TYPE_CODE_INT  
				      || code2 == TYPE_CODE_ENUM 
				      || code2 == TYPE_CODE_RANGE))
    {
      /* TYPE_LENGTH (type) is the length of a pointer, but we really
	 want the length of an address! -- we are really dealing with
	 addresses (i.e., gdb representations) not pointers (i.e.,
	 target representations) here.

	 This allows things like "print *(int *)0x01000234" to work
	 without printing a misleading message -- which would
	 otherwise occur when dealing with a target having two byte
	 pointers and four byte addresses.  */

      int addr_bit = gdbarch_addr_bit (get_type_arch (type2));
      LONGEST longest = value_as_long (arg2);

      if (addr_bit < sizeof (LONGEST) * HOST_CHAR_BIT)
	{
	  if (longest >= ((LONGEST) 1 << addr_bit)
	      || longest <= -((LONGEST) 1 << addr_bit))
	    warning (_("value truncated"));
	}
      return value_from_longest (to_type, longest);
    }
  else if (code1 == TYPE_CODE_METHODPTR && code2 == TYPE_CODE_INT
	   && value_as_long (arg2) == 0)
    {
      struct value *result = allocate_value (to_type);

      cplus_make_method_ptr (to_type, value_contents_writeable (result), 0, 0);
      return result;
    }
  else if (code1 == TYPE_CODE_MEMBERPTR && code2 == TYPE_CODE_INT
	   && value_as_long (arg2) == 0)
    {
      /* The Itanium C++ ABI represents NULL pointers to members as
	 minus one, instead of biasing the normal case.  */
      return value_from_longest (to_type, -1);
    }
  else if (code1 == TYPE_CODE_ARRAY && TYPE_VECTOR (type)
	   && code2 == TYPE_CODE_ARRAY && TYPE_VECTOR (type2)
	   && TYPE_LENGTH (type) != TYPE_LENGTH (type2))
    error (_("Cannot convert between vector values of different sizes"));
  else if (code1 == TYPE_CODE_ARRAY && TYPE_VECTOR (type) && scalar
	   && TYPE_LENGTH (type) != TYPE_LENGTH (type2))
    error (_("can only cast scalar to vector of same size"));
  else if (code1 == TYPE_CODE_VOID)
    {
      return value_zero (to_type, not_lval);
    }
  else if (TYPE_LENGTH (type) == TYPE_LENGTH (type2))
    {
      if (code1 == TYPE_CODE_PTR && code2 == TYPE_CODE_PTR)
	return value_cast_pointers (to_type, arg2, 0);

      arg2 = value_copy (arg2);
      deprecated_set_value_type (arg2, to_type);
      set_value_enclosing_type (arg2, to_type);
      set_value_pointed_to_offset (arg2, 0);	/* pai: chk_val */
      return arg2;
    }
  else if (VALUE_LVAL (arg2) == lval_memory)
    return value_at_lazy (to_type, value_address (arg2));
  else
    {
      if (current_language->la_language == language_ada)
	error (_("Invalid type conversion."));
      error (_("Invalid cast."));
    }
}

/* The C++ reinterpret_cast operator.  */

struct value *
value_reinterpret_cast (struct type *type, struct value *arg)
{
  struct value *result;
  struct type *real_type = check_typedef (type);
  struct type *arg_type, *dest_type;
  int is_ref = 0;
  enum type_code dest_code, arg_code;

  /* Do reference, function, and array conversion.  */
  arg = coerce_array (arg);

  /* Attempt to preserve the type the user asked for.  */
  dest_type = type;

  /* If we are casting to a reference type, transform
     reinterpret_cast<T&[&]>(V) to *reinterpret_cast<T*>(&V).  */
  if (TYPE_IS_REFERENCE (real_type))
    {
      is_ref = 1;
      arg = value_addr (arg);
      dest_type = lookup_pointer_type (TYPE_TARGET_TYPE (dest_type));
      real_type = lookup_pointer_type (real_type);
    }

  arg_type = value_type (arg);

  dest_code = TYPE_CODE (real_type);
  arg_code = TYPE_CODE (arg_type);

  /* We can convert pointer types, or any pointer type to int, or int
     type to pointer.  */
  if ((dest_code == TYPE_CODE_PTR && arg_code == TYPE_CODE_INT)
      || (dest_code == TYPE_CODE_INT && arg_code == TYPE_CODE_PTR)
      || (dest_code == TYPE_CODE_METHODPTR && arg_code == TYPE_CODE_INT)
      || (dest_code == TYPE_CODE_INT && arg_code == TYPE_CODE_METHODPTR)
      || (dest_code == TYPE_CODE_MEMBERPTR && arg_code == TYPE_CODE_INT)
      || (dest_code == TYPE_CODE_INT && arg_code == TYPE_CODE_MEMBERPTR)
      || (dest_code == arg_code
	  && (dest_code == TYPE_CODE_PTR
	      || dest_code == TYPE_CODE_METHODPTR
	      || dest_code == TYPE_CODE_MEMBERPTR)))
    result = value_cast (dest_type, arg);
  else
    error (_("Invalid reinterpret_cast"));

  if (is_ref)
    result = value_cast (type, value_ref (value_ind (result),
                                          TYPE_CODE (type)));

  return result;
}

/* A helper for value_dynamic_cast.  This implements the first of two
   runtime checks: we iterate over all the base classes of the value's
   class which are equal to the desired class; if only one of these
   holds the value, then it is the answer.  */

static int
dynamic_cast_check_1 (struct type *desired_type,
		      const gdb_byte *valaddr,
		      LONGEST embedded_offset,
		      CORE_ADDR address,
		      struct value *val,
		      struct type *search_type,
		      CORE_ADDR arg_addr,
		      struct type *arg_type,
		      struct value **result)
{
  int i, result_count = 0;

  for (i = 0; i < TYPE_N_BASECLASSES (search_type) && result_count < 2; ++i)
    {
      LONGEST offset = baseclass_offset (search_type, i, valaddr,
					 embedded_offset,
					 address, val);

      if (class_types_same_p (desired_type, TYPE_BASECLASS (search_type, i)))
	{
	  if (address + embedded_offset + offset >= arg_addr
	      && address + embedded_offset + offset < arg_addr + TYPE_LENGTH (arg_type))
	    {
	      ++result_count;
	      if (!*result)
		*result = value_at_lazy (TYPE_BASECLASS (search_type, i),
					 address + embedded_offset + offset);
	    }
	}
      else
	result_count += dynamic_cast_check_1 (desired_type,
					      valaddr,
					      embedded_offset + offset,
					      address, val,
					      TYPE_BASECLASS (search_type, i),
					      arg_addr,
					      arg_type,
					      result);
    }

  return result_count;
}

/* A helper for value_dynamic_cast.  This implements the second of two
   runtime checks: we look for a unique public sibling class of the
   argument's declared class.  */

static int
dynamic_cast_check_2 (struct type *desired_type,
		      const gdb_byte *valaddr,
		      LONGEST embedded_offset,
		      CORE_ADDR address,
		      struct value *val,
		      struct type *search_type,
		      struct value **result)
{
  int i, result_count = 0;

  for (i = 0; i < TYPE_N_BASECLASSES (search_type) && result_count < 2; ++i)
    {
      LONGEST offset;

      if (! BASETYPE_VIA_PUBLIC (search_type, i))
	continue;

      offset = baseclass_offset (search_type, i, valaddr, embedded_offset,
				 address, val);
      if (class_types_same_p (desired_type, TYPE_BASECLASS (search_type, i)))
	{
	  ++result_count;
	  if (*result == NULL)
	    *result = value_at_lazy (TYPE_BASECLASS (search_type, i),
				     address + embedded_offset + offset);
	}
      else
	result_count += dynamic_cast_check_2 (desired_type,
					      valaddr,
					      embedded_offset + offset,
					      address, val,
					      TYPE_BASECLASS (search_type, i),
					      result);
    }

  return result_count;
}

/* The C++ dynamic_cast operator.  */

struct value *
value_dynamic_cast (struct type *type, struct value *arg)
{
  int full, using_enc;
  LONGEST top;
  struct type *resolved_type = check_typedef (type);
  struct type *arg_type = check_typedef (value_type (arg));
  struct type *class_type, *rtti_type;
  struct value *result, *tem, *original_arg = arg;
  CORE_ADDR addr;
  int is_ref = TYPE_IS_REFERENCE (resolved_type);

  if (TYPE_CODE (resolved_type) != TYPE_CODE_PTR
      && !TYPE_IS_REFERENCE (resolved_type))
    error (_("Argument to dynamic_cast must be a pointer or reference type"));
  if (TYPE_CODE (TYPE_TARGET_TYPE (resolved_type)) != TYPE_CODE_VOID
      && TYPE_CODE (TYPE_TARGET_TYPE (resolved_type)) != TYPE_CODE_STRUCT)
    error (_("Argument to dynamic_cast must be pointer to class or `void *'"));

  class_type = check_typedef (TYPE_TARGET_TYPE (resolved_type));
  if (TYPE_CODE (resolved_type) == TYPE_CODE_PTR)
    {
      if (TYPE_CODE (arg_type) != TYPE_CODE_PTR
	  && ! (TYPE_CODE (arg_type) == TYPE_CODE_INT
		&& value_as_long (arg) == 0))
	error (_("Argument to dynamic_cast does not have pointer type"));
      if (TYPE_CODE (arg_type) == TYPE_CODE_PTR)
	{
	  arg_type = check_typedef (TYPE_TARGET_TYPE (arg_type));
	  if (TYPE_CODE (arg_type) != TYPE_CODE_STRUCT)
	    error (_("Argument to dynamic_cast does "
		     "not have pointer to class type"));
	}

      /* Handle NULL pointers.  */
      if (value_as_long (arg) == 0)
	return value_zero (type, not_lval);

      arg = value_ind (arg);
    }
  else
    {
      if (TYPE_CODE (arg_type) != TYPE_CODE_STRUCT)
	error (_("Argument to dynamic_cast does not have class type"));
    }

  /* If the classes are the same, just return the argument.  */
  if (class_types_same_p (class_type, arg_type))
    return value_cast (type, arg);

  /* If the target type is a unique base class of the argument's
     declared type, just cast it.  */
  if (is_ancestor (class_type, arg_type))
    {
      if (is_unique_ancestor (class_type, arg))
	return value_cast (type, original_arg);
      error (_("Ambiguous dynamic_cast"));
    }

  rtti_type = value_rtti_type (arg, &full, &top, &using_enc);
  if (! rtti_type)
    error (_("Couldn't determine value's most derived type for dynamic_cast"));

  /* Compute the most derived object's address.  */
  addr = value_address (arg);
  if (full)
    {
      /* Done.  */
    }
  else if (using_enc)
    addr += top;
  else
    addr += top + value_embedded_offset (arg);

  /* dynamic_cast<void *> means to return a pointer to the
     most-derived object.  */
  if (TYPE_CODE (resolved_type) == TYPE_CODE_PTR
      && TYPE_CODE (TYPE_TARGET_TYPE (resolved_type)) == TYPE_CODE_VOID)
    return value_at_lazy (type, addr);

  tem = value_at (type, addr);
  type = value_type (tem);

  /* The first dynamic check specified in 5.2.7.  */
  if (is_public_ancestor (arg_type, TYPE_TARGET_TYPE (resolved_type)))
    {
      if (class_types_same_p (rtti_type, TYPE_TARGET_TYPE (resolved_type)))
	return tem;
      result = NULL;
      if (dynamic_cast_check_1 (TYPE_TARGET_TYPE (resolved_type),
				value_contents_for_printing (tem),
				value_embedded_offset (tem),
				value_address (tem), tem,
				rtti_type, addr,
				arg_type,
				&result) == 1)
	return value_cast (type,
			   is_ref
			   ? value_ref (result, TYPE_CODE (resolved_type))
			   : value_addr (result));
    }

  /* The second dynamic check specified in 5.2.7.  */
  result = NULL;
  if (is_public_ancestor (arg_type, rtti_type)
      && dynamic_cast_check_2 (TYPE_TARGET_TYPE (resolved_type),
			       value_contents_for_printing (tem),
			       value_embedded_offset (tem),
			       value_address (tem), tem,
			       rtti_type, &result) == 1)
    return value_cast (type,
		       is_ref
		       ? value_ref (result, TYPE_CODE (resolved_type))
		       : value_addr (result));

  if (TYPE_CODE (resolved_type) == TYPE_CODE_PTR)
    return value_zero (type, not_lval);

  error (_("dynamic_cast failed"));
}

/* Create a value of type TYPE that is zero, and return it.  */

struct value *
value_zero (struct type *type, enum lval_type lv)
{
  struct value *val = allocate_value (type);

  VALUE_LVAL (val) = (lv == lval_computed ? not_lval : lv);
  return val;
}

/* Create a not_lval value of numeric type TYPE that is one, and return it.  */

struct value *
value_one (struct type *type)
{
  struct type *type1 = check_typedef (type);
  struct value *val;

  if (is_integral_type (type1) || is_floating_type (type1))
    {
      val = value_from_longest (type, (LONGEST) 1);
    }
  else if (TYPE_CODE (type1) == TYPE_CODE_ARRAY && TYPE_VECTOR (type1))
    {
      struct type *eltype = check_typedef (TYPE_TARGET_TYPE (type1));
      int i;
      LONGEST low_bound, high_bound;
      struct value *tmp;

      if (!get_array_bounds (type1, &low_bound, &high_bound))
	error (_("Could not determine the vector bounds"));

      val = allocate_value (type);
      for (i = 0; i < high_bound - low_bound + 1; i++)
	{
	  tmp = value_one (eltype);
	  memcpy (value_contents_writeable (val) + i * TYPE_LENGTH (eltype),
		  value_contents_all (tmp), TYPE_LENGTH (eltype));
	}
    }
  else
    {
      error (_("Not a numeric type."));
    }

  /* value_one result is never used for assignments to.  */
  gdb_assert (VALUE_LVAL (val) == not_lval);

  return val;
}

/* Helper function for value_at, value_at_lazy, and value_at_lazy_stack.
   The type of the created value may differ from the passed type TYPE.
   Make sure to retrieve the returned values's new type after this call
   e.g. in case the type is a variable length array.  */

static struct value *
get_value_at (struct type *type, CORE_ADDR addr, int lazy)
{
  struct value *val;

  if (TYPE_CODE (check_typedef (type)) == TYPE_CODE_VOID)
    error (_("Attempt to dereference a generic pointer."));

  val = value_from_contents_and_address (type, NULL, addr);

  if (!lazy)
    value_fetch_lazy (val);

  return val;
}

/* Return a value with type TYPE located at ADDR.

   Call value_at only if the data needs to be fetched immediately;
   if we can be 'lazy' and defer the fetch, perhaps indefinitely, call
   value_at_lazy instead.  value_at_lazy simply records the address of
   the data and sets the lazy-evaluation-required flag.  The lazy flag
   is tested in the value_contents macro, which is used if and when
   the contents are actually required.  The type of the created value
   may differ from the passed type TYPE.  Make sure to retrieve the
   returned values's new type after this call e.g. in case the type
   is a variable length array.

   Note: value_at does *NOT* handle embedded offsets; perform such
   adjustments before or after calling it.  */

struct value *
value_at (struct type *type, CORE_ADDR addr)
{
  return get_value_at (type, addr, 0);
}

/* Return a lazy value with type TYPE located at ADDR (cf. value_at).
   The type of the created value may differ from the passed type TYPE.
   Make sure to retrieve the returned values's new type after this call
   e.g. in case the type is a variable length array.  */

struct value *
value_at_lazy (struct type *type, CORE_ADDR addr)
{
  return get_value_at (type, addr, 1);
}

void
read_value_memory (struct value *val, LONGEST bit_offset,
		   int stack, CORE_ADDR memaddr,
		   gdb_byte *buffer, size_t length)
{
  ULONGEST xfered_total = 0;
  struct gdbarch *arch = get_value_arch (val);
  int unit_size = gdbarch_addressable_memory_unit_size (arch);
  enum target_object object;

  object = stack ? TARGET_OBJECT_STACK_MEMORY : TARGET_OBJECT_MEMORY;

  while (xfered_total < length)
    {
      enum target_xfer_status status;
      ULONGEST xfered_partial;

      status = target_xfer_partial (current_top_target (),
				    object, NULL,
				    buffer + xfered_total * unit_size, NULL,
				    memaddr + xfered_total,
				    length - xfered_total,
				    &xfered_partial);

      if (status == TARGET_XFER_OK)
	/* nothing */;
      else if (status == TARGET_XFER_UNAVAILABLE)
	mark_value_bits_unavailable (val, (xfered_total * HOST_CHAR_BIT
					   + bit_offset),
				     xfered_partial * HOST_CHAR_BIT);
      else if (status == TARGET_XFER_EOF)
	memory_error (TARGET_XFER_E_IO, memaddr + xfered_total);
      else
	memory_error (status, memaddr + xfered_total);

      xfered_total += xfered_partial;
      QUIT;
    }
}

/* Store the contents of FROMVAL into the location of TOVAL.
   Return a new value with the location of TOVAL and contents of FROMVAL.  */

struct value *
value_assign (struct value *toval, struct value *fromval)
{
  struct type *type;
  struct value *val;
  struct frame_id old_frame;

  if (!deprecated_value_modifiable (toval))
    error (_("Left operand of assignment is not a modifiable lvalue."));

  toval = coerce_ref (toval);

  type = value_type (toval);
  if (VALUE_LVAL (toval) != lval_internalvar)
    fromval = value_cast (type, fromval);
  else
    {
      /* Coerce arrays and functions to pointers, except for arrays
	 which only live in GDB's storage.  */
      if (!value_must_coerce_to_target (fromval))
	fromval = coerce_array (fromval);
    }

  type = check_typedef (type);

  /* Since modifying a register can trash the frame chain, and
     modifying memory can trash the frame cache, we save the old frame
     and then restore the new frame afterwards.  */
  old_frame = get_frame_id (deprecated_safe_get_selected_frame ());

  switch (VALUE_LVAL (toval))
    {
    case lval_internalvar:
      set_internalvar (VALUE_INTERNALVAR (toval), fromval);
      return value_of_internalvar (get_type_arch (type),
				   VALUE_INTERNALVAR (toval));

    case lval_internalvar_component:
      {
	LONGEST offset = value_offset (toval);

	/* Are we dealing with a bitfield?

	   It is important to mention that `value_parent (toval)' is
	   non-NULL iff `value_bitsize (toval)' is non-zero.  */
	if (value_bitsize (toval))
	  {
	    /* VALUE_INTERNALVAR below refers to the parent value, while
	       the offset is relative to this parent value.  */
	    gdb_assert (value_parent (value_parent (toval)) == NULL);
	    offset += value_offset (value_parent (toval));
	  }

	set_internalvar_component (VALUE_INTERNALVAR (toval),
				   offset,
				   value_bitpos (toval),
				   value_bitsize (toval),
				   fromval);
      }
      break;

    case lval_memory:
      {
	const gdb_byte *dest_buffer;
	CORE_ADDR changed_addr;
	int changed_len;
        gdb_byte buffer[sizeof (LONGEST)];

	if (value_bitsize (toval))
	  {
	    struct value *parent = value_parent (toval);

	    changed_addr = value_address (parent) + value_offset (toval);
	    changed_len = (value_bitpos (toval)
			   + value_bitsize (toval)
			   + HOST_CHAR_BIT - 1)
	      / HOST_CHAR_BIT;

	    /* If we can read-modify-write exactly the size of the
	       containing type (e.g. short or int) then do so.  This
	       is safer for volatile bitfields mapped to hardware
	       registers.  */
	    if (changed_len < TYPE_LENGTH (type)
		&& TYPE_LENGTH (type) <= (int) sizeof (LONGEST)
		&& ((LONGEST) changed_addr % TYPE_LENGTH (type)) == 0)
	      changed_len = TYPE_LENGTH (type);

	    if (changed_len > (int) sizeof (LONGEST))
	      error (_("Can't handle bitfields which "
		       "don't fit in a %d bit word."),
		     (int) sizeof (LONGEST) * HOST_CHAR_BIT);

	    read_memory (changed_addr, buffer, changed_len);
	    modify_field (type, buffer, value_as_long (fromval),
			  value_bitpos (toval), value_bitsize (toval));
	    dest_buffer = buffer;
	  }
	else
	  {
	    changed_addr = value_address (toval);
	    changed_len = type_length_units (type);
	    dest_buffer = value_contents (fromval);
	  }

	write_memory_with_notification (changed_addr, dest_buffer, changed_len);
      }
      break;

    case lval_register:
      {
	struct frame_info *frame;
	struct gdbarch *gdbarch;
	int value_reg;

	/* Figure out which frame this is in currently.
	
	   We use VALUE_FRAME_ID for obtaining the value's frame id instead of
	   VALUE_NEXT_FRAME_ID due to requiring a frame which may be passed to
	   put_frame_register_bytes() below.  That function will (eventually)
	   perform the necessary unwind operation by first obtaining the next
	   frame.  */
	frame = frame_find_by_id (VALUE_FRAME_ID (toval));

	value_reg = VALUE_REGNUM (toval);

	if (!frame)
	  error (_("Value being assigned to is no longer active."));

	gdbarch = get_frame_arch (frame);

	if (value_bitsize (toval))
	  {
	    struct value *parent = value_parent (toval);
	    LONGEST offset = value_offset (parent) + value_offset (toval);
	    int changed_len;
	    gdb_byte buffer[sizeof (LONGEST)];
	    int optim, unavail;

	    changed_len = (value_bitpos (toval)
			   + value_bitsize (toval)
			   + HOST_CHAR_BIT - 1)
			  / HOST_CHAR_BIT;

	    if (changed_len > (int) sizeof (LONGEST))
	      error (_("Can't handle bitfields which "
		       "don't fit in a %d bit word."),
		     (int) sizeof (LONGEST) * HOST_CHAR_BIT);

	    if (!get_frame_register_bytes (frame, value_reg, offset,
					   changed_len, buffer,
					   &optim, &unavail))
	      {
		if (optim)
		  throw_error (OPTIMIZED_OUT_ERROR,
			       _("value has been optimized out"));
		if (unavail)
		  throw_error (NOT_AVAILABLE_ERROR,
			       _("value is not available"));
	      }

	    modify_field (type, buffer, value_as_long (fromval),
			  value_bitpos (toval), value_bitsize (toval));

	    put_frame_register_bytes (frame, value_reg, offset,
				      changed_len, buffer);
	  }
	else
	  {
	    if (gdbarch_convert_register_p (gdbarch, VALUE_REGNUM (toval),
					    type))
	      {
		/* If TOVAL is a special machine register requiring
		   conversion of program values to a special raw
		   format.  */
		gdbarch_value_to_register (gdbarch, frame,
					   VALUE_REGNUM (toval), type,
					   value_contents (fromval));
	      }
	    else
	      {
		put_frame_register_bytes (frame, value_reg,
					  value_offset (toval),
					  TYPE_LENGTH (type),
					  value_contents (fromval));
	      }
	  }

	gdb::observers::register_changed.notify (frame, value_reg);
	break;
      }

    case lval_computed:
      {
	const struct lval_funcs *funcs = value_computed_funcs (toval);

	if (funcs->write != NULL)
	  {
	    funcs->write (toval, fromval);
	    break;
	  }
      }
      /* Fall through.  */

    default:
      error (_("Left operand of assignment is not an lvalue."));
    }

  /* Assigning to the stack pointer, frame pointer, and other
     (architecture and calling convention specific) registers may
     cause the frame cache and regcache to be out of date.  Assigning to memory
     also can.  We just do this on all assignments to registers or
     memory, for simplicity's sake; I doubt the slowdown matters.  */
  switch (VALUE_LVAL (toval))
    {
    case lval_memory:
    case lval_register:
    case lval_computed:

      gdb::observers::target_changed.notify (current_top_target ());

      /* Having destroyed the frame cache, restore the selected
	 frame.  */

      /* FIXME: cagney/2002-11-02: There has to be a better way of
	 doing this.  Instead of constantly saving/restoring the
	 frame.  Why not create a get_selected_frame() function that,
	 having saved the selected frame's ID can automatically
	 re-find the previously selected frame automatically.  */

      {
	struct frame_info *fi = frame_find_by_id (old_frame);

	if (fi != NULL)
	  select_frame (fi);
      }

      break;
    default:
      break;
    }
  
  /* If the field does not entirely fill a LONGEST, then zero the sign
     bits.  If the field is signed, and is negative, then sign
     extend.  */
  if ((value_bitsize (toval) > 0)
      && (value_bitsize (toval) < 8 * (int) sizeof (LONGEST)))
    {
      LONGEST fieldval = value_as_long (fromval);
      LONGEST valmask = (((ULONGEST) 1) << value_bitsize (toval)) - 1;

      fieldval &= valmask;
      if (!TYPE_UNSIGNED (type) 
	  && (fieldval & (valmask ^ (valmask >> 1))))
	fieldval |= ~valmask;

      fromval = value_from_longest (type, fieldval);
    }

  /* The return value is a copy of TOVAL so it shares its location
     information, but its contents are updated from FROMVAL.  This
     implies the returned value is not lazy, even if TOVAL was.  */
  val = value_copy (toval);
  set_value_lazy (val, 0);
  memcpy (value_contents_raw (val), value_contents (fromval),
	  TYPE_LENGTH (type));

  /* We copy over the enclosing type and pointed-to offset from FROMVAL
     in the case of pointer types.  For object types, the enclosing type
     and embedded offset must *not* be copied: the target object refered
     to by TOVAL retains its original dynamic type after assignment.  */
  if (TYPE_CODE (type) == TYPE_CODE_PTR)
    {
      set_value_enclosing_type (val, value_enclosing_type (fromval));
      set_value_pointed_to_offset (val, value_pointed_to_offset (fromval));
    }

  return val;
}

/* Extend a value VAL to COUNT repetitions of its type.  */

struct value *
value_repeat (struct value *arg1, int count)
{
  struct value *val;

  if (VALUE_LVAL (arg1) != lval_memory)
    error (_("Only values in memory can be extended with '@'."));
  if (count < 1)
    error (_("Invalid number %d of repetitions."), count);

  val = allocate_repeat_value (value_enclosing_type (arg1), count);

  VALUE_LVAL (val) = lval_memory;
  set_value_address (val, value_address (arg1));

  read_value_memory (val, 0, value_stack (val), value_address (val),
		     value_contents_all_raw (val),
		     type_length_units (value_enclosing_type (val)));

  return val;
}

struct value *
value_of_variable (struct symbol *var, const struct block *b)
{
  struct frame_info *frame = NULL;

  if (symbol_read_needs_frame (var))
    frame = get_selected_frame (_("No frame selected."));

  return read_var_value (var, b, frame);
}

struct value *
address_of_variable (struct symbol *var, const struct block *b)
{
  struct type *type = SYMBOL_TYPE (var);
  struct value *val;

  /* Evaluate it first; if the result is a memory address, we're fine.
     Lazy evaluation pays off here.  */

  val = value_of_variable (var, b);
  type = value_type (val);

  if ((VALUE_LVAL (val) == lval_memory && value_lazy (val))
      || TYPE_CODE (type) == TYPE_CODE_FUNC)
    {
      CORE_ADDR addr = value_address (val);

      return value_from_pointer (lookup_pointer_type (type), addr);
    }

  /* Not a memory address; check what the problem was.  */
  switch (VALUE_LVAL (val))
    {
    case lval_register:
      {
	struct frame_info *frame;
	const char *regname;

	frame = frame_find_by_id (VALUE_NEXT_FRAME_ID (val));
	gdb_assert (frame);

	regname = gdbarch_register_name (get_frame_arch (frame),
					 VALUE_REGNUM (val));
	gdb_assert (regname && *regname);

	error (_("Address requested for identifier "
		 "\"%s\" which is in register $%s"),
	       var->print_name (), regname);
	break;
      }

    default:
      error (_("Can't take address of \"%s\" which isn't an lvalue."),
	     var->print_name ());
      break;
    }

  return val;
}

/* See value.h.  */

bool
value_must_coerce_to_target (struct value *val)
{
  struct type *valtype;

  /* The only lval kinds which do not live in target memory.  */
  if (VALUE_LVAL (val) != not_lval
      && VALUE_LVAL (val) != lval_internalvar
      && VALUE_LVAL (val) != lval_xcallable)
    return false;

  valtype = check_typedef (value_type (val));

  switch (TYPE_CODE (valtype))
    {
    case TYPE_CODE_ARRAY:
      return TYPE_VECTOR (valtype) ? 0 : 1;
    case TYPE_CODE_STRING:
      return true;
    default:
      return false;
    }
}

/* Make sure that VAL lives in target memory if it's supposed to.  For
   instance, strings are constructed as character arrays in GDB's
   storage, and this function copies them to the target.  */

struct value *
value_coerce_to_target (struct value *val)
{
  LONGEST length;
  CORE_ADDR addr;

  if (!value_must_coerce_to_target (val))
    return val;

  length = TYPE_LENGTH (check_typedef (value_type (val)));
  addr = allocate_space_in_inferior (length);
  write_memory (addr, value_contents (val), length);
  return value_at_lazy (value_type (val), addr);
}

/* Given a value which is an array, return a value which is a pointer
   to its first element, regardless of whether or not the array has a
   nonzero lower bound.

   FIXME: A previous comment here indicated that this routine should
   be substracting the array's lower bound.  It's not clear to me that
   this is correct.  Given an array subscripting operation, it would
   certainly work to do the adjustment here, essentially computing:

   (&array[0] - (lowerbound * sizeof array[0])) + (index * sizeof array[0])

   However I believe a more appropriate and logical place to account
   for the lower bound is to do so in value_subscript, essentially
   computing:

   (&array[0] + ((index - lowerbound) * sizeof array[0]))

   As further evidence consider what would happen with operations
   other than array subscripting, where the caller would get back a
   value that had an address somewhere before the actual first element
   of the array, and the information about the lower bound would be
   lost because of the coercion to pointer type.  */

struct value *
value_coerce_array (struct value *arg1)
{
  struct type *type = check_typedef (value_type (arg1));

  /* If the user tries to do something requiring a pointer with an
     array that has not yet been pushed to the target, then this would
     be a good time to do so.  */
  arg1 = value_coerce_to_target (arg1);

  if (VALUE_LVAL (arg1) != lval_memory)
    error (_("Attempt to take address of value not located in memory."));

  return value_from_pointer (lookup_pointer_type (TYPE_TARGET_TYPE (type)),
			     value_address (arg1));
}

/* Given a value which is a function, return a value which is a pointer
   to it.  */

struct value *
value_coerce_function (struct value *arg1)
{
  struct value *retval;

  if (VALUE_LVAL (arg1) != lval_memory)
    error (_("Attempt to take address of value not located in memory."));

  retval = value_from_pointer (lookup_pointer_type (value_type (arg1)),
			       value_address (arg1));
  return retval;
}

/* Return a pointer value for the object for which ARG1 is the
   contents.  */

struct value *
value_addr (struct value *arg1)
{
  struct value *arg2;
  struct type *type = check_typedef (value_type (arg1));

  if (TYPE_IS_REFERENCE (type))
    {
      if (value_bits_synthetic_pointer (arg1, value_embedded_offset (arg1),
	  TARGET_CHAR_BIT * TYPE_LENGTH (type)))
	arg1 = coerce_ref (arg1);
      else
	{
	  /* Copy the value, but change the type from (T&) to (T*).  We
	     keep the same location information, which is efficient, and
	     allows &(&X) to get the location containing the reference.
	     Do the same to its enclosing type for consistency.  */
	  struct type *type_ptr
	    = lookup_pointer_type (TYPE_TARGET_TYPE (type));
	  struct type *enclosing_type
	    = check_typedef (value_enclosing_type (arg1));
	  struct type *enclosing_type_ptr
	    = lookup_pointer_type (TYPE_TARGET_TYPE (enclosing_type));

	  arg2 = value_copy (arg1);
	  deprecated_set_value_type (arg2, type_ptr);
	  set_value_enclosing_type (arg2, enclosing_type_ptr);

	  return arg2;
	}
    }
  if (TYPE_CODE (type) == TYPE_CODE_FUNC)
    return value_coerce_function (arg1);

  /* If this is an array that has not yet been pushed to the target,
     then this would be a good time to force it to memory.  */
  arg1 = value_coerce_to_target (arg1);

  if (VALUE_LVAL (arg1) != lval_memory)
    error (_("Attempt to take address of value not located in memory."));

  /* Get target memory address.  */
  arg2 = value_from_pointer (lookup_pointer_type (value_type (arg1)),
			     (value_address (arg1)
			      + value_embedded_offset (arg1)));

  /* This may be a pointer to a base subobject; so remember the
     full derived object's type ...  */
  set_value_enclosing_type (arg2,
			    lookup_pointer_type (value_enclosing_type (arg1)));
  /* ... and also the relative position of the subobject in the full
     object.  */
  set_value_pointed_to_offset (arg2, value_embedded_offset (arg1));
  return arg2;
}

/* Return a reference value for the object for which ARG1 is the
   contents.  */

struct value *
value_ref (struct value *arg1, enum type_code refcode)
{
  struct value *arg2;
  struct type *type = check_typedef (value_type (arg1));

  gdb_assert (refcode == TYPE_CODE_REF || refcode == TYPE_CODE_RVALUE_REF);

  if ((TYPE_CODE (type) == TYPE_CODE_REF
       || TYPE_CODE (type) == TYPE_CODE_RVALUE_REF)
      && TYPE_CODE (type) == refcode)
    return arg1;

  arg2 = value_addr (arg1);
  deprecated_set_value_type (arg2, lookup_reference_type (type, refcode));
  return arg2;
}

/* Given a value of a pointer type, apply the C unary * operator to
   it.  */

struct value *
value_ind (struct value *arg1)
{
  struct type *base_type;
  struct value *arg2;

  arg1 = coerce_array (arg1);

  base_type = check_typedef (value_type (arg1));

  if (VALUE_LVAL (arg1) == lval_computed)
    {
      const struct lval_funcs *funcs = value_computed_funcs (arg1);

      if (funcs->indirect)
	{
	  struct value *result = funcs->indirect (arg1);

	  if (result)
	    return result;
	}
    }

  if (TYPE_CODE (base_type) == TYPE_CODE_PTR)
    {
      struct type *enc_type;

      /* We may be pointing to something embedded in a larger object.
         Get the real type of the enclosing object.  */
      enc_type = check_typedef (value_enclosing_type (arg1));
      enc_type = TYPE_TARGET_TYPE (enc_type);

      if (TYPE_CODE (check_typedef (enc_type)) == TYPE_CODE_FUNC
	  || TYPE_CODE (check_typedef (enc_type)) == TYPE_CODE_METHOD)
	/* For functions, go through find_function_addr, which knows
	   how to handle function descriptors.  */
	arg2 = value_at_lazy (enc_type, 
			      find_function_addr (arg1, NULL));
      else
	/* Retrieve the enclosing object pointed to.  */
	arg2 = value_at_lazy (enc_type, 
			      (value_as_address (arg1)
			       - value_pointed_to_offset (arg1)));

      enc_type = value_type (arg2);
      return readjust_indirect_value_type (arg2, enc_type, base_type, arg1);
    }

  error (_("Attempt to take contents of a non-pointer value."));
}

/* Create a value for an array by allocating space in GDB, copying the
   data into that space, and then setting up an array value.

   The array bounds are set from LOWBOUND and HIGHBOUND, and the array
   is populated from the values passed in ELEMVEC.

   The element type of the array is inherited from the type of the
   first element, and all elements must have the same size (though we
   don't currently enforce any restriction on their types).  */

struct value *
value_array (int lowbound, int highbound, struct value **elemvec)
{
  int nelem;
  int idx;
  ULONGEST typelength;
  struct value *val;
  struct type *arraytype;

  /* Validate that the bounds are reasonable and that each of the
     elements have the same size.  */

  nelem = highbound - lowbound + 1;
  if (nelem <= 0)
    {
      error (_("bad array bounds (%d, %d)"), lowbound, highbound);
    }
  typelength = type_length_units (value_enclosing_type (elemvec[0]));
  for (idx = 1; idx < nelem; idx++)
    {
      if (type_length_units (value_enclosing_type (elemvec[idx]))
	  != typelength)
	{
	  error (_("array elements must all be the same size"));
	}
    }

  arraytype = lookup_array_range_type (value_enclosing_type (elemvec[0]),
				       lowbound, highbound);

  if (!current_language->c_style_arrays)
    {
      val = allocate_value (arraytype);
      for (idx = 0; idx < nelem; idx++)
	value_contents_copy (val, idx * typelength, elemvec[idx], 0,
			     typelength);
      return val;
    }

  /* Allocate space to store the array, and then initialize it by
     copying in each element.  */

  val = allocate_value (arraytype);
  for (idx = 0; idx < nelem; idx++)
    value_contents_copy (val, idx * typelength, elemvec[idx], 0, typelength);
  return val;
}

struct value *
value_cstring (const char *ptr, ssize_t len, struct type *char_type)
{
  struct value *val;
  int lowbound = current_language->string_lower_bound;
  ssize_t highbound = len / TYPE_LENGTH (char_type);
  struct type *stringtype
    = lookup_array_range_type (char_type, lowbound, highbound + lowbound - 1);

  val = allocate_value (stringtype);
  memcpy (value_contents_raw (val), ptr, len);
  return val;
}

/* Create a value for a string constant by allocating space in the
   inferior, copying the data into that space, and returning the
   address with type TYPE_CODE_STRING.  PTR points to the string
   constant data; LEN is number of characters.

   Note that string types are like array of char types with a lower
   bound of zero and an upper bound of LEN - 1.  Also note that the
   string may contain embedded null bytes.  */

struct value *
value_string (const char *ptr, ssize_t len, struct type *char_type)
{
  struct value *val;
  int lowbound = current_language->string_lower_bound;
  ssize_t highbound = len / TYPE_LENGTH (char_type);
  struct type *stringtype
    = lookup_string_range_type (char_type, lowbound, highbound + lowbound - 1);

  val = allocate_value (stringtype);
  memcpy (value_contents_raw (val), ptr, len);
  return val;
}


/* See if we can pass arguments in T2 to a function which takes
   arguments of types T1.  T1 is a list of NARGS arguments, and T2 is
   a NULL-terminated vector.  If some arguments need coercion of some
   sort, then the coerced values are written into T2.  Return value is
   0 if the arguments could be matched, or the position at which they
   differ if not.

   STATICP is nonzero if the T1 argument list came from a static
   member function.  T2 will still include the ``this'' pointer, but
   it will be skipped.

   For non-static member functions, we ignore the first argument,
   which is the type of the instance variable.  This is because we
   want to handle calls with objects from derived classes.  This is
   not entirely correct: we should actually check to make sure that a
   requested operation is type secure, shouldn't we?  FIXME.  */

static int
typecmp (int staticp, int varargs, int nargs,
	 struct field t1[], struct value *t2[])
{
  int i;

  if (t2 == 0)
    internal_error (__FILE__, __LINE__, 
		    _("typecmp: no argument list"));

  /* Skip ``this'' argument if applicable.  T2 will always include
     THIS.  */
  if (staticp)
    t2 ++;

  for (i = 0;
       (i < nargs) && TYPE_CODE (t1[i].type) != TYPE_CODE_VOID;
       i++)
    {
      struct type *tt1, *tt2;

      if (!t2[i])
	return i + 1;

      tt1 = check_typedef (t1[i].type);
      tt2 = check_typedef (value_type (t2[i]));

      if (TYPE_IS_REFERENCE (tt1)
	  /* We should be doing hairy argument matching, as below.  */
	  && (TYPE_CODE (check_typedef (TYPE_TARGET_TYPE (tt1)))
	      == TYPE_CODE (tt2)))
	{
	  if (TYPE_CODE (tt2) == TYPE_CODE_ARRAY)
	    t2[i] = value_coerce_array (t2[i]);
	  else
	    t2[i] = value_ref (t2[i], TYPE_CODE (tt1));
	  continue;
	}

      /* djb - 20000715 - Until the new type structure is in the
	 place, and we can attempt things like implicit conversions,
	 we need to do this so you can take something like a map<const
	 char *>, and properly access map["hello"], because the
	 argument to [] will be a reference to a pointer to a char,
	 and the argument will be a pointer to a char.  */
      while (TYPE_IS_REFERENCE (tt1) || TYPE_CODE (tt1) == TYPE_CODE_PTR)
	{
	  tt1 = check_typedef( TYPE_TARGET_TYPE(tt1) );
	}
      while (TYPE_CODE(tt2) == TYPE_CODE_ARRAY
	     || TYPE_CODE(tt2) == TYPE_CODE_PTR
	     || TYPE_IS_REFERENCE (tt2))
	{
	  tt2 = check_typedef (TYPE_TARGET_TYPE(tt2));
	}
      if (TYPE_CODE (tt1) == TYPE_CODE (tt2))
	continue;
      /* Array to pointer is a `trivial conversion' according to the
	 ARM.  */

      /* We should be doing much hairier argument matching (see
         section 13.2 of the ARM), but as a quick kludge, just check
         for the same type code.  */
      if (TYPE_CODE (t1[i].type) != TYPE_CODE (value_type (t2[i])))
	return i + 1;
    }
  if (varargs || t2[i] == NULL)
    return 0;
  return i + 1;
}

/* Helper class for do_search_struct_field that updates *RESULT_PTR
   and *LAST_BOFFSET, and possibly throws an exception if the field
   search has yielded ambiguous results.  */

static void
update_search_result (struct value **result_ptr, struct value *v,
		      LONGEST *last_boffset, LONGEST boffset,
		      const char *name, struct type *type)
{
  if (v != NULL)
    {
      if (*result_ptr != NULL
	  /* The result is not ambiguous if all the classes that are
	     found occupy the same space.  */
	  && *last_boffset != boffset)
	error (_("base class '%s' is ambiguous in type '%s'"),
	       name, TYPE_SAFE_NAME (type));
      *result_ptr = v;
      *last_boffset = boffset;
    }
}

/* A helper for search_struct_field.  This does all the work; most
   arguments are as passed to search_struct_field.  The result is
   stored in *RESULT_PTR, which must be initialized to NULL.
   OUTERMOST_TYPE is the type of the initial type passed to
   search_struct_field; this is used for error reporting when the
   lookup is ambiguous.  */

static void
do_search_struct_field (const char *name, struct value *arg1, LONGEST offset,
			struct type *type, int looking_for_baseclass,
			struct value **result_ptr,
			LONGEST *last_boffset,
			struct type *outermost_type)
{
  int i;
  int nbases;

  type = check_typedef (type);
  nbases = TYPE_N_BASECLASSES (type);

  if (!looking_for_baseclass)
    for (i = TYPE_NFIELDS (type) - 1; i >= nbases; i--)
      {
	const char *t_field_name = TYPE_FIELD_NAME (type, i);

	if (t_field_name && (strcmp_iw (t_field_name, name) == 0))
	  {
	    struct value *v;

	    if (field_is_static (&TYPE_FIELD (type, i)))
	      v = value_static_field (type, i);
	    else
	      v = value_primitive_field (arg1, offset, i, type);
	    *result_ptr = v;
	    return;
	  }

	if (t_field_name
	    && t_field_name[0] == '\0')
	  {
	    struct type *field_type = TYPE_FIELD_TYPE (type, i);

	    if (TYPE_CODE (field_type) == TYPE_CODE_UNION
		|| TYPE_CODE (field_type) == TYPE_CODE_STRUCT)
	      {
		/* Look for a match through the fields of an anonymous
		   union, or anonymous struct.  C++ provides anonymous
		   unions.

		   In the GNU Chill (now deleted from GDB)
		   implementation of variant record types, each
		   <alternative field> has an (anonymous) union type,
		   each member of the union represents a <variant
		   alternative>.  Each <variant alternative> is
		   represented as a struct, with a member for each
		   <variant field>.  */

		struct value *v = NULL;
		LONGEST new_offset = offset;

		/* This is pretty gross.  In G++, the offset in an
		   anonymous union is relative to the beginning of the
		   enclosing struct.  In the GNU Chill (now deleted
		   from GDB) implementation of variant records, the
		   bitpos is zero in an anonymous union field, so we
		   have to add the offset of the union here.  */
		if (TYPE_CODE (field_type) == TYPE_CODE_STRUCT
		    || (TYPE_NFIELDS (field_type) > 0
			&& TYPE_FIELD_BITPOS (field_type, 0) == 0))
		  new_offset += TYPE_FIELD_BITPOS (type, i) / 8;

		do_search_struct_field (name, arg1, new_offset, 
					field_type,
					looking_for_baseclass, &v,
					last_boffset,
					outermost_type);
		if (v)
		  {
		    *result_ptr = v;
		    return;
		  }
	      }
	  }
      }

  for (i = 0; i < nbases; i++)
    {
      struct value *v = NULL;
      struct type *basetype = check_typedef (TYPE_BASECLASS (type, i));
      /* If we are looking for baseclasses, this is what we get when
         we hit them.  But it could happen that the base part's member
         name is not yet filled in.  */
      int found_baseclass = (looking_for_baseclass
			     && TYPE_BASECLASS_NAME (type, i) != NULL
			     && (strcmp_iw (name, 
					    TYPE_BASECLASS_NAME (type, 
								 i)) == 0));
      LONGEST boffset = value_embedded_offset (arg1) + offset;

      if (BASETYPE_VIA_VIRTUAL (type, i))
	{
	  struct value *v2;

	  boffset = baseclass_offset (type, i,
				      value_contents_for_printing (arg1),
				      value_embedded_offset (arg1) + offset,
				      value_address (arg1),
				      arg1);

	  /* The virtual base class pointer might have been clobbered
	     by the user program.  Make sure that it still points to a
	     valid memory location.  */

	  boffset += value_embedded_offset (arg1) + offset;
	  if (boffset < 0
	      || boffset >= TYPE_LENGTH (value_enclosing_type (arg1)))
	    {
	      CORE_ADDR base_addr;

	      base_addr = value_address (arg1) + boffset;
	      v2 = value_at_lazy (basetype, base_addr);
	      if (target_read_memory (base_addr, 
				      value_contents_raw (v2),
				      TYPE_LENGTH (value_type (v2))) != 0)
		error (_("virtual baseclass botch"));
	    }
	  else
	    {
	      v2 = value_copy (arg1);
	      deprecated_set_value_type (v2, basetype);
	      set_value_embedded_offset (v2, boffset);
	    }

	  if (found_baseclass)
	    v = v2;
	  else
	    {
	      do_search_struct_field (name, v2, 0,
				      TYPE_BASECLASS (type, i),
				      looking_for_baseclass,
				      result_ptr, last_boffset,
				      outermost_type);
	    }
	}
      else if (found_baseclass)
	v = value_primitive_field (arg1, offset, i, type);
      else
	{
	  do_search_struct_field (name, arg1,
				  offset + TYPE_BASECLASS_BITPOS (type, 
								  i) / 8,
				  basetype, looking_for_baseclass,
				  result_ptr, last_boffset,
				  outermost_type);
	}

      update_search_result (result_ptr, v, last_boffset,
			    boffset, name, outermost_type);
    }
}

/* Helper function used by value_struct_elt to recurse through
   baseclasses.  Look for a field NAME in ARG1.  Search in it assuming
   it has (class) type TYPE.  If found, return value, else return NULL.

   If LOOKING_FOR_BASECLASS, then instead of looking for struct
   fields, look for a baseclass named NAME.  */

static struct value *
search_struct_field (const char *name, struct value *arg1,
		     struct type *type, int looking_for_baseclass)
{
  struct value *result = NULL;
  LONGEST boffset = 0;

  do_search_struct_field (name, arg1, 0, type, looking_for_baseclass,
			  &result, &boffset, type);
  return result;
}

/* Helper function used by value_struct_elt to recurse through
   baseclasses.  Look for a field NAME in ARG1.  Adjust the address of
   ARG1 by OFFSET bytes, and search in it assuming it has (class) type
   TYPE.

   If found, return value, else if name matched and args not return
   (value) -1, else return NULL.  */

static struct value *
search_struct_method (const char *name, struct value **arg1p,
		      struct value **args, LONGEST offset,
		      int *static_memfuncp, struct type *type)
{
  int i;
  struct value *v;
  int name_matched = 0;

  type = check_typedef (type);
  for (i = TYPE_NFN_FIELDS (type) - 1; i >= 0; i--)
    {
      const char *t_field_name = TYPE_FN_FIELDLIST_NAME (type, i);

      if (t_field_name && (strcmp_iw (t_field_name, name) == 0))
	{
	  int j = TYPE_FN_FIELDLIST_LENGTH (type, i) - 1;
	  struct fn_field *f = TYPE_FN_FIELDLIST1 (type, i);

	  name_matched = 1;
	  check_stub_method_group (type, i);
	  if (j > 0 && args == 0)
	    error (_("cannot resolve overloaded method "
		     "`%s': no arguments supplied"), name);
	  else if (j == 0 && args == 0)
	    {
	      v = value_fn_field (arg1p, f, j, type, offset);
	      if (v != NULL)
		return v;
	    }
	  else
	    while (j >= 0)
	      {
		if (!typecmp (TYPE_FN_FIELD_STATIC_P (f, j),
			      TYPE_VARARGS (TYPE_FN_FIELD_TYPE (f, j)),
			      TYPE_NFIELDS (TYPE_FN_FIELD_TYPE (f, j)),
			      TYPE_FN_FIELD_ARGS (f, j), args))
		  {
		    if (TYPE_FN_FIELD_VIRTUAL_P (f, j))
		      return value_virtual_fn_field (arg1p, f, j, 
						     type, offset);
		    if (TYPE_FN_FIELD_STATIC_P (f, j) 
			&& static_memfuncp)
		      *static_memfuncp = 1;
		    v = value_fn_field (arg1p, f, j, type, offset);
		    if (v != NULL)
		      return v;       
		  }
		j--;
	      }
	}
    }

  for (i = TYPE_N_BASECLASSES (type) - 1; i >= 0; i--)
    {
      LONGEST base_offset;
      LONGEST this_offset;

      if (BASETYPE_VIA_VIRTUAL (type, i))
	{
	  struct type *baseclass = check_typedef (TYPE_BASECLASS (type, i));
	  struct value *base_val;
	  const gdb_byte *base_valaddr;

	  /* The virtual base class pointer might have been
	     clobbered by the user program.  Make sure that it
	     still points to a valid memory location.  */

	  if (offset < 0 || offset >= TYPE_LENGTH (type))
	    {
	      CORE_ADDR address;

	      gdb::byte_vector tmp (TYPE_LENGTH (baseclass));
	      address = value_address (*arg1p);

	      if (target_read_memory (address + offset,
				      tmp.data (), TYPE_LENGTH (baseclass)) != 0)
		error (_("virtual baseclass botch"));

	      base_val = value_from_contents_and_address (baseclass,
							  tmp.data (),
							  address + offset);
	      base_valaddr = value_contents_for_printing (base_val);
	      this_offset = 0;
	    }
	  else
	    {
	      base_val = *arg1p;
	      base_valaddr = value_contents_for_printing (*arg1p);
	      this_offset = offset;
	    }

	  base_offset = baseclass_offset (type, i, base_valaddr,
					  this_offset, value_address (base_val),
					  base_val);
	}
      else
	{
	  base_offset = TYPE_BASECLASS_BITPOS (type, i) / 8;
	}
      v = search_struct_method (name, arg1p, args, base_offset + offset,
				static_memfuncp, TYPE_BASECLASS (type, i));
      if (v == (struct value *) - 1)
	{
	  name_matched = 1;
	}
      else if (v)
	{
	  /* FIXME-bothner:  Why is this commented out?  Why is it here?  */
	  /* *arg1p = arg1_tmp; */
	  return v;
	}
    }
  if (name_matched)
    return (struct value *) - 1;
  else
    return NULL;
}

/* Given *ARGP, a value of type (pointer to a)* structure/union,
   extract the component named NAME from the ultimate target
   structure/union and return it as a value with its appropriate type.
   ERR is used in the error message if *ARGP's type is wrong.

   C++: ARGS is a list of argument types to aid in the selection of
   an appropriate method.  Also, handle derived types.

   STATIC_MEMFUNCP, if non-NULL, points to a caller-supplied location
   where the truthvalue of whether the function that was resolved was
   a static member function or not is stored.

   ERR is an error message to be printed in case the field is not
   found.  */

struct value *
value_struct_elt (struct value **argp, struct value **args,
		  const char *name, int *static_memfuncp, const char *err)
{
  struct type *t;
  struct value *v;

  *argp = coerce_array (*argp);

  t = check_typedef (value_type (*argp));

  /* Follow pointers until we get to a non-pointer.  */

  while (TYPE_CODE (t) == TYPE_CODE_PTR || TYPE_IS_REFERENCE (t))
    {
      *argp = value_ind (*argp);
      /* Don't coerce fn pointer to fn and then back again!  */
      if (TYPE_CODE (check_typedef (value_type (*argp))) != TYPE_CODE_FUNC)
	*argp = coerce_array (*argp);
      t = check_typedef (value_type (*argp));
    }

  if (TYPE_CODE (t) != TYPE_CODE_STRUCT
      && TYPE_CODE (t) != TYPE_CODE_UNION)
    error (_("Attempt to extract a component of a value that is not a %s."),
	   err);

  /* Assume it's not, unless we see that it is.  */
  if (static_memfuncp)
    *static_memfuncp = 0;

  if (!args)
    {
      /* if there are no arguments ...do this...  */

      /* Try as a field first, because if we succeed, there is less
         work to be done.  */
      v = search_struct_field (name, *argp, t, 0);
      if (v)
	return v;

      /* C++: If it was not found as a data field, then try to
         return it as a pointer to a method.  */
      v = search_struct_method (name, argp, args, 0, 
				static_memfuncp, t);

      if (v == (struct value *) - 1)
	error (_("Cannot take address of method %s."), name);
      else if (v == 0)
	{
	  if (TYPE_NFN_FIELDS (t))
	    error (_("There is no member or method named %s."), name);
	  else
	    error (_("There is no member named %s."), name);
	}
      return v;
    }

  v = search_struct_method (name, argp, args, 0, 
			    static_memfuncp, t);
  
  if (v == (struct value *) - 1)
    {
      error (_("One of the arguments you tried to pass to %s could not "
	       "be converted to what the function wants."), name);
    }
  else if (v == 0)
    {
      /* See if user tried to invoke data as function.  If so, hand it
         back.  If it's not callable (i.e., a pointer to function),
         gdb should give an error.  */
      v = search_struct_field (name, *argp, t, 0);
      /* If we found an ordinary field, then it is not a method call.
	 So, treat it as if it were a static member function.  */
      if (v && static_memfuncp)
	*static_memfuncp = 1;
    }

  if (!v)
    throw_error (NOT_FOUND_ERROR,
                 _("Structure has no component named %s."), name);
  return v;
}

/* Given *ARGP, a value of type structure or union, or a pointer/reference
   to a structure or union, extract and return its component (field) of
   type FTYPE at the specified BITPOS.
   Throw an exception on error.  */

struct value *
value_struct_elt_bitpos (struct value **argp, int bitpos, struct type *ftype,
			 const char *err)
{
  struct type *t;
  int i;

  *argp = coerce_array (*argp);

  t = check_typedef (value_type (*argp));

  while (TYPE_CODE (t) == TYPE_CODE_PTR || TYPE_IS_REFERENCE (t))
    {
      *argp = value_ind (*argp);
      if (TYPE_CODE (check_typedef (value_type (*argp))) != TYPE_CODE_FUNC)
	*argp = coerce_array (*argp);
      t = check_typedef (value_type (*argp));
    }

  if (TYPE_CODE (t) != TYPE_CODE_STRUCT
      && TYPE_CODE (t) != TYPE_CODE_UNION)
    error (_("Attempt to extract a component of a value that is not a %s."),
	   err);

  for (i = TYPE_N_BASECLASSES (t); i < TYPE_NFIELDS (t); i++)
    {
      if (!field_is_static (&TYPE_FIELD (t, i))
	  && bitpos == TYPE_FIELD_BITPOS (t, i)
	  && types_equal (ftype, TYPE_FIELD_TYPE (t, i)))
	return value_primitive_field (*argp, 0, i, t);
    }

  error (_("No field with matching bitpos and type."));

  /* Never hit.  */
  return NULL;
}

/* See value.h.  */

int
value_union_variant (struct type *union_type, const gdb_byte *contents)
{
  gdb_assert (TYPE_CODE (union_type) == TYPE_CODE_UNION
	      && TYPE_FLAG_DISCRIMINATED_UNION (union_type));

  struct dynamic_prop *discriminant_prop
    = get_dyn_prop (DYN_PROP_DISCRIMINATED, union_type);
  gdb_assert (discriminant_prop != nullptr);

  struct discriminant_info *info
    = (struct discriminant_info *) discriminant_prop->data.baton;
  gdb_assert (info != nullptr);

  /* If this is a univariant union, just return the sole field.  */
  if (TYPE_NFIELDS (union_type) == 1)
    return 0;
  /* This should only happen for univariants, which we already dealt
     with.  */
  gdb_assert (info->discriminant_index != -1);

  /* Compute the discriminant.  Note that unpack_field_as_long handles
     sign extension when necessary, as does the DWARF reader -- so
     signed discriminants will be handled correctly despite the use of
     an unsigned type here.  */
  ULONGEST discriminant = unpack_field_as_long (union_type, contents,
						info->discriminant_index);

  for (int i = 0; i < TYPE_NFIELDS (union_type); ++i)
    {
      if (i != info->default_index
	  && i != info->discriminant_index
	  && discriminant == info->discriminants[i])
	return i;
    }

  if (info->default_index == -1)
    error (_("Could not find variant corresponding to discriminant %s"),
	   pulongest (discriminant));
  return info->default_index;
}

/* Search through the methods of an object (and its bases) to find a
   specified method.  Return a reference to the fn_field list METHODS of
   overloaded instances defined in the source language.  If available
   and matching, a vector of matching xmethods defined in extension
   languages are also returned in XMETHODS.

   Helper function for value_find_oload_list.
   ARGP is a pointer to a pointer to a value (the object).
   METHOD is a string containing the method name.
   OFFSET is the offset within the value.
   TYPE is the assumed type of the object.
   METHODS is a pointer to the matching overloaded instances defined
      in the source language.  Since this is a recursive function,
      *METHODS should be set to NULL when calling this function.
   NUM_FNS is the number of overloaded instances.  *NUM_FNS should be set to
      0 when calling this function.
   XMETHODS is the vector of matching xmethod workers.  *XMETHODS
      should also be set to NULL when calling this function.
   BASETYPE is set to the actual type of the subobject where the
      method is found.
   BOFFSET is the offset of the base subobject where the method is found.  */

static void
find_method_list (struct value **argp, const char *method,
		  LONGEST offset, struct type *type,
		  gdb::array_view<fn_field> *methods,
		  std::vector<xmethod_worker_up> *xmethods,
		  struct type **basetype, LONGEST *boffset)
{
  int i;
  struct fn_field *f = NULL;

  gdb_assert (methods != NULL && xmethods != NULL);
  type = check_typedef (type);

  /* First check in object itself.
     This function is called recursively to search through base classes.
     If there is a source method match found at some stage, then we need not
     look for source methods in consequent recursive calls.  */
  if (methods->empty ())
    {
      for (i = TYPE_NFN_FIELDS (type) - 1; i >= 0; i--)
	{
	  /* pai: FIXME What about operators and type conversions?  */
	  const char *fn_field_name = TYPE_FN_FIELDLIST_NAME (type, i);

	  if (fn_field_name && (strcmp_iw (fn_field_name, method) == 0))
	    {
	      int len = TYPE_FN_FIELDLIST_LENGTH (type, i);
	      f = TYPE_FN_FIELDLIST1 (type, i);
	      *methods = gdb::make_array_view (f, len);

	      *basetype = type;
	      *boffset = offset;

	      /* Resolve any stub methods.  */
	      check_stub_method_group (type, i);

	      break;
	    }
	}
    }

  /* Unlike source methods, xmethods can be accumulated over successive
     recursive calls.  In other words, an xmethod named 'm' in a class
     will not hide an xmethod named 'm' in its base class(es).  We want
     it to be this way because xmethods are after all convenience functions
     and hence there is no point restricting them with something like method
     hiding.  Moreover, if hiding is done for xmethods as well, then we will
     have to provide a mechanism to un-hide (like the 'using' construct).  */
  get_matching_xmethod_workers (type, method, xmethods);

  /* If source methods are not found in current class, look for them in the
     base classes.  We also have to go through the base classes to gather
     extension methods.  */
  for (i = TYPE_N_BASECLASSES (type) - 1; i >= 0; i--)
    {
      LONGEST base_offset;

      if (BASETYPE_VIA_VIRTUAL (type, i))
	{
	  base_offset = baseclass_offset (type, i,
					  value_contents_for_printing (*argp),
					  value_offset (*argp) + offset,
					  value_address (*argp), *argp);
	}
      else /* Non-virtual base, simply use bit position from debug
	      info.  */
	{
	  base_offset = TYPE_BASECLASS_BITPOS (type, i) / 8;
	}

      find_method_list (argp, method, base_offset + offset,
			TYPE_BASECLASS (type, i), methods,
			xmethods, basetype, boffset);
    }
}

/* Return the list of overloaded methods of a specified name.  The methods
   could be those GDB finds in the binary, or xmethod.  Methods found in
   the binary are returned in METHODS, and xmethods are returned in
   XMETHODS.

   ARGP is a pointer to a pointer to a value (the object).
   METHOD is the method name.
   OFFSET is the offset within the value contents.
   METHODS is the list of matching overloaded instances defined in
      the source language.
   XMETHODS is the vector of matching xmethod workers defined in
      extension languages.
   BASETYPE is set to the type of the base subobject that defines the
      method.
   BOFFSET is the offset of the base subobject which defines the method.  */

static void
value_find_oload_method_list (struct value **argp, const char *method,
			      LONGEST offset,
			      gdb::array_view<fn_field> *methods,
			      std::vector<xmethod_worker_up> *xmethods,
			      struct type **basetype, LONGEST *boffset)
{
  struct type *t;

  t = check_typedef (value_type (*argp));

  /* Code snarfed from value_struct_elt.  */
  while (TYPE_CODE (t) == TYPE_CODE_PTR || TYPE_IS_REFERENCE (t))
    {
      *argp = value_ind (*argp);
      /* Don't coerce fn pointer to fn and then back again!  */
      if (TYPE_CODE (check_typedef (value_type (*argp))) != TYPE_CODE_FUNC)
	*argp = coerce_array (*argp);
      t = check_typedef (value_type (*argp));
    }

  if (TYPE_CODE (t) != TYPE_CODE_STRUCT
      && TYPE_CODE (t) != TYPE_CODE_UNION)
    error (_("Attempt to extract a component of a "
	     "value that is not a struct or union"));

  gdb_assert (methods != NULL && xmethods != NULL);

  /* Clear the lists.  */
  *methods = {};
  xmethods->clear ();

  find_method_list (argp, method, 0, t, methods, xmethods,
		    basetype, boffset);
}

/* Given an array of arguments (ARGS) (which includes an entry for
   "this" in the case of C++ methods), the NAME of a function, and
   whether it's a method or not (METHOD), find the best function that
   matches on the argument types according to the overload resolution
   rules.

   METHOD can be one of three values:
     NON_METHOD for non-member functions.
     METHOD: for member functions.
     BOTH: used for overload resolution of operators where the
       candidates are expected to be either member or non member
       functions.  In this case the first argument ARGTYPES
       (representing 'this') is expected to be a reference to the
       target object, and will be dereferenced when attempting the
       non-member search.

   In the case of class methods, the parameter OBJ is an object value
   in which to search for overloaded methods.

   In the case of non-method functions, the parameter FSYM is a symbol
   corresponding to one of the overloaded functions.

   Return value is an integer: 0 -> good match, 10 -> debugger applied
   non-standard coercions, 100 -> incompatible.

   If a method is being searched for, VALP will hold the value.
   If a non-method is being searched for, SYMP will hold the symbol 
   for it.

   If a method is being searched for, and it is a static method,
   then STATICP will point to a non-zero value.

   If NO_ADL argument dependent lookup is disabled.  This is used to prevent
   ADL overload candidates when performing overload resolution for a fully
   qualified name.

   If NOSIDE is EVAL_AVOID_SIDE_EFFECTS, then OBJP's memory cannot be
   read while picking the best overload match (it may be all zeroes and thus
   not have a vtable pointer), in which case skip virtual function lookup.
   This is ok as typically EVAL_AVOID_SIDE_EFFECTS is only used to determine
   the result type.

   Note: This function does *not* check the value of
   overload_resolution.  Caller must check it to see whether overload
   resolution is permitted.  */

int
find_overload_match (gdb::array_view<value *> args,
		     const char *name, enum oload_search_type method,
		     struct value **objp, struct symbol *fsym,
		     struct value **valp, struct symbol **symp, 
		     int *staticp, const int no_adl,
		     const enum noside noside)
{
  struct value *obj = (objp ? *objp : NULL);
  struct type *obj_type = obj ? value_type (obj) : NULL;
  /* Index of best overloaded function.  */
  int func_oload_champ = -1;
  int method_oload_champ = -1;
  int src_method_oload_champ = -1;
  int ext_method_oload_champ = -1;

  /* The measure for the current best match.  */
  badness_vector method_badness;
  badness_vector func_badness;
  badness_vector ext_method_badness;
  badness_vector src_method_badness;

  struct value *temp = obj;
  /* For methods, the list of overloaded methods.  */
  gdb::array_view<fn_field> methods;
  /* For non-methods, the list of overloaded function symbols.  */
  std::vector<symbol *> functions;
  /* For xmethods, the vector of xmethod workers.  */
  std::vector<xmethod_worker_up> xmethods;
  struct type *basetype = NULL;
  LONGEST boffset;

  const char *obj_type_name = NULL;
  const char *func_name = NULL;
  gdb::unique_xmalloc_ptr<char> temp_func;
  enum oload_classification match_quality;
  enum oload_classification method_match_quality = INCOMPATIBLE;
  enum oload_classification src_method_match_quality = INCOMPATIBLE;
  enum oload_classification ext_method_match_quality = INCOMPATIBLE;
  enum oload_classification func_match_quality = INCOMPATIBLE;

  /* Get the list of overloaded methods or functions.  */
  if (method == METHOD || method == BOTH)
    {
      gdb_assert (obj);

      /* OBJ may be a pointer value rather than the object itself.  */
      obj = coerce_ref (obj);
      while (TYPE_CODE (check_typedef (value_type (obj))) == TYPE_CODE_PTR)
	obj = coerce_ref (value_ind (obj));
      obj_type_name = TYPE_NAME (value_type (obj));

      /* First check whether this is a data member, e.g. a pointer to
	 a function.  */
      if (TYPE_CODE (check_typedef (value_type (obj))) == TYPE_CODE_STRUCT)
	{
	  *valp = search_struct_field (name, obj,
				       check_typedef (value_type (obj)), 0);
	  if (*valp)
	    {
	      *staticp = 1;
	      return 0;
	    }
	}

      /* Retrieve the list of methods with the name NAME.  */
      value_find_oload_method_list (&temp, name, 0, &methods,
				    &xmethods, &basetype, &boffset);
      /* If this is a method only search, and no methods were found
         the search has failed.  */
      if (method == METHOD && methods.empty () && xmethods.empty ())
	error (_("Couldn't find method %s%s%s"),
	       obj_type_name,
	       (obj_type_name && *obj_type_name) ? "::" : "",
	       name);
      /* If we are dealing with stub method types, they should have
	 been resolved by find_method_list via
	 value_find_oload_method_list above.  */
      if (!methods.empty ())
	{
	  gdb_assert (TYPE_SELF_TYPE (methods[0].type) != NULL);

	  src_method_oload_champ
	    = find_oload_champ (args,
				methods.size (),
				methods.data (), NULL, NULL,
				&src_method_badness);

	  src_method_match_quality = classify_oload_match
	    (src_method_badness, args.size (),
	     oload_method_static_p (methods.data (), src_method_oload_champ));
	}

      if (!xmethods.empty ())
	{
	  ext_method_oload_champ
	    = find_oload_champ (args,
				xmethods.size (),
				NULL, xmethods.data (), NULL,
				&ext_method_badness);
	  ext_method_match_quality = classify_oload_match (ext_method_badness,
							   args.size (), 0);
	}

      if (src_method_oload_champ >= 0 && ext_method_oload_champ >= 0)
	{
	  switch (compare_badness (ext_method_badness, src_method_badness))
	    {
	      case 0: /* Src method and xmethod are equally good.  */
		/* If src method and xmethod are equally good, then
		   xmethod should be the winner.  Hence, fall through to the
		   case where a xmethod is better than the source
		   method, except when the xmethod match quality is
		   non-standard.  */
		/* FALLTHROUGH */
	      case 1: /* Src method and ext method are incompatible.  */
		/* If ext method match is not standard, then let source method
		   win.  Otherwise, fallthrough to let xmethod win.  */
		if (ext_method_match_quality != STANDARD)
		  {
		    method_oload_champ = src_method_oload_champ;
		    method_badness = src_method_badness;
		    ext_method_oload_champ = -1;
		    method_match_quality = src_method_match_quality;
		    break;
		  }
		/* FALLTHROUGH */
	      case 2: /* Ext method is champion.  */
		method_oload_champ = ext_method_oload_champ;
		method_badness = ext_method_badness;
		src_method_oload_champ = -1;
		method_match_quality = ext_method_match_quality;
		break;
	      case 3: /* Src method is champion.  */
		method_oload_champ = src_method_oload_champ;
		method_badness = src_method_badness;
		ext_method_oload_champ = -1;
		method_match_quality = src_method_match_quality;
		break;
	      default:
		gdb_assert_not_reached ("Unexpected overload comparison "
					"result");
		break;
	    }
	}
      else if (src_method_oload_champ >= 0)
	{
	  method_oload_champ = src_method_oload_champ;
	  method_badness = src_method_badness;
	  method_match_quality = src_method_match_quality;
	}
      else if (ext_method_oload_champ >= 0)
	{
	  method_oload_champ = ext_method_oload_champ;
	  method_badness = ext_method_badness;
	  method_match_quality = ext_method_match_quality;
	}
    }

  if (method == NON_METHOD || method == BOTH)
    {
      const char *qualified_name = NULL;

      /* If the overload match is being search for both as a method
         and non member function, the first argument must now be
         dereferenced.  */
      if (method == BOTH)
	args[0] = value_ind (args[0]);

      if (fsym)
        {
          qualified_name = fsym->natural_name ();

          /* If we have a function with a C++ name, try to extract just
	     the function part.  Do not try this for non-functions (e.g.
	     function pointers).  */
          if (qualified_name
              && TYPE_CODE (check_typedef (SYMBOL_TYPE (fsym)))
	      == TYPE_CODE_FUNC)
            {
	      temp_func = cp_func_name (qualified_name);

	      /* If cp_func_name did not remove anything, the name of the
	         symbol did not include scope or argument types - it was
	         probably a C-style function.  */
	      if (temp_func != nullptr)
		{
		  if (strcmp (temp_func.get (), qualified_name) == 0)
		    func_name = NULL;
		  else
		    func_name = temp_func.get ();
		}
            }
        }
      else
	{
	  func_name = name;
	  qualified_name = name;
	}

      /* If there was no C++ name, this must be a C-style function or
	 not a function at all.  Just return the same symbol.  Do the
	 same if cp_func_name fails for some reason.  */
      if (func_name == NULL)
        {
	  *symp = fsym;
          return 0;
        }

      func_oload_champ = find_oload_champ_namespace (args,
                                                     func_name,
                                                     qualified_name,
                                                     &functions,
                                                     &func_badness,
                                                     no_adl);

      if (func_oload_champ >= 0)
	func_match_quality = classify_oload_match (func_badness,
						   args.size (), 0);
    }

  /* Did we find a match ?  */
  if (method_oload_champ == -1 && func_oload_champ == -1)
    throw_error (NOT_FOUND_ERROR,
                 _("No symbol \"%s\" in current context."),
                 name);

  /* If we have found both a method match and a function
     match, find out which one is better, and calculate match
     quality.  */
  if (method_oload_champ >= 0 && func_oload_champ >= 0)
    {
      switch (compare_badness (func_badness, method_badness))
        {
	  case 0: /* Top two contenders are equally good.  */
	    /* FIXME: GDB does not support the general ambiguous case.
	     All candidates should be collected and presented the
	     user.  */
	    error (_("Ambiguous overload resolution"));
	    break;
	  case 1: /* Incomparable top contenders.  */
	    /* This is an error incompatible candidates
	       should not have been proposed.  */
	    error (_("Internal error: incompatible "
		     "overload candidates proposed"));
	    break;
	  case 2: /* Function champion.  */
	    method_oload_champ = -1;
	    match_quality = func_match_quality;
	    break;
	  case 3: /* Method champion.  */
	    func_oload_champ = -1;
	    match_quality = method_match_quality;
	    break;
	  default:
	    error (_("Internal error: unexpected overload comparison result"));
	    break;
        }
    }
  else
    {
      /* We have either a method match or a function match.  */
      if (method_oload_champ >= 0)
	match_quality = method_match_quality;
      else
	match_quality = func_match_quality;
    }

  if (match_quality == INCOMPATIBLE)
    {
      if (method == METHOD)
	error (_("Cannot resolve method %s%s%s to any overloaded instance"),
	       obj_type_name,
	       (obj_type_name && *obj_type_name) ? "::" : "",
	       name);
      else
	error (_("Cannot resolve function %s to any overloaded instance"),
	       func_name);
    }
  else if (match_quality == NON_STANDARD)
    {
      if (method == METHOD)
	warning (_("Using non-standard conversion to match "
		   "method %s%s%s to supplied arguments"),
		 obj_type_name,
		 (obj_type_name && *obj_type_name) ? "::" : "",
		 name);
      else
	warning (_("Using non-standard conversion to match "
		   "function %s to supplied arguments"),
		 func_name);
    }

  if (staticp != NULL)
    *staticp = oload_method_static_p (methods.data (), method_oload_champ);

  if (method_oload_champ >= 0)
    {
      if (src_method_oload_champ >= 0)
	{
	  if (TYPE_FN_FIELD_VIRTUAL_P (methods, method_oload_champ)
	      && noside != EVAL_AVOID_SIDE_EFFECTS)
	    {
	      *valp = value_virtual_fn_field (&temp, methods.data (),
					      method_oload_champ, basetype,
					      boffset);
	    }
	  else
	    *valp = value_fn_field (&temp, methods.data (),
				    method_oload_champ, basetype, boffset);
	}
      else
	*valp = value_from_xmethod
	  (std::move (xmethods[ext_method_oload_champ]));
    }
  else
    *symp = functions[func_oload_champ];

  if (objp)
    {
      struct type *temp_type = check_typedef (value_type (temp));
      struct type *objtype = check_typedef (obj_type);

      if (TYPE_CODE (temp_type) != TYPE_CODE_PTR
	  && (TYPE_CODE (objtype) == TYPE_CODE_PTR
	      || TYPE_IS_REFERENCE (objtype)))
	{
	  temp = value_addr (temp);
	}
      *objp = temp;
    }

  switch (match_quality)
    {
    case INCOMPATIBLE:
      return 100;
    case NON_STANDARD:
      return 10;
    default:				/* STANDARD */
      return 0;
    }
}

/* Find the best overload match, searching for FUNC_NAME in namespaces
   contained in QUALIFIED_NAME until it either finds a good match or
   runs out of namespaces.  It stores the overloaded functions in
   *OLOAD_SYMS, and the badness vector in *OLOAD_CHAMP_BV.  If NO_ADL,
   argument dependent lookup is not performed.  */

static int
find_oload_champ_namespace (gdb::array_view<value *> args,
			    const char *func_name,
			    const char *qualified_name,
			    std::vector<symbol *> *oload_syms,
			    badness_vector *oload_champ_bv,
			    const int no_adl)
{
  int oload_champ;

  find_oload_champ_namespace_loop (args,
				   func_name,
				   qualified_name, 0,
				   oload_syms, oload_champ_bv,
				   &oload_champ,
				   no_adl);

  return oload_champ;
}

/* Helper function for find_oload_champ_namespace; NAMESPACE_LEN is
   how deep we've looked for namespaces, and the champ is stored in
   OLOAD_CHAMP.  The return value is 1 if the champ is a good one, 0
   if it isn't.  Other arguments are the same as in
   find_oload_champ_namespace.  */

static int
find_oload_champ_namespace_loop (gdb::array_view<value *> args,
				 const char *func_name,
				 const char *qualified_name,
				 int namespace_len,
				 std::vector<symbol *> *oload_syms,
				 badness_vector *oload_champ_bv,
				 int *oload_champ,
				 const int no_adl)
{
  int next_namespace_len = namespace_len;
  int searched_deeper = 0;
  int new_oload_champ;
  char *new_namespace;

  if (next_namespace_len != 0)
    {
      gdb_assert (qualified_name[next_namespace_len] == ':');
      next_namespace_len +=  2;
    }
  next_namespace_len +=
    cp_find_first_component (qualified_name + next_namespace_len);

  /* First, see if we have a deeper namespace we can search in.
     If we get a good match there, use it.  */

  if (qualified_name[next_namespace_len] == ':')
    {
      searched_deeper = 1;

      if (find_oload_champ_namespace_loop (args,
					   func_name, qualified_name,
					   next_namespace_len,
					   oload_syms, oload_champ_bv,
					   oload_champ, no_adl))
	{
	  return 1;
	}
    };

  /* If we reach here, either we're in the deepest namespace or we
     didn't find a good match in a deeper namespace.  But, in the
     latter case, we still have a bad match in a deeper namespace;
     note that we might not find any match at all in the current
     namespace.  (There's always a match in the deepest namespace,
     because this overload mechanism only gets called if there's a
     function symbol to start off with.)  */

  new_namespace = (char *) alloca (namespace_len + 1);
  strncpy (new_namespace, qualified_name, namespace_len);
  new_namespace[namespace_len] = '\0';

  std::vector<symbol *> new_oload_syms
    = make_symbol_overload_list (func_name, new_namespace);

  /* If we have reached the deepest level perform argument
     determined lookup.  */
  if (!searched_deeper && !no_adl)
    {
      int ix;
      struct type **arg_types;

      /* Prepare list of argument types for overload resolution.  */
      arg_types = (struct type **)
	alloca (args.size () * (sizeof (struct type *)));
      for (ix = 0; ix < args.size (); ix++)
	arg_types[ix] = value_type (args[ix]);
      add_symbol_overload_list_adl ({arg_types, args.size ()}, func_name,
				    &new_oload_syms);
    }

  badness_vector new_oload_champ_bv;
  new_oload_champ = find_oload_champ (args,
				      new_oload_syms.size (),
				      NULL, NULL, new_oload_syms.data (),
				      &new_oload_champ_bv);

  /* Case 1: We found a good match.  Free earlier matches (if any),
     and return it.  Case 2: We didn't find a good match, but we're
     not the deepest function.  Then go with the bad match that the
     deeper function found.  Case 3: We found a bad match, and we're
     the deepest function.  Then return what we found, even though
     it's a bad match.  */

  if (new_oload_champ != -1
      && classify_oload_match (new_oload_champ_bv, args.size (), 0) == STANDARD)
    {
      *oload_syms = std::move (new_oload_syms);
      *oload_champ = new_oload_champ;
      *oload_champ_bv = std::move (new_oload_champ_bv);
      return 1;
    }
  else if (searched_deeper)
    {
      return 0;
    }
  else
    {
      *oload_syms = std::move (new_oload_syms);
      *oload_champ = new_oload_champ;
      *oload_champ_bv = std::move (new_oload_champ_bv);
      return 0;
    }
}

/* Look for a function to take ARGS.  Find the best match from among
   the overloaded methods or functions given by METHODS or FUNCTIONS
   or XMETHODS, respectively.  One, and only one of METHODS, FUNCTIONS
   and XMETHODS can be non-NULL.

   NUM_FNS is the length of the array pointed at by METHODS, FUNCTIONS
   or XMETHODS, whichever is non-NULL.

   Return the index of the best match; store an indication of the
   quality of the match in OLOAD_CHAMP_BV.  */

static int
find_oload_champ (gdb::array_view<value *> args,
		  size_t num_fns,
		  fn_field *methods,
		  xmethod_worker_up *xmethods,
		  symbol **functions,
		  badness_vector *oload_champ_bv)
{
  /* A measure of how good an overloaded instance is.  */
  badness_vector bv;
  /* Index of best overloaded function.  */
  int oload_champ = -1;
  /* Current ambiguity state for overload resolution.  */
  int oload_ambiguous = 0;
  /* 0 => no ambiguity, 1 => two good funcs, 2 => incomparable funcs.  */

  /* A champion can be found among methods alone, or among functions
     alone, or in xmethods alone, but not in more than one of these
     groups.  */
  gdb_assert ((methods != NULL) + (functions != NULL) + (xmethods != NULL)
	      == 1);

  /* Consider each candidate in turn.  */
  for (size_t ix = 0; ix < num_fns; ix++)
    {
      int jj;
      int static_offset = 0;
      std::vector<type *> parm_types;

      if (xmethods != NULL)
	parm_types = xmethods[ix]->get_arg_types ();
      else
	{
	  size_t nparms;

	  if (methods != NULL)
	    {
	      nparms = TYPE_NFIELDS (TYPE_FN_FIELD_TYPE (methods, ix));
	      static_offset = oload_method_static_p (methods, ix);
	    }
	  else
	    nparms = TYPE_NFIELDS (SYMBOL_TYPE (functions[ix]));

	  parm_types.reserve (nparms);
	  for (jj = 0; jj < nparms; jj++)
	    {
	      type *t = (methods != NULL
			 ? (TYPE_FN_FIELD_ARGS (methods, ix)[jj].type)
			 : TYPE_FIELD_TYPE (SYMBOL_TYPE (functions[ix]),
					    jj));
	      parm_types.push_back (t);
	    }
	}

      /* Compare parameter types to supplied argument types.  Skip
         THIS for static methods.  */
      bv = rank_function (parm_types,
			  args.slice (static_offset));

      if (oload_champ_bv->empty ())
	{
	  *oload_champ_bv = std::move (bv);
	  oload_champ = 0;
	}
      else /* See whether current candidate is better or worse than
	      previous best.  */
	switch (compare_badness (bv, *oload_champ_bv))
	  {
	  case 0:		/* Top two contenders are equally good.  */
	    oload_ambiguous = 1;
	    break;
	  case 1:		/* Incomparable top contenders.  */
	    oload_ambiguous = 2;
	    break;
	  case 2:		/* New champion, record details.  */
	    *oload_champ_bv = std::move (bv);
	    oload_ambiguous = 0;
	    oload_champ = ix;
	    break;
	  case 3:
	  default:
	    break;
	  }
      if (overload_debug)
	{
	  if (methods != NULL)
	    fprintf_filtered (gdb_stderr,
			      "Overloaded method instance %s, # of parms %d\n",
			      methods[ix].physname, (int) parm_types.size ());
	  else if (xmethods != NULL)
	    fprintf_filtered (gdb_stderr,
			      "Xmethod worker, # of parms %d\n",
			      (int) parm_types.size ());
	  else
	    fprintf_filtered (gdb_stderr,
			      "Overloaded function instance "
			      "%s # of parms %d\n",
			      functions[ix]->demangled_name (),
			      (int) parm_types.size ());
	  for (jj = 0; jj < args.size () - static_offset; jj++)
	    fprintf_filtered (gdb_stderr,
			      "...Badness @ %d : %d\n", 
			      jj, bv[jj].rank);
	  fprintf_filtered (gdb_stderr, "Overload resolution "
			    "champion is %d, ambiguous? %d\n", 
			    oload_champ, oload_ambiguous);
	}
    }

  return oload_champ;
}

/* Return 1 if we're looking at a static method, 0 if we're looking at
   a non-static method or a function that isn't a method.  */

static int
oload_method_static_p (struct fn_field *fns_ptr, int index)
{
  if (fns_ptr && index >= 0 && TYPE_FN_FIELD_STATIC_P (fns_ptr, index))
    return 1;
  else
    return 0;
}

/* Check how good an overload match OLOAD_CHAMP_BV represents.  */

static enum oload_classification
classify_oload_match (const badness_vector &oload_champ_bv,
		      int nargs,
		      int static_offset)
{
  int ix;
  enum oload_classification worst = STANDARD;

  for (ix = 1; ix <= nargs - static_offset; ix++)
    {
      /* If this conversion is as bad as INCOMPATIBLE_TYPE_BADNESS
         or worse return INCOMPATIBLE.  */
      if (compare_ranks (oload_champ_bv[ix],
                         INCOMPATIBLE_TYPE_BADNESS) <= 0)
	return INCOMPATIBLE;	/* Truly mismatched types.  */
      /* Otherwise If this conversion is as bad as
         NS_POINTER_CONVERSION_BADNESS or worse return NON_STANDARD.  */
      else if (compare_ranks (oload_champ_bv[ix],
                              NS_POINTER_CONVERSION_BADNESS) <= 0)
	worst = NON_STANDARD;	/* Non-standard type conversions
				   needed.  */
    }

  /* If no INCOMPATIBLE classification was found, return the worst one
     that was found (if any).  */
  return worst;
}

/* C++: return 1 is NAME is a legitimate name for the destructor of
   type TYPE.  If TYPE does not have a destructor, or if NAME is
   inappropriate for TYPE, an error is signaled.  Parameter TYPE should not yet
   have CHECK_TYPEDEF applied, this function will apply it itself.  */

int
destructor_name_p (const char *name, struct type *type)
{
  if (name[0] == '~')
    {
      const char *dname = type_name_or_error (type);
      const char *cp = strchr (dname, '<');
      unsigned int len;

      /* Do not compare the template part for template classes.  */
      if (cp == NULL)
	len = strlen (dname);
      else
	len = cp - dname;
      if (strlen (name + 1) != len || strncmp (dname, name + 1, len) != 0)
	error (_("name of destructor must equal name of class"));
      else
	return 1;
    }
  return 0;
}

/* Find an enum constant named NAME in TYPE.  TYPE must be an "enum
   class".  If the name is found, return a value representing it;
   otherwise throw an exception.  */

static struct value *
enum_constant_from_type (struct type *type, const char *name)
{
  int i;
  int name_len = strlen (name);

  gdb_assert (TYPE_CODE (type) == TYPE_CODE_ENUM
	      && TYPE_DECLARED_CLASS (type));

  for (i = TYPE_N_BASECLASSES (type); i < TYPE_NFIELDS (type); ++i)
    {
      const char *fname = TYPE_FIELD_NAME (type, i);
      int len;

      if (TYPE_FIELD_LOC_KIND (type, i) != FIELD_LOC_KIND_ENUMVAL
	  || fname == NULL)
	continue;

      /* Look for the trailing "::NAME", since enum class constant
	 names are qualified here.  */
      len = strlen (fname);
      if (len + 2 >= name_len
	  && fname[len - name_len - 2] == ':'
	  && fname[len - name_len - 1] == ':'
	  && strcmp (&fname[len - name_len], name) == 0)
	return value_from_longest (type, TYPE_FIELD_ENUMVAL (type, i));
    }

  error (_("no constant named \"%s\" in enum \"%s\""),
	 name, TYPE_NAME (type));
}

/* C++: Given an aggregate type CURTYPE, and a member name NAME,
   return the appropriate member (or the address of the member, if
   WANT_ADDRESS).  This function is used to resolve user expressions
   of the form "DOMAIN::NAME".  For more details on what happens, see
   the comment before value_struct_elt_for_reference.  */

struct value *
value_aggregate_elt (struct type *curtype, const char *name,
		     struct type *expect_type, int want_address,
		     enum noside noside)
{
  switch (TYPE_CODE (curtype))
    {
    case TYPE_CODE_STRUCT:
    case TYPE_CODE_UNION:
      return value_struct_elt_for_reference (curtype, 0, curtype, 
					     name, expect_type,
					     want_address, noside);
    case TYPE_CODE_NAMESPACE:
      return value_namespace_elt (curtype, name, 
				  want_address, noside);

    case TYPE_CODE_ENUM:
      return enum_constant_from_type (curtype, name);

    default:
      internal_error (__FILE__, __LINE__,
		      _("non-aggregate type in value_aggregate_elt"));
    }
}

/* Compares the two method/function types T1 and T2 for "equality" 
   with respect to the methods' parameters.  If the types of the
   two parameter lists are the same, returns 1; 0 otherwise.  This
   comparison may ignore any artificial parameters in T1 if
   SKIP_ARTIFICIAL is non-zero.  This function will ALWAYS skip
   the first artificial parameter in T1, assumed to be a 'this' pointer.

   The type T2 is expected to have come from make_params (in eval.c).  */

static int
compare_parameters (struct type *t1, struct type *t2, int skip_artificial)
{
  int start = 0;

  if (TYPE_NFIELDS (t1) > 0 && TYPE_FIELD_ARTIFICIAL (t1, 0))
    ++start;

  /* If skipping artificial fields, find the first real field
     in T1.  */
  if (skip_artificial)
    {
      while (start < TYPE_NFIELDS (t1)
	     && TYPE_FIELD_ARTIFICIAL (t1, start))
	++start;
    }

  /* Now compare parameters.  */

  /* Special case: a method taking void.  T1 will contain no
     non-artificial fields, and T2 will contain TYPE_CODE_VOID.  */
  if ((TYPE_NFIELDS (t1) - start) == 0 && TYPE_NFIELDS (t2) == 1
      && TYPE_CODE (TYPE_FIELD_TYPE (t2, 0)) == TYPE_CODE_VOID)
    return 1;

  if ((TYPE_NFIELDS (t1) - start) == TYPE_NFIELDS (t2))
    {
      int i;

      for (i = 0; i < TYPE_NFIELDS (t2); ++i)
	{
	  if (compare_ranks (rank_one_type (TYPE_FIELD_TYPE (t1, start + i),
					    TYPE_FIELD_TYPE (t2, i), NULL),
	                     EXACT_MATCH_BADNESS) != 0)
	    return 0;
	}

      return 1;
    }

  return 0;
}

/* C++: Given an aggregate type VT, and a class type CLS, search
   recursively for CLS using value V; If found, store the offset
   which is either fetched from the virtual base pointer if CLS
   is virtual or accumulated offset of its parent classes if
   CLS is non-virtual in *BOFFS, set ISVIRT to indicate if CLS
   is virtual, and return true.  If not found, return false.  */

static bool
get_baseclass_offset (struct type *vt, struct type *cls,
		      struct value *v, int *boffs, bool *isvirt)
{
  for (int i = 0; i < TYPE_N_BASECLASSES (vt); i++)
    {
      struct type *t = TYPE_FIELD_TYPE (vt, i);
      if (types_equal (t, cls))
        {
          if (BASETYPE_VIA_VIRTUAL (vt, i))
            {
	      const gdb_byte *adr = value_contents_for_printing (v);
	      *boffs = baseclass_offset (vt, i, adr, value_offset (v),
					 value_as_long (v), v);
	      *isvirt = true;
            }
          else
	    *isvirt = false;
          return true;
        }

      if (get_baseclass_offset (check_typedef (t), cls, v, boffs, isvirt))
        {
	  if (*isvirt == false)	/* Add non-virtual base offset.  */
	    {
	      const gdb_byte *adr = value_contents_for_printing (v);
	      *boffs += baseclass_offset (vt, i, adr, value_offset (v),
					  value_as_long (v), v);
	    }
	  return true;
	}
    }

  return false;
}

/* C++: Given an aggregate type CURTYPE, and a member name NAME,
   return the address of this member as a "pointer to member" type.
   If INTYPE is non-null, then it will be the type of the member we
   are looking for.  This will help us resolve "pointers to member
   functions".  This function is used to resolve user expressions of
   the form "DOMAIN::NAME".  */

static struct value *
value_struct_elt_for_reference (struct type *domain, int offset,
				struct type *curtype, const char *name,
				struct type *intype, 
				int want_address,
				enum noside noside)
{
  struct type *t = check_typedef (curtype);
  int i;
  struct value *result;

  if (TYPE_CODE (t) != TYPE_CODE_STRUCT
      && TYPE_CODE (t) != TYPE_CODE_UNION)
    error (_("Internal error: non-aggregate type "
	     "to value_struct_elt_for_reference"));

  for (i = TYPE_NFIELDS (t) - 1; i >= TYPE_N_BASECLASSES (t); i--)
    {
      const char *t_field_name = TYPE_FIELD_NAME (t, i);

      if (t_field_name && strcmp (t_field_name, name) == 0)
	{
	  if (field_is_static (&TYPE_FIELD (t, i)))
	    {
	      struct value *v = value_static_field (t, i);
	      if (want_address)
		v = value_addr (v);
	      return v;
	    }
	  if (TYPE_FIELD_PACKED (t, i))
	    error (_("pointers to bitfield members not allowed"));

	  if (want_address)
	    return value_from_longest
	      (lookup_memberptr_type (TYPE_FIELD_TYPE (t, i), domain),
	       offset + (LONGEST) (TYPE_FIELD_BITPOS (t, i) >> 3));
	  else if (noside != EVAL_NORMAL)
	    return allocate_value (TYPE_FIELD_TYPE (t, i));
	  else
	    {
	      /* Try to evaluate NAME as a qualified name with implicit
		 this pointer.  In this case, attempt to return the
		 equivalent to `this->*(&TYPE::NAME)'.  */
	      struct value *v = value_of_this_silent (current_language);
	      if (v != NULL)
		{
		  struct value *ptr, *this_v = v;
		  long mem_offset;
		  struct type *type, *tmp;

		  ptr = value_aggregate_elt (domain, name, NULL, 1, noside);
		  type = check_typedef (value_type (ptr));
		  gdb_assert (type != NULL
			      && TYPE_CODE (type) == TYPE_CODE_MEMBERPTR);
		  tmp = lookup_pointer_type (TYPE_SELF_TYPE (type));
		  v = value_cast_pointers (tmp, v, 1);
		  mem_offset = value_as_long (ptr);
		  if (domain != curtype)
		    {
		      /* Find class offset of type CURTYPE from either its
			 parent type DOMAIN or the type of implied this.  */
		      int boff = 0;
		      bool isvirt = false;
		      if (get_baseclass_offset (domain, curtype, v, &boff,
						&isvirt))
		        mem_offset += boff;
		      else
		        {
		          struct type *p = check_typedef (value_type (this_v));
		          p = check_typedef (TYPE_TARGET_TYPE (p));
		          if (get_baseclass_offset (p, curtype, this_v,
						    &boff, &isvirt))
		            mem_offset += boff;
		        }
		    }
		  tmp = lookup_pointer_type (TYPE_TARGET_TYPE (type));
		  result = value_from_pointer (tmp,
					       value_as_long (v) + mem_offset);
		  return value_ind (result);
		}

	      error (_("Cannot reference non-static field \"%s\""), name);
	    }
	}
    }

  /* C++: If it was not found as a data field, then try to return it
     as a pointer to a method.  */

  /* Perform all necessary dereferencing.  */
  while (intype && TYPE_CODE (intype) == TYPE_CODE_PTR)
    intype = TYPE_TARGET_TYPE (intype);

  for (i = TYPE_NFN_FIELDS (t) - 1; i >= 0; --i)
    {
      const char *t_field_name = TYPE_FN_FIELDLIST_NAME (t, i);

      if (t_field_name && strcmp (t_field_name, name) == 0)
	{
	  int j;
	  int len = TYPE_FN_FIELDLIST_LENGTH (t, i);
	  struct fn_field *f = TYPE_FN_FIELDLIST1 (t, i);

	  check_stub_method_group (t, i);

	  if (intype)
	    {
	      for (j = 0; j < len; ++j)
		{
		  if (TYPE_CONST (intype) != TYPE_FN_FIELD_CONST (f, j))
		    continue;
		  if (TYPE_VOLATILE (intype) != TYPE_FN_FIELD_VOLATILE (f, j))
		    continue;

		  if (compare_parameters (TYPE_FN_FIELD_TYPE (f, j), intype, 0)
		      || compare_parameters (TYPE_FN_FIELD_TYPE (f, j),
					     intype, 1))
		    break;
		}

	      if (j == len)
		error (_("no member function matches "
			 "that type instantiation"));
	    }
	  else
	    {
	      int ii;

	      j = -1;
	      for (ii = 0; ii < len; ++ii)
		{
		  /* Skip artificial methods.  This is necessary if,
		     for example, the user wants to "print
		     subclass::subclass" with only one user-defined
		     constructor.  There is no ambiguity in this case.
		     We are careful here to allow artificial methods
		     if they are the unique result.  */
		  if (TYPE_FN_FIELD_ARTIFICIAL (f, ii))
		    {
		      if (j == -1)
			j = ii;
		      continue;
		    }

		  /* Desired method is ambiguous if more than one
		     method is defined.  */
		  if (j != -1 && !TYPE_FN_FIELD_ARTIFICIAL (f, j))
		    error (_("non-unique member `%s' requires "
			     "type instantiation"), name);

		  j = ii;
		}

	      if (j == -1)
		error (_("no matching member function"));
	    }

	  if (TYPE_FN_FIELD_STATIC_P (f, j))
	    {
	      struct symbol *s = 
		lookup_symbol (TYPE_FN_FIELD_PHYSNAME (f, j),
			       0, VAR_DOMAIN, 0).symbol;

	      if (s == NULL)
		return NULL;

	      if (want_address)
		return value_addr (read_var_value (s, 0, 0));
	      else
		return read_var_value (s, 0, 0);
	    }

	  if (TYPE_FN_FIELD_VIRTUAL_P (f, j))
	    {
	      if (want_address)
		{
		  result = allocate_value
		    (lookup_methodptr_type (TYPE_FN_FIELD_TYPE (f, j)));
		  cplus_make_method_ptr (value_type (result),
					 value_contents_writeable (result),
					 TYPE_FN_FIELD_VOFFSET (f, j), 1);
		}
	      else if (noside == EVAL_AVOID_SIDE_EFFECTS)
		return allocate_value (TYPE_FN_FIELD_TYPE (f, j));
	      else
		error (_("Cannot reference virtual member function \"%s\""),
		       name);
	    }
	  else
	    {
	      struct symbol *s = 
		lookup_symbol (TYPE_FN_FIELD_PHYSNAME (f, j),
			       0, VAR_DOMAIN, 0).symbol;

	      if (s == NULL)
		return NULL;

	      struct value *v = read_var_value (s, 0, 0);
	      if (!want_address)
		result = v;
	      else
		{
		  result = allocate_value (lookup_methodptr_type (TYPE_FN_FIELD_TYPE (f, j)));
		  cplus_make_method_ptr (value_type (result),
					 value_contents_writeable (result),
					 value_address (v), 0);
		}
	    }
	  return result;
	}
    }
  for (i = TYPE_N_BASECLASSES (t) - 1; i >= 0; i--)
    {
      struct value *v;
      int base_offset;

      if (BASETYPE_VIA_VIRTUAL (t, i))
	base_offset = 0;
      else
	base_offset = TYPE_BASECLASS_BITPOS (t, i) / 8;
      v = value_struct_elt_for_reference (domain,
					  offset + base_offset,
					  TYPE_BASECLASS (t, i),
					  name, intype, 
					  want_address, noside);
      if (v)
	return v;
    }

  /* As a last chance, pretend that CURTYPE is a namespace, and look
     it up that way; this (frequently) works for types nested inside
     classes.  */

  return value_maybe_namespace_elt (curtype, name, 
				    want_address, noside);
}

/* C++: Return the member NAME of the namespace given by the type
   CURTYPE.  */

static struct value *
value_namespace_elt (const struct type *curtype,
		     const char *name, int want_address,
		     enum noside noside)
{
  struct value *retval = value_maybe_namespace_elt (curtype, name,
						    want_address, 
						    noside);

  if (retval == NULL)
    error (_("No symbol \"%s\" in namespace \"%s\"."), 
	   name, TYPE_NAME (curtype));

  return retval;
}

/* A helper function used by value_namespace_elt and
   value_struct_elt_for_reference.  It looks up NAME inside the
   context CURTYPE; this works if CURTYPE is a namespace or if CURTYPE
   is a class and NAME refers to a type in CURTYPE itself (as opposed
   to, say, some base class of CURTYPE).  */

static struct value *
value_maybe_namespace_elt (const struct type *curtype,
			   const char *name, int want_address,
			   enum noside noside)
{
  const char *namespace_name = TYPE_NAME (curtype);
  struct block_symbol sym;
  struct value *result;

  sym = cp_lookup_symbol_namespace (namespace_name, name,
				    get_selected_block (0), VAR_DOMAIN);

  if (sym.symbol == NULL)
    return NULL;
  else if ((noside == EVAL_AVOID_SIDE_EFFECTS)
	   && (SYMBOL_CLASS (sym.symbol) == LOC_TYPEDEF))
    result = allocate_value (SYMBOL_TYPE (sym.symbol));
  else
    result = value_of_variable (sym.symbol, sym.block);

  if (want_address)
    result = value_addr (result);

  return result;
}

/* Given a pointer or a reference value V, find its real (RTTI) type.

   Other parameters FULL, TOP, USING_ENC as with value_rtti_type()
   and refer to the values computed for the object pointed to.  */

struct type *
value_rtti_indirect_type (struct value *v, int *full, 
			  LONGEST *top, int *using_enc)
{
  struct value *target = NULL;
  struct type *type, *real_type, *target_type;

  type = value_type (v);
  type = check_typedef (type);
  if (TYPE_IS_REFERENCE (type))
    target = coerce_ref (v);
  else if (TYPE_CODE (type) == TYPE_CODE_PTR)
    {

      try
        {
	  target = value_ind (v);
        }
      catch (const gdb_exception_error &except)
	{
	  if (except.error == MEMORY_ERROR)
	    {
	      /* value_ind threw a memory error. The pointer is NULL or
	         contains an uninitialized value: we can't determine any
	         type.  */
	      return NULL;
	    }
	  throw;
	}
    }
  else
    return NULL;

  real_type = value_rtti_type (target, full, top, using_enc);

  if (real_type)
    {
      /* Copy qualifiers to the referenced object.  */
      target_type = value_type (target);
      real_type = make_cv_type (TYPE_CONST (target_type),
				TYPE_VOLATILE (target_type), real_type, NULL);
      if (TYPE_IS_REFERENCE (type))
        real_type = lookup_reference_type (real_type, TYPE_CODE (type));
      else if (TYPE_CODE (type) == TYPE_CODE_PTR)
        real_type = lookup_pointer_type (real_type);
      else
        internal_error (__FILE__, __LINE__, _("Unexpected value type."));

      /* Copy qualifiers to the pointer/reference.  */
      real_type = make_cv_type (TYPE_CONST (type), TYPE_VOLATILE (type),
				real_type, NULL);
    }

  return real_type;
}

/* Given a value pointed to by ARGP, check its real run-time type, and
   if that is different from the enclosing type, create a new value
   using the real run-time type as the enclosing type (and of the same
   type as ARGP) and return it, with the embedded offset adjusted to
   be the correct offset to the enclosed object.  RTYPE is the type,
   and XFULL, XTOP, and XUSING_ENC are the other parameters, computed
   by value_rtti_type().  If these are available, they can be supplied
   and a second call to value_rtti_type() is avoided.  (Pass RTYPE ==
   NULL if they're not available.  */

struct value *
value_full_object (struct value *argp, 
		   struct type *rtype, 
		   int xfull, int xtop,
		   int xusing_enc)
{
  struct type *real_type;
  int full = 0;
  LONGEST top = -1;
  int using_enc = 0;
  struct value *new_val;

  if (rtype)
    {
      real_type = rtype;
      full = xfull;
      top = xtop;
      using_enc = xusing_enc;
    }
  else
    real_type = value_rtti_type (argp, &full, &top, &using_enc);

  /* If no RTTI data, or if object is already complete, do nothing.  */
  if (!real_type || real_type == value_enclosing_type (argp))
    return argp;

  /* In a destructor we might see a real type that is a superclass of
     the object's type.  In this case it is better to leave the object
     as-is.  */
  if (full
      && TYPE_LENGTH (real_type) < TYPE_LENGTH (value_enclosing_type (argp)))
    return argp;

  /* If we have the full object, but for some reason the enclosing
     type is wrong, set it.  */
  /* pai: FIXME -- sounds iffy */
  if (full)
    {
      argp = value_copy (argp);
      set_value_enclosing_type (argp, real_type);
      return argp;
    }

  /* Check if object is in memory.  */
  if (VALUE_LVAL (argp) != lval_memory)
    {
      warning (_("Couldn't retrieve complete object of RTTI "
		 "type %s; object may be in register(s)."), 
	       TYPE_NAME (real_type));

      return argp;
    }

  /* All other cases -- retrieve the complete object.  */
  /* Go back by the computed top_offset from the beginning of the
     object, adjusting for the embedded offset of argp if that's what
     value_rtti_type used for its computation.  */
  new_val = value_at_lazy (real_type, value_address (argp) - top +
			   (using_enc ? 0 : value_embedded_offset (argp)));
  deprecated_set_value_type (new_val, value_type (argp));
  set_value_embedded_offset (new_val, (using_enc
				       ? top + value_embedded_offset (argp)
				       : top));
  return new_val;
}


/* Return the value of the local variable, if one exists.  Throw error
   otherwise, such as if the request is made in an inappropriate context.  */

struct value *
value_of_this (const struct language_defn *lang)
{
  struct block_symbol sym;
  const struct block *b;
  struct frame_info *frame;

  if (!lang->la_name_of_this)
    error (_("no `this' in current language"));

  frame = get_selected_frame (_("no frame selected"));

  b = get_frame_block (frame, NULL);

  sym = lookup_language_this (lang, b);
  if (sym.symbol == NULL)
    error (_("current stack frame does not contain a variable named `%s'"),
	   lang->la_name_of_this);

  return read_var_value (sym.symbol, sym.block, frame);
}

/* Return the value of the local variable, if one exists.  Return NULL
   otherwise.  Never throw error.  */

struct value *
value_of_this_silent (const struct language_defn *lang)
{
  struct value *ret = NULL;

  try
    {
      ret = value_of_this (lang);
    }
  catch (const gdb_exception_error &except)
    {
    }

  return ret;
}

/* Create a slice (sub-string, sub-array) of ARRAY, that is LENGTH
   elements long, starting at LOWBOUND.  The result has the same lower
   bound as the original ARRAY.  */

struct value *
value_slice (struct value *array, int lowbound, int length)
{
  struct type *slice_range_type, *slice_type, *range_type;
  LONGEST lowerbound, upperbound;
  struct value *slice;
  struct type *array_type;

  array_type = check_typedef (value_type (array));
  if (TYPE_CODE (array_type) != TYPE_CODE_ARRAY
      && TYPE_CODE (array_type) != TYPE_CODE_STRING)
    error (_("cannot take slice of non-array"));

  if (type_not_allocated (array_type))
    error (_("array not allocated"));
  if (type_not_associated (array_type))
    error (_("array not associated"));

  range_type = TYPE_INDEX_TYPE (array_type);
  if (get_discrete_bounds (range_type, &lowerbound, &upperbound) < 0)
    error (_("slice from bad array or bitstring"));

  if (lowbound < lowerbound || length < 0
      || lowbound + length - 1 > upperbound)
    error (_("slice out of range"));

  /* FIXME-type-allocation: need a way to free this type when we are
     done with it.  */
  slice_range_type = create_static_range_type (NULL,
					       TYPE_TARGET_TYPE (range_type),
					       lowbound,
					       lowbound + length - 1);

  {
    struct type *element_type = TYPE_TARGET_TYPE (array_type);
    LONGEST offset
      = (lowbound - lowerbound) * TYPE_LENGTH (check_typedef (element_type));

    slice_type = create_array_type (NULL,
				    element_type,
				    slice_range_type);
    TYPE_CODE (slice_type) = TYPE_CODE (array_type);

    if (VALUE_LVAL (array) == lval_memory && value_lazy (array))
      slice = allocate_value_lazy (slice_type);
    else
      {
	slice = allocate_value (slice_type);
	value_contents_copy (slice, 0, array, offset,
			     type_length_units (slice_type));
      }

    set_value_component_location (slice, array);
    set_value_offset (slice, value_offset (array) + offset);
  }

  return slice;
}

/* Create a value for a FORTRAN complex number.  Currently most of the
   time values are coerced to COMPLEX*16 (i.e. a complex number
   composed of 2 doubles.  This really should be a smarter routine
   that figures out precision intelligently as opposed to assuming
   doubles.  FIXME: fmb  */

struct value *
value_literal_complex (struct value *arg1, 
		       struct value *arg2,
		       struct type *type)
{
  struct value *val;
  struct type *real_type = TYPE_TARGET_TYPE (type);

  val = allocate_value (type);
  arg1 = value_cast (real_type, arg1);
  arg2 = value_cast (real_type, arg2);

  memcpy (value_contents_raw (val),
	  value_contents (arg1), TYPE_LENGTH (real_type));
  memcpy (value_contents_raw (val) + TYPE_LENGTH (real_type),
	  value_contents (arg2), TYPE_LENGTH (real_type));
  return val;
}

/* Cast a value into the appropriate complex data type.  */

static struct value *
cast_into_complex (struct type *type, struct value *val)
{
  struct type *real_type = TYPE_TARGET_TYPE (type);

  if (TYPE_CODE (value_type (val)) == TYPE_CODE_COMPLEX)
    {
      struct type *val_real_type = TYPE_TARGET_TYPE (value_type (val));
      struct value *re_val = allocate_value (val_real_type);
      struct value *im_val = allocate_value (val_real_type);

      memcpy (value_contents_raw (re_val),
	      value_contents (val), TYPE_LENGTH (val_real_type));
      memcpy (value_contents_raw (im_val),
	      value_contents (val) + TYPE_LENGTH (val_real_type),
	      TYPE_LENGTH (val_real_type));

      return value_literal_complex (re_val, im_val, type);
    }
  else if (TYPE_CODE (value_type (val)) == TYPE_CODE_FLT
	   || TYPE_CODE (value_type (val)) == TYPE_CODE_INT)
    return value_literal_complex (val, 
				  value_zero (real_type, not_lval), 
				  type);
  else
    error (_("cannot cast non-number to complex"));
}

void
_initialize_valops (void)
{
  add_setshow_boolean_cmd ("overload-resolution", class_support,
			   &overload_resolution, _("\
Set overload resolution in evaluating C++ functions."), _("\
Show overload resolution in evaluating C++ functions."), 
			   NULL, NULL,
			   show_overload_resolution,
			   &setlist, &showlist);
  overload_resolution = 1;
}
