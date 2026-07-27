/* A Bison parser, made by GNU Bison 3.7.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2020 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "3.7.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 1 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"

#include <stdio.h>
#include <stdlib.h>

#include "pager_filter.h"
#include "pager_infra.h"

// stuff from flex that bison needs to know about:
extern int yylex();
extern FILE *yyin;
extern int line_num;
extern int ch_num;

void yyerror(void **output, void *store, const char *s);

#line 87 "pager_filter.bison.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "pager_filter.bison.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_AND = 3,                        /* AND  */
  YYSYMBOL_OR = 4,                         /* OR  */
  YYSYMBOL_NOT = 5,                        /* NOT  */
  YYSYMBOL_HAS = 6,                        /* HAS  */
  YYSYMBOL_LIKE = 7,                       /* LIKE  */
  YYSYMBOL_IN = 8,                         /* IN  */
  YYSYMBOL_RARROW = 9,                     /* RARROW  */
  YYSYMBOL_PROP_CPU = 10,                  /* PROP_CPU  */
  YYSYMBOL_PROP_TRACE = 11,                /* PROP_TRACE  */
  YYSYMBOL_PROP_FUNC = 12,                 /* PROP_FUNC  */
  YYSYMBOL_PROP_FILE = 13,                 /* PROP_FILE  */
  YYSYMBOL_PROP_FMT = 14,                  /* PROP_FMT  */
  YYSYMBOL_PROP_SEV = 15,                  /* PROP_SEV  */
  YYSYMBOL_PROP_STICKY = 16,               /* PROP_STICKY  */
  YYSYMBOL_PROP_STICKY_END = 17,           /* PROP_STICKY_END  */
  YYSYMBOL_INT = 18,                       /* INT  */
  YYSYMBOL_STRING = 19,                    /* STRING  */
  YYSYMBOL_TOKENID = 20,                   /* TOKENID  */
  YYSYMBOL_21_ = 21,                       /* ','  */
  YYSYMBOL_22_ = 22,                       /* '['  */
  YYSYMBOL_23_ = 23,                       /* ']'  */
  YYSYMBOL_24_ = 24,                       /* '='  */
  YYSYMBOL_25_ = 25,                       /* '<'  */
  YYSYMBOL_26_ = 26,                       /* '>'  */
  YYSYMBOL_27_ = 27,                       /* '('  */
  YYSYMBOL_28_ = 28,                       /* ')'  */
  YYSYMBOL_29_ = 29,                       /* '{'  */
  YYSYMBOL_30_ = 30,                       /* '}'  */
  YYSYMBOL_31_ = 31,                       /* '+'  */
  YYSYMBOL_YYACCEPT = 32,                  /* $accept  */
  YYSYMBOL_filter = 33,                    /* filter  */
  YYSYMBOL_sticky_list = 34,               /* sticky_list  */
  YYSYMBOL_sticky_until = 35,              /* sticky_until  */
  YYSYMBOL_sticky = 36,                    /* sticky  */
  YYSYMBOL_tokens_list = 37,               /* tokens_list  */
  YYSYMBOL_exp = 38,                       /* exp  */
  YYSYMBOL_composite = 39,                 /* composite  */
  YYSYMBOL_composite_kwargs = 40,          /* composite_kwargs  */
  YYSYMBOL_tokenid = 41,                   /* tokenid  */
  YYSYMBOL_literal = 42,                   /* literal  */
  YYSYMBOL_range = 43                      /* range  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && ! defined __ICC && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                            \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  26
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   80

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  32
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  12
/* YYNRULES -- Number of rules.  */
#define YYNRULES  35
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  84

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   275


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      27,    28,     2,    31,    21,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      25,    24,    26,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,    22,     2,    23,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    29,     2,    30,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20
};

#if YYDEBUG
  /* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint8 yyrline[] =
{
       0,    62,    62,    65,    71,    74,    79,    81,    87,    93,
      96,   102,   105,   108,   114,   120,   126,   132,   138,   144,
     150,   156,   162,   168,   174,   180,   186,   192,   198,   204,
     207,   213,   222,   228,   237,   240
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "AND", "OR", "NOT",
  "HAS", "LIKE", "IN", "RARROW", "PROP_CPU", "PROP_TRACE", "PROP_FUNC",
  "PROP_FILE", "PROP_FMT", "PROP_SEV", "PROP_STICKY", "PROP_STICKY_END",
  "INT", "STRING", "TOKENID", "','", "'['", "']'", "'='", "'<'", "'>'",
  "'('", "')'", "'{'", "'}'", "'+'", "$accept", "filter", "sticky_list",
  "sticky_until", "sticky", "tokens_list", "exp", "composite",
  "composite_kwargs", "tokenid", "literal", "range", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#ifdef YYPRINT
/* YYTOKNUM[NUM] -- (External) token number corresponding to the
   (internal) symbol number NUM (which must be that of a token).  */
static const yytype_int16 yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,    44,    91,    93,    61,    60,    62,    40,    41,   123,
     125,    43
};
#endif

#define YYPACT_NINF (-33)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int8 yypact[] =
{
       5,     5,    11,    12,    16,    17,    18,     6,    36,     4,
       5,    43,    31,   -33,    -3,    35,   -33,    27,    29,    32,
      33,    34,    24,    28,    37,     2,   -33,     5,     5,    38,
      39,    40,   -11,   -11,   -11,   -11,   -33,   -33,   -33,   -33,
     -33,    41,    42,   -33,    30,     7,   -33,    35,    35,    46,
       5,   -33,    47,   -33,   -33,   -33,   -33,   -33,   -33,    44,
      -7,   -11,    37,   -33,    45,    35,   -33,    48,    50,    51,
     -33,    49,    52,    53,    54,    55,    58,    57,   -33,   -33,
     -33,    52,   -33,   -33
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       0,     0,     0,     0,     0,     0,     0,     0,     0,    31,
       0,     0,     2,    11,     0,    26,    23,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     1,     0,     0,     0,
       6,     4,     0,     0,     0,     0,    21,    17,    18,    19,
      20,     0,     0,    12,     0,     0,    27,    24,    25,     0,
       0,     3,     0,    32,    33,    16,    13,    14,    15,     0,
       0,     0,     0,    28,     0,     7,     5,     0,     0,     0,
      29,    30,     0,     0,     0,     0,     9,     0,    22,    34,
      35,     0,     8,    10
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
     -33,   -33,    10,   -33,   -33,   -17,    -1,   -33,     9,   -33,
     -32,   -33
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int8 yydefgoto[] =
{
      -1,    11,    30,    51,    31,    77,    12,    13,    45,    14,
      55,    43
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int8 yytable[] =
{
      15,    56,    57,    58,    32,    27,    28,    53,    54,    25,
       1,     2,    23,    21,    68,     3,     4,     5,     6,     7,
       8,    33,    34,    35,    69,     9,    47,    48,    62,    70,
      46,    16,    10,    24,    27,    28,    17,    63,    27,    28,
      18,    19,    20,    26,    22,    36,    41,    29,    37,    65,
      42,    38,    39,    40,    61,    64,    50,    44,    49,    59,
      60,    52,    66,    29,    83,    67,    73,    72,    74,    75,
      62,    71,    76,     0,     0,     0,    78,    79,    80,    81,
      82
};

static const yytype_int8 yycheck[] =
{
       1,    33,    34,    35,     7,     3,     4,    18,    19,    10,
       5,     6,     8,     7,    21,    10,    11,    12,    13,    14,
      15,    24,    25,    26,    31,    20,    27,    28,    21,    61,
      28,    20,    27,    29,     3,     4,    24,    30,     3,     4,
      24,    24,    24,     0,     8,    18,    22,    16,    19,    50,
      22,    19,    19,    19,    24,     9,    17,    20,    20,    18,
      18,    21,    52,    16,    81,    21,    18,    22,    18,    18,
      21,    62,    20,    -1,    -1,    -1,    23,    23,    23,    21,
      23
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,     5,     6,    10,    11,    12,    13,    14,    15,    20,
      27,    33,    38,    39,    41,    38,    20,    24,    24,    24,
      24,     7,     8,     8,    29,    38,     0,     3,     4,    16,
      34,    36,     7,    24,    25,    26,    18,    19,    19,    19,
      19,    22,    22,    43,    20,    40,    28,    38,    38,    20,
      17,    35,    21,    18,    19,    42,    42,    42,    42,    18,
      18,    24,    21,    30,     9,    38,    34,    21,    21,    31,
      42,    40,    22,    18,    18,    18,    20,    37,    23,    23,
      23,    21,    23,    37
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_int8 yyr1[] =
{
       0,    32,    33,    33,    34,    34,    35,    35,    36,    37,
      37,    38,    38,    38,    38,    38,    38,    38,    38,    38,
      38,    38,    38,    38,    38,    38,    38,    38,    39,    40,
      40,    41,    42,    42,    43,    43
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     1,     3,     1,     3,     0,     2,     6,     1,
       3,     1,     3,     3,     3,     3,     3,     3,     3,     3,
       3,     3,     7,     2,     3,     3,     2,     3,     4,     3,
       3,     1,     1,     1,     5,     5
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (output, store, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)

/* This macro is provided for backward compatibility. */
# ifndef YY_LOCATION_PRINT
#  define YY_LOCATION_PRINT(File, Loc) ((void) 0)
# endif


# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value, output, store); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, void **output, void *store)
{
  FILE *yyoutput = yyo;
  YYUSE (yyoutput);
  YYUSE (output);
  YYUSE (store);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yykind < YYNTOKENS)
    YYPRINT (yyo, yytoknum[yykind], *yyvaluep);
# endif
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, void **output, void *store)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep, output, store);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule, void **output, void *store)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)], output, store);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule, output, store); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep, void **output, void *store)
{
  YYUSE (yyvaluep);
  YYUSE (output);
  YYUSE (store);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;




/*----------.
| yyparse.  |
`----------*/

int
yyparse (void **output, void *store)
{
    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */
  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    goto yyexhaustedlab;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        goto yyexhaustedlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          goto yyexhaustedlab;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */

  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* filter: exp  */
#line 62 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
      {
    *output = create_filter((yyvsp[0].node), NULL, NULL);
}
#line 1165 "pager_filter.bison.c"
    break;

  case 3: /* filter: exp sticky_list sticky_until  */
#line 65 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                               {
    *output = create_filter((yyvsp[-2].node), (yyvsp[-1].node), (yyvsp[0].node));
}
#line 1173 "pager_filter.bison.c"
    break;

  case 4: /* sticky_list: sticky  */
#line 71 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
         {
    (yyval.node) = extend_sticky(store, NULL, (yyvsp[0].node));
}
#line 1181 "pager_filter.bison.c"
    break;

  case 5: /* sticky_list: sticky ',' sticky_list  */
#line 74 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                         {
    (yyval.node) = extend_sticky(store, (yyvsp[0].node), (yyvsp[-2].node));
}
#line 1189 "pager_filter.bison.c"
    break;

  case 6: /* sticky_until: %empty  */
#line 79 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
              {
      (yyval.node) = NULL;
}
#line 1197 "pager_filter.bison.c"
    break;

  case 7: /* sticky_until: PROP_STICKY_END exp  */
#line 81 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                        {
      (yyval.node) = (yyvsp[0].node);
  }
#line 1205 "pager_filter.bison.c"
    break;

  case 8: /* sticky: PROP_STICKY TOKENID RARROW '[' tokens_list ']'  */
#line 87 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                                                 {
      (yyval.node) = extend_sticky_token(store, (yyvsp[-4].sval), (yyvsp[-1].node), NULL);
  }
#line 1213 "pager_filter.bison.c"
    break;

  case 9: /* tokens_list: TOKENID  */
#line 93 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
          {
    (yyval.node) = extend_sticky_token(store, NULL, NULL, (yyvsp[0].sval));
}
#line 1221 "pager_filter.bison.c"
    break;

  case 10: /* tokens_list: TOKENID ',' tokens_list  */
#line 96 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                          {
    (yyval.node) = extend_sticky_token(store, NULL, (yyvsp[0].node), (yyvsp[-2].sval));
}
#line 1229 "pager_filter.bison.c"
    break;

  case 11: /* exp: composite  */
#line 102 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
            {
    (yyval.node) = (yyvsp[0].node);
}
#line 1237 "pager_filter.bison.c"
    break;

  case 12: /* exp: TOKENID IN range  */
#line 105 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                   { // ex: @VLBA in [100 + 20]
    (yyval.node) = create_range_node(store, (yyvsp[-2].sval), (yyvsp[0].node));
}
#line 1245 "pager_filter.bison.c"
    break;

  case 13: /* exp: tokenid '=' literal  */
#line 108 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                      { // ex: @RAID_VERSION = 15
    (yyval.node) = create_ast_node(store, NODE_KIND_EQ_EXPR,
                         (filter_ast_node_val_t){.sval = "="},
                         (yyvsp[-2].node),
                         (yyvsp[0].node));
}
#line 1256 "pager_filter.bison.c"
    break;

  case 14: /* exp: tokenid '<' literal  */
#line 114 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                      { // ex: @RAID_VERSION < 15
    (yyval.node) = create_ast_node(store, NODE_KIND_EQ_EXPR,
                         (filter_ast_node_val_t){.sval = "<"},
                         (yyvsp[-2].node),
                         (yyvsp[0].node));
}
#line 1267 "pager_filter.bison.c"
    break;

  case 15: /* exp: tokenid '>' literal  */
#line 120 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                      { // ex: @RAID_VERSION > 15
    (yyval.node) = create_ast_node(store, NODE_KIND_EQ_EXPR,
                         (filter_ast_node_val_t){.sval = ">"},
                         (yyvsp[-2].node),
                         (yyvsp[0].node));
}
#line 1278 "pager_filter.bison.c"
    break;

  case 16: /* exp: tokenid LIKE literal  */
#line 126 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                       { // ex: @NAME ~ "volume.*"
    (yyval.node) = create_ast_node(store, NODE_KIND_EQ_EXPR,
                         (filter_ast_node_val_t){.sval = "~"},
                         (yyvsp[-2].node),
                         (yyvsp[0].node));
}
#line 1289 "pager_filter.bison.c"
    break;

  case 17: /* exp: PROP_TRACE '=' STRING  */
#line 132 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                        { // ex: trace = "__D_trace_1_main"
    (yyval.node) = create_ast_node(store, NODE_KIND_EQ_TRACEID,
                         (filter_ast_node_val_t){.sval = (yyvsp[0].sval)},
                         NULL,
                         NULL);
}
#line 1300 "pager_filter.bison.c"
    break;

  case 18: /* exp: PROP_FUNC '=' STRING  */
#line 138 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                       { // ex: trace = "__D_trace_1_main"
    (yyval.node) = create_ast_node(store, NODE_KIND_EQ_FUNC,
                         (filter_ast_node_val_t){.sval = (yyvsp[0].sval)},
                         NULL,
                         NULL);
}
#line 1311 "pager_filter.bison.c"
    break;

  case 19: /* exp: PROP_FILE '=' STRING  */
#line 144 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                       { // ex: file = "nvmeibc_jam.c"
    (yyval.node) = create_ast_node(store, NODE_KIND_EQ_FILE,
                         (filter_ast_node_val_t){.sval = (yyvsp[0].sval)},
                         NULL,
                         NULL);
}
#line 1322 "pager_filter.bison.c"
    break;

  case 20: /* exp: PROP_FMT LIKE STRING  */
#line 150 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                       { // ex: FMT ~ "Error occurred in*"
    (yyval.node) = create_ast_node(store, NODE_KIND_FMT_LIKE,
                         (filter_ast_node_val_t){.sval = (yyvsp[0].sval)},
                         NULL,
                         NULL);
}
#line 1333 "pager_filter.bison.c"
    break;

  case 21: /* exp: PROP_CPU '=' INT  */
#line 156 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                   { // ex: cpu = 3
    (yyval.node) = create_ast_node(store, NODE_KIND_EQ_CPU,
                         (filter_ast_node_val_t){.ival = (yyvsp[0].ival)},
                         NULL,
                         NULL);
}
#line 1344 "pager_filter.bison.c"
    break;

  case 22: /* exp: PROP_SEV IN '[' INT ',' INT ']'  */
#line 162 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                                  { // ex: sev in [0, 4]
    (yyval.node) = create_ast_node(store, NODE_KIND_IN_RANGE_SEV,
                         (filter_ast_node_val_t){.sval = "IN"},
                         (void*)(yyvsp[-3].ival),
                         (void*)(yyvsp[-1].ival));
}
#line 1355 "pager_filter.bison.c"
    break;

  case 23: /* exp: HAS TOKENID  */
#line 168 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
              { // ex: has @TOPOLOGY
    (yyval.node) = create_ast_node(store, NODE_KIND_HAS_OPERATOR,
                         (filter_ast_node_val_t){.sval = (yyvsp[0].sval)},
                         NULL,
                         NULL);
}
#line 1366 "pager_filter.bison.c"
    break;

  case 24: /* exp: exp AND exp  */
#line 174 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
              {
    (yyval.node) = create_ast_node(store, NODE_KIND_AND_OPERATOR,
                         (filter_ast_node_val_t){.sval = "AND"},
                         (yyvsp[-2].node),
                         (yyvsp[0].node));
}
#line 1377 "pager_filter.bison.c"
    break;

  case 25: /* exp: exp OR exp  */
#line 180 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
             {
    (yyval.node) = create_ast_node(store, NODE_KIND_OR_OPERATOR,
                         (filter_ast_node_val_t){.sval = "OR"},
                         (yyvsp[-2].node),
                         (yyvsp[0].node));
}
#line 1388 "pager_filter.bison.c"
    break;

  case 26: /* exp: NOT exp  */
#line 186 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
          {
    (yyval.node) = create_ast_node(store, NODE_KIND_NOT_OPERATOR,
                         (filter_ast_node_val_t){.sval = "NOT"},
                         (yyvsp[0].node),
                         NULL);
}
#line 1399 "pager_filter.bison.c"
    break;

  case 27: /* exp: '(' exp ')'  */
#line 192 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
              {
    (yyval.node) = (yyvsp[-1].node); // For order only
}
#line 1407 "pager_filter.bison.c"
    break;

  case 28: /* composite: TOKENID '{' composite_kwargs '}'  */
#line 198 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                                   {
    (yyval.node) = create_composite(store, (yyvsp[-3].sval), (yyvsp[-1].node));
}
#line 1415 "pager_filter.bison.c"
    break;

  case 29: /* composite_kwargs: TOKENID '=' literal  */
#line 204 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                      {
    (yyval.node) = create_composite_kwargs(store, (yyvsp[-2].sval), (yyvsp[0].node));
}
#line 1423 "pager_filter.bison.c"
    break;

  case 30: /* composite_kwargs: composite_kwargs ',' composite_kwargs  */
#line 207 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                                        {
    (yyval.node) = merge_composite_kwargs(store, (yyvsp[-2].node), (yyvsp[0].node));
}
#line 1431 "pager_filter.bison.c"
    break;

  case 31: /* tokenid: TOKENID  */
#line 213 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
          {
    (yyval.node) = create_ast_node(store, NODE_KIND_TOKENID_VAR,
                         (filter_ast_node_val_t){.sval = (yyvsp[0].sval)},
                         NULL,
                         NULL);
}
#line 1442 "pager_filter.bison.c"
    break;

  case 32: /* literal: INT  */
#line 222 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
      {
    (yyval.node) = create_ast_node(store, NODE_KIND_INT_LITERAL,
                         (filter_ast_node_val_t){.ival = (yyvsp[0].ival)},
                         NULL,
                         NULL);
}
#line 1453 "pager_filter.bison.c"
    break;

  case 33: /* literal: STRING  */
#line 228 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
         {
    (yyval.node) = create_ast_node(store, NODE_KIND_STRING_LITERAL,
                         (filter_ast_node_val_t){.sval = (yyvsp[0].sval)},
                         NULL,
                         NULL);
}
#line 1464 "pager_filter.bison.c"
    break;

  case 34: /* range: '[' INT ',' INT ']'  */
#line 237 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                      {
    (yyval.node) = create_range((yyvsp[-3].ival), (yyvsp[-1].ival));
}
#line 1472 "pager_filter.bison.c"
    break;

  case 35: /* range: '[' INT '+' INT ']'  */
#line 240 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"
                      {
    (yyval.node) = create_range((yyvsp[-3].ival), (yyvsp[-1].ival) + (yyvsp[-3].ival));
}
#line 1480 "pager_filter.bison.c"
    break;


#line 1484 "pager_filter.bison.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (output, store, YY_("syntax error"));
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, output, store);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp, output, store);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;


#if !defined yyoverflow
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (output, store, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturn;
#endif


/*-------------------------------------------------------.
| yyreturn -- parsing is finished, clean up and return.  |
`-------------------------------------------------------*/
yyreturn:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, output, store);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp, output, store);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

#line 245 "/home/yuranu/projects/nvmesh/tools/traces_post_processor/pager_filter.y"


filter_t *build_filter(immutable_string_store_t *store, FILE *input_file) {
    filter_t *output = NULL;
    yyin = input_file;
    while (yyparse((void **)&output, store));
    return output;
}

void yyerror(void **output, void *store, const char *s) {
    printf("Filter parsing error at line %d col %d: %s\n", line_num, ch_num, s);
    exit(-1);
}
