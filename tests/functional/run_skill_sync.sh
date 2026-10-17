#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-17
# Checks that claude/skills/email-cli/SKILL.md is in step with the CLI.

# EMAIL-125 (cli-skill-model.md, "Kept in step with the CLI"): reads the
# commands and options from the CLI's own --help pages and fails when one is
# missing from SKILL.md, and when SKILL.md names a command or an option the
# --help pages do not have.  Needs the release build (bin/).
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
SKILL=claude/skills/email-cli/SKILL.md
BIN=bin
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

[ -f "$SKILL" ] && ok "1.1 $SKILL exists" || { bad "1.1 $SKILL is missing"; echo "Passed: $PASSED / $((PASSED + FAILED))"; exit 1; }

# Commands that have no page of their own for 'help <command>' (the usage is in
# the general help): listed here so that a new gap is not taken for one.
NO_PAGE=" migrate-credentials help "

pages=""      # all the help text
cmds=""       # command words
for prog in email-cli email-cli-ro; do
    top=$("$BIN/$prog" --help 2>&1) || { bad "2.0 $prog --help failed: $top"; continue; }
    pages="$pages"$'\n'"$top"
    for c in $(printf '%s\n' "$top" | awk '/^  [a-z]/ {print $1}' | sort -u); do
        cmds="$cmds $c"
        case "$NO_PAGE" in *" $c "*) continue ;; esac
        if [ "$c" = rules ]; then
            for sub in list apply add remove; do
                p=$("$BIN/$prog" help rules $sub 2>&1) || { bad "2.1 $prog help rules $sub failed: $p"; continue; }
                pages="$pages"$'\n'"$p"
            done
        fi
        p=$("$BIN/$prog" help "$c" 2>&1) || { bad "2.2 $prog help $c failed: $p"; continue; }
        pages="$pages"$'\n'"$p"
    done
done
cmds=$(printf '%s\n' $cmds | sort -u)
opts=$(printf '%s\n' "$pages" | grep -o -- '--[a-z][a-z-]*' | sort -u)
nc=$(printf '%s\n' "$cmds" | grep -c .); no=$(printf '%s\n' "$opts" | grep -c .)
[ "$nc" -ge 25 ] && [ "$no" -ge 20 ] && ok "2.3 read $nc commands and $no options from the --help pages" || bad "2.3 only $nc commands and $no options found"

# 3. everything the CLI has is in the skill
miss=0
for c in $cmds; do
    grep -Fq -- "$c" "$SKILL" || { bad "3.1 command '$c' is not in SKILL.md"; miss=$((miss + 1)); }
done
for o in $opts; do
    grep -Fq -- "$o" "$SKILL" || { bad "3.2 option '$o' is not in SKILL.md"; miss=$((miss + 1)); }
done
for s in show password reauth imap smtp; do
    grep -Fq -- "config $s" "$SKILL" || { bad "3.3 'config $s' is not in SKILL.md"; miss=$((miss + 1)); }
done
for s in list apply add remove; do
    grep -Fq -- "rules $s" "$SKILL" || { bad "3.4 'rules $s' is not in SKILL.md"; miss=$((miss + 1)); }
done
[ "$miss" -eq 0 ] && ok "3.5 every command and option of the CLI is in SKILL.md"

# 4. everything the skill names exists
extra=0
for o in $(grep -o -- '--[a-z][a-z-]*' "$SKILL" | sort -u); do
    printf '%s\n' "$opts" | grep -Fxq -- "$o" || { bad "4.1 SKILL.md names option '$o', the CLI has none"; extra=$((extra + 1)); }
done
for w in $(grep -oE '\b(mark|list|create|delete|add|remove|save|migrate)-[a-z-]+' "$SKILL" | sort -u); do
    printf '%s\n' "$cmds" | grep -Fxq -- "$w" || { bad "4.2 SKILL.md names command '$w', the CLI has none"; extra=$((extra + 1)); }
done
[ "$extra" -eq 0 ] && ok "4.3 everything SKILL.md names exists in the CLI"

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
