/*************************************************************************/
/* Copyright (C) 2019 Amir Plivatsky                                     */
/* All rights reserved                                                   */
/*                                                                       */
/* Use of the link grammar parsing system is subject to the terms of the */
/* license set forth in the LICENSE file included with this software.    */
/* This license allows free redistribution and use in source and binary  */
/* forms, with or without modification, subject to certain conditions.   */
/*                                                                       */
/*************************************************************************/

#include <string.h>

#include "api-structures.h"
#include "dialect.h"
#include "dict-api.h"                   // cost_stringify
#include "dict-common.h"
#include "dict-structures.h"
#include "dict-file/read-dialect.h"     // dialect_read_from_one_line_str
#include "file-utils.h"
#include "string-id.h"
#include "string-set.h"

#define D_DIALECT 7 /* Verbosity level for this file (also 8). */

static void free_dialect_table(Dialect *di)
{
	free(di->table);
	free(di->kept_input);
}

void free_dialect(Dialect *di)
{
	if (di == NULL) return;
	free_dialect_table(di);
	free(di->section);
	string_id_delete(di->section_set);
	free(di);
}

Dialect *dialect_alloc(void)
{
	Dialect *di = malloc(sizeof(*di));
	memset(di, 0, sizeof(*di));
	di->section_set = string_id_create();
	return di;
}

Exptag *exptag_add(Dictionary dict, const char *tag)
{
	expression_tag *et = &dict->tag;
	unsigned int tag_index = string_id_lookup(tag, et->set);

	if (tag_index != SI_NOTFOUND) return &et->array[tag_index];
	tag_index = string_id_add(tag, et->set);
	tag = string_set_add(tag, dict->string_set); /* FIXME: Refer to string-id */

	if (et->num == et->size)
	{
		if (et->num == 0)
			et->size = EXPTAG_SZ;
		else
			et->size *= 2;

		et->array = realloc(et->array, et->size * sizeof(*et->array));
	}
	et->array[tag_index] = (Exptag){ .name = tag, .index = tag_index };
	et->num++;
	assert(et->num == tag_index);

	return &et->array[tag_index];
}

/**
 * Set in the cost table the dialect component cost at \p table_index.
 * @retrun \c false iff the component doesn't exist in the dictionary.
 */
static bool apply_component(Dictionary dict, Dialect *di,
                            unsigned int table_index, float *cost_table)
{
	expression_tag *et = &dict->tag;
	unsigned int cost_index =
		string_id_lookup(di->table[table_index].name, et->set);

	if (cost_index == SI_NOTFOUND)
	{
		prt_error("Error: Dialect component \"%s\" is not in the dictionary.\n",
			di->table[table_index].name);
		return false;
	}
	cost_table[cost_index] = di->table[table_index].cost;

	return true;
}

/**
 * Apply the dialect settings at \p from to the dialect table index
 * \p table_index at \p to.
 * The dialect table at \p from has been created from the "4.0.dialect"
 * file or from the user setting kept in the dialect_info option.
 * The dialect table at \p to is always from "4.0.dialect".
 * Recursive (for applying sub-dialects).
 * @retrun \c true on success, \c false on failure.
 */
static bool apply_table_entry(Dictionary dict, Dialect *from,
                              unsigned int table_index, Dialect *to,
                              dialect_info *dinfo, bool *encountered)
{
	int skip = (int)(to == from); /* Skip an initial section header. */

	/* Apply everything until the next section (or "from" table end). */
	for (unsigned int i = table_index + skip; i < from->num_table_tags; i++)
	{
		if (cost_eq(from->table[i].cost, DIALECT_SECTION)) break;

		/* Apply components and sub-dialects. */
		lgdebug(+D_DIALECT, "Apply %s %s%s\n",
		        from->table[i].name, cost_stringify(from->table[i].cost),
		        (to == from) ? "" : " (user setup");
		if (!cost_eq(from->table[i].cost, DIALECT_SUB))
		{
			if (!apply_component(dict, from, i, dinfo->cost_table)) return false;
		}
		else
		{
			unsigned int sub_index = SI_NOTFOUND;
			if (to != NULL)
				sub_index = string_id_lookup(from->table[i].name, to->section_set);
			if (sub_index == SI_NOTFOUND)
			{
				prt_error("Error: Undefined dialect \"%s\"\n", from->table[i].name);
				return false;
			}
			if (encountered[sub_index])
			{
				prt_error("Error: Loop detected at sub-dialect \"%s\" "
				          "(of dialect \"%s\").\n",
				          to->table[i].name, to->table[table_index].name);
				return false;
			}
			encountered[sub_index] = true;
			if (!apply_table_entry(dict, to, to->section[sub_index].index, to,
			                       dinfo, encountered))
				return false;
		}
	}

	return true;
}

bool apply_dialect(Dictionary dict, Dialect *from, unsigned int table_index,
                   Dialect *to, dialect_info *dinfo)
{
	/* Loop detection (needed only if there are "4.0.dialect" definitions). */
	bool *loopdet;
	if (to == NULL)
	{
		loopdet = NULL;
	}
	else
	{
		loopdet = alloca(to->num_sections + 1);
		memset(loopdet, 0, to->num_sections + 1);
	}

	if (!apply_table_entry(dict, from, table_index, to, dinfo, loopdet))
		return false;

	return true;
}

static void print_cost_table(Dictionary dict, Dialect *di, dialect_info *dinfo)
{
	expression_tag *et = &dict->tag;

	if (et->num == 0)
	{
		assert(dinfo->cost_table == NULL, "Unexpected cost table.");
		prt_error("Debug: No dialect cost table (no tags in the dict).\n");
		return;
	}

	if (dinfo->cost_table == NULL)
	{
		prt_error("Debug: No dialect cost table.\n");
		return;
	}

	prt_error("Dialect cost table (%u components%s):\n\\",
	          et->num, et->num == 1 ? "" : "s");
	prt_error("%-15s %s\n", "component", "cost");

	for (unsigned int i = 1; i  <= et->num; i++)
	{
		prt_error("%-15s %s\n\\",
		          et->array[i].name, cost_stringify(dinfo->cost_table[i]));
	}
	lg_error_flush();
}

void free_cost_table(Parse_Options opts)
{
	free(opts->dialect.cost_table);
	opts->dialect.cost_table = NULL;
}

static bool dialect_conf_exists(dialect_info *dinfo)
{
	for (const char *p = dinfo->conf; *p != '\0'; p++)
		if (!lg_isspace(*p)) return true;
	return false;
}

const char no_dialect[] = "(unset the dialect option)\n";

/**
 * Build the dialect cost table if it doesn't exist.
 * Note: All IDs start from 1, so the 0'th elements are ignored
 * (but section[0] which points to the default section).
 * @retrun \c true on success, \c false on failure.
 */
bool setup_dialect(Dictionary dict, Parse_Options opts)
{
	Dialect *di = dict->dialect;
	dialect_info *dinfo = &opts->dialect;
	expression_tag *et = &dict->tag;

	if (et->num == 0)
	{
		if (!dialect_conf_exists(dinfo)) return true;
		prt_error("Error: Dialect setup failed: No dialects in the \"%s\" "
					 "dictionary %s.\n", dict->lang, no_dialect);
		return false;
	}

	if (dinfo->cost_table != NULL)
	{
		/* Cached table. */
		lgdebug(D_DIALECT, "Debug: Cached cost table found\n");

		if (verbosity_level(+D_DIALECT+1))
			print_cost_table(dict, di, dinfo);

		return true;
	}

	if (et->num != 0)
	{
		dinfo->cost_table = malloc((et->num + 1) * sizeof(*dinfo->cost_table));

		for (unsigned int i = 1; i <= et->num; i++)
			dinfo->cost_table[i] = DIALECT_COST_DISABLE;
	}

	/* Apply the default section of the dialect table, if exists. */
	if ((di != NULL) && (di->section != NULL) &&
	    (di->section[0].index != NO_INDEX))
	{
		if (!apply_dialect(dict, di, di->section[0].index, di, dinfo))
			return false;
	}

	if (dialect_conf_exists(dinfo))
	{
		Dialect user_setup = (Dialect){ 0 };
		if (!dialect_read_from_one_line_str(dict, &user_setup, dinfo->conf))
		{
			free_dialect_table(&user_setup);
			return false;
		}

		if (!apply_dialect(dict, &user_setup, /*table index*/0, di, dinfo))
		{
			free_dialect_table(&user_setup);
			return false;
		}
		free_dialect_table(&user_setup);
	}

	if (verbosity_level(+D_DIALECT+1))
		print_cost_table(dict, di, dinfo);

	return true;
}
