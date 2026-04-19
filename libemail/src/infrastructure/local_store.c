#include "local_store.h"
#include "fs_util.h"
#include "mime_util.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

/* ── Account base path (set by local_store_init) ─────────────────────── */

static char g_account_base[8192];

int local_store_init(const char *host_url, const char *username) {
    const char *data_base = platform_data_dir();
    if (!data_base) return -1;
    if (!host_url && (!username || !username[0])) return -1;

    /* The email address (username) uniquely identifies an account.
     * Use it directly as the directory key so two accounts on the same
     * server get separate local stores without a double-@ suffix.
     * Falls back to hostname-only for legacy single-account setups. */
    if (username && username[0]) {
        snprintf(g_account_base, sizeof(g_account_base),
                 "%s/email-cli/accounts/%s", data_base, username);
    } else {
        /* Extract hostname from URL: imaps://host:port → host */
        const char *p = strstr(host_url, "://");
        p = p ? p + 3 : host_url;
        char hostname[512];
        int i = 0;
        while (*p && *p != ':' && *p != '/' && i < (int)sizeof(hostname) - 1)
            hostname[i++] = *p++;
        hostname[i] = '\0';
        for (char *c = hostname; *c; c++) *c = (char)tolower((unsigned char)*c);
        snprintf(g_account_base, sizeof(g_account_base),
                 "%s/email-cli/accounts/imap.%s", data_base, hostname);
    }

    logger_log(LOG_DEBUG, "local_store: account base = %s", g_account_base);
    return 0;
}

/* ── Reverse digit bucketing helpers ─────────────────────────────────── */

static char digit1(const char *uid) {
    size_t len = strlen(uid);
    return len > 0 ? uid[len - 1] : '0';
}
static char digit2(const char *uid) {
    size_t len = strlen(uid);
    return len > 1 ? uid[len - 2] : '0';
}

/* ── Shared file I/O ─────────────────────────────────────────────────── */

static char *load_file(const char *path) {
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) return NULL;
    long size = ftell(fp);
    if (size <= 0) return NULL;
    rewind(fp);
    char *buf = malloc((size_t)size + 1);
    if (!buf) return NULL;
    if ((long)fread(buf, 1, (size_t)size, fp) != size) { free(buf); return NULL; }
    buf[size] = '\0';
    return buf;
}

static int write_file(const char *path, const char *content, size_t len) {
    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    if (fwrite(content, 1, len, fp) != len) return -1;
    return 0;
}

/** @brief Ensures the parent directory of a bucketed path exists. */
static int ensure_bucket_dir(const char *area, const char *folder, const char *uid) {
    RAII_STRING char *dir = NULL;
    if (asprintf(&dir, "%s/%s/%s/%c/%c",
                 g_account_base, area, folder, digit1(uid), digit2(uid)) == -1)
        return -1;
    return fs_mkdir_p(dir, 0700);
}

/* ── Message store ───────────────────────────────────────────────────── */

static char *msg_path(const char *folder, const char *uid) {
    if (!g_account_base[0]) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/store/%s/%c/%c/%s.eml",
                 g_account_base, folder, digit1(uid), digit2(uid), uid) == -1)
        return NULL;
    return path;
}

int local_msg_exists(const char *folder, const char *uid) {
    RAII_STRING char *path = msg_path(folder, uid);
    if (!path) return 0;
    RAII_FILE FILE *fp = fopen(path, "r");
    return fp != NULL;
}

int local_msg_save(const char *folder, const char *uid, const char *content, size_t len) {
    if (!g_account_base[0]) return -1;
    if (ensure_bucket_dir("store", folder, uid) != 0) {
        logger_log(LOG_ERROR, "Failed to create store bucket for %s/%s", folder, uid);
        return -1;
    }
    RAII_STRING char *path = msg_path(folder, uid);
    if (!path) return -1;
    if (write_file(path, content, len) != 0) {
        logger_log(LOG_ERROR, "Failed to write store file: %s", path);
        return -1;
    }
    logger_log(LOG_DEBUG, "Stored %s/%s at %s", folder, uid, path);
    return 0;
}

char *local_msg_load(const char *folder, const char *uid) {
    RAII_STRING char *path = msg_path(folder, uid);
    if (!path) return NULL;
    return load_file(path);
}

/* ── Header store ────────────────────────────────────────────────────── */

static char *hdr_path(const char *folder, const char *uid) {
    if (!g_account_base[0]) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/headers/%s/%c/%c/%s.hdr",
                 g_account_base, folder, digit1(uid), digit2(uid), uid) == -1)
        return NULL;
    return path;
}

int local_hdr_exists(const char *folder, const char *uid) {
    RAII_STRING char *path = hdr_path(folder, uid);
    if (!path) return 0;
    RAII_FILE FILE *fp = fopen(path, "r");
    return fp != NULL;
}

int local_hdr_save(const char *folder, const char *uid, const char *content, size_t len) {
    if (!g_account_base[0]) return -1;
    if (ensure_bucket_dir("headers", folder, uid) != 0) {
        logger_log(LOG_ERROR, "Failed to create header bucket for %s/%s", folder, uid);
        return -1;
    }
    RAII_STRING char *path = hdr_path(folder, uid);
    if (!path) return -1;
    if (write_file(path, content, len) != 0) return -1;
    logger_log(LOG_DEBUG, "Stored header %s/%s", folder, uid);
    return 0;
}

char *local_hdr_load(const char *folder, const char *uid) {
    RAII_STRING char *path = hdr_path(folder, uid);
    if (!path) return NULL;
    return load_file(path);
}

int local_hdr_update_flags(const char *folder, const char *uid, int new_flags) {
    char *hdr = local_hdr_load(folder, uid);
    if (!hdr) return -1;

    /* Find the last tab → flags field starts after it */
    char *last_tab = strrchr(hdr, '\t');
    if (!last_tab) { free(hdr); return -1; }

    /* Rebuild: keep everything up to and including last tab, replace flags */
    *last_tab = '\0';
    char *updated = NULL;
    if (asprintf(&updated, "%s\t%d", hdr, new_flags) == -1) {
        free(hdr);
        return -1;
    }
    free(hdr);

    int rc = local_hdr_save(folder, uid, updated, strlen(updated));
    free(updated);
    return rc;
}

static int cmp_uid_evict(const void *a, const void *b) {
    return memcmp(a, b, 16);
}

void local_hdr_evict_stale(const char *folder,
                             const char (*keep_uids)[17], int keep_count) {
    if (!g_account_base[0]) return;

    char (*sorted)[17] = malloc((size_t)keep_count * sizeof(char[17]));
    if (!sorted) return;
    memcpy(sorted, keep_uids, (size_t)keep_count * sizeof(char[17]));
    qsort(sorted, (size_t)keep_count, sizeof(char[17]), cmp_uid_evict);

    /* Walk all 100 buckets (10 × 10) */
    for (int d1 = 0; d1 <= 9; d1++) {
        for (int d2 = 0; d2 <= 9; d2++) {
            RAII_STRING char *dir = NULL;
            if (asprintf(&dir, "%s/headers/%s/%d/%d",
                         g_account_base, folder, d1, d2) == -1)
                continue;

            RAII_DIR DIR *d = opendir(dir);
            if (!d) continue;

            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                const char *name = ent->d_name;
                const char *dot  = strrchr(name, '.');
                if (!dot || strcmp(dot, ".hdr") != 0) continue;
                size_t stem_len = (size_t)(dot - name);
                if (stem_len == 0 || stem_len > 16) continue;
                char key[17] = {0};
                memcpy(key, name, stem_len);
                if (!bsearch(key, sorted, (size_t)keep_count,
                             sizeof(char[17]), cmp_uid_evict)) {
                    RAII_STRING char *path = NULL;
                    if (asprintf(&path, "%s/%s", dir, name) != -1) {
                        remove(path);
                        logger_log(LOG_DEBUG,
                                   "Evicted stale header: UID %s in %s", key, folder);
                    }
                }
            }
        }
    }
    free(sorted);
}

/* ── Index helpers ───────────────────────────────────────────────────── */

/** @brief Checks if a reference line already exists in an index file. */
static int index_has_ref(const char *path, const char *ref) {
    char *content = load_file(path);
    if (!content) return 0;
    size_t ref_len = strlen(ref);
    const char *p = content;
    while (*p) {
        if (strncmp(p, ref, ref_len) == 0 &&
            (p[ref_len] == '\n' || p[ref_len] == '\0')) {
            free(content);
            return 1;
        }
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    free(content);
    return 0;
}

/** @brief Appends a reference to an index file (skips duplicates). */
static int index_append(const char *dir_path, const char *file_name,
                        const char *ref) {
    if (fs_mkdir_p(dir_path, 0700) != 0) return -1;

    RAII_STRING char *path = NULL;
    if (asprintf(&path, "%s/%s", dir_path, file_name) == -1) return -1;

    if (index_has_ref(path, ref)) return 0; /* already indexed */

    RAII_FILE FILE *fp = fopen(path, "a");
    if (!fp) return -1;
    fprintf(fp, "%s\n", ref);
    return 0;
}

/** @brief Removes a reference from an index file. */
__attribute__((unused))
/** @brief Extracts email address parts from a From header value. */
static void extract_email_parts(const char *from,
                                char *domain, size_t dlen,
                                char *local_part, size_t llen) {
    domain[0] = '\0';
    local_part[0] = '\0';

    /* Try "Name <user@domain>" format first */
    const char *lt = strchr(from, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    const char *email;
    size_t elen;
    if (lt && gt && gt > lt + 1) {
        email = lt + 1;
        elen  = (size_t)(gt - email);
    } else {
        /* Bare address: skip leading whitespace */
        email = from;
        while (*email == ' ' || *email == '\t') email++;
        elen = strlen(email);
        /* Trim trailing whitespace */
        while (elen > 0 && (email[elen - 1] == ' ' || email[elen - 1] == '\n'
                            || email[elen - 1] == '\r'))
            elen--;
    }

    const char *at = memchr(email, '@', elen);
    if (!at) return;

    size_t ll = (size_t)(at - email);
    size_t dl = elen - ll - 1;
    if (ll >= llen) ll = llen - 1;
    if (dl >= dlen) dl = dlen - 1;
    memcpy(local_part, email, ll);
    local_part[ll] = '\0';
    memcpy(domain, at + 1, dl);
    domain[dl] = '\0';

    /* Lowercase domain */
    for (char *c = domain; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    /* Lowercase local part */
    for (char *c = local_part; *c; c++)
        *c = (char)tolower((unsigned char)*c);
}

int local_index_update(const char *folder, const char *uid, const char *raw_msg) {
    if (!g_account_base[0] || !raw_msg) return -1;

    char ref[512];
    snprintf(ref, sizeof(ref), "%s/%s", folder, uid);

    /* 1. From index: index/from/<domain>/<localpart> */
    RAII_STRING char *from_raw = mime_get_header(raw_msg, "From");
    if (from_raw) {
        char domain[256], local_part[256];
        extract_email_parts(from_raw, domain, sizeof(domain),
                            local_part, sizeof(local_part));
        if (domain[0] && local_part[0]) {
            RAII_STRING char *idx_dir = NULL;
            if (asprintf(&idx_dir, "%s/index/from/%s",
                         g_account_base, domain) != -1)
                index_append(idx_dir, local_part, ref);
        }
    }

    /* 2. Date index: index/date/<year>/<month>/<day> */
    RAII_STRING char *date_raw = mime_get_header(raw_msg, "Date");
    if (date_raw) {
        RAII_STRING char *formatted = mime_format_date(date_raw);
        if (formatted && strlen(formatted) >= 10) {
            int year, month, day;
            if (sscanf(formatted, "%d-%d-%d", &year, &month, &day) == 3) {
                RAII_STRING char *idx_dir = NULL;
                char day_str[4];
                snprintf(day_str, sizeof(day_str), "%02d", day);
                if (asprintf(&idx_dir, "%s/index/date/%04d/%02d",
                             g_account_base, year, month) != -1)
                    index_append(idx_dir, day_str, ref);
            }
        }
    }

    return 0;
}

int local_msg_delete(const char *folder, const char *uid) {
    if (!g_account_base[0]) return -1;

    char ref[512];
    snprintf(ref, sizeof(ref), "%s/%s", folder, uid);

    /* 1. Remove .eml file */
    RAII_STRING char *mpath = msg_path(folder, uid);
    if (mpath) remove(mpath);

    /* 2. Remove .hdr file */
    RAII_STRING char *hpath = hdr_path(folder, uid);
    if (hpath) remove(hpath);

    /* 3. Remove from indexes — best effort scan of from/ and date/ */
    /* For from/: we'd need to know which file has this ref.
     * Since we don't track that, just load the message (if still cached)
     * or accept the stale entry.  A full re-index can clean up. */
    logger_log(LOG_DEBUG, "Deleted %s/%s", folder, uid);
    return 0;
}

/* ── UI preferences ──────────────────────────────────────────────────── */

static char *ui_pref_path(void) {
    const char *data_base = platform_data_dir();
    if (!data_base) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/email-cli/ui.ini", data_base) == -1)
        return NULL;
    return path;
}

int ui_pref_get_int(const char *key, int default_val) {
    RAII_STRING char *path = ui_pref_path();
    if (!path) return default_val;
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return default_val;
    char line[256];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), fp))
        if (strncmp(line, key, klen) == 0 && line[klen] == '=')
            return atoi(line + klen + 1);
    return default_val;
}

int ui_pref_set_int(const char *key, int value) {
    const char *data_base = platform_data_dir();
    if (!data_base) return -1;
    RAII_STRING char *dir = NULL;
    if (asprintf(&dir, "%s/email-cli", data_base) == -1) return -1;
    if (fs_mkdir_p(dir, 0700) != 0) return -1;
    RAII_STRING char *path = ui_pref_path();
    if (!path) return -1;

    char *existing = load_file(path);

    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) { free(existing); return -1; }

    size_t klen = strlen(key);
    if (existing) {
        char *line = existing;
        while (*line) {
            char *nl = strchr(line, '\n');
            size_t llen = nl ? (size_t)(nl - line + 1) : strlen(line);
            if (!(strncmp(line, key, klen) == 0 && line[klen] == '='))
                fwrite(line, 1, llen, fp);
            line += llen;
        }
        free(existing);
    }
    fprintf(fp, "%s=%d\n", key, value);
    logger_log(LOG_DEBUG, "UI pref %s=%d saved", key, value);
    return 0;
}

char *ui_pref_get_str(const char *key) {
    RAII_STRING char *path = ui_pref_path();
    if (!path) return NULL;
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char line[1024];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *val = line + klen + 1;
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r'))
                val[--vlen] = '\0';
            return strdup(val);
        }
    }
    return NULL;
}

int ui_pref_set_str(const char *key, const char *value) {
    const char *data_base = platform_data_dir();
    if (!data_base) return -1;
    RAII_STRING char *dir = NULL;
    if (asprintf(&dir, "%s/email-cli", data_base) == -1) return -1;
    if (fs_mkdir_p(dir, 0700) != 0) return -1;
    RAII_STRING char *path = ui_pref_path();
    if (!path) return -1;

    char *existing = load_file(path);

    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) { free(existing); return -1; }

    size_t klen = strlen(key);
    if (existing) {
        char *line = existing;
        while (*line) {
            char *nl = strchr(line, '\n');
            size_t llen = nl ? (size_t)(nl - line + 1) : strlen(line);
            if (!(strncmp(line, key, klen) == 0 && line[klen] == '='))
                fwrite(line, 1, llen, fp);
            line += llen;
        }
        free(existing);
    }
    fprintf(fp, "%s=%s\n", key, value);
    logger_log(LOG_DEBUG, "UI pref %s=%s saved", key, value);
    return 0;
}

/* ── Folder manifest ─────────────────────────────────────────────────── */

static char *manifest_path(const char *folder) {
    if (!g_account_base[0]) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/manifests/%s.tsv", g_account_base, folder) == -1)
        return NULL;
    return path;
}

/** @brief Duplicates a string, replacing tabs with spaces. */
static char *sanitise(const char *s) {
    if (!s) return strdup("");
    char *d = strdup(s);
    if (d) for (char *p = d; *p; p++) if (*p == '\t') *p = ' ';
    return d;
}

Manifest *manifest_load(const char *folder) {
    RAII_STRING char *path = manifest_path(folder);
    logger_log(LOG_DEBUG, "manifest_load: folder=%s account_base=%s path=%s",
               folder, g_account_base, path ? path : "(null)");
    if (!path) return NULL;

    char *data = load_file(path);
    if (!data) return NULL;

    Manifest *m = calloc(1, sizeof(*m));
    if (!m) { free(data); return NULL; }
    m->capacity = 64;
    m->entries = malloc((size_t)m->capacity * sizeof(ManifestEntry));
    if (!m->entries) { free(m); free(data); return NULL; }

    char *line = data;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Parse: uid\tfrom\tsubject\tdate */
        char *t1 = strchr(line, '\t');
        if (!t1 || t1 == line) {
            line = nl ? nl + 1 : line + strlen(line);
            continue;
        }
        *t1 = '\0';
        char *uid_field = line;
        char *from_start = t1 + 1;
        char *t2 = strchr(from_start, '\t');
        if (!t2) { line = nl ? nl + 1 : line + strlen(line); continue; }
        *t2 = '\0';
        char *subj_start = t2 + 1;
        char *t3 = strchr(subj_start, '\t');
        if (!t3) { line = nl ? nl + 1 : line + strlen(line); continue; }
        *t3 = '\0';
        char *date_start = t3 + 1;
        /* Optional 5th field: unseen flag */
        int unseen_val = 0;
        char *t4 = strchr(date_start, '\t');
        if (t4) {
            *t4 = '\0';
            unseen_val = atoi(t4 + 1);
        }

        if (m->count == m->capacity) {
            m->capacity *= 2;
            ManifestEntry *tmp = realloc(m->entries,
                                         (size_t)m->capacity * sizeof(ManifestEntry));
            if (!tmp) break;
            m->entries = tmp;
        }
        ManifestEntry *e = &m->entries[m->count++];
        snprintf(e->uid, sizeof(e->uid), "%s", uid_field);
        e->from    = strdup(from_start);
        e->subject = strdup(subj_start);
        e->date    = strdup(date_start);
        e->flags   = unseen_val;

        line = nl ? nl + 1 : line + strlen(line);
    }
    free(data);
    return m;
}

int manifest_save(const char *folder, const Manifest *m) {
    if (!g_account_base[0] || !m) return -1;

    RAII_STRING char *dir = NULL;
    if (asprintf(&dir, "%s/manifests", g_account_base) == -1) return -1;
    if (fs_mkdir_p(dir, 0700) != 0) return -1;

    /* For nested folders like "munka/ai" we need the parent dir */
    RAII_STRING char *path = manifest_path(folder);
    if (!path) return -1;

    /* Ensure parent directory exists (folder path may have slashes) */
    char *last_slash = strrchr(path, '/');
    if (last_slash) {
        char saved = *last_slash;
        *last_slash = '\0';
        fs_mkdir_p(path, 0700);
        *last_slash = saved;
    }

    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    for (int i = 0; i < m->count; i++) {
        ManifestEntry *e = &m->entries[i];
        RAII_STRING char *f = sanitise(e->from);
        RAII_STRING char *s = sanitise(e->subject);
        RAII_STRING char *d = sanitise(e->date);
        fprintf(fp, "%s\t%s\t%s\t%s\t%d\n", e->uid, f ? f : "", s ? s : "", d ? d : "", e->flags);
    }
    logger_log(LOG_DEBUG, "Manifest saved: %s (%d entries)", folder, m->count);
    return 0;
}

void manifest_free(Manifest *m) {
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        free(m->entries[i].from);
        free(m->entries[i].subject);
        free(m->entries[i].date);
    }
    free(m->entries);
    free(m);
}

ManifestEntry *manifest_find(const Manifest *m, const char *uid) {
    if (!m) return NULL;
    for (int i = 0; i < m->count; i++)
        if (strcmp(m->entries[i].uid, uid) == 0) return &m->entries[i];
    return NULL;
}

void manifest_upsert(Manifest *m, const char *uid,
                     char *from, char *subject, char *date, int flags) {
    if (!m) return;
    ManifestEntry *existing = manifest_find(m, uid);
    if (existing) {
        free(existing->from);    existing->from    = from;
        free(existing->subject); existing->subject = subject;
        free(existing->date);    existing->date    = date;
        existing->flags = flags;
        return;
    }
    if (m->count == m->capacity) {
        int new_cap = m->capacity ? m->capacity * 2 : 64;
        ManifestEntry *tmp = realloc(m->entries,
                                     (size_t)new_cap * sizeof(ManifestEntry));
        if (!tmp) { free(from); free(subject); free(date); return; }
        m->entries = tmp;
        m->capacity = new_cap;
    }
    ManifestEntry *e = &m->entries[m->count++];
    snprintf(e->uid, sizeof(e->uid), "%s", uid);
    e->from = from; e->subject = subject; e->date = date;
    e->flags = flags;
}

void manifest_retain(Manifest *m, const char (*keep_uids)[17], int keep_count) {
    if (!m) return;
    int dst = 0;
    for (int i = 0; i < m->count; i++) {
        int found = 0;
        for (int j = 0; j < keep_count; j++) {
            if (strcmp(keep_uids[j], m->entries[i].uid) == 0) { found = 1; break; }
        }
        if (found) {
            if (dst != i) m->entries[dst] = m->entries[i];
            dst++;
        } else {
            free(m->entries[i].from);
            free(m->entries[i].subject);
            free(m->entries[i].date);
        }
    }
    m->count = dst;
}

/* ── Folder list cache ───────────────────────────────────────────────── */

int local_folder_list_save(const char **folders, int count, char sep) {
    if (!g_account_base[0]) return -1;
    RAII_STRING char *path = NULL;
    if (asprintf(&path, "%s/folders.cache", g_account_base) == -1) return -1;
    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "sep=%c\n", sep);
    for (int i = 0; i < count; i++)
        fprintf(fp, "%s\n", folders[i] ? folders[i] : "");
    logger_log(LOG_DEBUG, "Folder list cache saved: %d folders", count);
    return 0;
}

char **local_folder_list_load(int *count_out, char *sep_out) {
    *count_out = 0;
    if (!g_account_base[0]) return NULL;
    RAII_STRING char *path = NULL;
    if (asprintf(&path, "%s/folders.cache", g_account_base) == -1) return NULL;
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char line[1024];
    char sep = '.';
    /* First line: sep=<char> */
    if (!fgets(line, sizeof(line), fp)) return NULL;
    if (strncmp(line, "sep=", 4) == 0 && line[4] != '\n')
        sep = line[4];

    int cap = 32, cnt = 0;
    char **folders = malloc((size_t)cap * sizeof(char *));
    if (!folders) return NULL;
    while (fgets(line, sizeof(line), fp)) {
        /* strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;
        if (cnt == cap) {
            cap *= 2;
            char **tmp = realloc(folders, (size_t)cap * sizeof(char *));
            if (!tmp) { for (int i = 0; i < cnt; i++) free(folders[i]); free(folders); return NULL; }
            folders = tmp;
        }
        folders[cnt] = strdup(line);
        if (!folders[cnt]) { for (int i = 0; i < cnt; i++) free(folders[i]); free(folders); return NULL; }
        cnt++;
    }
    *count_out = cnt;
    if (sep_out) *sep_out = sep;
    logger_log(LOG_DEBUG, "Folder list cache loaded: %d folders", cnt);
    return folders;
}

void manifest_count_folder(const char *folder, int *total_out,
                            int *unseen_out, int *flagged_out) {
    *total_out = 0; *unseen_out = 0; *flagged_out = 0;
    Manifest *m = manifest_load(folder);
    if (!m) return;
    *total_out = m->count;
    for (int i = 0; i < m->count; i++) {
        if (m->entries[i].flags & MSG_FLAG_UNSEEN)  (*unseen_out)++;
        if (m->entries[i].flags & MSG_FLAG_FLAGGED) (*flagged_out)++;
    }
    manifest_free(m);
}

/* ── Pending flag changes ─────────────────────────────────────────────── */

static char *pending_flag_path(const char *folder) {
    if (!g_account_base[0]) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/pending_flags/%s.tsv", g_account_base, folder) == -1)
        return NULL;
    return path;
}

int local_pending_flag_add(const char *folder, const char *uid,
                            const char *flag_name, int add) {
    RAII_STRING char *path = pending_flag_path(folder);
    if (!path) return -1;

    /* Ensure parent directory exists (folder path may have slashes) */
    char *dir_end = strrchr(path, '/');
    if (dir_end) {
        char saved = *dir_end;
        *dir_end = '\0';
        fs_mkdir_p(path, 0700);
        *dir_end = saved;
    }

    RAII_FILE FILE *fp = fopen(path, "a");
    if (!fp) return -1;
    fprintf(fp, "%s\t%s\t%d\n", uid, flag_name, add);
    return 0;
}

PendingFlag *local_pending_flag_load(const char *folder, int *count_out) {
    *count_out = 0;
    RAII_STRING char *path = pending_flag_path(folder);
    if (!path) return NULL;

    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    int cap = 16, count = 0;
    PendingFlag *arr = malloc((size_t)cap * sizeof(PendingFlag));
    if (!arr) return NULL;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int add_val;
        char uid_str[17], flag[64];
        if (sscanf(line, "%16[^\t]\t%63[^\t]\t%d", uid_str, flag, &add_val) != 3)
            continue;
        if (count == cap) {
            cap *= 2;
            PendingFlag *tmp = realloc(arr, (size_t)cap * sizeof(PendingFlag));
            if (!tmp) break;
            arr = tmp;
        }
        snprintf(arr[count].uid, sizeof(arr[count].uid), "%s", uid_str);
        arr[count].add = add_val;
        strncpy(arr[count].flag_name, flag, sizeof(arr[count].flag_name) - 1);
        arr[count].flag_name[sizeof(arr[count].flag_name) - 1] = '\0';
        count++;
    }
    *count_out = count;
    return arr;
}

void local_pending_flag_clear(const char *folder) {
    RAII_STRING char *path = pending_flag_path(folder);
    if (path) remove(path);
}

/* ── Gmail label index files (.idx) ──────────────────────────────────── */

#define IDX_RECORD_SIZE 17  /* 16 char UID + '\n' */

/** @brief Returns heap-allocated path to labels/<label>.idx. */
static char *label_idx_path(const char *label) {
    if (!g_account_base[0] || !label) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/labels/%s.idx", g_account_base, label) == -1)
        return NULL;
    return path;
}

/** @brief Ensures the labels/ directory (and any parent for nested labels) exists. */
static int ensure_label_dir(const char *label) {
    RAII_STRING char *path = label_idx_path(label);
    if (!path) return -1;
    /* Find last slash and mkdir_p up to it */
    char *last_slash = strrchr(path, '/');
    if (!last_slash) return -1;
    *last_slash = '\0';
    int rc = fs_mkdir_p(path, 0700);
    return rc;
}

int label_idx_contains(const char *label, const char *uid) {
    char (*arr)[17] = NULL;
    int n = 0;
    if (label_idx_load(label, &arr, &n) != 0 || n == 0) {
        free(arr);
        return 0;
    }

    /* In-memory binary search (file is kept sorted) */
    int lo = 0, hi = n - 1, found = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(arr[mid], uid);
        if (cmp == 0) { found = 1; break; }
        if (cmp < 0) lo = mid + 1;
        else          hi = mid - 1;
    }
    free(arr);
    return found;
}

int label_idx_count(const char *label) {
    char (*arr)[17] = NULL;
    int n = 0;
    label_idx_load(label, &arr, &n);
    free(arr);
    return n;
}

int label_idx_load(const char *label, char (**uids_out)[17], int *count_out) {
    *uids_out  = NULL;
    *count_out = 0;

    RAII_STRING char *path = label_idx_path(label);
    if (!path) return -1;

    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return 0;  /* Empty / nonexistent label → 0 entries, not error */

    /* Use fgets-based reading to handle both old variable-length format
     * (where short Gmail IDs < 16 chars were stored without NUL padding)
     * and new fixed-width format (16 NUL-padded bytes + '\n'). */
    int cap = 256;
    char (*arr)[17] = malloc((size_t)cap * sizeof(char[17]));
    if (!arr) return -1;

    int count = 0;
    char line[64];
    while (fgets(line, sizeof(line), fp)) {
        /* fgets stops at '\n'; strip trailing whitespace/newline */
        size_t len = strlen(line);
        while (len > 0 && ((unsigned char)line[len-1] <= ' '))
            line[--len] = '\0';
        if (len == 0 || len > 16) continue;

        if (count >= cap) {
            cap *= 2;
            char (*tmp)[17] = realloc(arr, (size_t)cap * sizeof(char[17]));
            if (!tmp) { free(arr); return -1; }
            arr = tmp;
        }
        memset(arr[count], 0, sizeof(arr[count]));
        memcpy(arr[count], line, len);
        count++;
    }

    *uids_out  = arr;
    *count_out = count;
    return 0;
}

int label_idx_write(const char *label, const char (*uids)[17], int count) {
    if (ensure_label_dir(label) != 0) return -1;

    RAII_STRING char *path = label_idx_path(label);
    if (!path) return -1;

    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    /* Write fixed-width records: exactly 16 NUL-padded bytes + '\n' = 17 bytes.
     * Short Gmail IDs (< 16 chars) are padded with NUL so the record size is
     * always 17 bytes, preventing embedded newlines on read-back. */
    for (int i = 0; i < count; i++) {
        char padded[17];
        size_t uid_len = strlen(uids[i]);
        if (uid_len > 16) uid_len = 16;
        memset(padded, 0, 16);
        memcpy(padded, uids[i], uid_len);
        padded[16] = '\n';
        if (fwrite(padded, 1, 17, fp) != 17) return -1;
    }

    logger_log(LOG_DEBUG, "label_idx_write: %s → %d entries", label, count);
    return 0;
}

char *local_hdr_get_labels(const char *folder, const char *uid) {
    char *hdr = local_hdr_load(folder, uid);
    if (!hdr) return NULL;

    /* Parse 4th tab-separated field: from\tsubject\tdate\tLABELS\tflags */
    const char *p = hdr;
    for (int t = 0; t < 3; t++) {
        p = strchr(p, '\t');
        if (!p) { free(hdr); return NULL; }
        p++;
    }
    /* p now points to the start of the labels field */
    const char *end = strchr(p, '\t');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char *result = strndup(p, len);
    free(hdr);
    return result;
}

int label_idx_list(char ***labels_out, int *count_out) {
    *labels_out = NULL;
    *count_out  = 0;

    char dir_path[8300];
    snprintf(dir_path, sizeof(dir_path), "%s/labels", g_account_base);

    RAII_DIR DIR *dp = opendir(dir_path);
    if (!dp) return 0;  /* No labels directory → 0 labels */

    char **list = NULL;
    int count = 0, cap = 0;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen <= 4) continue;
        if (strcmp(name + nlen - 4, ".idx") != 0) continue;

        /* Extract label name (strip .idx) */
        char *label = strndup(name, nlen - 4);
        if (!label) continue;

        if (count == cap) {
            int newcap = cap ? cap * 2 : 16;
            char **tmp = realloc(list, (size_t)newcap * sizeof(char *));
            if (!tmp) { free(label); break; }
            list = tmp;
            cap = newcap;
        }
        list[count++] = label;
    }

    *labels_out = list;
    *count_out  = count;
    return 0;
}

int label_idx_add(const char *label, const char *uid) {
    if (!uid || strlen(uid) < 1) return -1;

    /* Load existing entries */
    char (*existing)[17] = NULL;
    int ecount = 0;
    label_idx_load(label, &existing, &ecount);

    /* Check if already present (binary search) */
    int lo = 0, hi = ecount - 1, insert_pos = ecount;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(existing[mid], uid);
        if (cmp == 0) { free(existing); return 0; }  /* Already present */
        if (cmp < 0) lo = mid + 1;
        else { insert_pos = mid; hi = mid - 1; }
    }
    if (lo < ecount && insert_pos == ecount) insert_pos = lo;

    /* Build new array with uid inserted at insert_pos */
    int newcount = ecount + 1;
    char (*arr)[17] = malloc((size_t)newcount * sizeof(char[17]));
    if (!arr) { free(existing); return -1; }

    if (insert_pos > 0 && existing)
        memcpy(arr, existing, (size_t)insert_pos * sizeof(char[17]));
    snprintf(arr[insert_pos], 17, "%.16s", uid);
    if (insert_pos < ecount && existing)
        memcpy(arr + insert_pos + 1, existing + insert_pos,
               (size_t)(ecount - insert_pos) * sizeof(char[17]));
    free(existing);

    int rc = label_idx_write(label, (const char (*)[17])arr, newcount);
    free(arr);
    return rc;
}

int label_idx_remove(const char *label, const char *uid) {
    if (!uid) return -1;

    char (*existing)[17] = NULL;
    int ecount = 0;
    label_idx_load(label, &existing, &ecount);
    if (!existing || ecount == 0) { free(existing); return 0; }

    /* Find uid with binary search */
    int lo = 0, hi = ecount - 1, found = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(existing[mid], uid);
        if (cmp == 0) { found = mid; break; }
        if (cmp < 0) lo = mid + 1;
        else          hi = mid - 1;
    }

    if (found < 0) { free(existing); return 0; }  /* Not present */

    /* Shift down */
    if (found < ecount - 1)
        memmove(existing + found, existing + found + 1,
                (size_t)(ecount - found - 1) * sizeof(char[17]));

    int rc = label_idx_write(label, (const char (*)[17])existing, ecount - 1);
    free(existing);
    return rc;
}

/* ── Gmail history ID ─────────────────────────────────────────────── */

/* ── Trash label backup (for untrash restore) ────────────────────── */

static char *trash_labels_path(const char *uid) {
    if (!g_account_base[0] || !uid) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/trash_labels/%s.lbl", g_account_base, uid) == -1)
        return NULL;
    return path;
}

int local_trash_labels_save(const char *uid, const char *labels) {
    if (!uid || !labels) return -1;
    /* Ensure directory exists */
    char dir[8300];
    snprintf(dir, sizeof(dir), "%s/trash_labels", g_account_base);
    fs_mkdir_p(dir, 0700);

    RAII_STRING char *path = trash_labels_path(uid);
    if (!path) return -1;
    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s\n", labels);
    return 0;
}

char *local_trash_labels_load(const char *uid) {
    RAII_STRING char *path = trash_labels_path(uid);
    if (!path) return NULL;
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char buf[4096];
    if (!fgets(buf, (int)sizeof(buf), fp)) return NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    return strdup(buf);
}

void local_trash_labels_remove(const char *uid) {
    RAII_STRING char *path = trash_labels_path(uid);
    if (path) unlink(path);
}

int local_gmail_label_names_save(char **ids, char **names, int count) {
    if (!g_account_base[0]) return -1;
    if (fs_mkdir_p(g_account_base, 0700) != 0) return -1;
    RAII_STRING char *path = NULL;
    if (asprintf(&path, "%s/gmail_label_names", g_account_base) == -1) return -1;
    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    for (int i = 0; i < count; i++)
        fprintf(fp, "%s\t%s\n", ids[i], names[i]);
    return 0;
}

char *local_gmail_label_name_lookup(const char *id) {
    if (!g_account_base[0] || !id) return NULL;
    RAII_STRING char *path = NULL;
    if (asprintf(&path, "%s/gmail_label_names", g_account_base) == -1) return NULL;
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char buf[1024];
    while (fgets(buf, (int)sizeof(buf), fp)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        char *tab = strchr(buf, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (strcmp(buf, id) == 0)
            return strdup(tab + 1);
    }
    return NULL;
}

char *local_gmail_label_id_lookup(const char *name) {
    if (!g_account_base[0] || !name) return NULL;
    RAII_STRING char *path = NULL;
    if (asprintf(&path, "%s/gmail_label_names", g_account_base) == -1) return NULL;
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char buf[1024];
    while (fgets(buf, (int)sizeof(buf), fp)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        char *tab = strchr(buf, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (strcasecmp(tab + 1, name) == 0)
            return strdup(buf); /* return the ID */
    }
    return NULL;
}

int local_gmail_history_save(const char *history_id) {
    if (!g_account_base[0] || !history_id) return -1;
    if (fs_mkdir_p(g_account_base, 0700) != 0) return -1;
    RAII_STRING char *path = NULL;
    if (asprintf(&path, "%s/gmail_history_id", g_account_base) == -1) return -1;
    return write_file(path, history_id, strlen(history_id));
}

char *local_gmail_history_load(void) {
    if (!g_account_base[0]) return NULL;
    RAII_STRING char *path = NULL;
    if (asprintf(&path, "%s/gmail_history_id", g_account_base) == -1) return NULL;
    char *data = load_file(path);
    if (!data) return NULL;
    /* Trim trailing whitespace */
    size_t len = strlen(data);
    while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r' || data[len-1] == ' '))
        data[--len] = '\0';
    return data;
}
