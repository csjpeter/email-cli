# VHS demo script for email-cli
# Requires: vhs (https://github.com/charmbracelet/vhs)
#   brew install vhs  /  sudo apt install vhs
#
# Run from the project root:
#   vhs demo/demo.tape

Output demo/demo.mp4

Set Shell "bash"
Set FontSize 14
Set Width 1100
Set Height 650
Set Theme "Dracula"
Set Padding 20
Set TypingSpeed 55ms

# ── Boot: start mock servers, set up demo HOME ────────────────────────────────
Hide
Type "source demo/demo_setup.sh"
Enter
Sleep 2s
Show

# ── Help overview ─────────────────────────────────────────────────────────────
Type "email-cli --help"
Enter
Sleep 4s

# ── List unread messages ──────────────────────────────────────────────────────
Type "email-cli --batch list"
Enter
Sleep 3.5s

# ── Show a message ────────────────────────────────────────────────────────────
Type "email-cli --batch show 1"
Enter
Sleep 3.5s

# ── List all messages + grep ──────────────────────────────────────────────────
Type "email-cli --batch list --all | grep -i kickoff"
Enter
Sleep 3s

# ── Folder tree ───────────────────────────────────────────────────────────────
Type "email-cli --batch folders --tree"
Enter
Sleep 2.5s

# ── List a specific folder ────────────────────────────────────────────────────
Type "email-cli --batch list --folder INBOX.Sent"
Enter
Sleep 2.5s

# ── Attachments ───────────────────────────────────────────────────────────────
Type "email-cli --batch attachments 1"
Enter
Sleep 2.5s

# ── Save an attachment ────────────────────────────────────────────────────────
Type "email-cli --batch save-attachment 1 notes.txt /tmp"
Enter
Sleep 2.5s

# ── Flag / mark commands ──────────────────────────────────────────────────────
Type "email-cli --batch mark-starred 1"
Enter
Sleep 1.5s
Type "email-cli --batch mark-read 1"
Enter
Sleep 1.5s

# ── Show accounts ────────────────────────────────────────────────────────────
Type "email-cli --batch show-accounts"
Enter
Sleep 2s

# ── Config show (passwords masked) ───────────────────────────────────────────
Type "email-cli --batch config show"
Enter
Sleep 2.5s

# ── Send a message ────────────────────────────────────────────────────────────
# Restart SMTP mock (it handles one exchange then exits)
Hide
Type "bash demo/restart_smtp.sh"
Enter
Sleep 1.5s
Show
Type `email-cli --batch send --to bob@acme.example --subject "Re: Kickoff" --body "See you Tuesday!"`
Enter
Sleep 3s

# ── Outro ─────────────────────────────────────────────────────────────────────
Type "# github.com/csjpeter/email-cli"
Enter
Sleep 3s
