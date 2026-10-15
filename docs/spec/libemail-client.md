# libemail-client

The mail client logic of email-cli (IMAP, SMTP, the Gmail REST API) as a C
library, so that other programs of the workspace send and read mail natively
instead of starting `email-cli` (`policies/local-service-access-model.md`,
"The client is the interface"; tickets EMAIL-101, EMAIL-99, EMAIL-100).

## Packages

One build produces three packages:

| Package | Contents |
|---|---|
| `email-cli` | the five programs, unchanged |
| `libemail-client0` | `libemail-client.so.0`; `shlibs` asks for `>=` the version |
| `libemail-client-dev` | `email-client.h`, `libemail-client.so`, `libemail-client.pc`, `libemail-client(3)`; depends exactly on the runtime package of the same build |

The SONAME's number is the library's ABI version, not the package's: it moves
only when the interface breaks. `email-cli` depends on `libemail-client0 (>=
version)`.

## Who uses it

Since EMAIL-99 the programs of this repository (`email-cli`, `email-cli-ro`,
`email-tui`, `email-sync`) reach a mail server only through this library:
`libemail/src/infrastructure/mail_account.h` turns their `Config` into an
`emc_account` and opens the client. They find the shared object beside
themselves in the build tree and in the install directory (`$ORIGIN`
run-path), and in the system library directory when installed from the
package.

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
| `emc_close`, `emc_free_list`, `emc_strerror`, `emc_last_error`, `emc_version` | housekeeping |

## Rules

- **No audit.** The library returns the outcome and the message id
  (`emc_send_result`); the caller writes its own audit record.
- **No configuration file, no terminal.** The caller passes an `emc_account`;
  the library reads nothing from the user's home and never prompts.
- **TLS is mandatory.** `emc_account.insecure_tls` disables certificate
  verification and exists for test servers only. (The network model's
  revocation check is a separate ticket.)
- **A failed copy to the sent folder is not a failed send**: `saved_copy` is 0.
- **Gmail** accounts send through the REST API; `host`/SMTP fields are unused.
- **Signals do not fail an operation.** A signal that interrupts a blocking
  read or write of the IMAP connection (a `SIGCHLD` handler installed without
  `SA_RESTART`, a timer) is retried, not reported as a connection error.
- One handle per thread.

## Tests

`tests/unit/test_email_client.c` drives the interface against in-process mock
servers (TLS SMTP, TLS IMAP, Gmail over HTTP). `tests/functional/
run_package_policy.sh` checks the packaged artefact: dependencies, `shlibs`,
the ldconfig trigger, that only `emc_*` symbols are exported, and that a
program compiles against the installed header and runs.
