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

/* ── Account base path (set by local_store_init) ─────────────────────── */

static char g_account_base[8192];

int local_store_init(const char *host_url) {
    const char *data_base = platform_data_dir();
    if (!data_base || !host_url) return -1;

    /* Extract hostname from URL: imaps://host:port → host */
    const char *p = strstr(host_url, "://");
    p = p ? p + 3 : host_url;

    char hostname[512];
    int i = 0;
    while (*p && *p != ':' && *p != '/' && i < (int)sizeof(hostname) - 1)
        hostname[i++] = *p++;
    hostname[i] = '\0';

    /* Lowercase the hostname */
    for (char *c = hostname; *c; c++) *c = (char)tolower((unsigned char)*c);

    snprintf(g_account_base, sizeof(g_account_base),
             "%s/email-cli/accounts/imap.%s", data_base, hostname);

    logger_log(LOG_DEBUG, "local_store: account base = %s", g_account_base);
    return 0;
}

/* ── Reverse digit bucketing helpers ─────────────────────────────────── */

static char digit1(int uid) { return (char)('0' + (uid % 10)); }
static char digit2(int uid) { return (char)('0' + ((uid / 10) % 10)); }

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
static int ensure_bucket_dir(const char *area, const char *folder, int uid) {
    RAII_STRING char *dir = NULL;
    if (asprintf(&dir, "%s/%s/%s/%c/%c",
                 g_account_base, area, folder, digit1(uid), digit2(uid)) == -1)
        return -1;
    return fs_mkdir_p(dir, 0700);
}

/* ── Message store ───────────────────────────────────────────────────── */

static char *msg_path(const char *folder, int uid) {
    if (!g_account_base[0]) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/store/%s/%c/%c/%d.eml",
                 g_account_base, folder, digit1(uid), digit2(uid), uid) == -1)
        return NULL;
    return path;
}

int local_msg_exists(const char *folder, int uid) {
    RAII_STRING char *path = msg_path(folder, uid);
    if (!path) return 0;
    RAII_FILE FILE *fp = fopen(path, "r");
    return fp != NULL;
}

int local_msg_save(const char *folder, int uid, const char *content, size_t len) {
    if (!g_account_base[0]) return -1;
    if (ensure_bucket_dir("store", folder, uid) != 0) {
        logger_log(LOG_ERROR, "Failed to create store bucket for %s/%d", folder, uid);
        return -1;
    }
    RAII_STRING char *path = msg_path(folder, uid);
    if (!path) return -1;
    if (write_file(path, content, len) != 0) {
        logger_log(LOG_ERROR, "Failed to write store file: %s", path);
        return -1;
    }
    logger_log(LOG_DEBUG, "Stored %s/%d at %s", folder, uid, path);
    return 0;
}

char *local_msg_load(const char *folder, int uid) {
    RAII_STRING char *path = msg_path(folder, uid);
    if (!path) return NULL;
    return load_file(path);
}

/* ── Header store ────────────────────────────────────────────────────── */

static char *hdr_path(const char *folder, int uid) {
    if (!g_account_base[0]) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/headers/%s/%c/%c/%d.hdr",
                 g_account_base, folder, digit1(uid), digit2(uid), uid) == -1)
        return NULL;
    return path;
}

int local_hdr_exists(const char *folder, int uid) {
    RAII_STRING char *path = hdr_path(folder, uid);
    if (!path) return 0;
    RAII_FILE FILE *fp = fopen(path, "r");
    return fp != NULL;
}

int local_hdr_save(const char *folder, int uid, const char *content, size_t len) {
    if (!g_account_base[0]) return -1;
    if (ensure_bucket_dir("headers", folder, uid) != 0) {
        logger_log(LOG_ERROR, "Failed to create header bucket for %s/%d", folder, uid);
        return -1;
    }
    RAII_STRING char *path = hdr_path(folder, uid);
    if (!path) return -1;
    if (write_file(path, content, len) != 0) return -1;
    logger_log(LOG_DEBUG, "Stored header %s/%d", folder, uid);
    return 0;
}

char *local_hdr_load(const char *folder, int uid) {
    RAII_STRING char *path = hdr_path(folder, uid);
    if (!path) return NULL;
    return load_file(path);
}

static int cmp_int_evict(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

void local_hdr_evict_stale(const char *folder,
                             const int *keep_uids, int keep_count) {
    if (!g_account_base[0]) return;

    int *sorted = malloc((size_t)keep_count * sizeof(int));
    if (!sorted) return;
    memcpy(sorted, keep_uids, (size_t)keep_count * sizeof(int));
    qsort(sorted, (size_t)keep_count, sizeof(int), cmp_int_evict);

    /* Walk all 100 buckets (10 × 10) */
    for (int d1 = 0; d1 <= 9; d1++) {
        for (int d2 = 0; d2 <= 9; d2++) {
            RAII_STRING char *dir = NULL;
            if (asprintf(&dir, "%s/headers/%s/%d/%d",
                         g_account_base, folder, d1, d2) == -1)
                continue;

            DIR *d = opendir(dir);
            if (!d) continue;

            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                const char *name = ent->d_name;
                const char *dot  = strrchr(name, '.');
                if (!dot || strcmp(dot, ".hdr") != 0) continue;
                char *end;
                long uid = strtol(name, &end, 10);
                if (end != dot || uid <= 0) continue;
                int key = (int)uid;
                if (!bsearch(&key, sorted, (size_t)keep_count,
                             sizeof(int), cmp_int_evict)) {
                    RAII_STRING char *path = NULL;
                    if (asprintf(&path, "%s/%s", dir, name) != -1) {
                        remove(path);
                        logger_log(LOG_DEBUG,
                                   "Evicted stale header: UID %ld in %s", uid, folder);
                    }
                }
            }
            closedir(d);
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

    FILE *fp = fopen(path, "a");
    if (!fp) return -1;
    fprintf(fp, "%s\n", ref);
    fclose(fp);
    return 0;
}

/** @brief Removes a reference from an index file. */
__attribute__((unused))
static void index_remove_ref(const char *path, const char *ref) {
    char *content = load_file(path);
    if (!content) return;

    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) { free(content); return; }

    size_t ref_len = strlen(ref);
    char *p = content;
    while (*p) {
        char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p + 1) : strlen(p);
        /* Skip the matching line */
        if (!(strncmp(p, ref, ref_len) == 0 &&
              (p[ref_len] == '\n' || p[ref_len] == '\0')))
            fwrite(p, 1, llen, fp);
        p += llen;
    }
    free(content);
}

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

int local_index_update(const char *folder, int uid, const char *raw_msg) {
    if (!g_account_base[0] || !raw_msg) return -1;

    char ref[512];
    snprintf(ref, sizeof(ref), "%s/%d", folder, uid);

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

int local_msg_delete(const char *folder, int uid) {
    if (!g_account_base[0]) return -1;

    char ref[512];
    snprintf(ref, sizeof(ref), "%s/%d", folder, uid);

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
    logger_log(LOG_DEBUG, "Deleted %s/%d", folder, uid);
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
        char *end;
        long uid = strtol(line, &end, 10);
        if (end == line || *end != '\t' || uid <= 0) {
            line = nl ? nl + 1 : line + strlen(line);
            continue;
        }
        char *from_start = end + 1;
        char *t2 = strchr(from_start, '\t');
        if (!t2) { line = nl ? nl + 1 : line + strlen(line); continue; }
        *t2 = '\0';
        char *subj_start = t2 + 1;
        char *t3 = strchr(subj_start, '\t');
        if (!t3) { line = nl ? nl + 1 : line + strlen(line); continue; }
        *t3 = '\0';
        char *date_start = t3 + 1;

        if (m->count == m->capacity) {
            m->capacity *= 2;
            ManifestEntry *tmp = realloc(m->entries,
                                         (size_t)m->capacity * sizeof(ManifestEntry));
            if (!tmp) break;
            m->entries = tmp;
        }
        ManifestEntry *e = &m->entries[m->count++];
        e->uid     = (int)uid;
        e->from    = strdup(from_start);
        e->subject = strdup(subj_start);
        e->date    = strdup(date_start);

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
        fprintf(fp, "%d\t%s\t%s\t%s\n", e->uid, f ? f : "", s ? s : "", d ? d : "");
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

ManifestEntry *manifest_find(const Manifest *m, int uid) {
    if (!m) return NULL;
    for (int i = 0; i < m->count; i++)
        if (m->entries[i].uid == uid) return &m->entries[i];
    return NULL;
}

void manifest_upsert(Manifest *m, int uid,
                     char *from, char *subject, char *date) {
    if (!m) return;
    ManifestEntry *existing = manifest_find(m, uid);
    if (existing) {
        free(existing->from);    existing->from    = from;
        free(existing->subject); existing->subject = subject;
        free(existing->date);    existing->date    = date;
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
    e->uid = uid; e->from = from; e->subject = subject; e->date = date;
}

void manifest_retain(Manifest *m, const int *keep_uids, int keep_count) {
    if (!m) return;
    int dst = 0;
    for (int i = 0; i < m->count; i++) {
        int found = 0;
        for (int j = 0; j < keep_count; j++) {
            if (keep_uids[j] == m->entries[i].uid) { found = 1; break; }
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
