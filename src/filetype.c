/* vifm
 * Copyright (C) 2001 Ken Steen.
 * Copyright (C) 2011 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "filetype.h"

#include <ctype.h> /* isspace() */
#include <stddef.h> /* NULL */
#include <stdlib.h> /* free() realloc() */
#include <string.h> /* strchr() strdup() strcasecmp() */

#include "menus/menus.h"
#include "utils/fs_limits.h"
#include "utils/str.h"
#include "utils/utils.h"
#include "globals.h"

const assoc_record_t NONE_PSEUDO_PROG =
{
	.command = "",
	.description = "",
};

/* Internal list that stores only currently active associations.
 * Since it holds only copies of structures from filetype and filextype lists,
 * it doesn't consume much memory, and its items shouldn't be freed */
static assoc_list_t active_filetypes;

/* Used to set type of new association records */
static assoc_record_type_t new_records_type = ART_CUSTOM;

/* Pointer to external command existence check function. */
static external_command_exists_t external_command_exists_func;

static void assoc_programs(const char pattern[],
		const assoc_records_t *programs, int for_x, int in_x);
static assoc_records_t parse_command_list(const char cmds[], int with_descr);
TSTATIC void replace_double_comma(char cmd[], int put_null);
static void register_assoc(assoc_t assoc, int for_x, int in_x);
static void add_assoc(assoc_list_t *assoc_list, assoc_t assoc);
static void assoc_viewers(const char pattern[], const assoc_records_t *viewers);
static assoc_records_t clone_assoc_records(const assoc_records_t *records);
static void reset_all_list(void);
static void add_defaults(int in_x);
static void reset_list(assoc_list_t *assoc_list);
static void reset_list_head(assoc_list_t *assoc_list);
static void free_assoc(assoc_t *assoc);
static void safe_free(char **adr);

void
config_filetypes(external_command_exists_t ece_func)
{
	external_command_exists_func = ece_func;
}

int
get_default_program_for_file(const char file[], assoc_record_t *result)
{
	int j;
	assoc_records_t records;
	assoc_record_t prog;

	j = 0;
	records = get_all_programs_for_file(file);
	while(j < records.count)
	{
		char name_buf[NAME_MAX];
		(void)extract_cmd_name(records.list[j].command, 0, sizeof(name_buf),
				name_buf);
		if(external_command_exists_func == NULL ||
				external_command_exists_func(name_buf))
			break;
		j++;
	}
	if(j >= records.count)
	{
		free(records.list);
		return 0;
	}

	prog = records.list[j];
	result->command = strdup(prog.command);
	result->description = strdup(prog.description);
	free(records.list);

	if(result->command == NULL || result->description == NULL)
	{
		free_assoc_record(result);
		return 0;
	}

	return 1;
}

const char *
get_viewer_for_file(const char file[])
{
	int i;

	for(i = 0; i < fileviewers.count; ++i)
	{
		int j;
		assoc_records_t records;

		if(!global_matches(fileviewers.list[i].pattern, file))
		{
			continue;
		}

		records = fileviewers.list[i].records;

		for(j = 0; j < records.count; ++j)
		{
			char name_buf[NAME_MAX];
			(void)extract_cmd_name(records.list[j].command, 0, sizeof(name_buf),
					name_buf);

			if(external_command_exists_func == NULL ||
					external_command_exists_func(name_buf))
			{
				return records.list[j].command;
			}
		}
	}

	return NULL;
}

assoc_records_t
get_all_programs_for_file(const char file[])
{
	int i;
	assoc_records_t result = {};

	for(i = 0; i < active_filetypes.count; i++)
	{
		assoc_records_t progs;
		int j;

		if(!global_matches(active_filetypes.list[i].pattern, file))
		{
			continue;
		}

		progs = active_filetypes.list[i].records;
		for(j = 0; j < progs.count; j++)
		{
			assoc_record_t prog = progs.list[j];
			add_assoc_record(&result, prog.command, prog.description);
		}
	}

	return result;
}

void
set_programs(const char patterns[], const char programs[], int for_x, int in_x)
{
	assoc_records_t prog_records = parse_command_list(programs, 1);

	char *pattern = strdup(patterns), *state = NULL;
	while((pattern = split_and_get(pattern, ',', &state)) != NULL)
	{
		assoc_programs(pattern, &prog_records, for_x, in_x);
	}
	free(pattern);

	free_assoc_records(&prog_records);
}

/* Associates pattern with list of comma separated programs either for X or
 * non-X associations and depending on current execution environment. */
static void
assoc_programs(const char pattern[], const assoc_records_t *programs, int for_x,
		int in_x)
{
	const assoc_t assoc =
	{
		.pattern = strdup(pattern),
		.records = clone_assoc_records(programs),
	};

	register_assoc(assoc, for_x, in_x);
}

/* Parses comma separated list of commands into array of associations.  Returns
 * the list. */
static assoc_records_t
parse_command_list(const char cmds[], int with_descr)
{
	assoc_records_t records = {};

	char *cmd;
	char *free_this;

	cmd = strdup(cmds);
	free_this = cmd;

	while(cmd != NULL)
	{
		char *ptr;
		const char *description = "";

		if((ptr = strchr(cmd, ',')) != NULL)
		{
			while(ptr != NULL && ptr[1] == ',')
			{
				ptr = strchr(ptr + 2, ',');
			}
			if(ptr != NULL)
			{
				*ptr = '\0';
				++ptr;
			}
		}

		while(isspace(*cmd) || *cmd == ',')
		{
			++cmd;
		}

		if(with_descr && *cmd == '{')
		{
			char *p = strchr(cmd + 1, '}');
			if(p != NULL)
			{
				*p = '\0';
				description = cmd + 1;
				cmd = skip_whitespace(p + 1);
			}
		}

		if(cmd[0] != '\0')
		{
			replace_double_comma(cmd, 0);
			add_assoc_record(&records, cmd, description);
		}
		cmd = ptr;
	}

	free(free_this);

	return records;
}

TSTATIC void
replace_double_comma(char cmd[], int put_null)
{
	char *p = cmd;
	while(*cmd != '\0')
	{
		if(cmd[0] == ',')
		{
			if(cmd[1] == ',')
			{
				*p++ = *cmd++;
				cmd++;
				continue;
			}
			else if(put_null)
			{
				break;
			}
		}
		*p++ = *cmd++;
	}
	*p = '\0';
}

/* Registers association in appropriate associations list and possibly in list
 * of active associations, which depends on association type and execution
 * environment. */
static void
register_assoc(assoc_t assoc, int for_x, int in_x)
{
	add_assoc(for_x ? &xfiletypes : &filetypes, assoc);
	if(!for_x || in_x)
	{
		add_assoc(&active_filetypes, assoc);
	}
}

void
set_fileviewer(const char patterns[], const char viewers[])
{
	assoc_records_t view_records = parse_command_list(viewers, 1);

	char *exptr;
	char *ex_copy = strdup(patterns);
	char *free_this = ex_copy;
	while((exptr = strchr(ex_copy, ',')) != NULL)
	{
		*exptr = '\0';

		assoc_viewers(ex_copy, &view_records);

		ex_copy = exptr + 1;
	}
	assoc_viewers(ex_copy, &view_records);
	free(free_this);

	free_assoc_records(&view_records);
}

static void
assoc_viewers(const char pattern[], const assoc_records_t *viewers)
{
	const assoc_t assoc =
	{
		.pattern = strdup(pattern),
		.records = clone_assoc_records(viewers),
	};

	add_assoc(&fileviewers, assoc);
}

/* Clones list of association records.  Returns the clone. */
static assoc_records_t
clone_assoc_records(const assoc_records_t *records)
{
	int i;
	assoc_records_t clone = {};

	for(i = 0; i < records->count; i++)
	{
		const assoc_record_t *const record = &records->list[i];
		add_assoc_record(&clone, record->command, record->description);
	}

	return clone;
}

static void
add_assoc(assoc_list_t *assoc_list, assoc_t assoc)
{
	void *p = realloc(assoc_list->list, (assoc_list->count + 1)*sizeof(assoc_t));
	if(p == NULL)
	{
		show_error_msg("Memory Error", "Unable to allocate enough memory");
		return;
	}

	assoc_list->list = p;
	assoc_list->list[assoc_list->count] = assoc;
	assoc_list->count++;
}

void
reset_all_file_associations(int in_x)
{
	reset_all_list();
	add_defaults(in_x);
}

static void
reset_all_list(void)
{
	reset_list(&filetypes);
	reset_list(&xfiletypes);
	reset_list(&fileviewers);

	reset_list_head(&active_filetypes);
}

/* Loads default (builtin) associations. */
static void
add_defaults(int in_x)
{
	new_records_type = ART_BUILTIN;
	set_programs("*/", "{Enter directory}" VIFM_PSEUDO_CMD, 0, in_x);
	new_records_type = ART_CUSTOM;
}

static void
reset_list(assoc_list_t *assoc_list)
{
	int i;
	for(i = 0; i < assoc_list->count; i++)
	{
		free_assoc(&assoc_list->list[i]);
	}
	reset_list_head(assoc_list);
}

static void
reset_list_head(assoc_list_t *assoc_list)
{
	free(assoc_list->list);
	assoc_list->list = NULL;
	assoc_list->count = 0;
}

static void
free_assoc(assoc_t *assoc)
{
	safe_free(&assoc->pattern);
	free_assoc_records(&assoc->records);
}

void
free_assoc_records(assoc_records_t *records)
{
	int i;
	for(i = 0; i < records->count; i++)
	{
		free_assoc_record(&records->list[i]);
	}

	free(records->list);
	records->list = NULL;
	records->count = 0;
}

void
free_assoc_record(assoc_record_t *record)
{
	safe_free(&record->command);
	safe_free(&record->description);
}

void
add_assoc_record(assoc_records_t *records, const char *command,
		const char *description)
{
	void *p = realloc(records->list, sizeof(assoc_record_t)*(records->count + 1));
	if(p == NULL)
	{
		show_error_msg("Memory Error", "Unable to allocate enough memory");
		return;
	}

	records->list = p;
	records->list[records->count].command = strdup(command);
	records->list[records->count].description = strdup(description);
	records->list[records->count].type = new_records_type;
	records->count++;
}

void
add_assoc_records(assoc_records_t *assocs, const assoc_records_t *src)
{
	int i;
	void *p;
	const int src_count = src->count;

	if(src_count == 0)
	{
		return;
	}

	p = realloc(assocs->list, sizeof(assoc_record_t)*(assocs->count + src_count));
	if(p == NULL)
	{
		show_error_msg("Memory Error", "Unable to allocate enough memory");
		return;
	}

	assocs->list = p;

	for(i = 0; i < src_count; i++)
	{
		assocs->list[assocs->count + i].command = strdup(src->list[i].command);
		assocs->list[assocs->count + i].description =
				strdup(src->list[i].description);
		assocs->list[assocs->count + i].type = src->list[i].type;
	}

	assocs->count += src_count;
}

static void
safe_free(char **adr)
{
	free(*adr);
	*adr = NULL;
}

int
assoc_prog_is_empty(const assoc_record_t *record)
{
	return record->command == NULL && record->description == NULL;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 : */
