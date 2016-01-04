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

#include "cmd_core.h"

#include <curses.h>

#include <sys/stat.h> /* gid_t uid_t */
#include <unistd.h> /* unlink() */

#include <assert.h> /* assert() */
#include <ctype.h> /* isspace() */
#include <errno.h> /* errno */
#include <signal.h>
#include <stddef.h> /* NULL size_t */
#include <stdio.h> /* snprintf() */
#include <stdlib.h> /* free() */
#include <string.h> /* strcmp() strcpy() strlen() */

#include "cfg/config.h"
#include "cfg/hist.h"
#include "compat/fs_limits.h"
#include "compat/os.h"
#include "engine/autocmds.h"
#include "engine/cmds.h"
#include "engine/mode.h"
#include "engine/parsing.h"
#include "engine/var.h"
#include "engine/variables.h"
#include "int/vim.h"
#include "modes/dialogs/msg_dialog.h"
#include "modes/modes.h"
#include "modes/normal.h"
#include "modes/view.h"
#include "modes/visual.h"
#include "ui/color_manager.h"
#include "ui/color_scheme.h"
#include "ui/colors.h"
#include "ui/fileview.h"
#include "ui/statusbar.h"
#include "ui/ui.h"
#include "utils/file_streams.h"
#include "utils/filter.h"
#include "utils/fs.h"
#include "utils/int_stack.h"
#include "utils/path.h"
#include "utils/str.h"
#include "utils/string_array.h"
#include "utils/test_helpers.h"
#include "utils/utils.h"
#include "bracket_notation.h"
#include "cmd_completion.h"
#include "cmd_handlers.h"
#include "filelist.h"
#include "filtering.h"
#include "macros.h"
#include "marks.h"
#include "opt_handlers.h"
#include "undo.h"

/* Type of command arguments. */
typedef enum
{
	CAT_REGULAR,       /* Can be separated by a |. */
	CAT_EXPR,          /* Accept expressions with || and terminate on |. */
	CAT_UNTIL_THE_END, /* Take the rest of line including all |. */
}
CmdArgsType;

/* Values for if_levels stack. */
typedef enum
{
	IF_SCOPE_GUARD,  /* Command scope marker, prevents mixing of levels. */
	IF_BEFORE_MATCH, /* Before condition that evaluates to true is found. */
	IF_MATCH,        /* Just found true condition and processing that branch. */
	IF_AFTER_MATCH,  /* Left branch corresponding to true condition. */
	IF_ELSE,         /* Else branch that should be run (no matches before it). */
	IF_FINISH,       /* After else branch, only endif is expected by now. */
}
IfFrame;

static int swap_range(void);
static int resolve_mark(char mark);
static char * cmds_expand_macros(const char str[], int for_shell, int *usr1,
		int *usr2);
static int setup_extcmd_file(const char path[], const char beginning[],
		CmdInputType type);
static void prepare_extcmd_file(FILE *fp, const char beginning[],
		CmdInputType type);
static hist_t * history_by_type(CmdInputType type);
static char * get_file_first_line(const char path[]);
static void execute_extcmd(const char command[], CmdInputType type);
static void save_extcmd(const char command[], CmdInputType type);
static void post(int id);
TSTATIC void select_range(int id, const cmd_info_t *cmd_info);
static int skip_at_beginning(int id, const char args[]);
static char * pattern_expand_hook(const char pattern[]);
static int cmd_should_be_processed(int cmd_id);
TSTATIC char ** break_cmdline(const char cmdline[], int for_menu);
static int is_out_of_arg(const char cmd[], const char pos[]);
TSTATIC int line_pos(const char begin[], const char end[], char sep,
		int rquoting);
static CmdArgsType get_cmd_args_type(const char cmd[]);
static char * skip_to_cmd_name(const char cmd[]);
static int repeat_command(FileView *view, CmdInputType type);
static int is_at_scope_bottom(const int_stack_t *scope_stack);
TSTATIC char * eval_arglist(const char args[], const char **stop_ptr);

/* Settings for the cmds unit. */
static cmds_conf_t cmds_conf = {
	.complete_args = &complete_args,
	.swap_range = &swap_range,
	.resolve_mark = &resolve_mark,
	.expand_macros = &cmds_expand_macros,
	.expand_envvars = &cmds_expand_envvars,
	.post = &post,
	.select_range = &select_range,
	.skip_at_beginning = &skip_at_beginning,
};

/* Shows whether view selection should be preserved on command-line finishing.
 * By default it's reset. */
static int keep_view_selection;
/* Stores condition evaluation result for all nesting if-endif statements as
 * well as file scope marks (SCOPE_GUARD). */
static int_stack_t if_levels;

static int
swap_range(void)
{
	return prompt_msg("Command Error", "Backwards range given, OK to swap?");
}

static int
resolve_mark(char mark)
{
	int result;

	result = check_mark_directory(curr_view, mark);
	if(result < 0)
		status_bar_errorf("Trying to use an invalid mark: '%c", mark);
	return result;
}

/* Implementation of macros expansion callback for cmds unit.  Returns newly
 * allocated memory. */
static char *
cmds_expand_macros(const char str[], int for_shell, int *usr1, int *usr2)
{
	char *result;
	MacroFlags flags = MF_NONE;

	result = expand_macros(str, NULL, &flags, for_shell);

	*usr1 = flags;

	return result;
}

char *
cmds_expand_envvars(const char str[])
{
	return expand_envvars(str, 1);
}

void
get_and_execute_command(const char line[], size_t line_pos, CmdInputType type)
{
	char *const cmd = get_ext_command(line, line_pos, type);
	if(cmd == NULL)
	{
		save_extcmd(line, type);
	}
	else
	{
		save_extcmd(cmd, type);
		execute_extcmd(cmd, type);
		free(cmd);
	}
}

char *
get_ext_command(const char beginning[], size_t line_pos, CmdInputType type)
{
	char cmd_file[PATH_MAX];
	char *cmd = NULL;

	generate_tmp_file_name("vifm.cmdline", cmd_file, sizeof(cmd_file));

	if(setup_extcmd_file(cmd_file, beginning, type) == 0)
	{
		if(vim_view_file(cmd_file, 1, line_pos, 0) == 0)
		{
			cmd = get_file_first_line(cmd_file);
		}
	}
	else
	{
		show_error_msgf("Error Creating Temporary File",
				"Could not create file %s: %s", cmd_file, strerror(errno));
	}

	unlink(cmd_file);
	return cmd;
}

/* Create and fill file for external command prompt.  Returns zero on success,
 * otherwise non-zero is returned and errno contains valid value. */
static int
setup_extcmd_file(const char path[], const char beginning[], CmdInputType type)
{
	FILE *const fp = os_fopen(path, "wt");
	if(fp == NULL)
	{
		return 1;
	}
	prepare_extcmd_file(fp, beginning, type);
	fclose(fp);
	return 0;
}

/* Fills the file with history (more recent goes first). */
static void
prepare_extcmd_file(FILE *fp, const char beginning[], CmdInputType type)
{
	const int is_cmd = (type == CIT_COMMAND);
	const hist_t *const hist = history_by_type(type);
	int i;

	fprintf(fp, "%s\n", beginning);
	for(i = 0; i <= hist->pos; i++)
	{
		fprintf(fp, "%s\n", hist->items[i]);
	}

	if(is_cmd)
	{
		fputs("\" vim: set filetype=vifm-cmdedit syntax=vifm :\n", fp);
	}
	else
	{
		fputs("\" vim: set filetype=vifm-edit :\n", fp);
	}
}

/* Picks history by command type.  Returns pointer to history. */
static hist_t *
history_by_type(CmdInputType type)
{
	switch(type)
	{
		case CIT_COMMAND:
			return &cfg.cmd_hist;
		case CIT_PROMPT_INPUT:
			return &cfg.prompt_hist;
		case CIT_FILTER_PATTERN:
			return &cfg.filter_hist;

		default:
			return &cfg.search_hist;
	}
}

/* Reads the first line of the file specified by the path.  Returns NULL on
 * error or an empty file, otherwise a newly allocated string, which should be
 * freed by the caller, is returned. */
static char *
get_file_first_line(const char path[])
{
	FILE *const fp = os_fopen(path, "rb");
	char *result = NULL;
	if(fp != NULL)
	{
		result = read_line(fp, NULL);
		fclose(fp);
	}
	return result;
}

/* Executes the command of the type. */
static void
execute_extcmd(const char command[], CmdInputType type)
{
	if(type == CIT_COMMAND)
	{
		curr_stats.save_msg = exec_commands(command, curr_view, type);
	}
	else
	{
		curr_stats.save_msg = exec_command(command, curr_view, type);
	}
}

/* Saves the command to the appropriate history. */
static void
save_extcmd(const char command[], CmdInputType type)
{
	if(type == CIT_COMMAND)
	{
		cfg_save_command_history(command);
	}
	else
	{
		cfg_save_search_history(command);
	}
}

int
is_history_command(const char command[])
{
	/* Don't add :!! or :! to history list. */
	return strcmp(command, "!!") != 0 && strcmp(command, "!") != 0;
}

int
command_accepts_expr(int cmd_id)
{
	return cmd_id == COM_ECHO
	    || cmd_id == COM_EXE
	    || cmd_id == COM_IF_STMT
	    || cmd_id == COM_ELSEIF_STMT
	    || cmd_id == COM_LET;
}

char *
commands_escape_for_insertion(const char cmd_line[], int pos, const char str[])
{
	const CmdLineLocation ipt = get_cmdline_location(cmd_line, cmd_line + pos);
	switch(ipt)
	{
		case CLL_R_QUOTING:
			/* XXX: Use of filename escape, while special one might be needed. */
		case CLL_OUT_OF_ARG:
		case CLL_NO_QUOTING:
			return shell_like_escape(str, 0);

		case CLL_S_QUOTING:
			return escape_for_squotes(str, 0);

		case CLL_D_QUOTING:
			return escape_for_dquotes(str, 0);

		default:
			return NULL;
	}
}

static void
post(int id)
{
	if(id != COM_GOTO && curr_view->selected_files > 0 && !keep_view_selection)
	{
		ui_view_reset_selection_and_reload(curr_view);
	}
}

TSTATIC void
select_range(int id, const cmd_info_t *cmd_info)
{
	/* TODO: refactor this function select_range() */

	int x;
	int y = 0;

	/* Both a starting range and an ending range are given. */
	if(cmd_info->begin > -1)
	{
		clean_selected_files(curr_view);

		for(x = cmd_info->begin; x <= cmd_info->end; x++)
		{
			if(is_parent_dir(curr_view->dir_entry[x].name) &&
					cmd_info->begin != cmd_info->end)
				continue;
			curr_view->dir_entry[x].selected = 1;
			y++;
		}
		curr_view->selected_files = y;
	}
	else if(curr_view->selected_files == 0)
	{
		if(cmd_info->end > -1)
		{
			clean_selected_files(curr_view);

			y = 0;
			for(x = cmd_info->end; x < curr_view->list_rows; x++)
			{
				if(y == 1)
					break;
				curr_view->dir_entry[x].selected = 1;
				y++;
			}
			curr_view->selected_files = y;
		}
		else if(id != COM_FIND && id != COM_GREP)
		{
			clean_selected_files(curr_view);

			y = 0;
			for(x = curr_view->list_pos; x < curr_view->list_rows; x++)
			{
				if(y == 1)
					break;

				curr_view->dir_entry[x].selected = 1;
				y++;
			}
			curr_view->selected_files = y;
		}
	}
	else
	{
		return;
	}

	if(curr_view->selected_files > 0)
		curr_view->user_selection = 0;
}

/* Command prefix remover for command parsing unit.  Returns < 0 to do nothing
 * or x to skip command name and x chars. */
static int
skip_at_beginning(int id, const char args[])
{
	if(id == COM_WINDO)
	{
		return 0;
	}

	if(id == COM_WINRUN)
	{
		args = vle_cmds_at_arg(args);
		if(*args != '\0')
		{
			return 1;
		}
	}
	return -1;
}

void
init_commands(void)
{
	if(cmds_conf.inner != NULL)
	{
		init_cmds(1, &cmds_conf);
		return;
	}

	/* We get here when init_commands() is called the first time. */

	init_cmds(1, &cmds_conf);
	add_builtin_commands((const cmd_add_t *)&cmds_list, cmds_list_size);

	/* Initialize modules used by this one. */
	init_bracket_notation();
	init_variables();

	vle_aucmd_set_expand_hook(&pattern_expand_hook);
}

/* Performs custom pattern expansion.  Allocate new expanded string. */
static char *
pattern_expand_hook(const char pattern[])
{
	char *const no_tilde = expand_tilde(pattern);
	char *const expanded_pattern = expand_envvars(no_tilde, 0);
	free(no_tilde);

	return expanded_pattern;
}

static void
remove_selection(FileView *view)
{
	/* TODO: maybe move this to filelist.c */
	if(view->selected_files == 0)
		return;

	clean_selected_files(view);
	redraw_view(view);
}

/* Returns negative value in case of error */
static int
execute_command(FileView *view, const char command[], int menu)
{
	int id;
	int result;
	FileView *tmp_curr, *tmp_other;

	if(command == NULL)
	{
		remove_selection(view);
		return 0;
	}

	command = skip_to_cmd_name(command);

	if(command[0] == '"')
		return 0;

	if(command[0] == '\0' && !menu)
	{
		remove_selection(view);
		return 0;
	}

	if(!menu)
	{
		init_cmds(1, &cmds_conf);
		cmds_conf.begin = 0;
		cmds_conf.current = view->list_pos;
		cmds_conf.end = view->list_rows - 1;
	}

	id = get_cmd_id(command);

	if(!cmd_should_be_processed(id))
	{
		return 0;
	}

	if(id == USER_CMD_ID)
	{
		char undo_msg[COMMAND_GROUP_INFO_LEN];

		snprintf(undo_msg, sizeof(undo_msg), "in %s: %s",
				replace_home_part(flist_get_dir(view)), command);

		cmd_group_begin(undo_msg);
		cmd_group_end();
	}

	ui_view_pick(view, &tmp_curr, &tmp_other);

	keep_view_selection = 0;
	result = execute_cmd(command);

	ui_view_unpick(view, tmp_curr, tmp_other);

	if(result >= 0)
		return result;

	switch(result)
	{
		case CMDS_ERR_LOOP:
			status_bar_error("Loop in commands");
			break;
		case CMDS_ERR_NO_MEM:
			status_bar_error("Unable to allocate enough memory");
			break;
		case CMDS_ERR_TOO_FEW_ARGS:
			status_bar_error("Too few arguments");
			break;
		case CMDS_ERR_TRAILING_CHARS:
			status_bar_error("Trailing characters");
			break;
		case CMDS_ERR_INCORRECT_NAME:
			status_bar_error("Incorrect command name");
			break;
		case CMDS_ERR_NEED_BANG:
			status_bar_error("Add bang to force");
			break;
		case CMDS_ERR_NO_BUILTIN_REDEFINE:
			status_bar_error("Can't redefine builtin command");
			break;
		case CMDS_ERR_INVALID_CMD:
			status_bar_error("Invalid command name");
			break;
		case CMDS_ERR_NO_BANG_ALLOWED:
			status_bar_error("No ! is allowed");
			break;
		case CMDS_ERR_NO_RANGE_ALLOWED:
			status_bar_error("No range is allowed");
			break;
		case CMDS_ERR_NO_QMARK_ALLOWED:
			status_bar_error("No ? is allowed");
			break;
		case CMDS_ERR_INVALID_RANGE:
			/* message dialog is enough */
			break;
		case CMDS_ERR_NO_SUCH_UDF:
			status_bar_error("No such user defined command");
			break;
		case CMDS_ERR_UDF_IS_AMBIGUOUS:
			status_bar_error("Ambiguous use of user-defined command");
			break;
		case CMDS_ERR_ZERO_COUNT:
			status_bar_error("Zero count");
			break;
		case CMDS_ERR_INVALID_ARG:
			status_bar_error("Invalid argument");
			break;
		case CMDS_ERR_CUSTOM:
			/* error message is posted by command handler */
			break;
		default:
			status_bar_error("Unknown error");
			break;
	}

	if(!menu && vle_mode_is(NORMAL_MODE))
	{
		remove_selection(view);
	}

	return -1;
}

/* Decides whether next command with id cmd_id should be processed or not,
 * taking state of conditional statements into account.  Returns non-zero if the
 * command should be processed, otherwise zero is returned. */
static int
cmd_should_be_processed(int cmd_id)
{
	static int skipped_nested_if_stmts;

	if(is_at_scope_bottom(&if_levels) || int_stack_top_is(&if_levels, IF_MATCH)
			|| int_stack_top_is(&if_levels, IF_ELSE))
	{
		return 1;
	}

	/* Get here only when in false branch of if statement. */

	if(cmd_id == COM_IF_STMT)
	{
		++skipped_nested_if_stmts;
		return 0;
	}

	if(cmd_id == COM_ELSEIF_STMT)
	{
		return (skipped_nested_if_stmts == 0);
	}

	if(cmd_id == COM_ELSE_STMT || cmd_id == COM_ENDIF_STMT)
	{
		if(skipped_nested_if_stmts > 0)
		{
			if(cmd_id == COM_ENDIF_STMT)
			{
				--skipped_nested_if_stmts;
			}
			return 0;
		}
		return 1;
	}

	return 0;
}

/* Determines current position in the command line.  Returns:
 *  - 0, if not inside an argument;
 *  - 1, if next character should be skipped (XXX: what does it mean?);
 *  - 2, if inside escaped argument;
 *  - 3, if inside single quoted argument;
 *  - 4, if inside double quoted argument;
 *  - 5, if inside regexp quoted argument. */
TSTATIC int
line_pos(const char begin[], const char end[], char sep, int rquoting)
{
	int state;
	int count;
	enum { BEGIN, NO_QUOTING, S_QUOTING, D_QUOTING, R_QUOTING };

	state = BEGIN;
	count = 0;
	while(begin != end)
	{
		switch(state)
		{
			case BEGIN:
				if(sep == ' ' && *begin == '\'')
					state = S_QUOTING;
				else if(sep == ' ' && *begin == '"')
					state = D_QUOTING;
				else if(sep == ' ' && *begin == '/' && rquoting)
					state = R_QUOTING;
				else if(*begin == '&' && begin == end - 1)
					state = BEGIN;
				else if(*begin != sep)
					state = NO_QUOTING;
				break;
			case NO_QUOTING:
				if(*begin == sep)
				{
					state = BEGIN;
					count++;
				}
				else if(*begin == '\'')
				{
					state = S_QUOTING;
				}
				else if(*begin == '"')
				{
					state = D_QUOTING;
				}
				else if(*begin == '\\')
				{
					begin++;
					if(begin == end)
						return 1;
				}
				break;
			case S_QUOTING:
				if(*begin == '\'')
					state = BEGIN;
				break;
			case D_QUOTING:
				if(*begin == '"')
				{
					state = BEGIN;
				}
				else if(*begin == '\\')
				{
					begin++;
					if(begin == end)
						return 1;
				}
				break;
			case R_QUOTING:
				if(*begin == '/')
				{
					state = BEGIN;
				}
				else if(*begin == '\\')
				{
					begin++;
					if(begin == end)
						return 1;
				}
				break;
		}
		begin++;
	}
	if(state == NO_QUOTING)
	{
		if(sep == ' ')
		{
			/* First element is not an argument. */
			return (count > 0) ? 2 : 0;
		}
		else if(count > 0 && count < 3)
		{
			return 2;
		}
	}
	else if(state != BEGIN)
	{
		/* "Error": no closing quote. */
		switch(state)
		{
			case S_QUOTING: return 3;
			case D_QUOTING: return 4;
			case R_QUOTING: return 5;

			default:
				assert(0 && "Unexpected state.");
				break;
		}
	}
	else if(sep != ' ' && count > 0 && *end != sep)
		return 2;

	return 0;
}

int
exec_commands(const char cmdline[], FileView *view, CmdInputType type)
{
	int save_msg = 0;
	char **cmds = break_cmdline(cmdline, type == CIT_MENU_COMMAND);
	char **cmd = cmds;

	while(*cmd != NULL)
	{
		const int ret = exec_command(*cmd, view, type);
		if(ret != 0)
		{
			save_msg = (ret < 0) ? -1 : 1;
		}

		free(*cmd++);
	}
	free(cmds);

	return save_msg;
}

/* Breaks command-line into sub-commands.  Returns NULL-terminated list of
 * sub-sommands. */
TSTATIC char **
break_cmdline(const char cmdline[], int for_menu)
{
	char **cmds = NULL;
	int len = 0;

	char cmdline_copy[strlen(cmdline) + 1];
	char *raw, *processed;

	CmdArgsType args_kind;

	if(*cmdline == '\0')
	{
		len = add_to_string_array(&cmds, len, 1, cmdline);
		goto finish;
	}

	strcpy(cmdline_copy, cmdline);
	cmdline = cmdline_copy;

	raw = cmdline_copy;
	processed = cmdline_copy;

	/* For non-menu commands set command-line mode configuration before calling
	 * is_out_of_arg() and get_cmd_args_type() function, which calls functions of
	 * the cmds module of the engine that use context. */
	if(!for_menu)
	{
		init_cmds(1, &cmds_conf);
	}

	cmdline = skip_to_cmd_name(cmdline);
	args_kind = get_cmd_args_type(cmdline);

	/* Throughout the following loop local variables have the following meaning:
	 * - save_msg  -- resultant state of message indication;
	 * - raw       -- not yet processed part of string that can contain \;
	 * - processed -- ready to consume part of string;
	 * - cmdline   -- points to the start of the last command. */
	while(*cmdline != '\0')
	{
		if(args_kind == CAT_REGULAR && *raw == '\\')
		{
			if(*(raw + 1) == '|')
			{
				*processed++ = '|';
				raw += 2;
			}
			else
			{
				*processed++ = *raw++;
				*processed++ = *raw++;
			}
		}
		else if((*raw == '|' && is_out_of_arg(cmdline, processed)) || *raw == '\0')
		{
			if(*raw != '\0')
			{
				++raw;
			}
			else
			{
				*processed = '\0';
			}

			/* Don't break line for whole line commands. */
			if(args_kind != CAT_UNTIL_THE_END)
			{
				if(args_kind == CAT_EXPR)
				{
					/* Move breaking point forward by consuming parts of the string after
					 * || until end of the string or | is found. */
					while(processed[0] == '|' && processed[1] == '|' &&
							processed[2] != '|')
					{
						processed = until_first(processed + 2, '|');
						raw = (processed[0] == '\0') ? processed : (processed + 1);
					}
				}

				*processed = '\0';
				processed = raw;
			}

			len = add_to_string_array(&cmds, len, 1, cmdline);

			if(args_kind == CAT_UNTIL_THE_END)
			{
				/* Whole line command takes the rest of the string, nothing more to
				 * process. */
				break;
			}

			cmdline = skip_to_cmd_name(processed);
			args_kind = get_cmd_args_type(cmdline);
		}
		else
		{
			*processed++ = *raw++;
		}
	}

finish:
	(void)put_into_string_array(&cmds, len, NULL);
	return cmds;
}

/* Checks whether character at given position in the given command-line is
 * outside quoted argument.  Returns non-zero if so, otherwise zero is
 * returned. */
static int
is_out_of_arg(const char cmd[], const char pos[])
{
	return get_cmdline_location(cmd, pos) == CLL_OUT_OF_ARG;
}

CmdLineLocation
get_cmdline_location(const char cmd[], const char pos[])
{
	char separator;
	int regex_quoting;

	cmd_info_t info;
	const int cmd_id = get_cmd_info(cmd, &info);

	switch(cmd_id)
	{
		case COM_FILTER:
			separator = ' ';
			regex_quoting = 1;
			break;
		case COM_SUBSTITUTE:
		case COM_TR:
			separator = info.sep;
			regex_quoting = 1;
			break;

		default:
			separator = ' ';
			regex_quoting = 0;
			break;
	}

	switch(line_pos(cmd, pos, separator, regex_quoting))
	{
		case 0: return CLL_OUT_OF_ARG;
		case 1: /* Fall through. */
		case 2: return CLL_NO_QUOTING;
		case 3: return CLL_S_QUOTING;
		case 4: return CLL_D_QUOTING;
		case 5: return CLL_R_QUOTING;

		default:
			assert(0 && "Unexpected return code.");
			return CLL_NO_QUOTING;
	}
}

/* Determines what kind of processing should be applied to the command pointed
 * to by the cmd.  Returns the kind. */
static CmdArgsType
get_cmd_args_type(const char cmd[])
{
	const int cmd_id = get_cmd_id(cmd);
	switch(cmd_id)
	{
		case COMMAND_CMD_ID:
		case COM_AUTOCMD:
		case COM_EXECUTE:
		case COM_CMAP:
		case COM_CNOREMAP:
		case COM_COMMAND:
		case COM_FILETYPE:
		case COM_FILEVIEWER:
		case COM_FILEXTYPE:
		case COM_MAP:
		case COM_MMAP:
		case COM_MNOREMAP:
		case COM_NMAP:
		case COM_NNOREMAP:
		case COM_NORMAL:
		case COM_QMAP:
		case COM_QNOREMAP:
		case COM_VMAP:
		case COM_VNOREMAP:
		case COM_NOREMAP:
		case COM_WINCMD:
		case COM_WINDO:
		case COM_WINRUN:
			return CAT_UNTIL_THE_END;

		default:
			return command_accepts_expr(cmd_id) ? CAT_EXPR : CAT_REGULAR;
	}
}

const char *
find_last_command(const char cmds[])
{
	const char *p, *q;

	p = cmds;
	q = cmds;
	while(*cmds != '\0')
	{
		if(*p == '\\')
		{
			q += (p[1] == '|') ? 1 : 2;
			p += 2;
		}
		else if(*p == '\0' || (*p == '|' &&
				line_pos(cmds, q, ' ', starts_with_lit(cmds, "fil")) == 0))
		{
			if(*p != '\0')
			{
				++p;
			}

			cmds = skip_to_cmd_name(cmds);
			if(*cmds == '!' || starts_with_lit(cmds, "com"))
			{
				break;
			}

			q = p;

			if(*q == '\0')
			{
				break;
			}

			cmds = q;
		}
		else
		{
			++q;
			++p;
		}
	}

	return cmds;
}

/* Skips consecutive whitespace or colon characters at the beginning of the
 * command.  Returns pointer to the first non whitespace and color character. */
static char *
skip_to_cmd_name(const char cmd[])
{
	while(isspace(*cmd) || *cmd == ':')
	{
		++cmd;
	}
	return (char *)cmd;
}

int
exec_command(const char cmd[], FileView *view, CmdInputType type)
{
	int menu;
	int backward;

	if(cmd == NULL)
	{
		return repeat_command(view, type);
	}

	menu = 0;
	backward = 0;
	switch(type)
	{
		case CIT_BSEARCH_PATTERN: backward = 1; /* Fall through. */
		case CIT_FSEARCH_PATTERN:
			return find_npattern(view, cmd, backward, 1);

		case CIT_VBSEARCH_PATTERN: backward = 1; /* Fall through. */
		case CIT_VFSEARCH_PATTERN:
			return find_vpattern(view, cmd, backward, 1);

		case CIT_VWBSEARCH_PATTERN: backward = 1; /* Fall through. */
		case CIT_VWFSEARCH_PATTERN:
			return find_vwpattern(cmd, backward);

		case CIT_MENU_COMMAND: menu = 1; /* Fall through. */
		case CIT_COMMAND:
			return execute_command(view, cmd, menu);

		case CIT_FILTER_PATTERN:
			local_filter_apply(view, cmd);
			return 0;

		default:
			assert(0 && "Command execution request of unknown/unexpected type.");
			return 0;
	};
}

/* Repeats last command of the specified type.  Returns 0 on success if no
 * message should be saved in the status bar, > 0 to save message on successful
 * execution and < 0 in case of error with error message. */
static int
repeat_command(FileView *view, CmdInputType type)
{
	int backward = 0;
	switch(type)
	{
		case CIT_BSEARCH_PATTERN: backward = 1; /* Fall through. */
		case CIT_FSEARCH_PATTERN:
			return find_npattern(view, cfg_get_last_search_pattern(), backward, 1);

		case CIT_VBSEARCH_PATTERN: backward = 1; /* Fall through. */
		case CIT_VFSEARCH_PATTERN:
			return find_vpattern(view, cfg_get_last_search_pattern(), backward, 1);

		case CIT_VWBSEARCH_PATTERN: backward = 1; /* Fall through. */
		case CIT_VWFSEARCH_PATTERN:
			return find_vwpattern(NULL, backward);

		case CIT_COMMAND:
			return execute_command(view, NULL, 0);

		case CIT_FILTER_PATTERN:
			local_filter_apply(view, "");
			return 0;

		default:
			assert(0 && "Command repetition request of unexpected type.");
			return 0;
	}
}

void
commands_scope_start(void)
{
	(void)int_stack_push(&if_levels, IF_SCOPE_GUARD);
}

int
commands_scope_finish(void)
{
	if(!is_at_scope_bottom(&if_levels))
	{
		status_bar_error("Missing :endif");
		int_stack_pop_seq(&if_levels, IF_SCOPE_GUARD);
		return 1;
	}

	int_stack_pop(&if_levels);
	return 0;
}

void
cmds_scoped_if(int cond)
{
	(void)int_stack_push(&if_levels, cond ? IF_MATCH : IF_BEFORE_MATCH);
	cmds_preserve_selection();
}

int
cmds_scoped_elseif(int cond)
{
	IfFrame if_frame;

	if(is_at_scope_bottom(&if_levels))
	{
		return 1;
	}

	if_frame = int_stack_get_top(&if_levels);
	if(if_frame == IF_ELSE || if_frame == IF_FINISH)
	{
		return 1;
	}

	int_stack_set_top(&if_levels, (if_frame == IF_BEFORE_MATCH) ?
			(cond ? IF_MATCH : IF_BEFORE_MATCH) :
			IF_AFTER_MATCH);
	cmds_preserve_selection();
	return 0;
}

int
cmds_scoped_else(void)
{
	IfFrame if_frame;

	if(is_at_scope_bottom(&if_levels))
	{
		return 1;
	}

	if_frame = int_stack_get_top(&if_levels);
	if(if_frame == IF_ELSE || if_frame == IF_FINISH)
	{
		return 1;
	}

	int_stack_set_top(&if_levels,
			(if_frame == IF_BEFORE_MATCH) ? IF_ELSE : IF_FINISH);
	cmds_preserve_selection();
	return 0;
}

int
cmds_scoped_endif(void)
{
	if(is_at_scope_bottom(&if_levels))
	{
		return 1;
	}

	int_stack_pop(&if_levels);
	return 0;
}

/* Checks that bottom of block scope is reached.  Returns non-zero if so,
 * otherwise zero is returned. */
static int
is_at_scope_bottom(const int_stack_t *scope_stack)
{
	return int_stack_is_empty(scope_stack)
	    || int_stack_top_is(scope_stack, IF_SCOPE_GUARD);
}

char *
eval_arglist(const char args[], const char **stop_ptr)
{
	size_t len = 0;
	char *eval_result = NULL;

	assert(args[0] != '\0');

	while(args[0] != '\0')
	{
		char *free_this = NULL;
		const char *tmp_result = NULL;

		var_t result = var_false();
		const ParsingErrors parsing_error = parse(args, &result);
		if(parsing_error == PE_INVALID_EXPRESSION && is_prev_token_whitespace())
		{
			result = get_parsing_result();
			tmp_result = free_this = var_to_string(result);
			args = get_last_parsed_char();
		}
		else if(parsing_error == PE_NO_ERROR)
		{
			tmp_result = free_this = var_to_string(result);
			args = get_last_position();
		}

		if(tmp_result == NULL)
		{
			var_free(result);
			break;
		}

		if(!is_null_or_empty(eval_result))
		{
			eval_result = extend_string(eval_result, " ", &len);
		}
		eval_result = extend_string(eval_result, tmp_result, &len);

		var_free(result);
		free(free_this);

		args = skip_whitespace(args);
	}
	if(args[0] == '\0')
	{
		return eval_result;
	}
	else
	{
		free(eval_result);
		*stop_ptr = args;
		return NULL;
	}
}

void
cmds_preserve_selection(void)
{
	keep_view_selection = 1;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */