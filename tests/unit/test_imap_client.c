#include "test_helpers.h"
#include "imap_client.h"
#include <stdlib.h>

/* imap_client connection tests require a live server, so we only test
 * that the header compiles and the RAII macro is usable. */
void test_imap_client(void) {
    /* Verify that a NULL pointer is handled gracefully */
    imap_disconnect(NULL);

    /* imap_connect with a bad host must return NULL, not crash */
    ImapClient *c = imap_connect("imaps://invalid.host.example.invalid",
                                  "user", "pass", 1);
    ASSERT(c == NULL, "imap_connect to invalid host should return NULL");
}
