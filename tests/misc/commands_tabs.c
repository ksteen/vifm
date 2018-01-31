#include <stic.h>

#include <stddef.h> /* NULL */
#include <string.h> /* strcpy() */

#include "../../src/cfg/config.h"
#include "../../src/engine/keys.h"
#include "../../src/modes/modes.h"
#include "../../src/modes/wk.h"
#include "../../src/ui/column_view.h"
#include "../../src/ui/tabs.h"
#include "../../src/ui/ui.h"
#include "../../src/cmd_core.h"
#include "../../src/compare.h"

#include "utils.h"

static void format_none(int id, const void *data, size_t buf_len, char buf[]);

SETUP()
{
	view_setup(&lwin);
	setup_grid(&lwin, 1, 1, 1);
	curr_view = &lwin;
	view_setup(&rwin);
	setup_grid(&rwin, 1, 1, 1);
	other_view = &rwin;

	init_modes();

	opt_handlers_setup();

	columns_add_column_desc(SK_BY_NAME, &format_none);
	columns_add_column_desc(SK_BY_SIZE, &format_none);

	init_commands();
}

static void
format_none(int id, const void *data, size_t buf_len, char buf[])
{
	buf[0] = '\0';
}

TEARDOWN()
{
	reset_cmds();

	tabs_only(&lwin);
	tabs_only(&rwin);
	cfg.pane_tabs = 0;
	tabs_only(&lwin);

	vle_keys_reset();

	opt_handlers_teardown();

	view_teardown(&lwin);
	view_teardown(&rwin);

	columns_clear_column_descs();
}

TEST(tab_without_name_is_created)
{
	tab_info_t tab_info;

	assert_success(exec_commands("tabnew", &lwin, CIT_COMMAND));
	assert_int_equal(2, tabs_count(&lwin));

	assert_true(tabs_get(&lwin, 1, &tab_info));
	assert_string_equal(NULL, tab_info.name);
}

TEST(tab_with_name_is_created)
{
	tab_info_t tab_info;

	assert_success(exec_commands("tabnew name", &lwin, CIT_COMMAND));
	assert_int_equal(2, tabs_count(&lwin));

	assert_true(tabs_get(&lwin, 1, &tab_info));
	assert_string_equal("name", tab_info.name);
}

TEST(newtab_fails_in_diff_mode_for_tab_panes)
{
	create_file(SANDBOX_PATH "/empty");

	strcpy(lwin.curr_dir, SANDBOX_PATH);
	strcpy(rwin.curr_dir, SANDBOX_PATH);

	cfg.pane_tabs = 1;
	(void)compare_two_panes(CT_CONTENTS, LT_ALL, 1, 0);
	assert_failure(exec_commands("tabnew", &lwin, CIT_COMMAND));
	assert_int_equal(1, tabs_count(&lwin));

	assert_success(remove(SANDBOX_PATH "/empty"));
}

TEST(tab_name_is_set)
{
	tab_info_t tab_info;

	assert_success(exec_commands("tabname new-name", &lwin, CIT_COMMAND));

	assert_true(tabs_get(&lwin, 0, &tab_info));
	assert_string_equal("new-name", tab_info.name);
}

TEST(tab_name_is_reset)
{
	tab_info_t tab_info;

	assert_success(exec_commands("tabname new-name", &lwin, CIT_COMMAND));
	assert_success(exec_commands("tabname", &lwin, CIT_COMMAND));

	assert_true(tabs_get(&lwin, 0, &tab_info));
	assert_string_equal(NULL, tab_info.name);
}

TEST(tab_is_closed)
{
	assert_success(exec_commands("tabnew", &lwin, CIT_COMMAND));
	assert_success(exec_commands("tabclose", &lwin, CIT_COMMAND));
	assert_int_equal(1, tabs_count(&lwin));
}

TEST(last_tab_is_not_closed)
{
	assert_success(exec_commands("tabclose", &lwin, CIT_COMMAND));
	assert_int_equal(1, tabs_count(&lwin));
}

TEST(quit_commands_close_tabs)
{
	assert_success(exec_commands("tabnew", &lwin, CIT_COMMAND));
	assert_success(exec_commands("quit", &lwin, CIT_COMMAND));
	assert_int_equal(1, tabs_count(&lwin));

	assert_success(exec_commands("tabnew", &lwin, CIT_COMMAND));
	assert_success(exec_commands("wq", &lwin, CIT_COMMAND));
	assert_int_equal(1, tabs_count(&lwin));

	assert_success(exec_commands("tabnew", &lwin, CIT_COMMAND));
	(void)vle_keys_exec_timed_out(WK_Z WK_Z);
	assert_int_equal(1, tabs_count(&lwin));

	assert_success(exec_commands("tabnew", &lwin, CIT_COMMAND));
	(void)vle_keys_exec_timed_out(WK_Z WK_Q);
	assert_int_equal(1, tabs_count(&lwin));
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 : */
