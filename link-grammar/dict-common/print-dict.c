/*************************************************************************/
/* Copyright (c) 2004                                                    */
/* Daniel Sleator, David Temperley, and John Lafferty                    */
/* Copyright 2013, 2014 Linas Vepstas                                    */
/* All rights reserved                                                   */
/*                                                                       */
/* Use of the link grammar parsing system is subject to the terms of the */
/* license set forth in the LICENSE file included with this software.    */
/* This license allows free redistribution and use in source and binary  */
/* forms, with or without modification, subject to certain conditions.   */
/*                                                                       */
/*************************************************************************/

#include <math.h>                   // fabs

#include "api-structures.h"         // Parse_Options_s  (seems hacky to me)
#include "dict-common.h"
#include "dict-defines.h"
#include "dict-file/word-file.h"
#include "dict-file/read-dict.h"
#include "dict-utils.h"             // copy_Exp
#include "disjunct-utils.h"
#include "prepare/build-disjuncts.h"    // build_disjuncts_for_exp
#include "print/print.h"
#include "print/print-util.h"
#include "regex-morph.h"
#include "tokenize/tokenize.h"      // word_add
#include "tokenize/word-structures.h"   // Word_struct
#include "utilities.h"              // GNU_UNUSED
/* ======================================================================== */

bool cost_eq(double cost1, double cost2)
{
	return (fabs(cost1 - cost2) < cost_epsilon);
}

/**
 * Convert cost to a string with at most cost_max_dec_places decimal places.
 */
const char *cost_stringify(double cost)
{
	static TLS char buf[16];

	int l = snprintf(buf, sizeof(buf), "%.*f", cost_max_dec_places, cost);
	if ((l < 0) || (l >= (int)sizeof(buf))) return "ERR_COST";

	return buf;
}

#define MACRO_INDENTATION 4

static void print_expression_tag_start(Dictionary dict, dyn_str *e, const Exp *n,
                                 int *indent)
{
	switch (n->tag_type)
	{
		case Exptag_none:
			break;
		case Exptag_dialect:
			dyn_strcat(e, "[");
			break;
		case Exptag_macro:
			if (*indent < 0) break;
			dyn_strcat(e, "\n");
			for(int i = 0; i < *indent; i++) dyn_strcat(e, " ");
			dyn_strcat(e, dict->macro_tag->name[n->tag_id]);
			dyn_strcat(e, ": ");
			*(indent) += MACRO_INDENTATION;
			break;
		default:
			for(int i = 0; i < *indent; i++) dyn_strcat(e, " ");
			append_string(e, "Unknown tag type %d: ", (int)n->tag_type);
			*(indent) += MACRO_INDENTATION;
	}
}

static void print_expression_tag_end(Dictionary dict, dyn_str *e, const Exp *n,
                                 int *indent)
{
	if (NULL == dict) return;

	switch (n->tag_type)
	{
		case Exptag_none:
			break;
		case Exptag_dialect:
			dyn_strcat(e, "]");
			dyn_strcat(e, dict->dialect_tag.name[n->tag_id]);
			break;
		case Exptag_macro:
			if (*indent < 0) break;
			dyn_strcat(e, "\n");
			for(int i = 0; i < *indent - MACRO_INDENTATION/2; i++)
				dyn_strcat(e, " ");
			(*indent) -= MACRO_INDENTATION;
			break;
		default:
			/* Handled in print_expression_tag_start(). */
			;
	}
}

static void get_expression_cost(const Exp *e, unsigned int *icost, double *dcost)
{
	if (e->cost < -cost_epsilon)
	{
		*icost = 1;
		*dcost = e->cost;
	}
	else if (cost_eq(e->cost, 0.0))
	{
		/* avoid [X+]-0.00 */
		*icost = 0;
		*dcost = 0;
	}
	else
	{
		*icost = (int) (e->cost);
		*dcost = e->cost - *icost;
		if (*dcost > cost_epsilon)
		{
			*dcost = e->cost;
			*icost = 1;
		}
		else
		{
			if (*icost > 4)
			{
				/* don't print too many [] levels */
				*dcost = *icost;
				*icost = 1;
			}
			else
			{
				*dcost = 0;
			}
		}
	}
}

static bool is_expression_optional(const Exp *e)
{
	Exp *o = e->operand_first;

	return (e->type == OR_type) && (o != NULL) && (o->type == AND_type) &&
	    (NULL == o->operand_first) && (o->cost == 0) &&
	    (o->tag_type == Exptag_none);
}

static void print_expression_parens(Dictionary dict, dyn_str *e, const Exp *n,
                                    bool need_parens, int *indent)

{
	unsigned int icost;
	double dcost;
	get_expression_cost(n, &icost, &dcost);
	for (unsigned int i = 0; i < icost; i++) dyn_strcat(e, "[");
	print_expression_tag_start(dict, e, n, indent);

	const char *opr = NULL;
	Exp *opd = n->operand_first;

	if (n->type == CONNECTOR_type)
	{
		if (n->multi) dyn_strcat(e, "@");
		dyn_strcat(e, n->condesc ? n->condesc->string : "error-null-connector");
		dyn_strcat(e, (const char []){ n->dir, '\0' });
	}
	else if (is_expression_optional(n))
	{
		dyn_strcat(e, "{");
		if (NULL == opd->operand_next)
			dyn_strcat(e, "error-no-next"); /* unary OR */
		else
			print_expression_parens(dict, e, opd->operand_next, false, indent);
		dyn_strcat(e, "}");
	}
	else
	{
		if (n->type == AND_type)
			opr = " & ";
		else if (n->type == OR_type)
			opr = " or ";
		else
			append_string(e, "error-exp-type-%d", (int)n->type);

		if (opr != NULL)
		{
			/* (opd == NULL) means this is a null expression. */
			if (((icost == 0) && need_parens) || (opd == NULL)) dyn_strcat(e, "(");

			if ((opd == NULL) && (n->type == OR_type))
				dyn_strcat(e, "error-zeroary-or");

			for (Exp *l = opd; l != NULL; l = l->operand_next)
			{
				print_expression_parens(dict, e, l, true, indent);

				if (l->operand_next != NULL)
					dyn_strcat(e, opr);
				else if ((n->type == OR_type) && (l == n->operand_first))
					dyn_strcat(e, " or error-no-next"); /* unary OR */
			}

			if (((icost == 0) && need_parens) || (opd == NULL)) dyn_strcat(e, ")");
		}
	}

	for (unsigned int i = 0; i < icost; i++) dyn_strcat(e, "]");
	if (dcost != 0) dyn_strcat(e, cost_stringify(dcost));
	print_expression_tag_end(dict, e, n, indent);
}


static const char *lg_exp_stringify_with_tags(Dictionary dict, const Exp *n,
                                              bool show_macros)
{
	static TLS char *e_str;
	int indent = show_macros ? 0 : -1;

	if (e_str != NULL) free(e_str);
	if (n == NULL)
	{
		e_str = NULL;
		return "(null)";
	}

	dyn_str *e = dyn_str_new();
	print_expression_parens(dict, e, n, false, &indent);
	e_str = dyn_str_take(e);
	return e_str;
}

/**
 * Stringify the given expression, ignoring tags.
 */
const char *lg_exp_stringify(const Exp *n)
{
	return lg_exp_stringify_with_tags(NULL, n, false);
}

#ifdef DEBUG
/* There is a much better lg_exp_stringify() elsewhere
 * This one is for low-level debug. */
GNUC_UNUSED void prt_exp(Exp *e, int i)
{
	if (e == NULL) return;

	for(int j =0; j<i; j++) printf(" ");
	printf ("type=%d dir=%c multi=%d cost=%s\n",
	        e->type, e->dir, e->multi, cost_stringify(e->cost));
	if (e->type != CONNECTOR_type)
	{
		for (e = e->operand_next; e != NULL; e = e->operand_next) prt_exp(e, i+2);
	}
	else
	{
		for(int j =0; j<i; j++) printf(" ");
		printf("con=%s\n", e->condesc->string);
	}
}
#endif

static const char *stringify_Exp_type(Exp_type type)
{
	static TLS char unknown_type[32] = "";
	const char *type_str;

	if (type > 0 && type <= 3)
	{
		type_str = ((const char *[]) {"OR", "AND", "CONNECTOR"}) [type-1];
	}
	else
	{
		snprintf(unknown_type, sizeof(unknown_type), "unknown_type-%d",
		         (int)(type));
		type_str = unknown_type;
	}

	return type_str;
}

static const char *stringify_Exp_tag(Exp *e, Dictionary dict)
{
	static TLS char tag_info[64];

		switch (e->tag_type)
		{
			case Exptag_none:
				return "";
			case Exptag_dialect:
				if (dict == NULL)
				{
					snprintf(tag_info, sizeof(tag_info), " dialect_tag=%u",
					         e->tag_id);
				}
				else
				{
					snprintf(tag_info, sizeof(tag_info), " dialect_tag=%s",
					       dict->dialect_tag.name[e->tag_id]);
				}
				break;
			case Exptag_macro:
				if (dict == NULL)
				{
					snprintf(tag_info, sizeof(tag_info), " macro_tag");
				}
				else
				{
					snprintf(tag_info, sizeof(tag_info), " macro_tag=%s",
					       dict->macro_tag->name[e->tag_id]);
				}
				break;
			default:
				snprintf(tag_info, sizeof(tag_info), " unknown_tag_type-%d",
				         (int)(e->tag_type));
				;
		}

	return tag_info;
}

static bool is_ASAN_uninitialized(uintptr_t a)
{
	static const uintptr_t asan_uninitialized = (uintptr_t)0xbebebebebebebebeULL;

	return (a == asan_uninitialized);
}

void prt_exp_all(dyn_str *s, Exp *e, int i, Dictionary dict)
{
	if (is_ASAN_uninitialized((uintptr_t)e))
	{
		dyn_strcat(s, "e=UNINITIALIZED\n");
		return;
	}
	if (e == NULL) return;

	for(int j =0; j<i; j++) dyn_strcat(s, " ");
	append_string(s, "e=%p: %s", e, stringify_Exp_type(e->type));

	if (is_ASAN_uninitialized((uintptr_t)e->operand_first))
		dyn_strcat(s, " (UNINITIALIZED operand_first)");
	if (is_ASAN_uninitialized((uintptr_t)e->operand_next))
		dyn_strcat(s, " (UNINITIALIZED operand_next)");

	if (e->type != CONNECTOR_type)
	{
		int operand_count = 0;
		for (Exp *opd = e->operand_first; NULL != opd; opd = opd->operand_next)
		{
			operand_count++;
			if (is_ASAN_uninitialized((uintptr_t)opd->operand_next))
			{
				append_string(s, " (operand %d: UNINITIALIZED operand_next)\n",
				              operand_count);
				return;
			}
		}
		append_string(s, " (%d operand%s) cost=%s%s\n", operand_count,
		       operand_count == 1 ? "" : "s", cost_stringify(e->cost),
		       stringify_Exp_tag(e, dict));
		for (Exp *opd = e->operand_first; NULL != opd; opd = opd->operand_next)
		{
			prt_exp_all(s, opd, i+2, dict);
		}
	}
	else
	{
		append_string(s, " %s%s%c cost=%s%s\n",
		              e->multi ? "@" : "",
		              e->condesc ? e->condesc->string : "(condesc=(null))",
		              e->dir, cost_stringify(e->cost),
		              stringify_Exp_tag(e, dict));
	}
}

GNUC_UNUSED static void prt_exp_mem(Exp *e)
{
	dyn_str *s = dyn_str_new();

	prt_exp_all(s, e, 0, NULL);
	char *e_str = dyn_str_take(s);
	printf("%s", e_str);
	free(e_str);
}

/* ================ Print disjuncts and connectors ============== */
static bool is_flag(uint32_t flags, char flag)
{
	return (flags>>(flag-'a')) & 1;
}

static uint32_t make_flag(char flag)
{
	return 1<<(flag-'a');
}

/* Print one connector with all the details.
 * mCnameD<tracon_id>{refcount}(nearest_word, length_limit)x
 * Optional m: "@" for multi (else nothing).
 * Cname: Connector name.
 * Optional D: "-" / "+" (if dir != -1).
 * Optional <tracon_id>: (flag 't').
 * Optional [nearest_word, length_limit or farthest_word]: (flag 'l').
 * x: Shallow/deep indication as "s" / "d" (if shallow != -1)
 */
static void dyn_print_one_connector(dyn_str *s, Connector *e, int dir,
                                    int shallow, uint32_t flags)
{
	if (e->multi)
		dyn_strcat(s, "@");
	dyn_strcat(s, connector_string(e));
	if (-1 != dir) dyn_strcat(s, (dir == 0) ? "-" : "+");
	if (is_flag(flags, 't') && e->tracon_id)
		append_string(s, "<%d>", e->tracon_id);
	if (is_flag(flags, 'r') && e->refcount)
		append_string(s, "{%d}",e->refcount);
	if (is_flag(flags, 'l'))
		append_string(s, "(%d,%d)", e->nearest_word, e->length_limit);
	if (-1 != shallow)
		dyn_strcat(s, (0 == shallow) ? "d" : "s");
}

GNUC_UNUSED static void print_one_connector(Connector *e, int dir, int shallow,
                                            uint32_t flags)
{
	dyn_str *s = dyn_str_new();

	dyn_print_one_connector(s, e, dir, shallow, flags);

	char *t = dyn_str_take(s);
	puts(t);
	free(t);
}

static void dyn_print_connector_list(dyn_str *s, Connector *e, int dir, uint32_t flags)
{

	if (e == NULL) return;
	dyn_print_connector_list(s, e->next, dir, flags);
	if (e->next != NULL) dyn_strcat(s, " ");
	dyn_print_one_connector(s, e, dir, /*shallow*/-1, flags);
}

void print_connector_list(Connector *e, uint32_t flags)
{
	dyn_str *s = dyn_str_new();

	dyn_print_connector_list(s, e, /*dir*/-1, flags);

	char *t = dyn_str_take(s);
	puts(t);
	free(t);
}

static void dyn_print_disjunct_list(dyn_str *s, Disjunct *dj, uint32_t flags)
{
	int i = 0;
	char word[MAX_WORD + 32];
	bool print_disjunct_address = test_enabled("disjunct-address");

	for (;dj != NULL; dj=dj->next)
	{
		lg_strlcpy(word, dj->word_string, sizeof(word));
		patch_subscript_mark(word);

		append_string(s, "%16s", word);
		if (print_disjunct_address) append_string(s, "(%p)", dj);
		dyn_strcat(s, ": ");

		append_string(s, "[%d]%s= ", i++, cost_stringify(dj->cost));

		dyn_print_connector_list(s, dj->left, /*dir*/0, flags);
		dyn_strcat(s, " <--> ");
		dyn_print_connector_list(s, dj->right, /*dir*/1, flags);
		dyn_strcat(s, "\n");
	}
}

void print_all_disjuncts(Sentence sent)
{
	dyn_str *s = dyn_str_new();
	uint32_t flags = make_flag('l') | make_flag('t');

	for (WordIdx w = 0; w < sent->length; w++)
	{
		append_string(s, "Word %zu:\n", w);
		dyn_print_disjunct_list(s, sent->word[w].d, flags);

	}

	char *t = dyn_str_take(s);
	puts(t);
	free(t);
}

/* ================ Display word expressions / disjuncts ================= */
/**
 * Display the disjuncts of expressions in \p dn.
 */
static char *display_disjuncts(Dictionary dict, const Dict_node *dn,
                               const void **arg)
{
	const void *rn = arg[0];
	const char *flags = arg[1];
	const Parse_Options opts = (Parse_Options)arg[2];
	double max_cost = opts->disjunct_cost;

	uint32_t int_flags = 0;
	if (flags != NULL)
	{
		for (const char *f = flags; *f != '\0'; f++)
			int_flags |= make_flag(*f);
	}

	/* build_disjuncts_for_exp() needs memory pools for efficiency. */
	Sentence dummy_sent = sentence_create("", dict); /* For memory pools. */
	dummy_sent->Disjunct_pool = pool_new(__func__, "Disjunct",
	                               /*num_elements*/8192, sizeof(Disjunct),
	                               /*zero_out*/false, /*align*/false, false);
	dummy_sent->Connector_pool = pool_new(__func__, "Connector",
	                              /*num_elements*/65536, sizeof(Connector),
	                              /*zero_out*/true, /*align*/false, false);

	/* copy_Exp() needs an Exp memory pool. */
	Pool_desc *Exp_pool = pool_new(__func__, "Exp", /*num_elements*/256,
	                               sizeof(Exp), /*zero_out*/false,
	                               /*align*/false, /*exact*/false);

	dyn_str *s = dyn_str_new();
	dyn_strcat(s, "disjuncts:\n");
	for (; dn != NULL; dn = dn->right)
	{
		/* Use copy_Exp() to assign dialect cost. */
		Exp *e = copy_Exp(dn->exp, Exp_pool, opts);
		Disjunct *d = build_disjuncts_for_exp(dummy_sent, e, dn->string, NULL,
		                                      max_cost, NULL);
		unsigned int dnum0 = count_disjuncts(d);
		d = eliminate_duplicate_disjuncts(d);
		unsigned int dnum1 = count_disjuncts(d);

		dyn_str *dyn_pdl = dyn_str_new();
		dyn_print_disjunct_list(dyn_pdl, d, int_flags);
		char *dliststr = dyn_str_take(dyn_pdl);

		pool_reuse(Exp_pool);
		pool_reuse(dummy_sent->Disjunct_pool);
		pool_reuse(dummy_sent->Connector_pool);

		/* Count number of disjuncts with tunnel connectors. */
		unsigned int tnum = 0;
		for (const char *p = dliststr; *p != '\0'; p++)
			if ((p[0] == ' ') && (p[1] == 'x')) tnum++;

		unsigned int dnum_selected = 0;
		dyn_str *selected = NULL;
		char *dstr = dliststr;
		char *end;
		if (rn != NULL)
		{
			selected = dyn_str_new();

			do
			{
				end = strchr(dstr, '\n');
				*end = '\0';
				if (match_regex(rn , dstr) != NULL)
				{
					dyn_strcat(selected, dstr);
					dyn_strcat(selected, "\n");
					dnum_selected++;
				}

				dstr = end + 1;
			} while (*dstr != '\0');

			free(dliststr);
			dliststr = dyn_str_take(selected);
		}

		append_string(s, "    %s %u/%u disjuncts", dn->string, dnum1, dnum0);
		if (tnum != 0) append_string(s, " (%u tunnels)", tnum);
		dyn_strcat(s, "\n");
		dyn_strcat(s, dliststr);
		dyn_strcat(s, "\n");
		free(dliststr);

		if (rn != NULL)
		{
			if (dnum_selected == dnum1)
				dyn_strcat(s, "(all the disjuncts matched)\n\n");
			else
				append_string(s, "(%u disjuncts matched)\n\n", dnum_selected);
		}
	}
	pool_delete(Exp_pool);
	sentence_delete(dummy_sent);

	return dyn_str_take(s);
}

const char do_display_expr; /* a sentinel to request an expression display */

static size_t unknown_flag(const char *display_type, const char *flags)
{
	const char *known_flags;

	if (&do_display_expr == display_type)
		known_flags = "lm";
	else
		known_flags = "";

	return strspn(flags, known_flags);
}

/**
 * Display the information about the given word.
 * If the word can split, display the information about each part.
 * Note that the splits may be invalid grammatically.
 *
 * Wild-card search is supported; the command-line user can type in !!word* or
 * !!word*.sub and get a list of all words that match up to the wild-card.
 * In this case no split is done.
 */
static char *display_word_split(Dictionary dict,
               const char * word, Parse_Options opts,
               char * (*display)(Dictionary, const char *, const void **),
               const char **arg)
{
	Sentence sent;

	if ('\0' == *word) return NULL; /* avoid trying null strings */

	/* SUBSCRIPT_DOT in a sentence word is not interpreted as SUBSCRIPT_MARK,
	 * and hence a subscripted word that is found in the dict will not
	 * get found in the dict if it can split. E.g: 's.v (the info for s.v
	 * will not be shown). Fix it by replacing it to SUBSCRIPT_MARK. */
	char *pword = strdupa(word);
	patch_subscript(pword);

	dyn_str *s = dyn_str_new();

	int spell_option = parse_options_get_spell_guess(opts);
	parse_options_set_spell_guess(opts, 0);
	sent = sentence_create(pword, dict);

	if (pword[0] == '<' && pword[strlen(pword)-1] == '>')
	{
		/* Dictionary macro - don't split. */
		if (!word0_set(sent, pword, opts)) goto display_word_split_error;
	}
	else
	{
		if (0 != sentence_split(sent, opts)) goto display_word_split_error;
	}

	/* List the splits */
	print_sentence_word_alternatives(s, sent, false, NULL, NULL, NULL);
	/* List the expression / disjunct information */

	/* Initialize the callback arguments */
	const void *carg[3] = { /*regex*/NULL, /*flags*/NULL, opts };

	Regex_node *rn = NULL;
	if (arg != NULL)
	{
		if (NULL != arg[1])
		{
			size_t unknown_flag_pos = unknown_flag(arg[0], arg[1]);
			if (arg[1][unknown_flag_pos] != '\0')
			{
				prt_error("Error: Token display: Unknown flag \"%c\".\n",
				              arg[1][unknown_flag_pos]);
				dyn_strcat(s, " "); /* avoid a no-match error */
				goto display_word_split_error;
			}
		}

		carg[1] = arg[1]; /* flags */

		if (arg[0] == &do_display_expr)
		{
			carg[0] = &do_display_expr;
		}
		else if (arg[0] != NULL)
		{
			/* A regex is specified, which means displaying disjuncts. */
			if (arg[0][0] != '\0')
			{
				rn = malloc(sizeof(Regex_node));
				rn->name = strdup("Disjunct regex");
				rn->re = NULL;
				rn->neg = false;
				rn->next = NULL;

				if (arg[0][strspn(arg[0], "0123456789")] != '\0')
				{
					rn->pattern = strdup(arg[0]);
				}
				else
				{
					rn->pattern = malloc(strlen(arg[0]) + 4); /* \ [ ] \0 */
					strcpy(rn->pattern, "\\[");
					strcat(rn->pattern, arg[0]);
					strcat(rn->pattern, "]");
				}

				if (compile_regexs(rn, NULL) != 0)
				{
					prt_error("Error: Failed to compile regex \"%s\".\n", arg[0]);
					return strdup(""); /* not NULL (NULL means no dict entry) */
				}

				carg[0] = rn;
			}
		}
	}
	print_sentence_word_alternatives(s, sent, false, display, carg, NULL);
	if (rn != NULL) free_regexs(rn);

display_word_split_error:
	sentence_delete(sent);
	parse_options_set_spell_guess(opts, spell_option);

	char *out = dyn_str_take(s);
	if ('\0' != out[0]) return out;
	free(out);
	return NULL; /* no dict entry */
}

/**
 * Count the number of clauses (disjuncts) for the expression e.
 * Should return the number of disjuncts that would be returned
 * by build_disjunct().  This in turn should be equal to the number
 * of clauses built by build_clause().
 *
 * Only one minor cheat here: we are ignoring the cost_cutoff, so
 * this potentially over-counts if the cost_cutoff is set low.
 */
static unsigned int count_clause(Exp *e)
{
	unsigned int cnt = 0;

	assert(e != NULL, "count_clause called with null parameter");
	if (e->type == AND_type)
	{
		/* multiplicative combinatorial explosion */
		cnt = 1;
		for (Exp *opd = e->operand_first; opd != NULL; opd = opd->operand_next)
			cnt *= count_clause(opd);
	}
	else if (e->type == OR_type)
	{
		/* Just additive */
		for (Exp *opd = e->operand_first; opd != NULL; opd = opd->operand_next)
			cnt += count_clause(opd);
	}
	else if (e->type == CONNECTOR_type)
	{
		return 1;
	}
	else
	{
		assert(false, "Unknown expression type %d", (int)e->type);
	}

	return cnt;
}

/**
 * Count number of disjuncts given the dict node dn.
 */
static unsigned int count_disjunct_for_dict_node(Dict_node *dn)
{
	return (NULL == dn) ? 0 : count_clause(dn->exp);
}

#define DJ_COL_WIDTH sizeof("                         ")

/**
 * Display the number of disjuncts associated with this dict node
 */
static char *display_counts(const char *word, Dict_node *dn)
{
	dyn_str *s = dyn_str_new();

	dyn_strcat(s, "matches:\n");
	for (; dn != NULL; dn = dn->right)
	{
		append_string(s, "    %-*s %8u  disjuncts",
		              display_width(DJ_COL_WIDTH, dn->string), dn->string,
		              count_disjunct_for_dict_node(dn));

		if (dn->file != NULL)
		{
			append_string(s, " <%s>", dn->file->file);
		}
		dyn_strcat(s, "\n\n");
	}
	return dyn_str_take(s);
}

/**
 * Display the expressions associated with this dict node.
 */
static char *display_expr(Dictionary dict, const char *word, Dict_node *dn,
								  const void **arg)
{
	const char *flags = arg[1];
	const Parse_Options opts = (Parse_Options)arg[2];
	bool show_macros = ((flags != NULL) && (strchr(flags, 'm') != NULL));
	bool low_level = ((flags != NULL) && (strchr(flags, 'l') != NULL));

	/* copy_Exp() needs an Exp memory pool. */
	Pool_desc *Exp_pool = pool_new(__func__, "Exp", /*num_elements*/256,
	                               sizeof(Exp), /*zero_out*/false,
	                               /*align*/false, /*exact*/false);

	dyn_str *s = dyn_str_new();
	dyn_strcat(s, "expressions:\n");
	for (; dn != NULL; dn = dn->right)
	{
		Exp *e = copy_Exp(dn->exp, Exp_pool, opts); /* assign dialect costs */
		pool_reuse(Exp_pool);

		if (low_level)
		{
			append_string(s, "    %s\n", dn->string);
			prt_exp_all(s, e, 0, dict);
			dyn_strcat(s, "\n\n");
		}

		const char *expstr = lg_exp_stringify_with_tags(dict, e, show_macros);

		append_string(s, "    %-*s %s",
		              display_width(DJ_COL_WIDTH, dn->string), dn->string,
		              expstr);
		dyn_strcat(s, "\n\n");
	}

	if (Exp_pool != NULL) pool_delete(Exp_pool);
	return dyn_str_take(s);
}

/**
 * A callback function to display \p word number of disjuncts and file name.
 *
 * @arg Callback args (unused).
 * @return String to display. Must be freed by the caller.
 */
static char *display_word_info(Dictionary dict, const char *word,
                               const void **arg)
{
	const char * regex_name;
	Dict_node *dn_head;

	dn_head = dictionary_lookup_wild(dict, word);
	if (dn_head)
	{
		char *out = display_counts(word, dn_head);
		free_lookup_list(dict, dn_head);
		return out;
	}

	/* Recurse, if it's a regex match */
	regex_name = match_regex(dict->regex_root, word);
	if (regex_name)
	{
		return display_word_info(dict, regex_name, arg);
	}

	return NULL;
}

/**
 * A callback function to display \p word expressions or disjuncts.
 * @param arg Callback data as follows:
 *    arg[0]: &do_display_expr or disjunct selection regex.
 *    arg[1]: flags
 *    argv[2]: Parse_Options
 * @return String to display. Must be freed by the caller.
 */
static char *display_word_expr(Dictionary dict, const char *word,
                               const void **arg)
{
	const char * regex_name;
	Dict_node *dn_head;
	char *out = NULL;

	dn_head = dictionary_lookup_wild(dict, word);
	if (dn_head)
	{
		if (arg[0] == &do_display_expr)
		{
			out = display_expr(dict, word, dn_head, arg);
		}
		else
		{
			out = display_disjuncts(dict, dn_head, arg);
		}
		free_lookup_list(dict, dn_head);
		return out;
	}

	/* Recurse, if it's a regex match */
	regex_name = match_regex(dict->regex_root, word);
	if (regex_name)
	{
		return display_word_expr(dict, regex_name, arg);
	}

	return NULL;
}

/**
 * Break "word", "word/flags" or "word/regex/flags" into components.
 * "regex" and "flags" are optional.  "word/" means an empty regex.
 * \p re and \p flags can be both NULL.
 * @param re[out] the regex component, unless \c NULL.
 * @param flags[out] the flags component, unless \c NULL.
 * @return The word component.
 */
static const char *display_word_extract(char *word, const char **re,
                                        const char **flags)
{
	if (re != NULL) *re = NULL;
	if (flags != NULL) *flags = NULL;

	char *r = strchr(word, '/');
	if (r == NULL) return word;
	*r = '\0';

	if (re != NULL)
	{
		char *f = strchr(r + 1, '/');
		if (f != NULL)
		{
			*re = r + 1;
			*f = '\0';
			*flags = f + 1;  /* disjunct display flags */
		}
		else
		{
			*flags = r + 1;  /* expression display flags */
		}
	}
	return word;
}

/**
 *  dict_display_word_info() - display the information about the given word.
 */
char *dict_display_word_info(Dictionary dict, const char *word,
		Parse_Options opts)
{
	char *wordbuf = strdupa(word);
	word = display_word_extract(wordbuf, NULL, NULL);

	return display_word_split(dict, word, opts, display_word_info, NULL);
}

/**
 *  dict_display_word_expr() - display the connector info for a given word.
 */
char *dict_display_word_expr(Dictionary dict, const char * word, Parse_Options opts)
{
	const char *arg[2];
	char *wordbuf = strdupa(word);
	word = display_word_extract(wordbuf, &arg[0], &arg[1]);

	/* If no regex component, then it's a request to display expressions. */
	if (arg[0] == NULL) arg[0] = &do_display_expr;

	return display_word_split(dict, word, opts, display_word_expr, arg);
}
