# VHS demo script for email-cli
# Requires: vhs (https://github.com/charmbracelet/vhs)
# Run from the project root: vhs demo/demo.tape

Output demo/demo.mp4

Set Shell "bash"
Set FontSize 15
Set Width 1600
Set Height 900
Set Theme "Dracula"
Set Padding 24
Set TypingSpeed 60ms

# ── Boot: start mock servers, set up demo HOME (hidden) ──────────────────────
Hide
Type "source demo/demo_setup.sh"
Enter
Sleep 3s
Show

# ── Help overview (~15s) ─────────────────────────────────────────────────────
Type "email-cli --help"
Enter
Sleep 6s

# ── Show accounts (~10s) ─────────────────────────────────────────────────────
Type "email-cli --batch show-accounts"
Enter
Sleep 5s

# ── List unread messages — before state (~20s) ────────────────────────────────
Type "email-cli --batch list"
Enter
Sleep 7s

# ── Show full message (~20s) ──────────────────────────────────────────────────
Type "email-cli --batch show 1"
Enter
Sleep 8s

# ── List all + grep (~15s) ───────────────────────────────────────────────────
Type "email-cli --batch list --all | grep -i kickoff"
Enter
Sleep 6s

# ── Folder tree (~15s) ───────────────────────────────────────────────────────
Type "email-cli --batch folders --tree"
Enter
Sleep 6s

# ── List specific folder (~12s) ───────────────────────────────────────────────
Type "email-cli --batch list --folder INBOX.Sent"
Enter
Sleep 5s

# ── Send a message — restart SMTP mock first (hidden) ────────────────────────
Hide
Type "bash demo/restart_smtp.sh"
Enter
Sleep 1.5s
Show
Type `email-cli --batch send --to bob@acme.example --subject "Re: Kickoff" --body "See you Tuesday!"`
Enter
Sleep 6s

# ── Attachments (~15s) ───────────────────────────────────────────────────────
Type "email-cli --batch attachments 1"
Enter
Sleep 5s

# ── Save attachment (~12s) ───────────────────────────────────────────────────
Type "email-cli --batch save-attachment 1 notes.txt /tmp"
Enter
Sleep 5s

# ── mark-read: BEFORE → action → AFTER (~25s) ────────────────────────────────
Type "# Before: message is unread"
Enter
Sleep 1s
Type "email-cli --batch list"
Enter
Sleep 6s
Type "email-cli --batch mark-read 1"
Enter
Sleep 3s
Type "# After: no unread messages"
Enter
Sleep 1s
Type "email-cli --batch list"
Enter
Sleep 6s

# ── mark-unread: restore for next demo (~15s) ────────────────────────────────
Type "email-cli --batch mark-unread 1"
Enter
Sleep 3s

# ── mark-starred: BEFORE → action → AFTER (~25s) ─────────────────────────────
Type "# Before: not starred"
Enter
Sleep 1s
Type "email-cli --batch list --all"
Enter
Sleep 6s
Type "email-cli --batch mark-starred 1"
Enter
Sleep 3s
Type "# After: message is starred"
Enter
Sleep 1s
Type "email-cli --batch list --all"
Enter
Sleep 6s

# ── Config show — passwords masked (~15s) ────────────────────────────────────
Type "email-cli --batch config show"
Enter
Sleep 7s

# ── Sync (~15s) ──────────────────────────────────────────────────────────────
Type "email-sync"
Enter
Sleep 8s

# ── Outro (~10s) ─────────────────────────────────────────────────────────────
Type "# github.com/csjpeter/email-cli"
Enter
Sleep 8s
