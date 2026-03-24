# Functional Testing

## Overview
Functional tests verify the application's end-to-end behavior against a real network interface.

## Mock IMAP Server
Since we cannot depend on external mail servers for automated tests, we use a custom **Mock IMAP Server** written in C (`tests/functional/mock_imap_server.c`).

### Behavior
The mock server implements a minimal IMAP state machine:
1. **Greeting**: Sends `* OK Mock IMAP server ready`.
2. **Capability**: Responds with supported features.
3. **Login**: Always returns `OK` for any credentials.
4. **Select**: Returns `* 1 EXISTS`.
5. **Fetch**: Returns a hardcoded test email message.

## Functional Scenarios
1. **First-Run Wizard**: Scripted input to verify the wizard collects and saves config correctly.
2. **Sync Fetch**: Verify the client can connect, authenticate, and print the test message from the mock server.
3. **Traffic Analysis**: Use `DEBUG` logs to verify that the client sends the correct IMAP commands.

## Automation
Run functional tests via the `run_functional.sh` script:
```bash
./tests/functional/run_functional.sh
```
This script starts the mock server in the background, runs the `email-cli` binary, and compares the output.
