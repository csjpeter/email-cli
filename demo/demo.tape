# VHS demo script for email-cli
# Requires: vhs (https://github.com/charmbracelet/vhs)
# Run from the project root: vhs demo/demo.tape
#
# Terminal is fixed at 80 columns (stty cols 80 in demo_setup.sh).
# FontSize + Width chosen so 80-char lines fill the viewport comfortably.

Output demo/demo.mp4

Set Shell "bash"
Set FontSize 20
Set Width 1200
Set Height 700
Set Theme "Dracula"
Set Padding 24
Set TypingSpeed 60ms

# ── Boot (hidden) ─────────────────────────────────────────────────────────────
Hide
Type "source demo/demo_setup.sh"
Enter
Sleep 3s
Show

# ════════════════════════════════════════════════════════════════════
# INTRO — help screen visible while narration covers project overview
# Narration: ~70 words, ~32s speech
# ════════════════════════════════════════════════════════════════════
Type "email-cli --help"
Enter
Sleep 36s

# ════════════════════════════════════════════════════════════════════
# SHOW-ACCOUNTS — konfigurált fiókok listája
# Narration: ~15 words, ~7s
# ════════════════════════════════════════════════════════════════════
Type "email-cli show-accounts"
Enter
Sleep 8s

# ════════════════════════════════════════════════════════════════════
# LIST — olvasatlan üzenetek
# Narration: ~18 words, ~8s
# ════════════════════════════════════════════════════════════════════
Type "email-cli list"
Enter
Sleep 9s

# ════════════════════════════════════════════════════════════════════
# SHOW — teljes üzenet megjelenítése
# Before (list): narration ~10 words, ~5s
# After  (show): narration ~20 words, ~9s
# ════════════════════════════════════════════════════════════════════
Type "email-cli list"
Enter
Sleep 5s
Type "email-cli show 1"
Enter
Sleep 10s

# ════════════════════════════════════════════════════════════════════
# LIST --all + GREP — szűrés pipe-pal
# Before (list --all): narration ~10 words, ~5s
# After  (grep):       narration ~14 words, ~6s
# ════════════════════════════════════════════════════════════════════
Type "email-cli list --all"
Enter
Sleep 5s
Type "email-cli list --all | grep -i kickoff"
Enter
Sleep 7s

# ════════════════════════════════════════════════════════════════════
# FOLDERS --tree — mappastruktúra
# Narration: ~18 words, ~8s
# ════════════════════════════════════════════════════════════════════
Type "email-cli folders --tree"
Enter
Sleep 9s

# ════════════════════════════════════════════════════════════════════
# LIST --folder — másik mappa megnyitása
# Before (INBOX): narration ~9 words, ~4s
# After (Sent):   narration ~14 words, ~6s
# ════════════════════════════════════════════════════════════════════
Type "email-cli list"
Enter
Sleep 5s
Type "email-cli list --folder INBOX.Sent"
Enter
Sleep 7s

# ════════════════════════════════════════════════════════════════════
# ATTACHMENTS — csatolmányok listázása
# Before (show): narration ~10 words, ~5s
# After (list):  narration ~14 words, ~6s
# ════════════════════════════════════════════════════════════════════
Type "email-cli show 1"
Enter
Sleep 5s
Type "email-cli attachments 1"
Enter
Sleep 7s

# ════════════════════════════════════════════════════════════════════
# SAVE-ATTACHMENT — csatolmány mentése
# Before: fájl nem létezik
# Action: mentés
# After:  fájl létezik
# Narration: ~30 words, ~14s
# ════════════════════════════════════════════════════════════════════
Type "ls /tmp/notes.txt 2>/dev/null || echo '(not downloaded yet)'"
Enter
Sleep 4s
Type "email-cli save-attachment 1 notes.txt /tmp"
Enter
Sleep 4s
Type "ls -lh /tmp/notes.txt"
Enter
Sleep 7s

# ════════════════════════════════════════════════════════════════════
# SEND — üzenet küldése
# Before: Sent mappa tartalma
# Action: küldés
# After:  megerősítés + Sent mappa frissülve
# Narration: ~40 words, ~18s
# ════════════════════════════════════════════════════════════════════
Type "email-cli list --folder INBOX.Sent"
Enter
Sleep 5s
Hide
Type "bash demo/restart_smtp.sh"
Enter
Sleep 1.5s
Show
Type `email-cli send --to colleague@example.com --subject "Re: Kickoff" --body "See you Tuesday!"`
Enter
Sleep 6s
Type "email-cli list --folder INBOX.Sent"
Enter
Sleep 8s

# ════════════════════════════════════════════════════════════════════
# MARK-READ — olvasottnak jelölés
# Before: 1 olvasatlan
# Action: mark-read
# After:  0 olvasatlan
# Narration: ~33 words, ~15s
# ════════════════════════════════════════════════════════════════════
Type "email-cli list"
Enter
Sleep 6s
Type "email-cli mark-read 1"
Enter
Sleep 3s
Type "email-cli list"
Enter
Sleep 7s

# ════════════════════════════════════════════════════════════════════
# MARK-UNREAD — olvasatlannak visszaállítás
# Before: 0 olvasatlan (az előző After)
# Action: mark-unread
# After:  1 olvasatlan
# Narration: ~20 words, ~9s
# ════════════════════════════════════════════════════════════════════
Type "email-cli mark-unread 1"
Enter
Sleep 3s
Type "email-cli list"
Enter
Sleep 7s

# ════════════════════════════════════════════════════════════════════
# MARK-STARRED — jelölés csillaggal
# Before: nincs csillagos üzenet
# Action: mark-starred
# After:  csillagos üzenet látható
# Narration: ~33 words, ~15s
# ════════════════════════════════════════════════════════════════════
Type "email-cli list --all"
Enter
Sleep 6s
Type "email-cli mark-starred 1"
Enter
Sleep 3s
Type "email-cli list --all"
Enter
Sleep 7s

# ════════════════════════════════════════════════════════════════════
# REMOVE-STARRED — csillag eltávolítása
# Before: csillagos (az előző After)
# Action: remove-starred
# After:  nincs csillag
# Narration: ~20 words, ~9s
# ════════════════════════════════════════════════════════════════════
Type "email-cli remove-starred 1"
Enter
Sleep 3s
Type "email-cli list --all"
Enter
Sleep 7s

# ════════════════════════════════════════════════════════════════════
# CONFIG SHOW — konfiguráció jelszó nélkül
# Narration: ~18 words, ~8s
# ════════════════════════════════════════════════════════════════════
Type "email-cli config show"
Enter
Sleep 9s

# ════════════════════════════════════════════════════════════════════
# SYNC — helyi tár frissítése
# Action: email-sync
# After:  fetched/stored összefoglaló IS a következmény
# Narration: ~22 words, ~10s
# ════════════════════════════════════════════════════════════════════
Type "email-sync"
Enter
Sleep 11s

# ════════════════════════════════════════════════════════════════════
# OUTRO
# Narration: ~22 words, ~10s
# ════════════════════════════════════════════════════════════════════
Type "# github.com/csjpeter/email-cli"
Enter
Sleep 12s
