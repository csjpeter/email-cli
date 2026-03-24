# email-cli

A robust, modular, and memory-safe email client written in C using libcurl and GNU RAII extensions.

## Architecture
This project follows **CLEAN Code** principles with a strictly layered architecture.
- **Memory Safety:** Leverages GCC's `__attribute__((cleanup))` for RAII.
- **Modular Design:** Clear separation between Core, Infrastructure, and Domain layers.
- **Secure Storage:** Credentials stored in `~/.config/email-cli/` with `0600` permissions.
- **Diagnostics:** Built-in support for ASAN, Valgrind, and GCOV.

## Build Requirements
- CMake (3.10+)
- GCC or Clang
- libcurl (with development headers)
- `libssl-dev`

## Usage & Development

The project includes a `manage.sh` script for a user-friendly experience:

```bash
./manage.sh build      # Build the application (Release)
./manage.sh run        # Build and run
./manage.sh test       # Run unit tests (with ASAN)
./manage.sh valgrind   # Run unit tests with Valgrind
./manage.sh coverage   # Generate coverage report
./manage.sh clean-logs # Purge log files
```

Alternatively, you can use `make` directly:
```bash
make help              # Show all build targets
```

## Setup Wizard
On the first run, `email-cli` will guide you through a configuration wizard to set up your IMAP server, username, and password.

## Documentation
See the `doc/` directory for detailed information on:
- `design.md`: Architectural principles.
- `raii.md`: Resource management.
- `logging.md`: Rotating logs and traffic analysis.
- `unit_testing.md`: Custom C testing methodology.

## License
GNU GPLv3
