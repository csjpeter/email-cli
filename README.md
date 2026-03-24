# Email Client (C Version)

A robust, modular, and memory-safe email client written in C using libcurl and GNU RAII extensions.

## Architecture
This project follows **CLEAN Code** principles with a strictly layered architecture. See `doc/design.md` for more details.

## Resource Management (RAII)
We leverage GCC's `__attribute__((cleanup))` to ensure automatic resource cleanup. See `doc/concepts.md` for a deeper dive.

## Build Requirements
- CMake (3.10+)
- GCC or Clang (supporting `__attribute__((cleanup))`)
- libcurl (with development headers)
- `libssl-dev` (for HTTPS/IMAPS)

## Build Instructions
```bash
cd src/clients/email
mkdir build && cd build
cmake ..
make
```

## Usage
The binary requires a `.env` file in the current working directory with the following variables:
- `EMAIL_HOST` (e.g., `imaps://imap.example.com`)
- `EMAIL_USER`
- `EMAIL_PASS`
- `EMAIL_FOLDER` (default: `INBOX`)

Run the client:
```bash
./email_client
```
