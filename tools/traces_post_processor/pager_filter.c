/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _GNU_SOURCE
#include <fnmatch.h>
#undef _GNU_SOURCE

#include "pager_filter.h"
#include "formatter.h"
#include "../../common/compat/kr_incs_data_structs.inc.c"	// WARN() defined in pager_filter.h -> pager_infra.h -> pager_hashtable.h

/*Struct body*/
struct wc_test_set {
	immutable_string_t *wc; /*The wildcard itsef*/
	immutable_string_store_t *tested; /*The ids we already tested*/
	immutable_string_store_t *matched; /*The ids that match (amongst those that are tested)*/
};

struct token_val {
	struct list_head link;
	enum arg_types type;
	union {
		long ival;
		immutable_string_t *sval;
	} val;
};

struct token_list_elem {
	struct list_head link;
	immutable_string_t *name;
};

struct sticky_token {
	struct list_head link;
	immutable_string_t *name;
	struct list_head remaps /*token_list_elem*/;
	struct list_head vals /*token_val*/;
};

struct sticky {
	struct list_head list /*sticky_token*/;
	immutable_string_store_t *store;
};

/**
 * Utility function - test wildcard agains string and store results in DP store
 */
int test_wc_test_set(wc_test_set_t *test_set, const char *str) {
	if (has_immutable_string(test_set->tested, str))
		return has_immutable_string(test_set->matched, str);
	else {
		if (!fnmatch(test_set->wc->token, str, FNM_CASEFOLD | FNM_NOESCAPE)) {
			get_immutable_string(test_set->matched, str);
			return 1;
		}
		return 0;
	}
}

void print_ast_node(filter_ast_node_t *n) {
	static const char *bracketso = "{[(";
	static const char *bracketsc = "}])";
	static int brackid = -1;
	brackid += 1;
	if (brackid > 2)
		brackid = 0;
	switch (n->kind) {
		case NODE_KIND_TOKENID_VAR:
			printf("%s", n->val.token->token);
			break;
		case NODE_KIND_HAS_OPERATOR:
			printf("HAS %s", n->val.token->token);
			break;
		case NODE_KIND_EQ_TRACEID:
			printf("TRACEID = %s", n->val.token->token);
			break;
		case NODE_KIND_EQ_FUNC:
			printf("FUNC = %s", n->val.token->token);
			break;
		case NODE_KIND_EQ_FILE:
			printf("FILE = %s", n->val.token->token);
			break;
		case NODE_KIND_IN_RANGE_SEV:
			printf("SEV IN [");
			print_ast_node(n->children[0]);
			printf(", ");
			print_ast_node(n->children[1]);
			printf("]");
			break;
		case NODE_KIND_FMT_LIKE:
			printf("FMT =~ %s", n->val.wc->wc->token);
			break;
		case NODE_KIND_EQ_CPU:
			printf("CPU = %lld", n->val.ival);
			break;
		case NODE_KIND_STRING_LITERAL:
			printf("%s", n->val.sval);
			break;
		case NODE_KIND_INT_LITERAL:
			printf("0x%llx", n->val.ival);
			break;
		// Binary
		case NODE_KIND_EQ_EXPR:
		case NODE_KIND_AND_OPERATOR:
		case NODE_KIND_OR_OPERATOR:
			printf("%c ", bracketso[brackid]);
			print_ast_node(n->children[0]);
			printf(" %s ", n->val.sval);
			print_ast_node(n->children[1]);
			printf(" %c", bracketsc[brackid]);
			break;
		// Unary
		case NODE_KIND_NOT_OPERATOR:
			printf("%s", n->val.sval);
			printf("%c ", bracketso[brackid]);
			print_ast_node(n->children[0]);
			printf(" %c", bracketsc[brackid]);
			break;
		case NODE_KIND_BRACKETS:
			printf("%c ", bracketso[brackid]);
			print_ast_node(n->children[0]);
			printf(" %c", bracketsc[brackid]);
			break;
		case NODE_KIND_BUFFER_LITERAL:
			assert(0); // Not implemented
			break;
	};
	brackid -= 1;
	if (brackid < 0)
		brackid = 2;
}

filter_ast_node_t *create_ast_node(immutable_string_store_t *store, filter_ast_node_kind_t kind, filter_ast_node_val_t val,
								   filter_ast_node_t *c1, filter_ast_node_t *c2) {
	filter_ast_node_t *node = calloc(1, sizeof(filter_ast_node_t));
	assert(node);
	node->kind = kind;
	if (kind == NODE_KIND_TOKENID_VAR || kind == NODE_KIND_EQ_TRACEID || kind == NODE_KIND_EQ_FUNC || kind == NODE_KIND_EQ_FILE ||
	    kind == NODE_KIND_HAS_OPERATOR) { /*Immutable string matching operations*/
		node->val.token = get_immutable_string(store, val.sval);
		free(val.sval);
	} else if (kind == NODE_KIND_FMT_LIKE ){ /*Wildcard operations*/
		assert((node->val.wc = calloc(1, sizeof(wc_test_set_t))));
		assert((node->val.wc->tested = init_immutable_string_store()));
		assert((node->val.wc->matched = init_immutable_string_store()));
		node->val.wc->wc = get_immutable_string(store, val.sval);
		free(val.sval);
	} else {/*Anything else*/
		node->val = val;
	}
	node->children[0] = c1;
	node->children[1] = c2;

	return node;
}

void free_ast_node(filter_ast_node_t *n) {
	if (!n)
		return;
	free_ast_node(n->children[0]);
	free_ast_node(n->children[1]);
	if (n->kind == NODE_KIND_STRING_LITERAL)
		free(n->val.sval);
	if (n->kind == NODE_KIND_FMT_LIKE) {
		free_immutable_string_store(n->val.wc->matched);
		free_immutable_string_store(n->val.wc->tested);
		free(n->val.wc);
	}
	free(n);
}

sticky_t *extend_sticky(immutable_string_store_t *store, sticky_t *sticky, sticky_token_t *sticky_token) {
	if (!sticky) {
		sticky = calloc(sizeof(*sticky), 1);
		INIT_LIST_HEAD(&sticky->list);
		sticky->store = store;
	}
	assert(sticky);
	assert(sticky->store == store);

	if (sticky_token)
		list_add_tail(&sticky_token->link, &sticky->list);

	return sticky;
}

sticky_token_t *extend_sticky_token(immutable_string_store_t *store, char *token_name, sticky_token_t *sticky_token, char *remap_name) {
	if (!sticky_token) {
		sticky_token = calloc(sizeof(*sticky_token), 1);
		INIT_LIST_HEAD(&sticky_token->remaps);
		INIT_LIST_HEAD(&sticky_token->vals);
	}
	assert(sticky_token);

	if (remap_name) {
		struct token_list_elem *remap = calloc(sizeof(*remap), 1);
		assert(remap);
		remap->name = get_immutable_string(store, remap_name);
		assert(remap->name);
		list_add_tail(&remap->link, &sticky_token->remaps);
	}

	if (token_name) {
		sticky_token->name = get_immutable_string(store, token_name);
		assert(sticky_token->name);
	}

	return sticky_token;
}

void free_sticky(sticky_t *sticky) {
	if (sticky) {
		sticky_token_t *token, *__tmp1;
		list_for_each_entry_safe(token, __tmp1, &sticky->list, link) {
			{
				struct token_list_elem *remap, *__tmp2;
				list_for_each_entry_safe(remap, __tmp2, &token->remaps, link) {
					list_del_init(&remap->link);
					free(remap);
				}
			}
			{
				struct token_val *val, *__tmp2;
				list_for_each_entry_safe(val, __tmp2, &token->vals, link) {
					list_del_init(&val->link);
					free(val);
				}
			}
			list_del_init(&token->link);
			free(token);
		}
		free(sticky);
	}
}

filter_t *create_filter(filter_ast_node_t *ast, sticky_t *sticky, filter_ast_node_t *sticky_until) {
	filter_t *f = calloc(sizeof(*f), 1);
	assert(f);
	f->ast = ast, f->sticky = sticky, f->sticky_until = sticky_until;

	return f;
}

/**
 * Free the resources used by the filter
 */
void free_filter(filter_t *f) {
	if (f) {
		free_ast_node(f->ast);
		free_sticky(f->sticky);
		free_ast_node(f->sticky_until);
	}
}

#define foreach_matching_token(i_, entry_, name_) \
	foreach_arg(                                  \
		i_, entry_) if((entry_)->args[i].name == name_ || (entry_)->args[i].short_name == name_)

/**
 * Filter check for equality expression. As it is more complex then others, moved to a separate routine.
 */
int _filter_eq_expr(binary_trace_t *trace, filter_ast_node_t *filter) {
	int i;
	assert(filter->kind == NODE_KIND_EQ_EXPR);

	// Check if any arg of current trace is same token we are looking for.
	// Here we use simple linear search, assuming list of arguments is usually very short.
	// Doing 1 or 2 iterations is probably faster then looking up hash tables or trees.
	foreach_matching_token (i, trace->entry, filter->children[0]->val.token) {
		// This trace has token id we are looking for. Now compare the value.
		// The way we compare depends on the type.
		if (trace->entry->args[i].type == ARG_INT && filter->children[1]->kind == NODE_KIND_INT_LITERAL) {
			// Integer comparison
			if (filter->val.sval[0] == '=')
				return trace->arg_vec[i] == filter->children[1]->val.ival;
			if (filter->val.sval[0] == '>')
				return trace->arg_vec[i] > filter->children[1]->val.ival;
			if (filter->val.sval[0] == '<')
				return trace->arg_vec[i] < filter->children[1]->val.ival;
		} else if (trace->entry->args[i].type == ARG_STRING &&
					filter->children[1]->kind == NODE_KIND_STRING_LITERAL) {
			// String comparison
			if (filter->val.sval[0] == '~') /*It is not comparison but a wildcard*/
				return !fnmatch(filter->children[1]->val.sval, (char *)trace->arg_vec[i], FNM_CASEFOLD | FNM_NOESCAPE);
			return !strcmp((char *)trace->arg_vec[i], filter->children[1]->val.sval);
		} else {
			// We do not check types at compilation type, so it is possible we get here in case of a bad
			// query.
			// TODO: Maybe add type validation at compilation time.
			fprintf(stderr, "Filter error: incompatible type filter for token %s\n",
					trace->entry->args[i].name->token);
		}
	}
	return 0; // Not found. Fail the filter.
}

/**
 * Filter check - whether the entry has a given token.
 * TODO: Maybe optimize to use a bitmap of all possible traces for each token
 */
int _filter_has_token(binary_trace_t *trace, filter_ast_node_t *filter) {
	int i;
	for (i = 0; i < MAX_ARGS && trace->entry->args[i].name; ++i) {
		if (trace->entry->args[i].name == filter->val.token)
			return 1;
	}
	return 0;
}

int trace_passes_filter(binary_trace_t *trace, filter_t *filter) {
	if (!filter) return 1;
	int pass = 0;
	if ((pass = trace_passes_filter_ast(trace, filter->ast))) {
		trace_add_sticky(trace, filter->sticky);
	} else {
		pass = trace_passes_sticky(trace, filter->sticky);
	}

	if (filter->sticky_until && trace_passes_filter_ast(trace, filter->sticky_until)) {
		/* Stickiness breaker */
		trace_remove_sticky(trace, filter->sticky);
	}

	return pass;
}

int trace_passes_filter_ast(binary_trace_t *trace, filter_ast_node_t *filter) {
	if (!filter) return 1; /* Empty filter = success always */
	switch (filter->kind) {
		// Expressions
		case NODE_KIND_EQ_EXPR:
			return _filter_eq_expr(trace, filter);
		case NODE_KIND_EQ_TRACEID:
			if (IS_SYSTEM_TRACE_ID(trace->entry->trace_id))
				return !strcmp(filter->val.token->token, "SYSTEM_TRACE"); /* @TODO: Hack, inefficient. Refactor.*/
			return trace->entry->trace_name == filter->val.token;
		case NODE_KIND_EQ_FUNC:
			return trace->entry->func_name == filter->val.token;
		case NODE_KIND_EQ_FILE:
			return trace->entry->file_name == filter->val.token;
		case NODE_KIND_FMT_LIKE:
			if (trace->entry->fmt)
				return test_wc_test_set(filter->val.wc, trace->entry->fmt);
			return 0;
		case NODE_KIND_EQ_CPU:
			return trace->cpu_id == filter->val.ival;
		case NODE_KIND_IN_RANGE_SEV:
			return trace->entry->severity >= (long)filter->children[0] && trace->entry->severity <= (long)filter->children[1];

		// Operators
		case NODE_KIND_AND_OPERATOR:
			return trace_passes_filter_ast(trace, filter->children[0]) && trace_passes_filter_ast(trace, filter->children[1]);
		case NODE_KIND_OR_OPERATOR:
			return trace_passes_filter_ast(trace, filter->children[0]) || trace_passes_filter_ast(trace, filter->children[1]);
		case NODE_KIND_NOT_OPERATOR:
			return !trace_passes_filter_ast(trace, filter->children[0]);
		case NODE_KIND_HAS_OPERATOR:
			return _filter_has_token(trace, filter);

		// Brackets / Vars / Literals.
		case NODE_KIND_TOKENID_VAR:
		case NODE_KIND_INT_LITERAL:
		case NODE_KIND_STRING_LITERAL:
		case NODE_KIND_BUFFER_LITERAL:
		case NODE_KIND_BRACKETS:
			assert(0); // Should never get here, bug if did. We must stop at expression level and not get any lower.
			return 1;
	}

	assert(0); // Should never get here, bug if did
	return 1;
}

int __arg_matches_any_token_val(long arg, enum arg_types type, struct list_head *vals) {
	struct token_val *val;
	list_for_each_entry(val, vals, link) {
		if (val->type != type) {
			fprintf(stderr, "Filter error: incompatible type comparison in sticky values\n");
		} else {
			switch (type) {
				case ARG_INT: if (val->val.ival == arg) return 1;
					break;
				case ARG_STRING: if (!strcmp(val->val.sval->token, (char*)arg)) return 1;
					break;
				default:
					fprintf(stderr, "Filter error: unsupported sticky type %d\n", type);
					break;
			}
		}
	}
	return 0;
}

void __remove_any_match_token_val(long arg, enum arg_types type, struct list_head *vals) {
	struct token_val *val, *__tmp;
	list_for_each_entry_safe(val, __tmp, vals, link) {
		if (val->type != type) {
			fprintf(stderr, "Filter error: incompatible type comparison in sticky values\n");
		} else {
			int match = 0;
			switch (type) {
				case ARG_INT: match = (val->val.ival == arg);
					break;
				case ARG_STRING: match = !strcmp(val->val.sval->token, (char*)arg);
					break;
				default:
					fprintf(stderr, "Filter error: unsupported sticky type %d\n", type);
					break;
			}
			if (match) {
				list_del_init(&val->link);
				free(val);
			}
		}
	}
}

void trace_add_sticky(binary_trace_t *trace, sticky_t *sticky) {
	if (!sticky) return; /* No sticky */
	int i;
	sticky_token_t *sticky_token;
	list_for_each_entry(sticky_token, &sticky->list, link) {
		assert(sticky_token->name);
		foreach_matching_token(i, trace->entry, sticky_token->name) {
			if (!__arg_matches_any_token_val(trace->arg_vec[i], trace->entry->args[i].type, &sticky_token->vals)) {
				struct token_val *val = calloc(sizeof(*val), 1);
				assert(val);
				val->type = trace->entry->args[i].type;
				if (val->type == ARG_INT) {
					val->val.ival = trace->arg_vec[i];
				} else if (val->type == ARG_STRING) {
					val->val.sval = get_immutable_string(sticky->store, (char *)trace->arg_vec[i]);
				} else {
					fprintf(stderr, "Filter error: unsupported type %d of a sticky token %s\n",
						trace->entry->args[i].type, trace->entry->args[i].name->token);
					break;
				}
				list_add_tail(&val->link, &sticky_token->vals);
			}
		}
	}
}

void trace_remove_sticky(binary_trace_t *trace, sticky_t *sticky) {
	assert(sticky);
	int i;
	sticky_token_t *sticky_token;
	list_for_each_entry(sticky_token, &sticky->list, link) {
		assert(sticky_token->name);
		foreach_matching_token(i, trace->entry, sticky_token->name) {
			__remove_any_match_token_val(trace->arg_vec[i], trace->entry->args[i].type, &sticky_token->vals);
		}
	}
}

int trace_passes_sticky(binary_trace_t *trace, sticky_t *sticky) {
	int i;
	if (!sticky) return 0; /* No sticky */
	sticky_token_t* sticky_token;
	list_for_each_entry(sticky_token, &sticky->list, link) {
		assert(sticky_token->name);
		struct token_list_elem* remap;
		list_for_each_entry(remap, &sticky_token->remaps, link) {
			foreach_matching_token(i, trace->entry, remap->name) {
				if(__arg_matches_any_token_val(
					   trace->arg_vec[i], trace->entry->args[i].type, &sticky_token->vals)) {
					return 1;
				}
			}
		}
	}
	return 0;
}

/* Utility used to conver hex string to long int */
unsigned long xtol(const char *str) {
    unsigned long res = 0;
    int i = 0;
    while (str[i]) {
        unsigned long chval = 0;
        if ('0' <= str[i] && str[i] <= '9')
            chval = str[i] - '0';
        else if ('a' <= str[i] && str[i] <= 'z')
            chval = str[i] - ('a' - 10);
        else if ('A' <= str[i] && str[i] <= 'Z')
            chval = str[i] - ('A' - 10);
        res = ((res << 4) + chval);
		++i;
    }
    return res;
}

filter_ast_node_t *create_composite(immutable_string_store_t *store, char *token, composite_kwargs_list_t *l1) {
	filter_ast_node_t *head = NULL;
	composite_kwargs_list_t *p = l1;
	char tmp[256];
	while (p) {
		composite_kwargs_list_t *prev = p;
		filter_ast_node_t *eq = NULL;
		filter_ast_node_t *tn = NULL;
		snprintf(tmp, sizeof(tmp), "%s.%s", token, p->token->token + 1);
		tn = create_ast_node(store, NODE_KIND_TOKENID_VAR, (filter_ast_node_val_t){.sval = strdup(tmp)}, NULL, NULL);
		eq = create_ast_node(store, NODE_KIND_EQ_EXPR, (filter_ast_node_val_t){.sval = "="}, tn, p->val);
		if (!head)
			head = eq;
		else
			head = create_ast_node(store, NODE_KIND_AND_OPERATOR, (filter_ast_node_val_t){.sval = "AND"}, head, eq);
		p = p->next;
		free(prev);
	}
	free(token);
	return head;
}

composite_kwargs_list_t *create_composite_kwargs(immutable_string_store_t *store, char *token, filter_ast_node_t *val) {
	composite_kwargs_list_t *out;
	assert((out = calloc(1, sizeof(composite_kwargs_list_t))));
	assert((out->token = get_immutable_string(store, token)));
	free(token);
	out->val = val;
	return out;
}

composite_kwargs_list_t *merge_composite_kwargs(immutable_string_store_t *store, composite_kwargs_list_t *l1, composite_kwargs_list_t *l2) {
	composite_kwargs_list_t *p = l1;
	while (p->next) p = p->next;
	p->next = l2;
	return l1;
}

filter_ast_node_t *create_range_node(immutable_string_store_t *store, char *token, long_range_t *r) {
	filter_ast_node_t *t1 = create_ast_node(store, NODE_KIND_TOKENID_VAR, (filter_ast_node_val_t){.sval = strdup(token)}, NULL, NULL);
	filter_ast_node_t *i1 = create_ast_node(store, NODE_KIND_INT_LITERAL, (filter_ast_node_val_t){.ival = r->a}, NULL, NULL);
	filter_ast_node_t *op1 = create_ast_node(store, NODE_KIND_EQ_EXPR, (filter_ast_node_val_t){.sval = "="}, t1, i1);

	filter_ast_node_t *t2 = create_ast_node(store, NODE_KIND_TOKENID_VAR, (filter_ast_node_val_t){.sval = strdup(token)}, NULL, NULL);
	filter_ast_node_t *i2 = create_ast_node(store, NODE_KIND_INT_LITERAL, (filter_ast_node_val_t){.ival = r->a}, NULL, NULL);
	filter_ast_node_t *op2 = create_ast_node(store, NODE_KIND_EQ_EXPR, (filter_ast_node_val_t){.sval = ">"}, t2, i2);

	filter_ast_node_t *t3 = create_ast_node(store, NODE_KIND_TOKENID_VAR, (filter_ast_node_val_t){.sval = strdup(token)}, NULL, NULL);
	filter_ast_node_t *i3 = create_ast_node(store, NODE_KIND_INT_LITERAL, (filter_ast_node_val_t){.ival = r->b}, NULL, NULL);
	filter_ast_node_t *op3 = create_ast_node(store, NODE_KIND_EQ_EXPR, (filter_ast_node_val_t){.sval = "="}, t3, i3);

	filter_ast_node_t *t4 = create_ast_node(store, NODE_KIND_TOKENID_VAR, (filter_ast_node_val_t){.sval = strdup(token)}, NULL, NULL);
	filter_ast_node_t *i4 = create_ast_node(store, NODE_KIND_INT_LITERAL, (filter_ast_node_val_t){.ival = r->b}, NULL, NULL);
	filter_ast_node_t *op4 = create_ast_node(store, NODE_KIND_EQ_EXPR, (filter_ast_node_val_t){.sval = "<"}, t4, i4);

	filter_ast_node_t *or1 = create_ast_node(store, NODE_KIND_OR_OPERATOR, (filter_ast_node_val_t){.sval = "OR"}, op1, op2);
	filter_ast_node_t *or2 = create_ast_node(store, NODE_KIND_OR_OPERATOR, (filter_ast_node_val_t){.sval = "OR"}, op3, op4);

	free(r);
	free(token);

	return create_ast_node(store, NODE_KIND_AND_OPERATOR, (filter_ast_node_val_t){.sval = "AND"}, or1, or2);
}
