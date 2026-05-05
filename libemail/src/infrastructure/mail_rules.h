#ifndef MAIL_RULES_H
#define MAIL_RULES_H

#include <time.h>
#include "when_expr.h"

/**
 * @file mail_rules.h
 * @brief Per-account mail sorting rules engine.
 *
 * Rule file location:
 *   ~/.config/email-cli/accounts/<account>/rules.ini
 *
 * Format example:
 *   [rule "GitHub notifications"]
 *   if-from    = *@github.com
 *   then-add-label    = GitHub
 *   then-remove-label = INBOX
 *
 * Conditions: if-from, if-subject, if-to, if-label (glob patterns).
 *             if-not-from, if-not-subject, if-not-to (negated glob patterns).
 * Actions:    then-add-label, then-remove-label, then-move-folder (IMAP only),
 *             then-forward-to (stored; forwarding execution is future work).
 *
 * Multiple then-add-label / then-remove-label lines are allowed per rule.
 * Conditions are ANDed; any omitted condition matches everything.
 * then-move-folder is silently ignored on Gmail accounts.
 */

#define MAIL_RULE_MAX_LABELS 32

typedef struct {
    char  *name;                              /**< Display name from [rule "..."] header */
    int    action_index;                      /**< US-82: action group number (0=default/1) */
    char  *when;                              /**< US-81: boolean condition expression (NULL = always true) */
    char  *if_from;                           /**< Glob (NULL = any) */
    char  *if_subject;                        /**< Glob (NULL = any) */
    char  *if_to;                             /**< Glob (NULL = any) */
    char  *if_label;                          /**< Glob (NULL = any) */
    char  *if_not_from;                       /**< Negated glob: match fails if field matches (NULL = disabled) */
    char  *if_not_subject;                    /**< Negated glob (NULL = disabled) */
    char  *if_not_to;                         /**< Negated glob (NULL = disabled) */
    char  *if_body;                           /**< Glob against plain-text body (NULL = disabled) */
    int    if_age_gt;                         /**< Minimum age in days (0 = disabled) */
    int    if_age_lt;                         /**< Maximum age in days (0 = disabled) */
    char  *then_add_label[MAIL_RULE_MAX_LABELS];  /**< Labels to add */
    int    then_add_count;
    char  *then_rm_label[MAIL_RULE_MAX_LABELS];   /**< Labels to remove */
    int    then_rm_count;
    char  *then_move_folder;                  /**< Destination folder (IMAP only; may be NULL) */
    char  *then_forward_to;                   /**< Forward target address (stored; not yet executed) */
} MailRule;

typedef struct {
    MailRule *rules;
    int       count;
    int       cap;
} MailRules;

/**
 * @brief Load rules from an arbitrary ini file path.
 *
 * @param path  Absolute path to a rules.ini file.
 * @return Heap-allocated MailRules, or NULL on allocation error or missing file.
 *         Caller must free with mail_rules_free().
 */
MailRules *mail_rules_load_path(const char *path);

/**
 * @brief Load rules from the account's rules.ini file.
 *
 * @param account_name  Account directory name under accounts/ (e.g. "user@example.com").
 * @return Heap-allocated MailRules, or NULL on allocation error or missing file.
 *         Returns an empty (count=0) rules set if the file exists but has no rules.
 *         Caller must free with mail_rules_free().
 */
MailRules *mail_rules_load(const char *account_name);

/**
 * @brief Save rules to the account's rules.ini file.
 *
 * @param account_name  Account directory name.
 * @param rules         Rules to write.
 * @return 0 on success, -1 on error.
 */
int mail_rules_save(const char *account_name, const MailRules *rules);

/**
 * @brief Free all memory owned by a MailRules struct.
 */
void mail_rules_free(MailRules *rules);

/**
 * @brief Apply rules to a single message and return aggregate label changes.
 *
 * Rules are tested in order; each matching rule's actions are accumulated.
 * The label sets from earlier rules are visible to later rules' if-label checks.
 *
 * @param rules        Rule set to apply.
 * @param from         Decoded From display string (may be NULL).
 * @param subject      Decoded Subject string (may be NULL).
 * @param to           Decoded To string (may be NULL).
 * @param labels_csv   Current comma-separated label string (may be NULL).
 * @param body         Plain-text body (may be NULL — body conditions are skipped).
 * @param message_date Unix timestamp of the message Date header (0 = unknown, age conditions skipped).
 * @param add_out      Heap-allocated array of label strings to add. Caller frees.
 * @param add_count    Number of entries in *add_out.
 * @param rm_out       Heap-allocated array of label strings to remove. Caller frees.
 * @param rm_count     Number of entries in *rm_out.
 * @return Number of rules that fired (0 = no action needed).
 */
int mail_rules_apply(const MailRules *rules,
                     const char *from, const char *subject,
                     const char *to, const char *labels_csv,
                     const char *body, time_t message_date,
                     char ***add_out, int *add_count,
                     char ***rm_out,  int *rm_count);

/**
 * Like mail_rules_apply() but also returns the first fired rule's then_move_folder.
 * @param move_folder_out  Set to a strdup'd copy of then_move_folder (caller frees),
 *                         or NULL if no fired rule has then_move_folder.
 */
int mail_rules_apply_ex(const MailRules *rules,
                        const char *from, const char *subject,
                        const char *to, const char *labels_csv,
                        const char *body, time_t message_date,
                        char ***add_out, int *add_count,
                        char ***rm_out,  int *rm_count,
                        char **move_folder_out);

/**
 * @brief Returns 1 if all conditions of the rule match the message, 0 otherwise.
 *
 * @param rule       Rule to test (NULL → returns 0).
 * @param from       Decoded From string (may be NULL).
 * @param subject    Decoded Subject string (may be NULL).
 * @param to         Decoded To string (may be NULL).
 * @param labels_csv Comma-separated label string (may be NULL).
 * @param body       Plain-text body (may be NULL — if-body skipped).
 * @param message_date Unix timestamp (0 = unknown — age conditions skipped).
 * @return 1 if all conditions match, 0 otherwise.
 */
int mail_rule_matches(const MailRule *rule,
                      const char *from, const char *subject,
                      const char *to, const char *labels_csv,
                      const char *body, time_t message_date);

#endif /* MAIL_RULES_H */
