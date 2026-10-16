/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* Main of the unit test runner that calls every test group. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_helpers.h"
#include "logger.h"

// Globals defined in test_helpers.h
int g_tests_run = 0;
int g_tests_failed = 0;
const char *g_test_filter = NULL;
int g_test_list_only = 0;
int g_tests_selected = 0;
int g_test_depth = 0;

// Forward declarations of test suites
void test_fs_util(void);
void test_config_store(void);
void test_logger(void);
void test_wizard(void);
void test_imap_client(void);
void test_mime_util(void);
void test_local_msg_store(void);
void test_local_msg_find_folders(void);
void test_local_hdr_evict(void);
void test_local_index(void);
void test_manifest(void);
void test_ui_prefs(void);
void test_local_msg_delete(void);
void test_local_index_email_extraction(void);
void test_local_trash_labels(void);
void test_local_gmail_history(void);
void test_local_hdr_get_labels(void);
void test_local_flag_search(void);
void test_manifest_count_after_flag_update(void);
void test_flag_search_folder_isolation(void);
void test_local_contacts_update(void);
void test_imap_util(void);
void test_platform(void);
void test_email_service(void);
void test_html_parser(void);
void test_html_render(void);
void test_html_render_style_balance(void);
void test_html_render_parent_close(void);
void test_html_render_color_filter(void);
void test_html_render_url_isolation(void);
void test_html_render_url_color(void);
void test_input_line(void);
void test_tui_frame(void);
void test_compose_draft(void);
void test_path_complete(void);
void run_json_util_tests(void);
void test_gmail_auth(void);
void test_gmail_client(void);
void test_label_idx(void);
void test_mail_client(void);
void test_gmail_sync(void);
void test_compose_service(void);
void test_smtp_adapter(void);
void test_email_client(void);
void test_tls_revocation(void);
void test_net_clients(void);
void test_charset(void);
void test_mail_rules(void);
void test_when_expr(void);
void test_tuikit_link(void);
void test_local_search(void);
void test_local_search_gmail(void);
void test_local_contacts_rebuild(void);
void test_local_pending_append(void);
void test_local_pending_fetch(void);
void test_local_save_outgoing(void);
void test_config_settings(void);
void test_config_load_account(void);
void test_config_migrate(void);
void test_manifest_dmarc_flags(void);

static void usage(const char *argv0) {
    printf("Usage: %s [--list] [FILTER]\n"
           "  FILTER   run only the test suites whose name contains FILTER\n"
           "  --list   print the names of the test suites and exit\n", argv0);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--list") == 0) {
            g_test_list_only = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        } else {
            g_test_filter = argv[i];
        }
    }

    /* The suites must never read the developer's real configuration: HOME is
     * a fixture and the XDG_* variables, which override it, are unset (a
     * test that needs one sets it itself). */
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CACHE_HOME");
    if (!g_test_list_only) setenv("HOME", TP("home"), 1);

    if (!g_test_list_only)
        printf("--- email-cli Unit Test Suite ---\n\n");

    // Suppress mirror of ERROR logs to stderr during testing
    logger_set_stderr(0);

    RUN_TEST(test_fs_util);
    RUN_TEST(test_config_store);
    RUN_TEST(test_logger);
    RUN_TEST(test_wizard);
    RUN_TEST(test_imap_client);
    RUN_TEST(test_mime_util);
    RUN_TEST(test_local_msg_store);
    RUN_TEST(test_local_msg_find_folders);
    RUN_TEST(test_local_hdr_evict);
    RUN_TEST(test_local_index);
    RUN_TEST(test_manifest);
    RUN_TEST(test_ui_prefs);
    RUN_TEST(test_local_msg_delete);
    RUN_TEST(test_local_index_email_extraction);
    RUN_TEST(test_local_trash_labels);
    RUN_TEST(test_local_gmail_history);
    RUN_TEST(test_local_hdr_get_labels);
    RUN_TEST(test_local_flag_search);
    RUN_TEST(test_manifest_count_after_flag_update);
    RUN_TEST(test_flag_search_folder_isolation);
    RUN_TEST(test_local_contacts_update);
    RUN_TEST(test_local_search);
    RUN_TEST(test_local_search_gmail);
    RUN_TEST(test_local_contacts_rebuild);
    RUN_TEST(test_local_pending_append);
    RUN_TEST(test_local_pending_fetch);
    RUN_TEST(test_local_save_outgoing);
    RUN_TEST(test_imap_util);
    RUN_TEST(test_platform);
    RUN_TEST(test_email_service);
    RUN_TEST(test_html_parser);
    RUN_TEST(test_html_render);
    RUN_TEST(test_html_render_style_balance);
    RUN_TEST(test_html_render_parent_close);
    RUN_TEST(test_html_render_color_filter);
    RUN_TEST(test_html_render_url_isolation);
    RUN_TEST(test_html_render_url_color);
    RUN_TEST(test_input_line);
    RUN_TEST(test_tui_frame);
    RUN_TEST(test_compose_draft);
    RUN_TEST(test_path_complete);
    RUN_TEST(run_json_util_tests);
    RUN_TEST(test_gmail_auth);
    RUN_TEST(test_gmail_client);
    RUN_TEST(test_label_idx);
    RUN_TEST(test_mail_client);
    RUN_TEST(test_gmail_sync);
    RUN_TEST(test_compose_service);
    RUN_TEST(test_smtp_adapter);
    RUN_TEST(test_email_client);
    RUN_TEST(test_tls_revocation);
    RUN_TEST(test_net_clients);
    RUN_TEST(test_charset);
    RUN_TEST(test_mail_rules);
    RUN_TEST(test_when_expr);
    RUN_TEST(test_tuikit_link);
    RUN_TEST(test_config_settings);
    RUN_TEST(test_config_load_account);
    RUN_TEST(test_config_migrate);
    RUN_TEST(test_manifest_dmarc_flags);

    if (g_test_list_only) return EXIT_SUCCESS;
    if (g_test_filter && g_tests_selected == 0) {
        fprintf(stderr, "no test suite matches '%s'\n", g_test_filter);
        return EXIT_FAILURE;
    }

    printf("\n--- Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    if (g_tests_failed > 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
