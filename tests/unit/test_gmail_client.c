#include "test_helpers.h"
#include "gmail_client.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef ENABLE_GCOV
extern void __gcov_dump(void);
#  define GCOV_FLUSH() __gcov_dump()
#else
#  define GCOV_FLUSH() ((void)0)
#endif

/* ── gmail_connect — error paths ──────────────────────────────────── */

static void test_connect_not_gmail(void) {
    Config cfg = {0};
    /* gmail_mode is 0 → should fail */
    GmailClient *c = gmail_connect(&cfg);
    ASSERT(c == NULL, "connect: fails for non-Gmail account");
}

static void test_connect_no_token(void) {
    Config cfg = {0};
    cfg.gmail_mode = 1;
    /* No refresh_token → auth_refresh fails → connect fails */
    GmailClient *c = gmail_connect(&cfg);
    ASSERT(c == NULL, "connect: fails with no refresh_token");
}

static void test_disconnect_null(void) {
    /* Should not crash */
    gmail_disconnect(NULL);
    ASSERT(1, "disconnect NULL: no crash");
}

/* ── base64url encode/decode ───────────────────────────────────────── */

static void test_b64_roundtrip(void) {
    const char *orig = "Hello, Gmail API!";
    char *enc = gmail_base64url_encode((const unsigned char *)orig, strlen(orig));
    ASSERT(enc != NULL, "b64 encode: not NULL");

    size_t dec_len = 0;
    char *dec = gmail_base64url_decode(enc, strlen(enc), &dec_len);
    ASSERT(dec != NULL, "b64 decode: not NULL");
    ASSERT(dec_len == strlen(orig), "b64 roundtrip: length matches");
    ASSERT(memcmp(dec, orig, dec_len) == 0, "b64 roundtrip: content matches");
    free(enc);
    free(dec);
}

static void test_b64_empty(void) {
    char *enc = gmail_base64url_encode((const unsigned char *)"", 0);
    ASSERT(enc != NULL, "b64 encode empty: not NULL");
    ASSERT(enc[0] == '\0', "b64 encode empty: empty string");

    size_t dec_len = 0;
    char *dec = gmail_base64url_decode("", 0, &dec_len);
    ASSERT(dec != NULL, "b64 decode empty: not NULL");
    ASSERT(dec_len == 0, "b64 decode empty: zero length");
    free(enc);
    free(dec);
}

static void test_b64_known_vector(void) {
    /* "Man" → TWFu in standard base64, same in base64url */
    size_t len = 0;
    char *dec = gmail_base64url_decode("TWFu", 4, &len);
    ASSERT(dec != NULL && len == 3, "b64 known: length=3");
    ASSERT(memcmp(dec, "Man", 3) == 0, "b64 known: Man");
    free(dec);
}

static void test_b64_url_chars(void) {
    /* Verify - and _ (base64url) instead of + and / */
    unsigned char data[] = {0xfb, 0xff, 0xfe};
    char *enc = gmail_base64url_encode(data, 3);
    ASSERT(enc != NULL, "b64url chars: not NULL");
    ASSERT(strchr(enc, '+') == NULL, "b64url: no +");
    ASSERT(strchr(enc, '/') == NULL, "b64url: no /");
    ASSERT(strchr(enc, '=') == NULL, "b64url: no padding");
    free(enc);
}

static void test_b64_decode_null_len_out(void) {
    /* NULL out_len should not crash */
    char *dec = gmail_base64url_decode("TWFu", 4, NULL);
    ASSERT(dec != NULL, "b64 decode null len_out: not NULL");
    free(dec);
}

static void test_b64_large_roundtrip(void) {
    /* 256 bytes of binary data */
    unsigned char data[256];
    for (int i = 0; i < 256; i++) data[i] = (unsigned char)i;
    char *enc = gmail_base64url_encode(data, 256);
    ASSERT(enc != NULL, "b64 large encode: not NULL");
    size_t out_len = 0;
    char *dec = gmail_base64url_decode(enc, strlen(enc), &out_len);
    ASSERT(dec != NULL, "b64 large decode: not NULL");
    ASSERT(out_len == 256, "b64 large: length matches");
    free(enc);
    free(dec);
}

/* ── Mock HTTP server helpers ─────────────────────────────────────── */

/*
 * Create a listening TCP socket bound to a random loopback port.
 * Returns the fd and fills *port_out with the actual port number.
 */
static int make_mock_listener(int *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    *port_out = ntohs(addr.sin_port);
    return fd;
}

/* Base64url encode helper for mock server (same logic as production) */
static const char mock_b64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char *mock_b64url_encode(const char *data, size_t len) {
    size_t alloc = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(alloc);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned int)(unsigned char)data[i]) << 16;
        if (i + 1 < len) n |= ((unsigned int)(unsigned char)data[i+1]) << 8;
        if (i + 2 < len) n |= ((unsigned int)(unsigned char)data[i+2]);
        out[o++] = mock_b64url_chars[(n >> 18) & 0x3F];
        out[o++] = mock_b64url_chars[(n >> 12) & 0x3F];
        if (i + 1 < len) out[o++] = mock_b64url_chars[(n >> 6) & 0x3F];
        if (i + 2 < len) out[o++] = mock_b64url_chars[n & 0x3F];
    }
    out[o] = '\0';
    return out;
}

static void mock_send_json(int fd, int status_code, const char *body) {
    const char *reason = (status_code == 200) ? "OK" :
                         (status_code == 204) ? "No Content" :
                         (status_code == 401) ? "Unauthorized" :
                         (status_code == 404) ? "Not Found" : "Error";
    char header[512];
    size_t body_len = body ? strlen(body) : 0;
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             status_code, reason, body_len);
    ssize_t r;
    r = write(fd, header, strlen(header)); (void)r;
    if (body && body_len > 0) {
        r = write(fd, body, body_len); (void)r;
    }
}

/* Reads HTTP request headers into buf, returns bytes read */
static int mock_read_request(int fd, char *buf, int bufsz) {
    int total = 0;
    while (total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(bufsz - total - 1));
        if (n <= 0) break;
        total += (int)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[total] = '\0';
    return total;
}

/*
 * Dispatch one HTTP request and send a response based on the path.
 * This mock handles all Gmail API paths used by gmail_client.c.
 */
static void mock_handle_one(int fd) {
    char buf[8192];
    if (mock_read_request(fd, buf, (int)sizeof(buf)) <= 0) return;

    char method[16] = {0};
    char path[2048] = {0};
    if (sscanf(buf, "%15s %2047s", method, path) != 2) return;

    /* POST /token (auth refresh — used by 401 retry tests) */
    if (strstr(path, "/token")) {
        mock_send_json(fd, 200, "{\"access_token\":\"new_token_after_401\",\"expires_in\":3600}");
        return;
    }

    /* DELETE /labels/{id} */
    if (strstr(path, "/labels/") && strcmp(method, "DELETE") == 0) {
        mock_send_json(fd, 204, NULL);
        return;
    }

    /* POST /labels — create label */
    if (strstr(path, "/labels") && strcmp(method, "POST") == 0) {
        mock_send_json(fd, 200,
            "{\"id\":\"Label_Test001\","
            "\"name\":\"TestLabel\","
            "\"type\":\"user\"}");
        return;
    }

    /* GET /labels */
    if (strstr(path, "/labels") && strcmp(method, "GET") == 0) {
        mock_send_json(fd, 200,
            "{\"labels\":["
            "{\"id\":\"INBOX\",\"name\":\"INBOX\"},"
            "{\"id\":\"UNREAD\",\"name\":\"UNREAD\"},"
            "{\"id\":\"Work\",\"name\":\"Work\"}"
            "]}");
        return;
    }

    /* GET /profile */
    if (strstr(path, "/profile")) {
        mock_send_json(fd, 200,
            "{\"historyId\":\"12345\","
            "\"emailAddress\":\"test@gmail.com\"}");
        return;
    }

    /* GET /history */
    if (strstr(path, "/history")) {
        mock_send_json(fd, 200,
            "{\"historyId\":\"12346\","
            "\"history\":[]}");
        return;
    }

    /* POST /messages/{id}/modify */
    if (strstr(path, "/modify") && strcmp(method, "POST") == 0) {
        mock_send_json(fd, 200,
            "{\"id\":\"msg001\",\"labelIds\":[\"INBOX\"]}");
        return;
    }

    /* POST /messages/{id}/trash */
    if (strstr(path, "/trash") && strcmp(method, "POST") == 0) {
        mock_send_json(fd, 200,
            "{\"id\":\"msg001\",\"labelIds\":[\"TRASH\"]}");
        return;
    }

    /* POST /messages/{id}/untrash */
    if (strstr(path, "/untrash") && strcmp(method, "POST") == 0) {
        mock_send_json(fd, 200,
            "{\"id\":\"msg001\",\"labelIds\":[\"INBOX\"]}");
        return;
    }

    /* POST /messages/send */
    if (strstr(path, "/messages/send") && strcmp(method, "POST") == 0) {
        mock_send_json(fd, 200,
            "{\"id\":\"sent001\",\"labelIds\":[\"SENT\"]}");
        return;
    }

    /* GET /messages/{id}?format=raw — single message fetch */
    if (strstr(path, "/messages/") && strcmp(method, "GET") == 0) {
        const char *raw_email =
            "From: test@example.com\r\n"
            "To: me@gmail.com\r\n"
            "Subject: Test Message\r\n"
            "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n"
            "\r\n"
            "Hello, this is a test message body.\r\n";
        char *raw_b64 = mock_b64url_encode(raw_email, strlen(raw_email));
        if (!raw_b64) { mock_send_json(fd, 500, "{}"); return; }
        char body_buf[4096];
        snprintf(body_buf, sizeof(body_buf),
            "{\"id\":\"msg001\","
            "\"threadId\":\"thread001\","
            "\"labelIds\":[\"INBOX\",\"UNREAD\",\"STARRED\"],"
            "\"raw\":\"%s\"}",
            raw_b64);
        free(raw_b64);
        mock_send_json(fd, 200, body_buf);
        return;
    }

    /* GET /messages?... — list messages */
    if (strstr(path, "/messages") && strcmp(method, "GET") == 0) {
        mock_send_json(fd, 200,
            "{\"messages\":["
            "{\"id\":\"msg001\",\"threadId\":\"thread001\"},"
            "{\"id\":\"msg002\",\"threadId\":\"thread002\"}"
            "],\"resultSizeEstimate\":2,\"historyId\":\"12345\"}");
        return;
    }

    mock_send_json(fd, 404, "{}");
}

/*
 * Run the mock HTTP server child process.
 * Handles exactly `count` connections then exits.
 */
static void run_mock_http_server(int listen_fd, int count) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        mock_handle_one(cfd);
        close(cfd);
    }
    close(listen_fd);
    GCOV_FLUSH();
    _exit(0);
}

/* Start mock server in a forked child. Returns child PID or -1. */
static pid_t start_mock_server(int *port_out, int connection_count) {
    int listen_fd = make_mock_listener(port_out);
    if (listen_fd < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(listen_fd); return -1; }
    if (pid == 0) {
        run_mock_http_server(listen_fd, connection_count);
        /* unreachable */
    }
    close(listen_fd);
    return pid;
}

/* Build a connected GmailClient pointing at a local mock HTTP server. */
static GmailClient *make_test_client(int port) {
    /* Set test token and API base URL */
    char api_base[128];
    snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/gmail/v1/users/me", port);
    setenv("GMAIL_TEST_TOKEN", "test_access_token_12345", 1);
    setenv("GMAIL_API_BASE_URL", api_base, 1);

    Config cfg = {0};
    cfg.gmail_mode = 1;
    cfg.gmail_refresh_token = "fake_refresh_token";

    GmailClient *c = gmail_connect(&cfg);
    return c;
}

static void wait_child(pid_t pid) {
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

/* ── Tests using the mock HTTP server ─────────────────────────────── */

static void test_connect_with_test_token(void) {
    /* GMAIL_TEST_TOKEN allows connect without a real refresh */
    setenv("GMAIL_TEST_TOKEN", "test_token_abc", 1);
    setenv("GMAIL_API_BASE_URL", "http://127.0.0.1:1/gmail/v1/users/me", 1);

    Config cfg = {0};
    cfg.gmail_mode = 1;
    cfg.gmail_refresh_token = "any";

    GmailClient *c = gmail_connect(&cfg);
    ASSERT(c != NULL, "connect with GMAIL_TEST_TOKEN: succeeds");
    gmail_disconnect(c);

    unsetenv("GMAIL_TEST_TOKEN");
    unsetenv("GMAIL_API_BASE_URL");
}

static void test_set_progress(void) {
    setenv("GMAIL_TEST_TOKEN", "tok", 1);
    setenv("GMAIL_API_BASE_URL", "http://127.0.0.1:1/gmail/v1/users/me", 1);

    Config cfg = {0};
    cfg.gmail_mode = 1;
    cfg.gmail_refresh_token = "any";
    GmailClient *c = gmail_connect(&cfg);
    ASSERT(c != NULL, "set_progress: client created");

    /* NULL client should not crash */
    gmail_set_progress(NULL, NULL, NULL);

    /* set_progress with valid client */
    gmail_set_progress(c, NULL, NULL);

    gmail_disconnect(c);
    unsetenv("GMAIL_TEST_TOKEN");
    unsetenv("GMAIL_API_BASE_URL");
}

static void test_list_labels(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "list_labels: could not start mock server"); return; }

    usleep(20000); /* let child bind */

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "list_labels: client connected");

    char **names = NULL, **ids = NULL;
    int count = 0;
    int rc = gmail_list_labels(c, &names, &ids, &count);
    ASSERT(rc == 0, "list_labels: returns 0");
    ASSERT(count >= 1, "list_labels: at least one label");

    for (int i = 0; i < count; i++) { free(names[i]); free(ids[i]); }
    free(names);
    free(ids);
    gmail_disconnect(c);
    wait_child(pid);
}

static void test_list_messages(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "list_messages: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "list_messages: client connected");

    char (*uids)[17] = NULL;
    int count = 0;
    int rc = gmail_list_messages(c, "INBOX", NULL, &uids, &count, NULL);
    ASSERT(rc == 0, "list_messages: returns 0");
    ASSERT(count >= 1, "list_messages: at least one message");

    free(uids);
    gmail_disconnect(c);
    wait_child(pid);
}

static void test_list_messages_with_query(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "list_messages_query: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "list_messages_query: client connected");

    char (*uids)[17] = NULL;
    int count = 0;
    char *history_id = NULL;
    int rc = gmail_list_messages(c, NULL, "is:unread", &uids, &count, &history_id);
    ASSERT(rc == 0, "list_messages_query: returns 0");

    free(uids);
    free(history_id);
    gmail_disconnect(c);
    wait_child(pid);
}

static void test_fetch_message(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "fetch_message: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "fetch_message: client connected");

    char **labels = NULL;
    int label_count = 0;
    char *body = gmail_fetch_message(c, "msg001", &labels, &label_count);
    ASSERT(body != NULL, "fetch_message: body not NULL");
    ASSERT(label_count >= 1, "fetch_message: has labels");
    ASSERT(strstr(body, "Test Message") != NULL, "fetch_message: contains subject");

    for (int i = 0; i < label_count; i++) free(labels[i]);
    free(labels);
    free(body);
    gmail_disconnect(c);
    wait_child(pid);
}

static void test_fetch_message_no_labels(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "fetch_msg_nolabels: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "fetch_msg_nolabels: client connected");

    /* Pass NULL for labels_out, NULL for label_count_out */
    char *body = gmail_fetch_message(c, "msg001", NULL, NULL);
    ASSERT(body != NULL, "fetch_msg_nolabels: body not NULL");

    free(body);
    gmail_disconnect(c);
    wait_child(pid);
}

static void test_modify_labels(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "modify_labels: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "modify_labels: client connected");

    const char *add[]    = { "STARRED" };
    const char *remove[] = { "UNREAD" };
    int rc = gmail_modify_labels(c, "msg001", add, 1, remove, 1);
    ASSERT(rc == 0, "modify_labels: returns 0");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_modify_labels_add_only(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "modify_labels_add: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "modify_labels_add: client connected");

    const char *add[] = { "INBOX" };
    int rc = gmail_modify_labels(c, "msg001", add, 1, NULL, 0);
    ASSERT(rc == 0, "modify_labels_add: returns 0");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_modify_labels_remove_only(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "modify_labels_rm: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "modify_labels_rm: client connected");

    const char *rm[] = { "UNREAD" };
    int rc = gmail_modify_labels(c, "msg001", NULL, 0, rm, 1);
    ASSERT(rc == 0, "modify_labels_rm: returns 0");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_trash(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "trash: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "trash: client connected");

    int rc = gmail_trash(c, "msg001");
    ASSERT(rc == 0, "trash: returns 0");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_untrash(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "untrash: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "untrash: client connected");

    int rc = gmail_untrash(c, "msg001");
    ASSERT(rc == 0, "untrash: returns 0");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_send(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "send: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "send: client connected");

    const char *raw_msg =
        "From: me@gmail.com\r\n"
        "To: you@example.com\r\n"
        "Subject: Test\r\n"
        "\r\n"
        "Hello!\r\n";
    int rc = gmail_send(c, raw_msg, strlen(raw_msg));
    ASSERT(rc == 0, "send: returns 0");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_get_history_id(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "get_history_id: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "get_history_id: client connected");

    char *hid = gmail_get_history_id(c);
    ASSERT(hid != NULL, "get_history_id: not NULL");
    ASSERT(strlen(hid) > 0, "get_history_id: non-empty");
    free(hid);

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_get_history(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "get_history: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "get_history: client connected");

    char *resp = gmail_get_history(c, "12345");
    ASSERT(resp != NULL, "get_history: not NULL");
    free(resp);

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_create_label(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "create_label: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "create_label: client connected");

    char *id_out = NULL;
    int rc = gmail_create_label(c, "MyNewLabel", &id_out);
    ASSERT(rc == 0, "create_label: returns 0");
    ASSERT(id_out != NULL, "create_label: id_out not NULL");
    free(id_out);

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_create_label_no_id_out(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "create_label_noid: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "create_label_noid: client connected");

    int rc = gmail_create_label(c, "AnotherLabel", NULL);
    ASSERT(rc == 0, "create_label_noid: returns 0");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_delete_label(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "delete_label: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "delete_label: client connected");

    int rc = gmail_delete_label(c, "Label_Test001");
    ASSERT(rc == 0, "delete_label: returns 0");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_list_messages_with_history_id(void) {
    int port = 0;
    pid_t pid = start_mock_server(&port, 1);
    if (pid < 0) { ASSERT(0, "list_msg_histid: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "list_msg_histid: client connected");

    char (*uids)[17] = NULL;
    int count = 0;
    char *history_id = NULL;
    int rc = gmail_list_messages(c, "INBOX", NULL, &uids, &count, &history_id);
    ASSERT(rc == 0, "list_msg_histid: returns 0");
    ASSERT(history_id != NULL, "list_msg_histid: history_id not NULL");
    free(uids);
    free(history_id);

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Error path: HTTP 404 for message fetch ───────────────────────── */

/*
 * Mock server that returns 404 for message requests.
 */
static void run_404_server(int listen_fd, int count) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[2048];
        mock_read_request(cfd, buf, (int)sizeof(buf));
        mock_send_json(cfd, 404, "{\"error\":{\"code\":404}}");
        close(cfd);
    }
    close(listen_fd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_404_server(int *port_out, int count) {
    int listen_fd = make_mock_listener(port_out);
    if (listen_fd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(listen_fd); return -1; }
    if (pid == 0) { run_404_server(listen_fd, count); }
    close(listen_fd);
    return pid;
}

static void test_fetch_message_404(void) {
    int port = 0;
    pid_t pid = start_404_server(&port, 1);
    if (pid < 0) { ASSERT(0, "fetch_404: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "fetch_404: client connected");

    char *body = gmail_fetch_message(c, "nonexistent", NULL, NULL);
    ASSERT(body == NULL, "fetch_404: returns NULL on 404");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_list_labels_error(void) {
    int port = 0;
    pid_t pid = start_404_server(&port, 1);
    if (pid < 0) { ASSERT(0, "list_labels_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "list_labels_err: client connected");

    char **names = NULL, **ids = NULL;
    int count = 0;
    int rc = gmail_list_labels(c, &names, &ids, &count);
    ASSERT(rc != 0, "list_labels_err: returns error on 404");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_create_label_error(void) {
    int port = 0;
    pid_t pid = start_404_server(&port, 1);
    if (pid < 0) { ASSERT(0, "create_label_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "create_label_err: client connected");

    char *id = NULL;
    int rc = gmail_create_label(c, "Bad", &id);
    ASSERT(rc != 0, "create_label_err: returns error");
    ASSERT(id == NULL, "create_label_err: id is NULL on error");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_trash_error(void) {
    int port = 0;
    pid_t pid = start_404_server(&port, 1);
    if (pid < 0) { ASSERT(0, "trash_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "trash_err: client connected");

    int rc = gmail_trash(c, "msg_gone");
    ASSERT(rc != 0, "trash_err: returns error on 404");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_modify_labels_error(void) {
    int port = 0;
    pid_t pid = start_404_server(&port, 1);
    if (pid < 0) { ASSERT(0, "modify_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "modify_err: client connected");

    const char *add[] = { "INBOX" };
    int rc = gmail_modify_labels(c, "msg_gone", add, 1, NULL, 0);
    ASSERT(rc != 0, "modify_err: returns error on 404");

    gmail_disconnect(c);
    wait_child(pid);
}

static void test_get_history_id_error(void) {
    int port = 0;
    pid_t pid = start_404_server(&port, 1);
    if (pid < 0) { ASSERT(0, "histid_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "histid_err: client connected");

    char *hid = gmail_get_history_id(c);
    ASSERT(hid == NULL, "histid_err: returns NULL on error");

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Mock server: returns HTTP 500 for list_messages (covers break) ─ */

static void run_500_server(int listen_fd, int count) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[2048];
        mock_read_request(cfd, buf, (int)sizeof(buf));
        mock_send_json(cfd, 500, "{\"error\":\"server error\"}");
        close(cfd);
    }
    close(listen_fd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_500_server(int *port_out, int count) {
    int listen_fd = make_mock_listener(port_out);
    if (listen_fd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(listen_fd); return -1; }
    if (pid == 0) { run_500_server(listen_fd, count); }
    close(listen_fd);
    return pid;
}

static void test_list_messages_error(void) {
    int port = 0;
    pid_t pid = start_500_server(&port, 1);
    if (pid < 0) { ASSERT(0, "list_msg_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "list_msg_err: client connected");

    char (*uids)[17] = NULL;
    int count = 0;
    /* 500 response — loop should break, count=0, uids=NULL → rc=0 */
    gmail_list_messages(c, "INBOX", NULL, &uids, &count, NULL);
    ASSERT(count == 0, "list_msg_err: count is 0 on error");
    free(uids);

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Mock server: message with no 'raw' field ─────────────────────── */

static void run_noraw_server(int listen_fd, int count) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[2048];
        mock_read_request(cfd, buf, (int)sizeof(buf));
        /* Message response without 'raw' field */
        mock_send_json(cfd, 200, "{\"id\":\"msg001\",\"threadId\":\"t001\"}");
        close(cfd);
    }
    close(listen_fd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_noraw_server(int *port_out, int count) {
    int listen_fd = make_mock_listener(port_out);
    if (listen_fd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(listen_fd); return -1; }
    if (pid == 0) { run_noraw_server(listen_fd, count); }
    close(listen_fd);
    return pid;
}

static void test_fetch_message_no_raw_field(void) {
    int port = 0;
    pid_t pid = start_noraw_server(&port, 1);
    if (pid < 0) { ASSERT(0, "fetch_noraw: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "fetch_noraw: client connected");

    char *body = gmail_fetch_message(c, "msg001", NULL, NULL);
    ASSERT(body == NULL, "fetch_noraw: returns NULL when no raw field");

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Mock server: history 404 (expired) ──────────────────────────── */

static void run_history_expired_server(int listen_fd, int count) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[2048];
        mock_read_request(cfd, buf, (int)sizeof(buf));
        mock_send_json(cfd, 404, "{\"error\":{\"code\":404,\"message\":\"historyId expired\"}}");
        close(cfd);
    }
    close(listen_fd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_history_expired_server(int *port_out, int count) {
    int listen_fd = make_mock_listener(port_out);
    if (listen_fd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(listen_fd); return -1; }
    if (pid == 0) { run_history_expired_server(listen_fd, count); }
    close(listen_fd);
    return pid;
}

static void test_get_history_expired(void) {
    int port = 0;
    pid_t pid = start_history_expired_server(&port, 1);
    if (pid < 0) { ASSERT(0, "history_expired: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "history_expired: client connected");

    char *resp = gmail_get_history(c, "99999");
    ASSERT(resp == NULL, "history_expired: returns NULL on 404");

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Mock server: history non-200/404 response ────────────────────── */

static void run_history_503_server(int listen_fd, int count) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[2048];
        mock_read_request(cfd, buf, (int)sizeof(buf));
        const char *resp =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{}";
        ssize_t r = write(cfd, resp, strlen(resp)); (void)r;
        close(cfd);
    }
    close(listen_fd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_history_503_server(int *port_out, int count) {
    int listen_fd = make_mock_listener(port_out);
    if (listen_fd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(listen_fd); return -1; }
    if (pid == 0) { run_history_503_server(listen_fd, count); }
    close(listen_fd);
    return pid;
}

static void test_get_history_503(void) {
    int port = 0;
    pid_t pid = start_history_503_server(&port, 1);
    if (pid < 0) { ASSERT(0, "history_503: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "history_503: client connected");

    char *resp = gmail_get_history(c, "12345");
    ASSERT(resp == NULL, "history_503: returns NULL on 503");

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Mock server: untrash error ───────────────────────────────────── */

static void test_untrash_error(void) {
    int port = 0;
    pid_t pid = start_404_server(&port, 1);
    if (pid < 0) { ASSERT(0, "untrash_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "untrash_err: client connected");

    int rc = gmail_untrash(c, "msg_gone");
    ASSERT(rc != 0, "untrash_err: returns error on 404");

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Mock server: send error ──────────────────────────────────────── */

static void test_send_error(void) {
    int port = 0;
    pid_t pid = start_404_server(&port, 1);
    if (pid < 0) { ASSERT(0, "send_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "send_err: client connected");

    const char *msg = "From: a@b.com\r\n\r\nHi\r\n";
    int rc = gmail_send(c, msg, strlen(msg));
    ASSERT(rc != 0, "send_err: returns error on 404");

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Mock server: delete_label error ─────────────────────────────── */

static void run_delete_500_server(int listen_fd, int count) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[2048];
        mock_read_request(cfd, buf, (int)sizeof(buf));
        mock_send_json(cfd, 500, "{\"error\":\"internal\"}");
        close(cfd);
    }
    close(listen_fd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_delete_500_server(int *port_out, int count) {
    int listen_fd = make_mock_listener(port_out);
    if (listen_fd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(listen_fd); return -1; }
    if (pid == 0) { run_delete_500_server(listen_fd, count); }
    close(listen_fd);
    return pid;
}

static void test_delete_label_error(void) {
    int port = 0;
    pid_t pid = start_delete_500_server(&port, 1);
    if (pid < 0) { ASSERT(0, "delete_label_err: could not start mock server"); return; }

    usleep(20000);

    GmailClient *c = make_test_client(port);
    ASSERT(c != NULL, "delete_label_err: client connected");

    int rc = gmail_delete_label(c, "Label_Test001");
    ASSERT(rc != 0, "delete_label_err: returns error on 500");

    gmail_disconnect(c);
    wait_child(pid);
}

/* ── Registration ─────────────────────────────────────────────────── */

void test_gmail_client(void) {
    RUN_TEST(test_connect_not_gmail);
    RUN_TEST(test_connect_no_token);
    RUN_TEST(test_disconnect_null);
    RUN_TEST(test_b64_roundtrip);
    RUN_TEST(test_b64_empty);
    RUN_TEST(test_b64_known_vector);
    RUN_TEST(test_b64_url_chars);
    RUN_TEST(test_b64_decode_null_len_out);
    RUN_TEST(test_b64_large_roundtrip);
    RUN_TEST(test_connect_with_test_token);
    RUN_TEST(test_set_progress);
    RUN_TEST(test_list_labels);
    RUN_TEST(test_list_messages);
    RUN_TEST(test_list_messages_with_query);
    RUN_TEST(test_list_messages_with_history_id);
    RUN_TEST(test_fetch_message);
    RUN_TEST(test_fetch_message_no_labels);
    RUN_TEST(test_fetch_message_404);
    RUN_TEST(test_fetch_message_no_raw_field);
    RUN_TEST(test_modify_labels);
    RUN_TEST(test_modify_labels_add_only);
    RUN_TEST(test_modify_labels_remove_only);
    RUN_TEST(test_trash);
    RUN_TEST(test_untrash);
    RUN_TEST(test_send);
    RUN_TEST(test_get_history_id);
    RUN_TEST(test_get_history);
    RUN_TEST(test_get_history_expired);
    RUN_TEST(test_get_history_503);
    RUN_TEST(test_create_label);
    RUN_TEST(test_create_label_no_id_out);
    RUN_TEST(test_delete_label);
    RUN_TEST(test_list_labels_error);
    RUN_TEST(test_list_messages_error);
    RUN_TEST(test_create_label_error);
    RUN_TEST(test_trash_error);
    RUN_TEST(test_untrash_error);
    RUN_TEST(test_send_error);
    RUN_TEST(test_modify_labels_error);
    RUN_TEST(test_get_history_id_error);
    RUN_TEST(test_delete_label_error);
}
