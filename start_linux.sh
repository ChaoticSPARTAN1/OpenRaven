#!/bin/bash
# OpenRaven — Halo: Fireteam Raven (v3.11) Automatic Setup & Launcher for Linux (and WSL2)
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "================================================================"
echo "  OpenRaven — Halo: Fireteam Raven (v3.11) PC Loader"
echo "================================================================"

# 1. Check and install required system packages if missing
SUDO="sudo"
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
elif ! command -v sudo >/dev/null 2>&1; then
  if command -v doas >/dev/null 2>&1; then
    SUDO="doas"
  else
    SUDO=""
  fi
fi

need_pkgs=()
command -v bwrap >/dev/null 2>&1 || need_pkgs+=(bubblewrap)
command -v unzip >/dev/null 2>&1 || need_pkgs+=(unzip)
command -v gcc   >/dev/null 2>&1 || need_pkgs+=(gcc)
command -v g++   >/dev/null 2>&1 || need_pkgs+=(g++)
[ -f /usr/include/X11/Xlib.h ] || need_pkgs+=(libx11-dev)
[ -f /usr/include/vulkan/vulkan.h ] || need_pkgs+=(libvulkan-dev)

# Check for LAPACK runtime (required by game binary)
if ! ldconfig -p 2>/dev/null | grep -q "liblapack\.so\.3" && \
   [ ! -f /usr/lib/x86_64-linux-gnu/liblapack.so.3 ] && \
   [ ! -f /usr/lib64/liblapack.so.3 ] && \
   [ ! -f /usr/lib/liblapack.so.3 ]; then
  need_pkgs+=(liblapack3)
fi

IS_WSL=0
if grep -qi microsoft /proc/version 2>/dev/null; then
  IS_WSL=1
  if [ -z "$RAVENVK_WIN_SERVER" ]; then
    HOST_IP=$(ip route show default 2>/dev/null | awk '{print $3; exit}')
    export RAVENVK_WIN_SERVER="${HOST_IP:-127.0.0.1}:28765"
  fi
  command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 || need_pkgs+=(gcc-mingw-w64-x86-64)
fi

if [ "${#need_pkgs[@]}" -gt 0 ]; then
  echo "[OpenRaven] Installing missing dependencies (${need_pkgs[*]})..."
  if command -v apt-get >/dev/null 2>&1; then
    APT_PKGS=(bubblewrap unzip build-essential libgl1 libgl1-mesa-dri mesa-vulkan-drivers libvulkan1 libx11-6 libx11-dev libxi6 libxrandr2 liblapack3 libudev1 pulseaudio-utils alsa-utils libasound2-plugins swh-plugins ladspa-sdk libvulkan-dev wmctrl)
    [ "$IS_WSL" -eq 1 ] && APT_PKGS+=(gcc-mingw-w64-x86-64)
    $SUDO apt-get update && $SUDO apt-get install -y "${APT_PKGS[@]}"
  elif command -v dnf >/dev/null 2>&1; then
    $SUDO dnf install -y bubblewrap unzip gcc gcc-c++ make libGL mesa-dri-drivers mesa-vulkan-drivers vulkan-loader libX11 libX11-devel libXi libXrandr lapack systemd-libs pulseaudio-utils alsa-utils ladspa-swh-plugins vulkan-headers wmctrl
  elif command -v pacman >/dev/null 2>&1; then
    $SUDO pacman -Sy --needed --noconfirm bubblewrap unzip base-devel libglvnd mesa vulkan-radeon vulkan-intel vulkan-icd-loader libx11 libxi libxrandr lapack systemd-libs libpulse alsa-utils alsa-plugins swh-plugins vulkan-headers
  elif command -v zypper >/dev/null 2>&1; then
    $SUDO zypper install -y bubblewrap unzip gcc gcc-c++ make libGL1 Mesa-dri Mesa-vulkan-device-select libvulkan1 libX11-6 libX11-devel libXi6 libXrandr2 liblapack3 systemd-libs pulseaudio-utils alsa-utils ladspa-swh-plugins vulkan-headers wmctrl
  else
    echo "Please install the following packages using your distro's package manager: ${need_pkgs[*]}"
    exit 1
  fi
fi

# 2. Check if the game files are already extracted in ./pm/g7/halo/
if [ ! -f "$ROOT/pm/g7/halo/game" ] || [ ! -d "$ROOT/pm/g7/halo/data" ]; then
  echo ""
  echo "[OpenRaven] Game data not found in $ROOT/pm/g7/halo/."

  ZIP_PATH=""
  ARG_PATH="${1:-}"

  # Check if argument is a zip file or directory
  if [ -n "$ARG_PATH" ]; then
    if [ -f "$ARG_PATH" ]; then
      ZIP_PATH="$ARG_PATH"
    elif [ -d "$ARG_PATH" ]; then
      TARGET_DIR=""
      if [ -f "$ARG_PATH/g7/halo/game" ]; then
        TARGET_DIR="$ARG_PATH"
      elif [ -d "$ARG_PATH/pm" ] && [ -f "$ARG_PATH/pm/g7/halo/game" ]; then
        TARGET_DIR="$ARG_PATH/pm"
      fi
      if [ -n "$TARGET_DIR" ]; then
        echo "[OpenRaven] Linking existing 'pm' directory: $TARGET_DIR"
        rmdir "$ROOT/pm" 2>/dev/null || true
        ln -sfn "$(cd "$TARGET_DIR" && pwd)" "$ROOT/pm"
      else
        echo "[OpenRaven] Specified directory does not contain g7/halo/game: $ARG_PATH"
      fi
    fi
  fi

  # Auto-check common locations first if not passed as argument
  if [ -z "$ZIP_PATH" ] && [ ! -f "$ROOT/pm/g7/halo/game" ]; then
    for cand in \
      "$ROOT"/*[Rr]aven*.zip \
      "$ROOT"/../*[Rr]aven*.zip \
      "$HOME/Downloads"/*[Rr]aven*.zip \
      "$ROOT/Halo Fireteam Raven (3.11)(2018)[Raw Thrills].zip" \
      "$ROOT/../Halo Fireteam Raven (3.11)(2018)[Raw Thrills].zip" \
      "$HOME/Downloads/Halo Fireteam Raven (3.11)(2018)[Raw Thrills].zip"; do
      if [ -f "$cand" ]; then
        echo "[OpenRaven] Found archive: $cand"
        yn=""
        if [ -t 0 ]; then
          read -r -p "Use this file? [Y/n]: " yn || true
        fi
        if [[ -z "$yn" || "$yn" =~ ^[Yy] ]]; then
          ZIP_PATH="$cand"
          break
        fi
      fi
    done
  fi

  # Prompt the user (file picker if available, or terminal prompt / drag-and-drop)
  while [ ! -f "$ROOT/pm/g7/halo/game" ] && { [ -z "$ZIP_PATH" ] || [ ! -f "$ZIP_PATH" ]; }; do
    if [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; then
      if command -v zenity >/dev/null 2>&1 && [ -z "$NO_GUI_PICKER" ]; then
        echo "[OpenRaven] Opening file picker... (or cancel to type path in terminal)"
        ZIP_PATH=$(zenity --file-selection --title="Select Halo: Fireteam Raven game archive (.zip)" --file-filter="Zip archives (*.zip) | *.zip" 2>/dev/null || true)
      elif command -v kdialog >/dev/null 2>&1 && [ -z "$NO_GUI_PICKER" ]; then
        echo "[OpenRaven] Opening file picker... (or cancel to type path in terminal)"
        ZIP_PATH=$(kdialog --getopenfilename "$HOME" "*.zip|Zip archives" --title "Select Halo: Fireteam Raven game archive (.zip)" 2>/dev/null || true)
      fi
      NO_GUI_PICKER=1
    fi

    if [ -z "$ZIP_PATH" ] || [ ! -f "$ZIP_PATH" ]; then
      if [ ! -t 0 ]; then
        echo "Error: game files not found and no interactive terminal to prompt for path." >&2
        echo "Usage: $0 [/path/to/game.zip | /path/to/pm]" >&2
        exit 1
      fi
      echo ""
      echo "Please enter (or drag-and-drop) the full path to:"
      echo "  Halo Fireteam Raven (3.11)(2018)[Raw Thrills].zip"
      echo "  (Or provide the path to an already-extracted 'pm' folder)"
      read -r -p "> " INPUT_PATH || true
      # Strip surrounding single/double quotes and trailing whitespace from drag-and-drop
      INPUT_PATH="${INPUT_PATH#"${INPUT_PATH%%[![:space:]]*}"}"
      INPUT_PATH="${INPUT_PATH%"${INPUT_PATH##*[![:space:]]}"}"
      INPUT_PATH="${INPUT_PATH#\'}"
      INPUT_PATH="${INPUT_PATH%\'}"
      INPUT_PATH="${INPUT_PATH#\"}"
      INPUT_PATH="${INPUT_PATH%\"}"

      if [ -d "$INPUT_PATH" ] && [ -f "$INPUT_PATH/g7/halo/game" ]; then
        echo "[OpenRaven] Linking existing 'pm' directory: $INPUT_PATH"
        rmdir "$ROOT/pm" 2>/dev/null || true
        ln -sfn "$(cd "$INPUT_PATH" && pwd)" "$ROOT/pm"
        break
      elif [ -d "$INPUT_PATH/pm" ] && [ -f "$INPUT_PATH/pm/g7/halo/game" ]; then
        echo "[OpenRaven] Linking existing 'pm' directory: $INPUT_PATH/pm"
        rmdir "$ROOT/pm" 2>/dev/null || true
        ln -sfn "$(cd "$INPUT_PATH/pm" && pwd)" "$ROOT/pm"
        break
      elif [ -f "$INPUT_PATH" ]; then
        ZIP_PATH="$INPUT_PATH"
      else
        echo "File not found: $INPUT_PATH"
        ZIP_PATH=""
      fi
    fi
  done

  if [ -n "$ZIP_PATH" ] && [ ! -f "$ROOT/pm/g7/halo/game" ]; then
    echo ""
    echo "[OpenRaven] Extracting game files from:"
    echo "  $ZIP_PATH"
    echo "This is ~10.5 GB uncompressed and takes 1-3 minutes..."
    unzip -q -o "$ZIP_PATH" -d "$ROOT"
    if [ ! -f "$ROOT/pm/g7/halo/game" ]; then
      echo "Error: Extraction finished, but $ROOT/pm/g7/halo/game was not found." >&2
      exit 1
    fi
    echo "[OpenRaven] Extraction complete!"
  fi
fi

# 3. Verify and prepare game directory permissions & required folders
mkdir -p "$ROOT/pm/g7/dump" "$ROOT/pm/g7/halo/pmuser/messages"
chmod +x "$ROOT/pm/g7/halo/game" 2>/dev/null || true
chmod 755 "$ROOT/pm/g7/halo/game" "$ROOT/pm/g7/halo/halo.so" "$ROOT/openraven/"*.sh 2>/dev/null || true

# 4. Build OpenRaven shims if needed (or if source is newer)
if [ ! -f "$ROOT/openraven/rio2pad.so" ] || [ ! -f "$ROOT/openraven/ravengfx.so" ] || \
   [ "$ROOT/openraven/ravengfx.c" -nt "$ROOT/openraven/ravengfx.so" ] || \
   [ "$ROOT/openraven/rio2pad.c" -nt "$ROOT/openraven/rio2pad.so" ]; then
  echo "[OpenRaven] Building shims (rio2pad.so, ravengfx.so)..."
  "$ROOT/openraven/build.sh"
fi
if grep -qi microsoft /proc/version 2>/dev/null && [ ! -f "$ROOT/openraven/vk/ravenvk_win.exe" ] && command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  echo "[OpenRaven] Building native Windows Vulkan helper (ravenvk_win.exe)..."
  x86_64-w64-mingw32-gcc -O2 -Wall -idirafter /usr/include -o "$ROOT/openraven/vk/ravenvk_win.exe" "$ROOT/openraven/vk/ravenvk_win_daemon.c" "$ROOT/openraven/vk/ravenvk.c" -lws2_32
fi

echo ""
echo "[OpenRaven] Launching Halo: Fireteam Raven..."
echo "  - Press F1 in-game to open the OpenRaven Settings Overlay"
echo "  - Run ./openraven/stop_halo.sh to force-close the game"
echo ""

if [ $# -gt 0 ]; then
  if [ -f "$1" ] || [ -d "$1" ]; then
    shift
  fi
fi

exec "$ROOT/openraven/play_halo.sh" "$@"
