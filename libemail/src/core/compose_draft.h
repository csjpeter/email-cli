/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* The autosaved compose draft: one file in the data directory. */
#ifndef CORE_COMPOSE_DRAFT_H
#define CORE_COMPOSE_DRAFT_H

#include <stddef.h>

/** @brief One compose draft: the headers, the attachment paths and the body. */
typedef struct {
    char  to[512], cc[512], bcc[512], subject[512], in_reply_to[512];
    char  attach[16][1024];
    int   attach_count;
    char *body;            /**< heap, NUL-terminated; may be NULL when empty */
} ComposeDraft;

/**
 * @brief Writes @p d to the draft file (mode 0600), replacing the old one.
 * @return 0 on success, -1 on error (a message on stderr).
 */
int compose_draft_save(const ComposeDraft *d);

/**
 * @brief Reads the draft file into @p d (body allocated; free with
 *        compose_draft_free()).
 * @return 0 when a draft was read, -1 when there is none or it cannot be read.
 */
int compose_draft_load(ComposeDraft *d);

/** @brief Removes the draft file; a missing file is not an error. */
void compose_draft_remove(void);

/** @brief Frees what compose_draft_load() allocated. */
void compose_draft_free(ComposeDraft *d);

#endif /* CORE_COMPOSE_DRAFT_H */
