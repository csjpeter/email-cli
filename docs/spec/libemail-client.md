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
only when the interface breaks. The programs of this repository do not link
the shared library yet (EMAIL-99); they keep the static copy of the same
sources until then.

## Interface (`email-client.h`)

All names start with `emc_`; everything else in the object is hidden.

| Function | Does |
|---|---|
| `emc_open(&c, &account)` | copies the account; no network traffic |
| `emc_send(c, &message, &result)` | sends; returns the outcome and the Message-ID |
| `emc_list_folders` / `emc_list_messages` / `emc_fetch_message` | read |
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
