# libemail-client

The mail client logic of email-cli (IMAP, SMTP, the Gmail REST API) as a C
library, so that other programs of the workspace send and read mail natively
instead of starting `email-cli` (`policies/local-service-access-model.md`,
"The client is the interface"; tickets EMAIL-101, EMAIL-99, EMAIL-100).

## Packages

One build produces three packages:

| Package | Contents |
|---|---|
| `email-cli` | the five programs, fully static (EMAIL-103): no dependency |
| `libemail-client0` | `libemail-client.so.0`; `shlibs` asks for `>=` the version; needs libc and OpenSSL only |
| `libemail-client-dev` | `email-client.h`, `libemail-client.so`, `libemail-client.a`, `libemail-client.pc`, `libemail-client(3)`; depends exactly on the runtime package of the same build |

The SONAME's number is the library's ABI version, not the package's: it moves
only when the interface breaks. `email-cli` depends on no package: the
programs carry the library inside.

## Two forms, one set of objects (EMAIL-103)

`libemail-client/CMakeLists.txt` compiles the sources once (an object library)
and links them into `libemail-client.so.0` (what other programs link) and
`libemail-client.a` (what the email programs and the unit tests link).  The
library needs OpenSSL and nothing else: the TCP/TLS layer (`net_tls.c`), the
HTTP/1.1 client (`http_client.c`: Content-Length, chunked and close-delimited
bodies, no redirects or proxies) and the SMTP client (`smtp_adapter.c`: smtps
or STARTTLS, AUTH PLAIN or LOGIN, dot stuffing) are its own.  Name resolution
is the C library's `getaddrinfo()`.  Certificates are verified with the host
name checked; the trusted CAs are `SSL_CERT_FILE` when set, otherwise
OpenSSL's defaults plus the bundle paths of Debian, RHEL, openSUSE and Alpine.


## Who uses it

Since EMAIL-99 the programs of this repository (`email-cli`, `email-cli-ro`,
`email-tui`, `email-sync`) reach a mail server only through this library:
`libemail/src/infrastructure/mail_account.h` turns their `Config` into an
`emc_account` and opens the client. They link its static form.

The protocol modules (`imap_client`, `gmail_client`, `gmail_auth`,
`gmail_sync`, `mail_client`, `smtp_adapter`, `compose_service`, `json_util`)
live in `libemail-client/src/` and nowhere else (EMAIL-100); the programs'
build no longer compiles them. The unit tests still exercise those modules
directly, from their new place.

## Interface (`email-client.h`)

All names start with `emc_`; everything else in the object is hidden.

| Function | Does |
|---|---|
| `emc_open(&c, &account)` | copies the account; no network traffic |
| `emc_connect` | connects now (every other operation connects on demand) |
| `emc_send(c, &message, &result)` | composes, sends, optionally saves a sent copy; returns the outcome and the Message-ID |
| `emc_compose`, `emc_send_wire`, `emc_reply_meta` | the same in steps: build the wire form, send it, read reply data |
| `emc_list_folders` / `emc_list_messages` / `emc_fetch_message` | the short read interface |
| `emc_list_names`, `emc_list_with_ids`, `emc_select`, `emc_select_ext`, `emc_search`, `emc_fetch_headers`, `emc_fetch_body`, `emc_fetch_flags`, `emc_fetch_flags_changedsince` | the full read interface (CONDSTORE/QRESYNC included) |
| `emc_set_flag`, `emc_mark_junk`, `emc_mark_notjunk`, `emc_trash`, `emc_delete`, `emc_move_to_folder`, `emc_modify_label`, `emc_create_label`, `emc_delete_label`, `emc_create_folder`, `emc_delete_folder`, `emc_append` | changes on the server |
| `emc_sync`, `emc_rebuild_indexes` | Gmail synchronisation into the local store |
| `emc_gmail_authorize` | OAuth2 device flow; prints the code and URL on standard output |
| `emc_log_open`, `emc_log_close` | the library's own log (the programs point it at their log file) |
| `emc_take_security_notice` | the pending "revocation could not be checked" notice, if any |
| `emc_close`, `emc_free_list`, `emc_strerror`, `emc_last_error`, `emc_version` | housekeeping |

## Rules

- **No audit.** The library returns the outcome and the message id
  (`emc_send_result`); the caller writes its own audit record.
- **No configuration file, no terminal.** The caller passes an `emc_account`;
  the library reads nothing from the user's home and never prompts.
- **TLS is mandatory.** `emc_account.insecure_tls` disables certificate
  verification (and the revocation check) and exists for test servers only.
- **Revocation is checked** (EMAIL-102, network-and-config-model.md): after the
  chain is verified, the leaf certificate is checked, OCSP first at the
  responder it names (plain HTTP, answer verified against the trust store),
  then the CRL it names (downloaded, signature checked, cached on disk with its
  validity; an expired CRL counts as none and the message says it expired).
  A certificate known to be revoked is refused (`X509_V_ERR_CERT_REVOKED`).
  When neither source can be used the connection goes on, an error-level log
  entry is written and `emc_take_security_notice()` returns the text; at most
  one signal per certificate (SHA-256 fingerprint) and 6 hours, recorded in
  `$XDG_CACHE_HOME/email-cli/revocation/signals`.  The check runs for IMAP
  (OpenSSL verify callback), SMTP and the Gmail API (the same callback on the
  library's own TLS contexts), in `tls_revocation.c`.  A certificate that names no responder and
  no CRL gives the same signal.  `SSL_CERT_FILE` names the trusted CA file for
  both stacks (as OpenSSL does).
- **A failed copy to the sent folder is not a failed send**: `saved_copy` is 0.
- **Gmail** accounts send through the REST API; `host`/SMTP fields are unused.
- **Signals do not fail an operation.** A signal that interrupts a blocking
  read or write of the IMAP connection (a `SIGCHLD` handler installed without
  `SA_RESTART`, a timer) is retried, not reported as a connection error.
- One handle per thread.

## Tests

`tests/unit/test_email_client.c` drives the interface against in-process mock
servers (TLS SMTP, TLS IMAP, Gmail over HTTP).
`tests/unit/test_tls_revocation.c` generates a small PKI and runs the revocation
check against forked OCSP/CRL servers and TLS servers (good, revoked, down,
expired CRL, cache, 6-hour signal, IMAP and HTTPS paths, host name check). `tests/unit/test_net_clients.c`
drives the HTTP and SMTP clients against forked mocks. `tests/functional/
run_static_binaries.sh` runs the binary alone in an empty root against the mock
servers. `tests/functional/run_package_policy.sh` checks the packaged artefact: dependencies, `shlibs`,
the ldconfig trigger, that only `emc_*` symbols are exported, and that a
program compiles against the installed header and runs.
