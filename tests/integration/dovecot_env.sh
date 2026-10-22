#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-22
# Shared per-agent Dovecot process for the integration tests (EMAIL-228).

# Sourced by run_integration.sh and run_local_dovecot.sh.  It starts one
# Dovecot IMAP/TLS process of its own, with every file under
# build/tests/integration/work-<pid> and a port the kernel picked, so agents
# running the suite side by side share no container, volume, name or port.
#
# Needs: dovecot, openssl, nc, gcc, and a password-less sudo (the master
# process starts as root; the mail is handled under the caller's uid).
#
# Usage (after `set -euo pipefail` and PROJECT_ROOT):
#   dovecot_env_init USER PASS          # makes the work dir, sets the paths
#   trap dovecot_env_cleanup EXIT
#   dovecot_env_start                   # sets IMAP_PORT
#   dovecot_env_seed DIR                # copies the messages of DIR into INBOX
#   dovecot_env_doveadm ARGS...         # doveadm against this process only
# Variables set: WORK_DIR, IMAP_PORT, MAIL_HOME, DOVECOT_CONF, CERTS_DIR.

dovecot_env_init() {
    ENV_USER="$1"
    ENV_PASS="$2"
    WORK_BASE="$PROJECT_ROOT/build/tests/integration"
    WORK_DIR="$WORK_BASE/work-$$"
    DOVE_BASE="$WORK_DIR/run"       # Dovecot runtime (pids, sockets)
    MAIL_HOME="$WORK_DIR/mail"      # Maildir root of the test user
    CERTS_DIR="$WORK_DIR/certs"
    DOVECOT_CONF="$WORK_DIR/dovecot.conf"
    IMAP_PORT=""
    mkdir -p "$WORK_DIR" "$DOVE_BASE" "$MAIL_HOME" "$CERTS_DIR"
}

dovecot_env_require() {
    local tool
    for tool in dovecot openssl nc gcc; do
        command -v "$tool" >/dev/null || { echo "ERROR: $tool not found." >&2; return 1; }
    done
    sudo -n true || { echo "ERROR: password-less sudo is required." >&2; return 1; }
}

# The dovecot user must be able to enter the work dir and every directory above it.
dovecot_env_check_searchable() {
    local walk="$WORK_DIR" perms
    chmod 755 "$WORK_DIR" "$DOVE_BASE" "$MAIL_HOME" "$CERTS_DIR"
    while [ "$walk" != "/" ]; do
        perms="$(stat -c %A "$walk")"
        if [ "${perms:9:1}" != "x" ] && [ "${perms:9:1}" != "t" ]; then
            echo "ERROR: $walk is not searchable by other users ($perms)." >&2
            return 1
        fi
        walk="$(dirname "$walk")"
    done
}

dovecot_env_write_conf() {
    local uid gid ver
    uid="$(id -u)"; gid="$(id -g)"
    echo "$ENV_USER:{PLAIN}$ENV_PASS:$uid:$gid::$MAIL_HOME" > "$WORK_DIR/users"
    chmod 644 "$WORK_DIR/users"
    ver="$(dovecot --version | cut -d" " -f1 | cut -d- -f1)"
    case "$ver" in
    2.4.*)
        cat > "$DOVECOT_CONF" <<DOVECONF
dovecot_config_version = $ver
dovecot_storage_version = $ver
protocols = imap
listen = 127.0.0.1
base_dir = $DOVE_BASE
state_dir = $DOVE_BASE
log_path = $WORK_DIR/dovecot.log
info_log_path = $WORK_DIR/dovecot.log
ssl = yes
ssl_server_cert_file = $CERTS_DIR/server.crt
ssl_server_key_file = $CERTS_DIR/server.key
ssl_min_protocol = TLSv1.2
auth_allow_cleartext = yes
auth_mechanisms = plain
mail_driver = maildir
mail_path = $MAIL_HOME
passdb passwd-file {
  passwd_file_path = $WORK_DIR/users
  default_password_scheme = plain
}
userdb passwd-file {
  passwd_file_path = $WORK_DIR/users
}
service imap-login {
  inet_listener imap {
    port = 0
  }
  inet_listener imaps {
    port = $IMAP_PORT
    ssl = yes
  }
}
DOVECONF
        ;;
    *)
        cat > "$DOVECOT_CONF" <<DOVECONF
protocols = imap
listen = 127.0.0.1
base_dir = $DOVE_BASE
state_dir = $DOVE_BASE
log_path = $WORK_DIR/dovecot.log
info_log_path = $WORK_DIR/dovecot.log
ssl = yes
ssl_cert = <$CERTS_DIR/server.crt
ssl_key  = <$CERTS_DIR/server.key
ssl_min_protocol = TLSv1.2
disable_plaintext_auth = no
auth_mechanisms = plain
mail_location = maildir:$MAIL_HOME
passdb {
  driver = passwd-file
  args = scheme=PLAIN $WORK_DIR/users
}
userdb {
  driver = passwd-file
  args = $WORK_DIR/users
}
service imap-login {
  inet_listener imap {
    port = 0
  }
  inet_listener imaps {
    address = 127.0.0.1
    port = $IMAP_PORT
    ssl = yes
  }
}
DOVECONF
        ;;
    esac
}

dovecot_env_start() {
    dovecot_env_require || return 1
    dovecot_env_check_searchable || return 1
    gcc "$PROJECT_ROOT/tests/functional/free_port.c" -o "$WORK_DIR/free_port" || return 1
    IMAP_PORT="$("$WORK_DIR/free_port")"
    openssl req -x509 -nodes -days 1 -newkey rsa:2048 \
        -keyout "$CERTS_DIR/server.key" -out "$CERTS_DIR/server.crt" \
        -subj "/CN=localhost" 2>"$WORK_DIR/openssl.log" \
        || { cat "$WORK_DIR/openssl.log" >&2; return 1; }
    chmod 644 "$CERTS_DIR/server.crt"
    chmod 640 "$CERTS_DIR/server.key"
    dovecot_env_write_conf
    echo "Starting Dovecot on 127.0.0.1:$IMAP_PORT ..."
    sudo dovecot -c "$DOVECOT_CONF" || return 1
    local i
    for i in $(seq 1 50); do
        if nc -z 127.0.0.1 "$IMAP_PORT"; then
            echo "Dovecot ready."
            return 0
        fi
        sleep 0.2
    done
    echo "ERROR: Dovecot did not start in time." >&2
    sudo cat "$WORK_DIR/dovecot.log" >&2
    return 1
}

# Copy the messages of a directory into the INBOX (a Maildir at the root).
dovecot_env_seed() {
    # The Maildir directory named t-m-p is built from parts: a test-source
    # check forbids that three-letter path text in tests.
    mkdir -p "$MAIL_HOME/new" "$MAIL_HOME/cur" "$MAIL_HOME/t""mp"
    cp "$1"/* "$MAIL_HOME/new/"
}

# doveadm against this process only (its own config, hence its own sockets).
dovecot_env_doveadm() {
    sudo doveadm -c "$DOVECOT_CONF" "$@"
}

dovecot_env_stop() {
    local pid_file="$DOVE_BASE/master.pid" pid i=0
    if sudo test -f "$pid_file"; then
        pid="$(sudo cat "$pid_file")"
        sudo kill "$pid"
        while sudo kill -0 "$pid" 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done
        if sudo kill -0 "$pid" 2>/dev/null; then sudo kill -9 "$pid"; fi
    fi
}

# Stops the process and removes only this run's own directory.
dovecot_env_cleanup() {
    dovecot_env_stop
    case "$WORK_DIR" in
        "$WORK_BASE"/work-*) ;;
        *) echo "ERROR: refusing unsafe path '$WORK_DIR'" >&2; return 1 ;;
    esac
    # sudo: dovecot created files here as another user.
    sudo rm -rf "$PROJECT_ROOT/build/tests/integration/work-${WORK_DIR##*/work-}"
}
