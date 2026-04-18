# email-cli YouTube Demo Pipeline

Automated pipeline that produces a YouTube-ready demo video with **zero on-camera time**.

## Output

`email-cli-demo.mp4` — ~3-minute video: animated terminal session + AI voiceover.

## Files

| File | Purpose |
|------|---------|
| `demo.tape` | VHS script — defines every keystroke and timing |
| `narration_en.txt` | English narration script with timestamps |
| `narration_hu.txt` | Hungarian narration script with timestamps |
| `pipeline.sh` | End-to-end build script (VHS → TTS → FFmpeg) |

## Prerequisites

```bash
# VHS (terminal recorder)
brew install vhs           # macOS
sudo apt install vhs       # Ubuntu (Charm apt repo)

# FFmpeg
sudo apt install ffmpeg

# TTS API key (one of):
export ELEVENLABS_API_KEY=sk-...   # https://elevenlabs.io  (recommended)
export OPENAI_API_KEY=sk-...       # fallback
```

## Usage

```bash
chmod +x pipeline.sh

# English narration (default)
ELEVENLABS_API_KEY=sk-... ./pipeline.sh

# Hungarian narration
NARRATION_LANG=hu ELEVENLABS_API_KEY=sk-... ./pipeline.sh

# Custom ElevenLabs voice
VOICE_ID=<voice-id> ELEVENLABS_API_KEY=sk-... ./pipeline.sh
```

## Cost estimate

| Service | Usage | Cost |
|---------|-------|------|
| ElevenLabs free tier | 10 000 chars/month | $0 |
| OpenAI TTS (tts-1) | ~3 000 chars | ~$0.05 |
| VHS, FFmpeg | — | free |

## Customising the demo

- Edit `demo.tape` to change which commands are shown, timing, or terminal theme.
- Edit `narration_en.txt` / `narration_hu.txt` to adjust the spoken commentary.
- Change `VOICE_ID` to any ElevenLabs voice ID for a different narrator voice.
- For a more polished result, add a title card with FFmpeg's `drawtext` filter
  before the merge step in `pipeline.sh`.
