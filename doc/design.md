# Architectural Design

## CLEAN Principles
`email-cli` follows a strictly layered architecture to ensure modularity and maintainability.

### 1. Core Layer (`src/core/`)
Foundational utilities with zero internal dependencies.
- **RAII**: Memory and resource safety.
- **FS Util**: Secure path and permission management.
- **Logger**: Diagnostic data recording.

### 2. Infrastructure Layer (`src/infrastructure/`)
Adapters for external systems.
- **Config Store**: Persists settings to `~/.config/`.
- **CURL Adapter**: Wraps `libcurl` for IMAP communication.
- **Setup Wizard**: Interactive user input.

### 3. Domain Layer (`src/domain/`)
High-level business logic.
- **Email Service**: Coordinates fetching and parsing logic. Decoupled from `libcurl` specifics.

### 4. Application Layer (`src/main.c`)
Orchestration and CLI entry point.

## Dependency Inversion
Higher-level modules depend on abstractions where possible. For instance, the `EmailService` doesn't know how config is stored; it simply receives a `Config` struct.

## Doxygen-Compatible Documentation
All source code includes internal documentation in Doxygen format:
```c
/**
 * @brief Brief description.
 * @param p1 Parameter description.
 * @return Return value description.
 */
```
This ensures the codebase is self-documenting and easy to navigate.
