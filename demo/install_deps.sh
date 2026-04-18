#!/usr/bin/env bash
# demo/install_deps.sh — Install all dependencies for the demo video pipeline.
# Supports: Ubuntu 22.04 / 24.04 (x86_64, arm64), macOS (Homebrew)
# Run: bash demo/install_deps.sh

set -euo pipefail

OK="\033[0;32m✔\033[0m"
SKIP="\033[0;33m–\033[0m"
ERR="\033[0;31m✘\033[0m"

info()  { echo -e "  $*"; }
ok()    { echo -e "  ${OK} $*"; }
skip()  { echo -e "  ${SKIP} $*"; }
fail()  { echo -e "  ${ERR} $*"; exit 1; }

need_sudo() {
    if [[ $EUID -ne 0 ]]; then
        if ! command -v sudo &>/dev/null; then
            fail "Root required but sudo not found. Re-run as root."
        fi
        SUDO="sudo"
    else
        SUDO=""
    fi
}

# ────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== email-cli demo — dependency installer ==="
echo ""

OS="$(uname -s)"

# ── macOS ────────────────────────────────────────────────────────────────────
if [[ "$OS" == "Darwin" ]]; then
    if ! command -v brew &>/dev/null; then
        fail "Homebrew not found. Install it first: https://brew.sh"
    fi

    echo "--- macOS / Homebrew ---"

    if command -v ffmpeg &>/dev/null; then
        skip "ffmpeg already installed ($(ffmpeg -version 2>&1 | head -1 | awk '{print $3}'))"
    else
        info "Installing ffmpeg..."
        brew install ffmpeg
        ok "ffmpeg installed"
    fi

    if command -v vhs &>/dev/null; then
        skip "vhs already installed ($(vhs --version 2>/dev/null || echo 'unknown version'))"
    else
        info "Installing vhs..."
        brew install vhs
        ok "vhs installed"
    fi

# ── Linux ────────────────────────────────────────────────────────────────────
elif [[ "$OS" == "Linux" ]]; then
    need_sudo

    # Detect distro
    if [[ -f /etc/os-release ]]; then
        # shellcheck disable=SC1091
        source /etc/os-release
        DISTRO_ID="${ID:-unknown}"
    else
        DISTRO_ID="unknown"
    fi

    if [[ "$DISTRO_ID" == "ubuntu" || "$DISTRO_ID" == "debian" || "$DISTRO_ID" == "linuxmint" ]]; then
        echo "--- Ubuntu/Debian ---"

        info "Updating apt package list..."
        $SUDO apt-get update -qq

        # ffmpeg
        if command -v ffmpeg &>/dev/null; then
            skip "ffmpeg already installed ($(ffmpeg -version 2>&1 | head -1 | awk '{print $3}'))"
        else
            info "Installing ffmpeg..."
            $SUDO apt-get install -y ffmpeg
            ok "ffmpeg installed"
        fi

        # curl + gnupg (needed for Charm repo)
        for pkg in curl gnupg; do
            if ! dpkg -s "$pkg" &>/dev/null; then
                info "Installing $pkg..."
                $SUDO apt-get install -y "$pkg"
            fi
        done

        # vhs via Charm apt repository
        if command -v vhs &>/dev/null; then
            skip "vhs already installed ($(vhs --version 2>/dev/null || echo 'ok'))"
        else
            info "Adding Charm apt repository..."
            $SUDO mkdir -p /etc/apt/keyrings
            curl -fsSL https://repo.charm.sh/apt/gpg.key \
                | $SUDO gpg --dearmor -o /etc/apt/keyrings/charm.gpg
            echo "deb [signed-by=/etc/apt/keyrings/charm.gpg] https://repo.charm.sh/apt/ * *" \
                | $SUDO tee /etc/apt/sources.list.d/charm.list > /dev/null
            $SUDO apt-get update -qq
            info "Installing vhs..."
            $SUDO apt-get install -y vhs
            ok "vhs installed"
        fi

    elif [[ "$DISTRO_ID" == "fedora" || "$DISTRO_ID" == "rhel" || "$DISTRO_ID" == "rocky" || "$DISTRO_ID" == "almalinux" ]]; then
        echo "--- Fedora/RHEL ---"

        if command -v ffmpeg &>/dev/null; then
            skip "ffmpeg already installed"
        else
            info "Installing ffmpeg (RPM Fusion)..."
            $SUDO dnf install -y https://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm 2>/dev/null || true
            $SUDO dnf install -y ffmpeg
            ok "ffmpeg installed"
        fi

        if command -v vhs &>/dev/null; then
            skip "vhs already installed"
        else
            info "Installing vhs via Go (no RPM available)..."
            if ! command -v go &>/dev/null; then
                $SUDO dnf install -y golang
            fi
            go install github.com/charmbracelet/vhs@latest
            VHS_BIN="$(go env GOPATH)/bin/vhs"
            $SUDO ln -sf "$VHS_BIN" /usr/local/bin/vhs
            ok "vhs installed"
        fi

    else
        echo "--- Unknown Linux distro: trying Go install for vhs ---"

        if command -v ffmpeg &>/dev/null; then
            skip "ffmpeg already installed"
        else
            fail "ffmpeg not found — install it with your package manager, then re-run."
        fi

        if command -v vhs &>/dev/null; then
            skip "vhs already installed"
        else
            if ! command -v go &>/dev/null; then
                fail "Go not found — install Go 1.21+ then re-run, or install vhs manually: https://github.com/charmbracelet/vhs"
            fi
            go install github.com/charmbracelet/vhs@latest
            ok "vhs installed via go"
        fi
    fi

else
    fail "Unsupported OS: $OS"
fi

# ── Python / edge-tts (all platforms) ───────────────────────────────────────
echo ""
echo "--- Python / edge-tts ---"

# Ensure ~/.local/bin is in PATH (pipx installs there)
export PATH="$HOME/.local/bin:$PATH"

if command -v edge-tts &>/dev/null; then
    skip "edge-tts already installed ($(edge-tts --version 2>/dev/null || echo 'ok'))"
else
    # Prefer pipx (correct way on Ubuntu 24.04 PEP 668 systems)
    if ! command -v pipx &>/dev/null; then
        info "Installing pipx..."
        if [[ "$OS" == "Darwin" ]]; then
            brew install pipx
        else
            DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y pipx
        fi
        pipx ensurepath --quiet || true
        export PATH="$HOME/.local/bin:$PATH"
    fi

    if command -v pipx &>/dev/null; then
        info "Installing edge-tts via pipx..."
        pipx install edge-tts --quiet
        ok "edge-tts installed via pipx"
    else
        # Last resort: --break-system-packages (user confirmed dev machine)
        info "Installing edge-tts via pip (--user)..."
        python3 -m pip install --user --quiet --break-system-packages edge-tts
        ok "edge-tts installed via pip --user"
    fi
fi

# ── Verify all tools are reachable ──────────────────────────────────────────
echo ""
echo "--- Verification ---"

ALL_OK=true

check_tool() {
    local cmd="$1" label="$2"
    if command -v "$cmd" &>/dev/null; then
        ok "$label: $(command -v "$cmd")"
    else
        echo -e "  ${ERR} $label: NOT FOUND"
        ALL_OK=false
    fi
}

check_tool ffmpeg  "ffmpeg"
check_tool vhs     "vhs"

if command -v edge-tts &>/dev/null; then
    ok "edge-tts: $(command -v edge-tts)"
else
    echo -e "  ${ERR} edge-tts: NOT FOUND in PATH"
    ALL_OK=false
fi

echo ""
if $ALL_OK; then
    echo "All dependencies ready. Run the demo pipeline:"
    echo "  bash demo/pipeline.sh"
else
    echo "Some dependencies are missing — see errors above."
    exit 1
fi
