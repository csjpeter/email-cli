#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-22
# Tests that the integration compose environment carries the agent id (EMAIL-172).

# The integration environment must have no fixed container or volume name, and
# its project, volume and clean-up must be those of the calling agent only.
# Needs no Docker: a stub `docker` records its arguments.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
RUN="$ROOT/tests/integration/run_integration.sh"
COMPOSE_FILE="$ROOT/tests/integration/docker-compose.yml"
STUBS="$ROOT/build/compose-names-stubs-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

# 1. No fixed names in the compose file.
if grep -Eq '^[[:space:]]*container_name:' "$COMPOSE_FILE"; then bad "1.1 compose file fixes container_name"; else ok "1.1 no container_name"; fi
if grep -Eq '^[[:space:]]+name:' "$COMPOSE_FILE"; then bad "1.2 compose file fixes a volume name"; else ok "1.2 no fixed volume name"; fi

# 2. The names carry the agent id and differ between agents.
a1="$(AGENT_ID=a1 "$RUN" --names)"; a2="$(AGENT_ID=a2 "$RUN" --names)"
[ "$a1" = "project=email-cli-a1
volume=email-cli-a1_maildata" ] && ok "2.1 a1 names carry a1" || bad "2.1 a1 names: $a1"
[ "$a1" != "$a2" ] && ok "2.2 a1 and a2 differ" || bad "2.2 a1 and a2 are equal"
case "$(AGENT_ID='a/1' "$RUN" --names 2>&1)" in *ERROR*) ok "2.3 unusable agent id refused" ;; *) bad "2.3 unusable id accepted" ;; esac

# 3. Clean-up touches only the agent's own compose project.
mkdir -p "$STUBS" || exit 1
printf '#!/bin/sh\necho "$COMPOSE_PROJECT_NAME|$*" >> "%s/calls"\n' "$STUBS" > "$STUBS/docker"
chmod +x "$STUBS/docker"
PATH="$STUBS:$PATH" AGENT_ID=a2 "$RUN" --clean > /dev/null || bad "3.0 --clean failed"
PATH="$STUBS:$PATH" AGENT_ID=a2 "$RUN" --down > /dev/null || bad "3.0 --down failed"
if [ "$(wc -l < "$STUBS/calls")" -eq 2 ] && ! grep -v '^email-cli-a2|compose -p email-cli-a2 ' "$STUBS/calls" > /dev/null; then
    ok "3.1 --down and --clean address only project email-cli-a2"
else
    bad "3.1 clean-up calls: $(cat "$STUBS/calls")"
fi
rm -f "$STUBS/docker" "$STUBS/calls"
rmdir "$STUBS"

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
