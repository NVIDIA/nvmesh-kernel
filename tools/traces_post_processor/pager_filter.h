#ifndef PAGER_FILTER_H
#define PAGER_FILTER_H

#include "pager_infra.h"
#include "pager_msg_stream.h"
#include <stdio.h>

/**
 * All possible types of ast nodes for filter grammar
 */
typedef enum filter_ast_node_kind {

	// Expressions
	NODE_KIND_EQ_EXPR,
	NODE_KIND_EQ_TRACEID,
	NODE_KIND_EQ_FUNC,
	NODE_KIND_EQ_FILE,
	NODE_KIND_FMT_LIKE,
	NODE_KIND_EQ_CPU,
	NODE_KIND_IN_RANGE_SEV,

	// Variables
	NODE_KIND_TOKENID_VAR,

	// Literals
	NODE_KIND_INT_LITERAL,
	NODE_KIND_STRING_LITERAL,
	NODE_KIND_BUFFER_LITERAL,

	// Operators
	NODE_KIND_AND_OPERATOR,
	NODE_KIND_OR_OPERATOR,
	NODE_KIND_NOT_OPERATOR,
	NODE_KIND_HAS_OPERATOR,

	// Brackets
	NODE_KIND_BRACKETS
} filter_ast_node_kind_t;

/**
 * Object used to store information on per trace wildcard matching
 * It contains a wildcard and two sets - matching and tested traces.
 * @todo: If it has any use outside of this module - make it public
 */
struct wc_test_set;
typedef struct wc_test_set wc_test_set_t;

/**
 * Union used to store node value;
 * Can be long
 * - Integer (for all integral types)
 * - String pointer
 * - An arbitrary binary buffer (currently unsupported, for future versions)
 * - Wildacrd matching object
 */
typedef union filter_ast_node_val {
	long long ival; // Integral value
	char *sval;		// String type
	void *bval;		// Binary buffer
	immutable_string_t *token;	// Token / trace / func - any immutable string
	wc_test_set_t *wc; /*Wildcards matcher*/

} filter_ast_node_val_t;

/**
 * Ast node descriptor in filter grammar
 * Has kind (type of node), value (for variables or literals) and up to 2 children (grammar limitation, currently no
 * need for more)
 */
typedef struct filter_ast_node {
	filter_ast_node_kind_t kind;
	filter_ast_node_val_t val;
	struct filter_ast_node *children[2];
} filter_ast_node_t;

/**
 * Sticky tokens object, consist of a map from known token to a list
 * of sticky values.
 */
struct sticky;
struct sticky_token;
typedef struct sticky sticky_t;
typedef struct sticky_token sticky_token_t;

/**
 * Root filter object
 */
typedef struct filter {
	filter_ast_node_t *ast; /* AST root */
	sticky_t *sticky; /* Sticky storage */
	filter_ast_node_t *sticky_until; /* AST that stops the stickiness */
} filter_t;

typedef struct composite_kwargs_list {
	immutable_string_t *token;
	filter_ast_node_t *val;
	struct composite_kwargs_list *next;
} composite_kwargs_list_t;

/**
 * Used to parse range args
 */
typedef struct long_range {
	long a, b;
} long_range_t;

/**
 * Build ast describing filter from an open input file
 */
filter_t *build_filter(immutable_string_store_t *store, FILE *input_file);

/**
 * Print format ast in human readable way (for debug)
 */
void print_ast_node(filter_ast_node_t *n);

/**
 * This function is used by linter, probably shouldn't be called outside of it
 * (but if you know what you are doing, well... go on).
 * Create a single format ast node, with given kind, value and children. One or more children can be NULL, if
 * kind supports it.
 */
filter_ast_node_t *create_ast_node(immutable_string_store_t *store, filter_ast_node_kind_t kind, filter_ast_node_val_t val,
								   filter_ast_node_t *c1, filter_ast_node_t *c2);

/**
 * Free resouces recursively
 */
void free_ast_node(filter_ast_node_t *n);

/**
 * Create or extend a sticky store with a new sticky token.
 * Used by ast, no reason to call from any other code.
 */
sticky_t *extend_sticky(immutable_string_store_t *store, sticky_t *sticky, sticky_token_t *sticky_token);

/**
 * Create or extend a sticky token witha new remap or create if NULL.
 * Used by ast, no reason to call from any other code.
 */
sticky_token_t *extend_sticky_token(immutable_string_store_t *store, char *token_name, sticky_token_t *sticky_token, char *remap_name);

/**
 * Free the resources held by the sticky tokens
 */
void free_sticky(sticky_t *sticky);

/**
 * Generate filter object from filter ast and stick component from the previous stages
 */
filter_t *create_filter(filter_ast_node_t *ast, sticky_t *sticky, filter_ast_node_t *sticky_until);

/**
 * Free the resources used by the filter
 */
void free_filter(filter_t *filter);

/**
 * Return 1 if message passes given filter, 0 otherwise
 */
int trace_passes_filter(binary_trace_t *trace, filter_t *filter);

/**
 * Return 1 if message passes given filter (ast only), 0 otherwise
 */
int trace_passes_filter_ast(binary_trace_t *trace, filter_ast_node_t *filter);

/**
 * Checks if traces has any sticky tokens in it and if yes adds their values
 * to the sticky store
 */
void trace_add_sticky(binary_trace_t *trace, sticky_t *sticky);

/**
 * Checks if traces has any sticky tokens in it and if yes remove their values
 * from the sticky store
 */
void trace_remove_sticky(binary_trace_t *trace, sticky_t *sticky);

/**
 * Return 1 if message fits at least one value from the sticky store
 */
int trace_passes_sticky(binary_trace_t *trace, sticky_t *sticky);

filter_ast_node_t *create_composite(immutable_string_store_t *store, char *token, composite_kwargs_list_t *l1);
void free_composite(filter_ast_node_t *composite);
composite_kwargs_list_t *create_composite_kwargs(immutable_string_store_t *store, char *token, filter_ast_node_t *val);
composite_kwargs_list_t *merge_composite_kwargs(immutable_string_store_t *store, composite_kwargs_list_t *l1, composite_kwargs_list_t *l2);

static inline long_range_t *create_range(long a, long b) {
	long_range_t *r;
	assert((r = malloc(sizeof(long_range_t))));
	r->a = a; r->b = b;
	return r;
}

filter_ast_node_t *create_range_node(immutable_string_store_t *store, char *token, long_range_t *r);

#endif /*PAGER_FILTER_H*/
