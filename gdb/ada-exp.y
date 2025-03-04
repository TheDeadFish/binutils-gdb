/* YACC parser for Ada expressions, for GDB.
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

/* Parse an Ada expression from text in a string,
   and return the result as a  struct expression  pointer.
   That structure contains arithmetic operations in reverse polish,
   with constants represented by operations that are followed by special data.
   See expression.h for the details of the format.
   What is important here is that it can be built up sequentially
   during the process of parsing; the lower levels of the tree always
   come first in the result.

   malloc's and realloc's in this file are transformed to
   xmalloc and xrealloc respectively by the same sed command in the
   makefile that remaps any other malloc/realloc inserted by the parser
   generator.  Doing this with #defines and trying to control the interaction
   with include files (<malloc.h> and <stdlib.h> for example) just became
   too messy, particularly when such includes can be inserted at random
   times by the parser generator.  */

%{

#include "defs.h"
#include <ctype.h>
#include "expression.h"
#include "value.h"
#include "parser-defs.h"
#include "language.h"
#include "ada-lang.h"
#include "bfd.h" /* Required by objfiles.h.  */
#include "symfile.h" /* Required by objfiles.h.  */
#include "objfiles.h" /* For have_full_symbols and have_partial_symbols */
#include "frame.h"
#include "block.h"

#define parse_type(ps) builtin_type (ps->gdbarch ())

/* Remap normal yacc parser interface names (yyparse, yylex, yyerror,
   etc).  */
#define GDB_YY_REMAP_PREFIX ada_
#include "yy-remap.h"

struct name_info {
  struct symbol *sym;
  struct minimal_symbol *msym;
  const struct block *block;
  struct stoken stoken;
};

/* The state of the parser, used internally when we are parsing the
   expression.  */

static struct parser_state *pstate = NULL;

static struct stoken empty_stoken = { "", 0 };

/* If expression is in the context of TYPE'(...), then TYPE, else
 * NULL.  */
static struct type *type_qualifier;

int yyparse (void);

static int yylex (void);

static void yyerror (const char *);

static void write_int (struct parser_state *, LONGEST, struct type *);

static void write_object_renaming (struct parser_state *,
				   const struct block *, const char *, int,
				   const char *, int);

static struct type* write_var_or_type (struct parser_state *,
				       const struct block *, struct stoken);

static void write_name_assoc (struct parser_state *, struct stoken);

static void write_exp_op_with_string (struct parser_state *, enum exp_opcode,
				      struct stoken);

static const struct block *block_lookup (const struct block *, const char *);

static LONGEST convert_char_literal (struct type *, LONGEST);

static void write_ambiguous_var (struct parser_state *,
				 const struct block *, char *, int);

static struct type *type_int (struct parser_state *);

static struct type *type_long (struct parser_state *);

static struct type *type_long_long (struct parser_state *);

static struct type *type_long_double (struct parser_state *);

static struct type *type_char (struct parser_state *);

static struct type *type_boolean (struct parser_state *);

static struct type *type_system_address (struct parser_state *);

%}

%union
  {
    LONGEST lval;
    struct {
      LONGEST val;
      struct type *type;
    } typed_val;
    struct {
      gdb_byte val[16];
      struct type *type;
    } typed_val_float;
    struct type *tval;
    struct stoken sval;
    const struct block *bval;
    struct internalvar *ivar;
  }

%type <lval> positional_list component_groups component_associations
%type <lval> aggregate_component_list 
%type <tval> var_or_type

%token <typed_val> INT NULL_PTR CHARLIT
%token <typed_val_float> FLOAT
%token TRUEKEYWORD FALSEKEYWORD
%token COLONCOLON
%token <sval> STRING NAME DOT_ID 
%type <bval> block
%type <lval> arglist tick_arglist

%type <tval> save_qualifier

%token DOT_ALL

/* Special type cases, put in to allow the parser to distinguish different
   legal basetypes.  */
%token <sval> DOLLAR_VARIABLE

%nonassoc ASSIGN
%left _AND_ OR XOR THEN ELSE
%left '=' NOTEQUAL '<' '>' LEQ GEQ IN DOTDOT
%left '@'
%left '+' '-' '&'
%left UNARY
%left '*' '/' MOD REM
%right STARSTAR ABS NOT

/* Artificial token to give NAME => ... and NAME | priority over reducing 
   NAME to <primary> and to give <primary>' priority over reducing <primary>
   to <simple_exp>. */
%nonassoc VAR

%nonassoc ARROW '|'

%right TICK_ACCESS TICK_ADDRESS TICK_FIRST TICK_LAST TICK_LENGTH
%right TICK_MAX TICK_MIN TICK_MODULUS
%right TICK_POS TICK_RANGE TICK_SIZE TICK_TAG TICK_VAL
 /* The following are right-associative only so that reductions at this
    precedence have lower precedence than '.' and '('.  The syntax still
    forces a.b.c, e.g., to be LEFT-associated.  */
%right '.' '(' '[' DOT_ID DOT_ALL

%token NEW OTHERS


%%

start   :	exp1
	;

/* Expressions, including the sequencing operator.  */
exp1	:	exp
	|	exp1 ';' exp
			{ write_exp_elt_opcode (pstate, BINOP_COMMA); }
	| 	primary ASSIGN exp   /* Extension for convenience */
			{ write_exp_elt_opcode (pstate, BINOP_ASSIGN); }
	;

/* Expressions, not including the sequencing operator.  */
primary :	primary DOT_ALL
			{ write_exp_elt_opcode (pstate, UNOP_IND); }
	;

primary :	primary DOT_ID
			{ write_exp_op_with_string (pstate, STRUCTOP_STRUCT,
						    $2); }
	;

primary :	primary '(' arglist ')'
			{
			  write_exp_elt_opcode (pstate, OP_FUNCALL);
			  write_exp_elt_longcst (pstate, $3);
			  write_exp_elt_opcode (pstate, OP_FUNCALL);
		        }
	|	var_or_type '(' arglist ')'
			{
			  if ($1 != NULL)
			    {
			      if ($3 != 1)
				error (_("Invalid conversion"));
			      write_exp_elt_opcode (pstate, UNOP_CAST);
			      write_exp_elt_type (pstate, $1);
			      write_exp_elt_opcode (pstate, UNOP_CAST);
			    }
			  else
			    {
			      write_exp_elt_opcode (pstate, OP_FUNCALL);
			      write_exp_elt_longcst (pstate, $3);
			      write_exp_elt_opcode (pstate, OP_FUNCALL);
			    }
			}
	;

primary :	var_or_type '\'' save_qualifier { type_qualifier = $1; } 
		   '(' exp ')'
			{
			  if ($1 == NULL)
			    error (_("Type required for qualification"));
			  write_exp_elt_opcode (pstate, UNOP_QUAL);
			  write_exp_elt_type (pstate, $1);
			  write_exp_elt_opcode (pstate, UNOP_QUAL);
			  type_qualifier = $3;
			}
	;

save_qualifier : 	{ $$ = type_qualifier; }
	;

primary :
		primary '(' simple_exp DOTDOT simple_exp ')'
			{ write_exp_elt_opcode (pstate, TERNOP_SLICE); }
	|	var_or_type '(' simple_exp DOTDOT simple_exp ')'
			{ if ($1 == NULL) 
                            write_exp_elt_opcode (pstate, TERNOP_SLICE);
			  else
			    error (_("Cannot slice a type"));
			}
	;

primary :	'(' exp1 ')'	{ }
	;

/* The following rule causes a conflict with the type conversion
       var_or_type (exp)
   To get around it, we give '(' higher priority and add bridge rules for 
       var_or_type (exp, exp, ...)
       var_or_type (exp .. exp)
   We also have the action for  var_or_type(exp) generate a function call
   when the first symbol does not denote a type. */

primary :	var_or_type	%prec VAR
			{ if ($1 != NULL)
			    {
			      write_exp_elt_opcode (pstate, OP_TYPE);
			      write_exp_elt_type (pstate, $1);
			      write_exp_elt_opcode (pstate, OP_TYPE);
			    }
			}
	;

primary :	DOLLAR_VARIABLE /* Various GDB extensions */
			{ write_dollar_variable (pstate, $1); }
	;

primary :     	aggregate
        ;        

simple_exp : 	primary
	;

simple_exp :	'-' simple_exp    %prec UNARY
			{ write_exp_elt_opcode (pstate, UNOP_NEG); }
	;

simple_exp :	'+' simple_exp    %prec UNARY
			{ write_exp_elt_opcode (pstate, UNOP_PLUS); }
	;

simple_exp :	NOT simple_exp    %prec UNARY
			{ write_exp_elt_opcode (pstate, UNOP_LOGICAL_NOT); }
	;

simple_exp :    ABS simple_exp	   %prec UNARY
			{ write_exp_elt_opcode (pstate, UNOP_ABS); }
	;

arglist	:		{ $$ = 0; }
	;

arglist	:	exp
			{ $$ = 1; }
	|	NAME ARROW exp
			{ $$ = 1; }
	|	arglist ',' exp
			{ $$ = $1 + 1; }
	|	arglist ',' NAME ARROW exp
			{ $$ = $1 + 1; }
	;

primary :	'{' var_or_type '}' primary  %prec '.'
		/* GDB extension */
			{ 
			  if ($2 == NULL)
			    error (_("Type required within braces in coercion"));
			  write_exp_elt_opcode (pstate, UNOP_MEMVAL);
			  write_exp_elt_type (pstate, $2);
			  write_exp_elt_opcode (pstate, UNOP_MEMVAL);
			}
	;

/* Binary operators in order of decreasing precedence.  */

simple_exp 	: 	simple_exp STARSTAR simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_EXP); }
	;

simple_exp	:	simple_exp '*' simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_MUL); }
	;

simple_exp	:	simple_exp '/' simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_DIV); }
	;

simple_exp	:	simple_exp REM simple_exp /* May need to be fixed to give correct Ada REM */
			{ write_exp_elt_opcode (pstate, BINOP_REM); }
	;

simple_exp	:	simple_exp MOD simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_MOD); }
	;

simple_exp	:	simple_exp '@' simple_exp	/* GDB extension */
			{ write_exp_elt_opcode (pstate, BINOP_REPEAT); }
	;

simple_exp	:	simple_exp '+' simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_ADD); }
	;

simple_exp	:	simple_exp '&' simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_CONCAT); }
	;

simple_exp	:	simple_exp '-' simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_SUB); }
	;

relation :	simple_exp
	;

relation :	simple_exp '=' simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_EQUAL); }
	;

relation :	simple_exp NOTEQUAL simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_NOTEQUAL); }
	;

relation :	simple_exp LEQ simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_LEQ); }
	;

relation :	simple_exp IN simple_exp DOTDOT simple_exp
			{ write_exp_elt_opcode (pstate, TERNOP_IN_RANGE); }
        |       simple_exp IN primary TICK_RANGE tick_arglist
			{ write_exp_elt_opcode (pstate, BINOP_IN_BOUNDS);
			  write_exp_elt_longcst (pstate, (LONGEST) $5);
			  write_exp_elt_opcode (pstate, BINOP_IN_BOUNDS);
			}
 	|	simple_exp IN var_or_type	%prec TICK_ACCESS
			{ 
			  if ($3 == NULL)
			    error (_("Right operand of 'in' must be type"));
			  write_exp_elt_opcode (pstate, UNOP_IN_RANGE);
		          write_exp_elt_type (pstate, $3);
		          write_exp_elt_opcode (pstate, UNOP_IN_RANGE);
			}
	|	simple_exp NOT IN simple_exp DOTDOT simple_exp
			{ write_exp_elt_opcode (pstate, TERNOP_IN_RANGE);
		          write_exp_elt_opcode (pstate, UNOP_LOGICAL_NOT);
			}
        |       simple_exp NOT IN primary TICK_RANGE tick_arglist
			{ write_exp_elt_opcode (pstate, BINOP_IN_BOUNDS);
			  write_exp_elt_longcst (pstate, (LONGEST) $6);
			  write_exp_elt_opcode (pstate, BINOP_IN_BOUNDS);
		          write_exp_elt_opcode (pstate, UNOP_LOGICAL_NOT);
			}
 	|	simple_exp NOT IN var_or_type	%prec TICK_ACCESS
			{ 
			  if ($4 == NULL)
			    error (_("Right operand of 'in' must be type"));
			  write_exp_elt_opcode (pstate, UNOP_IN_RANGE);
		          write_exp_elt_type (pstate, $4);
		          write_exp_elt_opcode (pstate, UNOP_IN_RANGE);
		          write_exp_elt_opcode (pstate, UNOP_LOGICAL_NOT);
			}
	;

relation :	simple_exp GEQ simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_GEQ); }
	;

relation :	simple_exp '<' simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_LESS); }
	;

relation :	simple_exp '>' simple_exp
			{ write_exp_elt_opcode (pstate, BINOP_GTR); }
	;

exp	:	relation
	|	and_exp
	|	and_then_exp
	|	or_exp
	|	or_else_exp
	|	xor_exp
	;

and_exp :
		relation _AND_ relation 
			{ write_exp_elt_opcode (pstate, BINOP_BITWISE_AND); }
	|	and_exp _AND_ relation
			{ write_exp_elt_opcode (pstate, BINOP_BITWISE_AND); }
	;

and_then_exp :
	       relation _AND_ THEN relation
			{ write_exp_elt_opcode (pstate, BINOP_LOGICAL_AND); }
	|	and_then_exp _AND_ THEN relation
			{ write_exp_elt_opcode (pstate, BINOP_LOGICAL_AND); }
        ;

or_exp :
		relation OR relation 
			{ write_exp_elt_opcode (pstate, BINOP_BITWISE_IOR); }
	|	or_exp OR relation
			{ write_exp_elt_opcode (pstate, BINOP_BITWISE_IOR); }
	;

or_else_exp :
	       relation OR ELSE relation
			{ write_exp_elt_opcode (pstate, BINOP_LOGICAL_OR); }
	|      or_else_exp OR ELSE relation
			{ write_exp_elt_opcode (pstate, BINOP_LOGICAL_OR); }
        ;

xor_exp :       relation XOR relation
			{ write_exp_elt_opcode (pstate, BINOP_BITWISE_XOR); }
	|	xor_exp XOR relation
			{ write_exp_elt_opcode (pstate, BINOP_BITWISE_XOR); }
        ;

/* Primaries can denote types (OP_TYPE).  In cases such as 
   primary TICK_ADDRESS, where a type would be invalid, it will be
   caught when evaluate_subexp in ada-lang.c tries to evaluate the
   primary, expecting a value.  Precedence rules resolve the ambiguity
   in NAME TICK_ACCESS in favor of shifting to form a var_or_type.  A
   construct such as aType'access'access will again cause an error when
   aType'access evaluates to a type that evaluate_subexp attempts to 
   evaluate. */
primary :	primary TICK_ACCESS
			{ write_exp_elt_opcode (pstate, UNOP_ADDR); }
	|	primary TICK_ADDRESS
			{ write_exp_elt_opcode (pstate, UNOP_ADDR);
			  write_exp_elt_opcode (pstate, UNOP_CAST);
			  write_exp_elt_type (pstate,
					      type_system_address (pstate));
			  write_exp_elt_opcode (pstate, UNOP_CAST);
			}
	|	primary TICK_FIRST tick_arglist
			{ write_int (pstate, $3, type_int (pstate));
			  write_exp_elt_opcode (pstate, OP_ATR_FIRST); }
	|	primary TICK_LAST tick_arglist
			{ write_int (pstate, $3, type_int (pstate));
			  write_exp_elt_opcode (pstate, OP_ATR_LAST); }
	| 	primary TICK_LENGTH tick_arglist
			{ write_int (pstate, $3, type_int (pstate));
			  write_exp_elt_opcode (pstate, OP_ATR_LENGTH); }
        |       primary TICK_SIZE
			{ write_exp_elt_opcode (pstate, OP_ATR_SIZE); }
	|	primary TICK_TAG
			{ write_exp_elt_opcode (pstate, OP_ATR_TAG); }
        |       opt_type_prefix TICK_MIN '(' exp ',' exp ')'
			{ write_exp_elt_opcode (pstate, OP_ATR_MIN); }
        |       opt_type_prefix TICK_MAX '(' exp ',' exp ')'
			{ write_exp_elt_opcode (pstate, OP_ATR_MAX); }
	| 	opt_type_prefix TICK_POS '(' exp ')'
			{ write_exp_elt_opcode (pstate, OP_ATR_POS); }
	|	type_prefix TICK_VAL '(' exp ')'
			{ write_exp_elt_opcode (pstate, OP_ATR_VAL); }
	|	type_prefix TICK_MODULUS
			{ write_exp_elt_opcode (pstate, OP_ATR_MODULUS); }
	;

tick_arglist :			%prec '('
			{ $$ = 1; }
	| 	'(' INT ')'
			{ $$ = $2.val; }
	;

type_prefix :
                var_or_type
			{ 
			  if ($1 == NULL)
			    error (_("Prefix must be type"));
			  write_exp_elt_opcode (pstate, OP_TYPE);
			  write_exp_elt_type (pstate, $1);
			  write_exp_elt_opcode (pstate, OP_TYPE); }
	;

opt_type_prefix :
		type_prefix
	| 	/* EMPTY */
			{ write_exp_elt_opcode (pstate, OP_TYPE);
			  write_exp_elt_type (pstate,
					  parse_type (pstate)->builtin_void);
			  write_exp_elt_opcode (pstate, OP_TYPE); }
	;


primary	:	INT
			{ write_int (pstate, (LONGEST) $1.val, $1.type); }
	;

primary	:	CHARLIT
                  { write_int (pstate,
			       convert_char_literal (type_qualifier, $1.val),
			       (type_qualifier == NULL) 
			       ? $1.type : type_qualifier);
		  }
	;

primary	:	FLOAT
			{ write_exp_elt_opcode (pstate, OP_FLOAT);
			  write_exp_elt_type (pstate, $1.type);
			  write_exp_elt_floatcst (pstate, $1.val);
			  write_exp_elt_opcode (pstate, OP_FLOAT);
			}
	;

primary	:	NULL_PTR
			{ write_int (pstate, 0, type_int (pstate)); }
	;

primary	:	STRING
			{ 
			  write_exp_op_with_string (pstate, OP_STRING, $1);
			}
	;

primary :	TRUEKEYWORD
			{ write_int (pstate, 1, type_boolean (pstate)); }
        |	FALSEKEYWORD
			{ write_int (pstate, 0, type_boolean (pstate)); }
	;

primary	: 	NEW NAME
			{ error (_("NEW not implemented.")); }
	;

var_or_type:	NAME   	    %prec VAR
				{ $$ = write_var_or_type (pstate, NULL, $1); }
	|	block NAME  %prec VAR
                                { $$ = write_var_or_type (pstate, $1, $2); }
	|       NAME TICK_ACCESS 
			{ 
			  $$ = write_var_or_type (pstate, NULL, $1);
			  if ($$ == NULL)
			    write_exp_elt_opcode (pstate, UNOP_ADDR);
			  else
			    $$ = lookup_pointer_type ($$);
			}
	|	block NAME TICK_ACCESS
			{ 
			  $$ = write_var_or_type (pstate, $1, $2);
			  if ($$ == NULL)
			    write_exp_elt_opcode (pstate, UNOP_ADDR);
			  else
			    $$ = lookup_pointer_type ($$);
			}
	;

/* GDB extension */
block   :       NAME COLONCOLON
			{ $$ = block_lookup (NULL, $1.ptr); }
	|	block NAME COLONCOLON
			{ $$ = block_lookup ($1, $2.ptr); }
	;

aggregate :
		'(' aggregate_component_list ')'  
			{
			  write_exp_elt_opcode (pstate, OP_AGGREGATE);
			  write_exp_elt_longcst (pstate, $2);
			  write_exp_elt_opcode (pstate, OP_AGGREGATE);
		        }
	;

aggregate_component_list :
		component_groups	 { $$ = $1; }
	|	positional_list exp
			{ write_exp_elt_opcode (pstate, OP_POSITIONAL);
			  write_exp_elt_longcst (pstate, $1);
			  write_exp_elt_opcode (pstate, OP_POSITIONAL);
			  $$ = $1 + 1;
			}
	|	positional_list component_groups
					 { $$ = $1 + $2; }
	;

positional_list :
		exp ','
			{ write_exp_elt_opcode (pstate, OP_POSITIONAL);
			  write_exp_elt_longcst (pstate, 0);
			  write_exp_elt_opcode (pstate, OP_POSITIONAL);
			  $$ = 1;
			} 
	|	positional_list exp ','
			{ write_exp_elt_opcode (pstate, OP_POSITIONAL);
			  write_exp_elt_longcst (pstate, $1);
			  write_exp_elt_opcode (pstate, OP_POSITIONAL);
			  $$ = $1 + 1; 
			}
	;

component_groups:
		others			 { $$ = 1; }
	|	component_group		 { $$ = 1; }
	|	component_group ',' component_groups
					 { $$ = $3 + 1; }
	;

others 	:	OTHERS ARROW exp
			{ write_exp_elt_opcode (pstate, OP_OTHERS); }
	;

component_group :
		component_associations
			{
			  write_exp_elt_opcode (pstate, OP_CHOICES);
			  write_exp_elt_longcst (pstate, $1);
			  write_exp_elt_opcode (pstate, OP_CHOICES);
		        }
	;

/* We use this somewhat obscure definition in order to handle NAME => and
   NAME | differently from exp => and exp |.  ARROW and '|' have a precedence
   above that of the reduction of NAME to var_or_type.  By delaying 
   decisions until after the => or '|', we convert the ambiguity to a 
   resolved shift/reduce conflict. */
component_associations :
		NAME ARROW 
			{ write_name_assoc (pstate, $1); }
		    exp	{ $$ = 1; }
	|	simple_exp ARROW exp
			{ $$ = 1; }
	|	simple_exp DOTDOT simple_exp ARROW 
			{ write_exp_elt_opcode (pstate, OP_DISCRETE_RANGE);
			  write_exp_op_with_string (pstate, OP_NAME,
						    empty_stoken);
			}
		    exp { $$ = 1; }
	|	NAME '|' 
		        { write_name_assoc (pstate, $1); }
		    component_associations  { $$ = $4 + 1; }
	|	simple_exp '|'  
	            component_associations  { $$ = $3 + 1; }
	|	simple_exp DOTDOT simple_exp '|'
			{ write_exp_elt_opcode (pstate, OP_DISCRETE_RANGE); }
		    component_associations  { $$ = $6 + 1; }
	;

/* Some extensions borrowed from C, for the benefit of those who find they
   can't get used to Ada notation in GDB.  */

primary	:	'*' primary		%prec '.'
			{ write_exp_elt_opcode (pstate, UNOP_IND); }
	|	'&' primary		%prec '.'
			{ write_exp_elt_opcode (pstate, UNOP_ADDR); }
	|	primary '[' exp ']'
			{ write_exp_elt_opcode (pstate, BINOP_SUBSCRIPT); }
	;

%%

/* yylex defined in ada-lex.c: Reads one token, getting characters */
/* through lexptr.  */

/* Remap normal flex interface names (yylex) as well as gratuitiously */
/* global symbol names, so we can have multiple flex-generated parsers */
/* in gdb.  */

/* (See note above on previous definitions for YACC.) */

#define yy_create_buffer ada_yy_create_buffer
#define yy_delete_buffer ada_yy_delete_buffer
#define yy_init_buffer ada_yy_init_buffer
#define yy_load_buffer_state ada_yy_load_buffer_state
#define yy_switch_to_buffer ada_yy_switch_to_buffer
#define yyrestart ada_yyrestart
#define yytext ada_yytext

static struct obstack temp_parse_space;

/* The following kludge was found necessary to prevent conflicts between */
/* defs.h and non-standard stdlib.h files.  */
#define qsort __qsort__dummy
#include "ada-lex.c"

int
ada_parse (struct parser_state *par_state)
{
  /* Setting up the parser state.  */
  scoped_restore pstate_restore = make_scoped_restore (&pstate);
  gdb_assert (par_state != NULL);
  pstate = par_state;

  lexer_init (yyin);		/* (Re-)initialize lexer.  */
  type_qualifier = NULL;
  obstack_free (&temp_parse_space, NULL);
  obstack_init (&temp_parse_space);

  return yyparse ();
}

static void
yyerror (const char *msg)
{
  error (_("Error in expression, near `%s'."), pstate->lexptr);
}

/* Emit expression to access an instance of SYM, in block BLOCK (if
   non-NULL).  */

static void
write_var_from_sym (struct parser_state *par_state,
		    const struct block *block,
		    struct symbol *sym)
{
  if (symbol_read_needs_frame (sym))
    par_state->block_tracker->update (block, INNERMOST_BLOCK_FOR_SYMBOLS);

  write_exp_elt_opcode (par_state, OP_VAR_VALUE);
  write_exp_elt_block (par_state, block);
  write_exp_elt_sym (par_state, sym);
  write_exp_elt_opcode (par_state, OP_VAR_VALUE);
}

/* Write integer or boolean constant ARG of type TYPE.  */

static void
write_int (struct parser_state *par_state, LONGEST arg, struct type *type)
{
  write_exp_elt_opcode (par_state, OP_LONG);
  write_exp_elt_type (par_state, type);
  write_exp_elt_longcst (par_state, arg);
  write_exp_elt_opcode (par_state, OP_LONG);
}

/* Write an OPCODE, string, OPCODE sequence to the current expression.  */
static void
write_exp_op_with_string (struct parser_state *par_state,
			  enum exp_opcode opcode, struct stoken token)
{
  write_exp_elt_opcode (par_state, opcode);
  write_exp_string (par_state, token);
  write_exp_elt_opcode (par_state, opcode);
}
  
/* Emit expression corresponding to the renamed object named 
 * designated by RENAMED_ENTITY[0 .. RENAMED_ENTITY_LEN-1] in the
 * context of ORIG_LEFT_CONTEXT, to which is applied the operations
 * encoded by RENAMING_EXPR.  MAX_DEPTH is the maximum number of
 * cascaded renamings to allow.  If ORIG_LEFT_CONTEXT is null, it
 * defaults to the currently selected block. ORIG_SYMBOL is the 
 * symbol that originally encoded the renaming.  It is needed only
 * because its prefix also qualifies any index variables used to index
 * or slice an array.  It should not be necessary once we go to the
 * new encoding entirely (FIXME pnh 7/20/2007).  */

static void
write_object_renaming (struct parser_state *par_state,
		       const struct block *orig_left_context,
		       const char *renamed_entity, int renamed_entity_len,
		       const char *renaming_expr, int max_depth)
{
  char *name;
  enum { SIMPLE_INDEX, LOWER_BOUND, UPPER_BOUND } slice_state;
  struct block_symbol sym_info;

  if (max_depth <= 0)
    error (_("Could not find renamed symbol"));

  if (orig_left_context == NULL)
    orig_left_context = get_selected_block (NULL);

  name = obstack_strndup (&temp_parse_space, renamed_entity,
			  renamed_entity_len);
  ada_lookup_encoded_symbol (name, orig_left_context, VAR_DOMAIN, &sym_info);
  if (sym_info.symbol == NULL)
    error (_("Could not find renamed variable: %s"), ada_decode (name).c_str ());
  else if (SYMBOL_CLASS (sym_info.symbol) == LOC_TYPEDEF)
    /* We have a renaming of an old-style renaming symbol.  Don't
       trust the block information.  */
    sym_info.block = orig_left_context;

  {
    const char *inner_renamed_entity;
    int inner_renamed_entity_len;
    const char *inner_renaming_expr;

    switch (ada_parse_renaming (sym_info.symbol, &inner_renamed_entity,
				&inner_renamed_entity_len,
				&inner_renaming_expr))
      {
      case ADA_NOT_RENAMING:
	write_var_from_sym (par_state, sym_info.block, sym_info.symbol);
	break;
      case ADA_OBJECT_RENAMING:
	write_object_renaming (par_state, sym_info.block,
			       inner_renamed_entity, inner_renamed_entity_len,
			       inner_renaming_expr, max_depth - 1);
	break;
      default:
	goto BadEncoding;
      }
  }

  slice_state = SIMPLE_INDEX;
  while (*renaming_expr == 'X')
    {
      renaming_expr += 1;

      switch (*renaming_expr) {
      case 'A':
        renaming_expr += 1;
        write_exp_elt_opcode (par_state, UNOP_IND);
        break;
      case 'L':
	slice_state = LOWER_BOUND;
	/* FALLTHROUGH */
      case 'S':
	renaming_expr += 1;
	if (isdigit (*renaming_expr))
	  {
	    char *next;
	    long val = strtol (renaming_expr, &next, 10);
	    if (next == renaming_expr)
	      goto BadEncoding;
	    renaming_expr = next;
	    write_exp_elt_opcode (par_state, OP_LONG);
	    write_exp_elt_type (par_state, type_int (par_state));
	    write_exp_elt_longcst (par_state, (LONGEST) val);
	    write_exp_elt_opcode (par_state, OP_LONG);
	  }
	else
	  {
	    const char *end;
	    char *index_name;
	    struct block_symbol index_sym_info;

	    end = strchr (renaming_expr, 'X');
	    if (end == NULL)
	      end = renaming_expr + strlen (renaming_expr);

	    index_name = obstack_strndup (&temp_parse_space, renaming_expr,
					  end - renaming_expr);
	    renaming_expr = end;

	    ada_lookup_encoded_symbol (index_name, orig_left_context,
				       VAR_DOMAIN, &index_sym_info);
	    if (index_sym_info.symbol == NULL)
	      error (_("Could not find %s"), index_name);
	    else if (SYMBOL_CLASS (index_sym_info.symbol) == LOC_TYPEDEF)
	      /* Index is an old-style renaming symbol.  */
	      index_sym_info.block = orig_left_context;
	    write_var_from_sym (par_state, index_sym_info.block,
				index_sym_info.symbol);
	  }
	if (slice_state == SIMPLE_INDEX)
	  {
	    write_exp_elt_opcode (par_state, OP_FUNCALL);
	    write_exp_elt_longcst (par_state, (LONGEST) 1);
	    write_exp_elt_opcode (par_state, OP_FUNCALL);
	  }
	else if (slice_state == LOWER_BOUND)
	  slice_state = UPPER_BOUND;
	else if (slice_state == UPPER_BOUND)
	  {
	    write_exp_elt_opcode (par_state, TERNOP_SLICE);
	    slice_state = SIMPLE_INDEX;
	  }
	break;

      case 'R':
	{
	  struct stoken field_name;
	  const char *end;
	  char *buf;

	  renaming_expr += 1;

	  if (slice_state != SIMPLE_INDEX)
	    goto BadEncoding;
	  end = strchr (renaming_expr, 'X');
	  if (end == NULL)
	    end = renaming_expr + strlen (renaming_expr);
	  field_name.length = end - renaming_expr;
	  buf = (char *) malloc (end - renaming_expr + 1);
	  field_name.ptr = buf;
	  strncpy (buf, renaming_expr, end - renaming_expr);
	  buf[end - renaming_expr] = '\000';
	  renaming_expr = end;
	  write_exp_op_with_string (par_state, STRUCTOP_STRUCT, field_name);
	  break;
	}

      default:
	goto BadEncoding;
      }
    }
  if (slice_state == SIMPLE_INDEX)
    return;

 BadEncoding:
  error (_("Internal error in encoding of renaming declaration"));
}

static const struct block*
block_lookup (const struct block *context, const char *raw_name)
{
  const char *name;
  std::vector<struct block_symbol> syms;
  int nsyms;
  struct symtab *symtab;
  const struct block *result = NULL;

  if (raw_name[0] == '\'')
    {
      raw_name += 1;
      name = raw_name;
    }
  else
    name = ada_encode (raw_name);

  nsyms = ada_lookup_symbol_list (name, context, VAR_DOMAIN, &syms);

  if (context == NULL
      && (nsyms == 0 || SYMBOL_CLASS (syms[0].symbol) != LOC_BLOCK))
    symtab = lookup_symtab (name);
  else
    symtab = NULL;

  if (symtab != NULL)
    result = BLOCKVECTOR_BLOCK (SYMTAB_BLOCKVECTOR (symtab), STATIC_BLOCK);
  else if (nsyms == 0 || SYMBOL_CLASS (syms[0].symbol) != LOC_BLOCK)
    {
      if (context == NULL)
	error (_("No file or function \"%s\"."), raw_name);
      else
	error (_("No function \"%s\" in specified context."), raw_name);
    }
  else
    {
      if (nsyms > 1)
	warning (_("Function name \"%s\" ambiguous here"), raw_name);
      result = SYMBOL_BLOCK_VALUE (syms[0].symbol);
    }

  return result;
}

static struct symbol*
select_possible_type_sym (const std::vector<struct block_symbol> &syms)
{
  int i;
  int preferred_index;
  struct type *preferred_type;
	  
  preferred_index = -1; preferred_type = NULL;
  for (i = 0; i < syms.size (); i += 1)
    switch (SYMBOL_CLASS (syms[i].symbol))
      {
      case LOC_TYPEDEF:
	if (ada_prefer_type (SYMBOL_TYPE (syms[i].symbol), preferred_type))
	  {
	    preferred_index = i;
	    preferred_type = SYMBOL_TYPE (syms[i].symbol);
	  }
	break;
      case LOC_REGISTER:
      case LOC_ARG:
      case LOC_REF_ARG:
      case LOC_REGPARM_ADDR:
      case LOC_LOCAL:
      case LOC_COMPUTED:
	return NULL;
      default:
	break;
      }
  if (preferred_type == NULL)
    return NULL;
  return syms[preferred_index].symbol;
}

static struct type*
find_primitive_type (struct parser_state *par_state, char *name)
{
  struct type *type;
  type = language_lookup_primitive_type (par_state->language (),
					 par_state->gdbarch (),
					 name);
  if (type == NULL && strcmp ("system__address", name) == 0)
    type = type_system_address (par_state);

  if (type != NULL)
    {
      /* Check to see if we have a regular definition of this
	 type that just didn't happen to have been read yet.  */
      struct symbol *sym;
      char *expanded_name = 
	(char *) alloca (strlen (name) + sizeof ("standard__"));
      strcpy (expanded_name, "standard__");
      strcat (expanded_name, name);
      sym = ada_lookup_symbol (expanded_name, NULL, VAR_DOMAIN).symbol;
      if (sym != NULL && SYMBOL_CLASS (sym) == LOC_TYPEDEF)
	type = SYMBOL_TYPE (sym);
    }

  return type;
}

static int
chop_selector (char *name, int end)
{
  int i;
  for (i = end - 1; i > 0; i -= 1)
    if (name[i] == '.' || (name[i] == '_' && name[i+1] == '_'))
      return i;
  return -1;
}

/* If NAME is a string beginning with a separator (either '__', or
   '.'), chop this separator and return the result; else, return
   NAME.  */

static char *
chop_separator (char *name)
{
  if (*name == '.')
   return name + 1;

  if (name[0] == '_' && name[1] == '_')
    return name + 2;

  return name;
}

/* Given that SELS is a string of the form (<sep><identifier>)*, where
   <sep> is '__' or '.', write the indicated sequence of
   STRUCTOP_STRUCT expression operators. */
static void
write_selectors (struct parser_state *par_state, char *sels)
{
  while (*sels != '\0')
    {
      struct stoken field_name;
      char *p = chop_separator (sels);
      sels = p;
      while (*sels != '\0' && *sels != '.' 
	     && (sels[0] != '_' || sels[1] != '_'))
	sels += 1;
      field_name.length = sels - p;
      field_name.ptr = p;
      write_exp_op_with_string (par_state, STRUCTOP_STRUCT, field_name);
    }
}

/* Write a variable access (OP_VAR_VALUE) to ambiguous encoded name
   NAME[0..LEN-1], in block context BLOCK, to be resolved later.  Writes
   a temporary symbol that is valid until the next call to ada_parse.
   */
static void
write_ambiguous_var (struct parser_state *par_state,
		     const struct block *block, char *name, int len)
{
  struct symbol *sym = new (&temp_parse_space) symbol ();

  SYMBOL_DOMAIN (sym) = UNDEF_DOMAIN;
  SYMBOL_SET_LINKAGE_NAME (sym,
			   obstack_strndup (&temp_parse_space, name, len));
  SYMBOL_LANGUAGE (sym) = language_ada;

  write_exp_elt_opcode (par_state, OP_VAR_VALUE);
  write_exp_elt_block (par_state, block);
  write_exp_elt_sym (par_state, sym);
  write_exp_elt_opcode (par_state, OP_VAR_VALUE);
}

/* A convenient wrapper around ada_get_field_index that takes
   a non NUL-terminated FIELD_NAME0 and a FIELD_NAME_LEN instead
   of a NUL-terminated field name.  */

static int
ada_nget_field_index (const struct type *type, const char *field_name0,
                      int field_name_len, int maybe_missing)
{
  char *field_name = (char *) alloca ((field_name_len + 1) * sizeof (char));

  strncpy (field_name, field_name0, field_name_len);
  field_name[field_name_len] = '\0';
  return ada_get_field_index (type, field_name, maybe_missing);
}

/* If encoded_field_name is the name of a field inside symbol SYM,
   then return the type of that field.  Otherwise, return NULL.

   This function is actually recursive, so if ENCODED_FIELD_NAME
   doesn't match one of the fields of our symbol, then try to see
   if ENCODED_FIELD_NAME could not be a succession of field names
   (in other words, the user entered an expression of the form
   TYPE_NAME.FIELD1.FIELD2.FIELD3), in which case we evaluate
   each field name sequentially to obtain the desired field type.
   In case of failure, we return NULL.  */

static struct type *
get_symbol_field_type (struct symbol *sym, char *encoded_field_name)
{
  char *field_name = encoded_field_name;
  char *subfield_name;
  struct type *type = SYMBOL_TYPE (sym);
  int fieldno;

  if (type == NULL || field_name == NULL)
    return NULL;
  type = check_typedef (type);

  while (field_name[0] != '\0')
    {
      field_name = chop_separator (field_name);

      fieldno = ada_get_field_index (type, field_name, 1);
      if (fieldno >= 0)
        return TYPE_FIELD_TYPE (type, fieldno);

      subfield_name = field_name;
      while (*subfield_name != '\0' && *subfield_name != '.' 
	     && (subfield_name[0] != '_' || subfield_name[1] != '_'))
	subfield_name += 1;

      if (subfield_name[0] == '\0')
        return NULL;

      fieldno = ada_nget_field_index (type, field_name,
                                      subfield_name - field_name, 1);
      if (fieldno < 0)
        return NULL;

      type = TYPE_FIELD_TYPE (type, fieldno);
      field_name = subfield_name;
    }

  return NULL;
}

/* Look up NAME0 (an unencoded identifier or dotted name) in BLOCK (or 
   expression_block_context if NULL).  If it denotes a type, return
   that type.  Otherwise, write expression code to evaluate it as an
   object and return NULL. In this second case, NAME0 will, in general,
   have the form <name>(.<selector_name>)*, where <name> is an object
   or renaming encoded in the debugging data.  Calls error if no
   prefix <name> matches a name in the debugging data (i.e., matches
   either a complete name or, as a wild-card match, the final 
   identifier).  */

static struct type*
write_var_or_type (struct parser_state *par_state,
		   const struct block *block, struct stoken name0)
{
  int depth;
  char *encoded_name;
  int name_len;

  if (block == NULL)
    block = par_state->expression_context_block;

  encoded_name = ada_encode (name0.ptr);
  name_len = strlen (encoded_name);
  encoded_name = obstack_strndup (&temp_parse_space, encoded_name, name_len);
  for (depth = 0; depth < MAX_RENAMING_CHAIN_LENGTH; depth += 1)
    {
      int tail_index;
      
      tail_index = name_len;
      while (tail_index > 0)
	{
	  int nsyms;
	  std::vector<struct block_symbol> syms;
	  struct symbol *type_sym;
	  struct symbol *renaming_sym;
	  const char* renaming;
	  int renaming_len;
	  const char* renaming_expr;
	  int terminator = encoded_name[tail_index];

	  encoded_name[tail_index] = '\0';
	  nsyms = ada_lookup_symbol_list (encoded_name, block,
					  VAR_DOMAIN, &syms);
	  encoded_name[tail_index] = terminator;

	  type_sym = select_possible_type_sym (syms);

	  if (type_sym != NULL)
	    renaming_sym = type_sym;
	  else if (nsyms == 1)
	    renaming_sym = syms[0].symbol;
	  else 
	    renaming_sym = NULL;

	  switch (ada_parse_renaming (renaming_sym, &renaming,
				      &renaming_len, &renaming_expr))
	    {
	    case ADA_NOT_RENAMING:
	      break;
	    case ADA_PACKAGE_RENAMING:
	    case ADA_EXCEPTION_RENAMING:
	    case ADA_SUBPROGRAM_RENAMING:
	      {
	        int alloc_len = renaming_len + name_len - tail_index + 1;
		char *new_name
		  = (char *) obstack_alloc (&temp_parse_space, alloc_len);
		strncpy (new_name, renaming, renaming_len);
		strcpy (new_name + renaming_len, encoded_name + tail_index);
		encoded_name = new_name;
		name_len = renaming_len + name_len - tail_index;
		goto TryAfterRenaming;
	      }	
	    case ADA_OBJECT_RENAMING:
	      write_object_renaming (par_state, block, renaming, renaming_len,
				     renaming_expr, MAX_RENAMING_CHAIN_LENGTH);
	      write_selectors (par_state, encoded_name + tail_index);
	      return NULL;
	    default:
	      internal_error (__FILE__, __LINE__,
			      _("impossible value from ada_parse_renaming"));
	    }

	  if (type_sym != NULL)
	    {
              struct type *field_type;
              
              if (tail_index == name_len)
		return SYMBOL_TYPE (type_sym);

              /* We have some extraneous characters after the type name.
                 If this is an expression "TYPE_NAME.FIELD0.[...].FIELDN",
                 then try to get the type of FIELDN.  */
              field_type
                = get_symbol_field_type (type_sym, encoded_name + tail_index);
              if (field_type != NULL)
		return field_type;
	      else 
		error (_("Invalid attempt to select from type: \"%s\"."),
                       name0.ptr);
	    }
	  else if (tail_index == name_len && nsyms == 0)
	    {
	      struct type *type = find_primitive_type (par_state,
						       encoded_name);

	      if (type != NULL)
		return type;
	    }

	  if (nsyms == 1)
	    {
	      write_var_from_sym (par_state, syms[0].block, syms[0].symbol);
	      write_selectors (par_state, encoded_name + tail_index);
	      return NULL;
	    }
	  else if (nsyms == 0) 
	    {
	      struct bound_minimal_symbol msym
		= ada_lookup_simple_minsym (encoded_name);
	      if (msym.minsym != NULL)
		{
		  write_exp_msymbol (par_state, msym);
		  /* Maybe cause error here rather than later? FIXME? */
		  write_selectors (par_state, encoded_name + tail_index);
		  return NULL;
		}

	      if (tail_index == name_len
		  && strncmp (encoded_name, "standard__", 
			      sizeof ("standard__") - 1) == 0)
		error (_("No definition of \"%s\" found."), name0.ptr);

	      tail_index = chop_selector (encoded_name, tail_index);
	    } 
	  else
	    {
	      write_ambiguous_var (par_state, block, encoded_name,
				   tail_index);
	      write_selectors (par_state, encoded_name + tail_index);
	      return NULL;
	    }
	}

      if (!have_full_symbols () && !have_partial_symbols () && block == NULL)
	error (_("No symbol table is loaded.  Use the \"file\" command."));
      if (block == par_state->expression_context_block)
	error (_("No definition of \"%s\" in current context."), name0.ptr);
      else
	error (_("No definition of \"%s\" in specified context."), name0.ptr);
      
    TryAfterRenaming: ;
    }

  error (_("Could not find renamed symbol \"%s\""), name0.ptr);

}

/* Write a left side of a component association (e.g., NAME in NAME =>
   exp).  If NAME has the form of a selected component, write it as an
   ordinary expression.  If it is a simple variable that unambiguously
   corresponds to exactly one symbol that does not denote a type or an
   object renaming, also write it normally as an OP_VAR_VALUE.
   Otherwise, write it as an OP_NAME.

   Unfortunately, we don't know at this point whether NAME is supposed
   to denote a record component name or the value of an array index.
   Therefore, it is not appropriate to disambiguate an ambiguous name
   as we normally would, nor to replace a renaming with its referent.
   As a result, in the (one hopes) rare case that one writes an
   aggregate such as (R => 42) where R renames an object or is an
   ambiguous name, one must write instead ((R) => 42). */
   
static void
write_name_assoc (struct parser_state *par_state, struct stoken name)
{
  if (strchr (name.ptr, '.') == NULL)
    {
      std::vector<struct block_symbol> syms;
      int nsyms = ada_lookup_symbol_list (name.ptr,
					  par_state->expression_context_block,
					  VAR_DOMAIN, &syms);

      if (nsyms != 1 || SYMBOL_CLASS (syms[0].symbol) == LOC_TYPEDEF)
	write_exp_op_with_string (par_state, OP_NAME, name);
      else
	write_var_from_sym (par_state, syms[0].block, syms[0].symbol);
    }
  else
    if (write_var_or_type (par_state, NULL, name) != NULL)
      error (_("Invalid use of type."));
}

/* Convert the character literal whose ASCII value would be VAL to the
   appropriate value of type TYPE, if there is a translation.
   Otherwise return VAL.  Hence, in an enumeration type ('A', 'B'),
   the literal 'A' (VAL == 65), returns 0.  */

static LONGEST
convert_char_literal (struct type *type, LONGEST val)
{
  char name[7];
  int f;

  if (type == NULL)
    return val;
  type = check_typedef (type);
  if (TYPE_CODE (type) != TYPE_CODE_ENUM)
    return val;

  if ((val >= 'a' && val <= 'z') || (val >= '0' && val <= '9'))
    xsnprintf (name, sizeof (name), "Q%c", (int) val);
  else
    xsnprintf (name, sizeof (name), "QU%02x", (int) val);
  size_t len = strlen (name);
  for (f = 0; f < TYPE_NFIELDS (type); f += 1)
    {
      /* Check the suffix because an enum constant in a package will
	 have a name like "pkg__QUxx".  This is safe enough because we
	 already have the correct type, and because mangling means
	 there can't be clashes.  */
      const char *ename = TYPE_FIELD_NAME (type, f);
      size_t elen = strlen (ename);

      if (elen >= len && strcmp (name, ename + elen - len) == 0)
	return TYPE_FIELD_ENUMVAL (type, f);
    }
  return val;
}

static struct type *
type_int (struct parser_state *par_state)
{
  return parse_type (par_state)->builtin_int;
}

static struct type *
type_long (struct parser_state *par_state)
{
  return parse_type (par_state)->builtin_long;
}

static struct type *
type_long_long (struct parser_state *par_state)
{
  return parse_type (par_state)->builtin_long_long;
}

static struct type *
type_long_double (struct parser_state *par_state)
{
  return parse_type (par_state)->builtin_long_double;
}

static struct type *
type_char (struct parser_state *par_state)
{
  return language_string_char_type (par_state->language (),
				    par_state->gdbarch ());
}

static struct type *
type_boolean (struct parser_state *par_state)
{
  return parse_type (par_state)->builtin_bool;
}

static struct type *
type_system_address (struct parser_state *par_state)
{
  struct type *type 
    = language_lookup_primitive_type (par_state->language (),
				      par_state->gdbarch (),
				      "system__address");
  return  type != NULL ? type : parse_type (par_state)->builtin_data_ptr;
}

void
_initialize_ada_exp (void)
{
  obstack_init (&temp_parse_space);
}
