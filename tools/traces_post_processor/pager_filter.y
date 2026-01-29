%{
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
%}

%parse-param {void **output}{void *store}

%union {
    long ival;
    char *sval;
    void *node;
}

// define the constant-string tokens:
%token AND
%token OR
%token NOT
%token HAS
%token LIKE
%token IN
%token RARROW

%token PROP_CPU
%token PROP_TRACE
%token PROP_FUNC
%token PROP_FILE
%token PROP_FMT
%token PROP_SEV
%token PROP_STICKY
%token PROP_STICKY_END

// define the terminal symbol token types
%token <ival> INT
%token <sval> STRING
%token <sval> TOKENID

%type <node> filter
%type <node> exp
%type <node> sticky_list
%type <node> sticky_until
%type <node> sticky
%type <node> tokens_list
%type <node> composite
%type <node> composite_kwargs
%type <node> tokenid
%type <node> literal
%type <node> range

%%
filter:
  exp {
    *output = create_filter($1, NULL, NULL);
}
| exp sticky_list sticky_until {
    *output = create_filter($1, $2, $3);
}
;

sticky_list: 
  sticky {
    $$ = extend_sticky(store, NULL, $1);
}
| sticky ',' sticky_list {
    $$ = extend_sticky(store, $3, $1);
}
;

sticky_until: {
      $$ = NULL;
} | PROP_STICKY_END exp {
      $$ = $2;
  }
;

sticky:
  PROP_STICKY TOKENID RARROW '[' tokens_list ']' {
      $$ = extend_sticky_token(store, $2, $5, NULL);
  }
;

tokens_list:
  TOKENID {
    $$ = extend_sticky_token(store, NULL, NULL, $1);
}
| TOKENID ',' tokens_list {
    $$ = extend_sticky_token(store, NULL, $3, $1);
}
;

exp:
  composite {
    $$ = $1;
}
| TOKENID IN range { // ex: @VLBA in [100 + 20]
    $$ = create_range_node(store, $1, $3);
}
| tokenid '=' literal { // ex: @RAID_VERSION = 15
    $$ = create_ast_node(store, NODE_KIND_EQ_EXPR,
                         (filter_ast_node_val_t){.sval = "="},
                         $1,
                         $3);
}
| tokenid '<' literal { // ex: @RAID_VERSION < 15
    $$ = create_ast_node(store, NODE_KIND_EQ_EXPR,
                         (filter_ast_node_val_t){.sval = "<"},
                         $1,
                         $3);
}
| tokenid '>' literal { // ex: @RAID_VERSION > 15
    $$ = create_ast_node(store, NODE_KIND_EQ_EXPR,
                         (filter_ast_node_val_t){.sval = ">"},
                         $1,
                         $3);
}
| tokenid LIKE literal { // ex: @NAME ~ "volume.*"
    $$ = create_ast_node(store, NODE_KIND_EQ_EXPR,
                         (filter_ast_node_val_t){.sval = "~"},
                         $1,
                         $3);
}
| PROP_TRACE '=' STRING { // ex: trace = "__D_trace_1_main"
    $$ = create_ast_node(store, NODE_KIND_EQ_TRACEID,
                         (filter_ast_node_val_t){.sval = $3},
                         NULL,
                         NULL);
}
| PROP_FUNC '=' STRING { // ex: trace = "__D_trace_1_main"
    $$ = create_ast_node(store, NODE_KIND_EQ_FUNC,
                         (filter_ast_node_val_t){.sval = $3},
                         NULL,
                         NULL);
}
| PROP_FILE '=' STRING { // ex: file = "nvmeibc_jam.c"
    $$ = create_ast_node(store, NODE_KIND_EQ_FILE,
                         (filter_ast_node_val_t){.sval = $3},
                         NULL,
                         NULL);
}
| PROP_FMT LIKE STRING { // ex: FMT ~ "Error occurred in*"
    $$ = create_ast_node(store, NODE_KIND_FMT_LIKE,
                         (filter_ast_node_val_t){.sval = $3},
                         NULL,
                         NULL);
}
| PROP_CPU '=' INT { // ex: cpu = 3
    $$ = create_ast_node(store, NODE_KIND_EQ_CPU,
                         (filter_ast_node_val_t){.ival = $3},
                         NULL,
                         NULL);
}
| PROP_SEV IN '[' INT ',' INT ']' { // ex: sev in [0, 4]
    $$ = create_ast_node(store, NODE_KIND_IN_RANGE_SEV,
                         (filter_ast_node_val_t){.sval = "IN"},
                         (void*)$4,
                         (void*)$6);
}
| HAS TOKENID { // ex: has @TOPOLOGY
    $$ = create_ast_node(store, NODE_KIND_HAS_OPERATOR,
                         (filter_ast_node_val_t){.sval = $2},
                         NULL,
                         NULL);
}
| exp AND exp {
    $$ = create_ast_node(store, NODE_KIND_AND_OPERATOR,
                         (filter_ast_node_val_t){.sval = "AND"},
                         $1,
                         $3);
}
| exp OR exp {
    $$ = create_ast_node(store, NODE_KIND_OR_OPERATOR,
                         (filter_ast_node_val_t){.sval = "OR"},
                         $1,
                         $3);
}
| NOT exp {
    $$ = create_ast_node(store, NODE_KIND_NOT_OPERATOR,
                         (filter_ast_node_val_t){.sval = "NOT"},
                         $2,
                         NULL);
}
| '(' exp ')' {
    $$ = $2; // For order only
}
;

composite:
  TOKENID '{' composite_kwargs '}' {
    $$ = create_composite(store, $1, $3);
}
;

composite_kwargs:
  TOKENID '=' literal {
    $$ = create_composite_kwargs(store, $1, $3);
}
| composite_kwargs ',' composite_kwargs {
    $$ = merge_composite_kwargs(store, $1, $3);
}
;

tokenid:
  TOKENID {
    $$ = create_ast_node(store, NODE_KIND_TOKENID_VAR,
                         (filter_ast_node_val_t){.sval = $1},
                         NULL,
                         NULL);
}
;

literal:
  INT {
    $$ = create_ast_node(store, NODE_KIND_INT_LITERAL,
                         (filter_ast_node_val_t){.ival = $1},
                         NULL,
                         NULL);
}
| STRING {
    $$ = create_ast_node(store, NODE_KIND_STRING_LITERAL,
                         (filter_ast_node_val_t){.sval = $1},
                         NULL,
                         NULL);
}
;

range:
  '[' INT ',' INT ']' {
    $$ = create_range($2, $4);
}
| '[' INT '+' INT ']' {
    $$ = create_range($2, $4 + $2);
}
;

%%

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
