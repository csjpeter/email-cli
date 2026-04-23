#ifndef MAIL_RULES_H
#define MAIL_RULES_H

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
 * Actions:    then-add-label, then-remove-label, then-move-folder (IMAP only).
 *
 * Multiple then-add-label / then-remove-label lines are allowed per rule.
 * Conditions are ANDed; any omitted condition matches everything.
 * then-move-folder is silently ignored on Gmail accounts.
 */

#define MAIL_RULE_MAX_LABELS 32

typedef struct {
    char  *name;                              /**< Display name from [rule "..."] header */
    char  *if_from;                           /**< Glob (NULL = any) */
    char  *if_subject;                        /**< Glob (NULL = any) */
    char  *if_to;                             /**< Glob (NULL = any) */
    char  *if_label;                          /**< Glob (NULL = any) */
    char  *then_add_label[MAIL_RULE_MAX_LABELS];  /**< Labels to add */
    int    then_add_count;
    char  *then_rm_label[MAIL_RULE_MAX_LABELS];   /**< Labels to remove */
    int    then_rm_count;
    char  *then_move_folder;                  /**< Destination folder (IMAP only; may be NULL) */
} MailRule;

typedef struct {
    MailRule *rules;
    int       count;
    int       cap;
} MailRules;

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
 * @param add_out      Heap-allocated array of label strings to add. Caller frees.
 * @param add_count    Number of entries in *add_out.
 * @param rm_out       Heap-allocated array of label strings to remove. Caller frees.
 * @param rm_count     Number of entries in *rm_out.
 * @return Number of rules that fired (0 = no action needed).
 */
int mail_rules_apply(const MailRules *rules,
                     const char *from, const char *subject,
                     const char *to, const char *labels_csv,
                     char ***add_out, int *add_count,
                     char ***rm_out,  int *rm_count);

#endif /* MAIL_RULES_H */
