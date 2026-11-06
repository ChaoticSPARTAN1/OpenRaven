# OpenRaven - Windows (WSL2 + WSLg) Automatic Setup & Launcher
# Halo: Fireteam Raven runs natively on x86_64 Linux inside original arcade cabinets.
# On Windows 10/11, OpenRaven runs it via WSL2 with full GPU acceleration (WSLg) and automatic Xbox/XInput gamepad bridging.

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GameArgs
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $RepoRoot

function Write-Step($msg) {
    Write-Host "`n[OpenRaven] $msg" -ForegroundColor Cyan
}

# 1. Check WSL2 availability
Write-Step "Checking WSL2 (Windows Subsystem for Linux)..."
$wslCmd = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wslCmd) {
    Write-Host "WSL2 is not installed. Installing WSL2 with Ubuntu..." -ForegroundColor Yellow
    Start-Process powershell -Verb RunAs -Wait -ArgumentList "-NoProfile -Command `"wsl --install -d Ubuntu`""
    Write-Host "`nWSL2 has been installed! Please reboot Windows if prompted, then run start_windows.bat again." -ForegroundColor Green
    exit 0
}

# Check if a WSL distribution is installed
$wslList = & wsl.exe --list --quiet 2>$null | Out-String
$wslList = $wslList -replace "`0", ""
if ([string]::IsNullOrWhiteSpace($wslList)) {
    Write-Host "No WSL Linux distribution found. Installing Ubuntu..." -ForegroundColor Yellow
    & wsl.exe --install -d Ubuntu
    Write-Host "`nUbuntu installation initiated. Once you finish setting up your Ubuntu username/password, run start_windows.bat again." -ForegroundColor Green
    exit 0
}

# 2. Check if game files are already extracted in .\pm\g7\halo\
$GameBin = Join-Path $RepoRoot "pm\g7\halo\game"
$GameData = Join-Path $RepoRoot "pm\g7\halo\data"

$WinZipPath = ""
if (-not (Test-Path $GameBin) -or -not (Test-Path $GameData)) {
    Write-Step "Game files not found in .\pm\g7\halo\."

    # Check common paths first
    $CandidateDirs = @($RepoRoot, (Split-Path -Parent $RepoRoot), (Join-Path $env:USERPROFILE "Downloads"))
    $FoundZips = [System.Collections.Generic.List[string]]::new()
    foreach ($dir in $CandidateDirs) {
        if (Test-Path -LiteralPath $dir) {
            $matched = Get-ChildItem -LiteralPath $dir -Filter "*Raven*.zip" -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
            if ($matched) { $FoundZips.AddRange([string[]]$matched) }
        }
    }

    foreach ($cand in $FoundZips) {
        if (Test-Path -LiteralPath $cand) {
            Write-Host "Found archive: $cand" -ForegroundColor Green
            $ans = Read-Host "Use this file? [Y/n]"
            if ([string]::IsNullOrWhiteSpace($ans) -or $ans -match "^[Yy]") {
                $WinZipPath = $cand
                break
            }
        }
    }

    # Open Windows File Picker Dialog if not found yet
    if ([string]::IsNullOrWhiteSpace($WinZipPath)) {
        Write-Host "Opening file selection dialog for Halo: Fireteam Raven game archive (.zip)..." -ForegroundColor Yellow
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dlg = New-Object System.Windows.Forms.OpenFileDialog
            $dlg.Title = "Select Halo: Fireteam Raven game archive (.zip)"
            $dlg.Filter = "Zip Archives (*.zip)|*.zip|All Files (*.*)|*.*"
            $dlg.InitialDirectory = [Environment]::GetFolderPath("UserProfile") + "\Downloads"
            if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                $WinZipPath = $dlg.FileName
            }
        } catch {
            # Fallback to console prompt
        }
    }

    while ([string]::IsNullOrWhiteSpace($WinZipPath) -or -not (Test-Path -LiteralPath $WinZipPath)) {
        Write-Host "`nPlease paste (or drag-and-drop) the full path to:" -ForegroundColor Yellow
        Write-Host "  Halo Fireteam Raven (3.11)(2018)[Raw Thrills].zip"
        $inputPath = Read-Host ">"
        $inputPath = $inputPath.Trim().Trim('"').Trim("'")
        if (Test-Path $inputPath) {
            $WinZipPath = $inputPath
        } else {
            Write-Host "File not found: $inputPath" -ForegroundColor Red
        }
    }
}

# 3. Start background Windows XInput -> OpenRaven controller bridge
# Bridges up to 4 Windows Xbox/XInput controllers directly into rio2pad.so without needing usbipd!
$PadStateFile = Join-Path $RepoRoot "openraven\run_logs\winpad.bin"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $PadStateFile) | Out-Null

$XInputJob = Start-Job -ScriptBlock {
    param($StatePath)
    $code = @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

public class XInputBridge {
    [StructLayout(LayoutKind.Sequential)]
    public struct XINPUT_GAMEPAD {
        public ushort wButtons;
        public byte bLeftTrigger;
        public byte bRightTrigger;
        public short sThumbLX;
        public short sThumbLY;
        public short sThumbRX;
        public short sThumbRY;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct XINPUT_STATE {
        public uint dwPacketNumber;
        public XINPUT_GAMEPAD Gamepad;
    }
    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
    public static extern uint XInputGetState14(uint dwUserIndex, out XINPUT_STATE pState);
    [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
    public static extern uint XInputGetState910(uint dwUserIndex, out XINPUT_STATE pState);

    static uint GetState(uint idx, out XINPUT_STATE st) {
        try { return XInputGetState14(idx, out st); }
        catch { return XInputGetState910(idx, out st); }
    }

    public static void Run(string path) {
        byte[] buf = new byte[4 + 16 + 4 * 32 * 4 + 4 * 8 * 4]; // 660 bytes
        using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite)) {
            while (true) {
                Buffer.BlockCopy(BitConverter.GetBytes((uint)0x52494F32), 0, buf, 0, 4);
                for (uint p = 0; p < 4; p++) {
                    XINPUT_STATE st;
                    bool ok = GetState(p, out st) == 0;
                    Buffer.BlockCopy(BitConverter.GetBytes(ok ? 1 : 0), 0, buf, 4 + (int)p * 4, 4);
                    int bBase = 20 + (int)p * 128;
                    int aBase = 20 + 512 + (int)p * 32;
                    if (ok) {
                        ushort b = st.Gamepad.wButtons;
                        // PAD_A=0, PAD_B=1, PAD_X=2, PAD_Y=3, PAD_LB=4, PAD_RB=5, PAD_BACK=6, PAD_START=7
                        int[] btns = new int[8] {
                            (b & 0x1000) != 0 ? 1 : 0, // A
                            (b & 0x2000) != 0 ? 1 : 0, // B
                            (b & 0x4000) != 0 ? 1 : 0, // X
                            (b & 0x8000) != 0 ? 1 : 0, // Y
                            (b & 0x0100) != 0 ? 1 : 0, // LB
                            (b & 0x0200) != 0 ? 1 : 0, // RB
                            (b & 0x0020) != 0 ? 1 : 0, // BACK
                            (b & 0x0010) != 0 ? 1 : 0  // START
                        };
                        for (int i = 0; i < 8; i++)
                            Buffer.BlockCopy(BitConverter.GetBytes(btns[i]), 0, buf, bBase + i * 4, 4);

                        short lx = st.Gamepad.sThumbLX;
                        short ly = (short)(st.Gamepad.sThumbLY == -32768 ? 32767 : -st.Gamepad.sThumbLY);
                        short rx = st.Gamepad.sThumbRX;
                        short ry = (short)(st.Gamepad.sThumbRY == -32768 ? 32767 : -st.Gamepad.sThumbRY);
                        short lt = (short)(st.Gamepad.bLeftTrigger * 257 - 32767);
                        short rt = (short)(st.Gamepad.bRightTrigger * 257 - 32767);
                        int[] axes = new int[6] { lx, ly, lt, rx, ry, rt };
                        for (int i = 0; i < 6; i++)
                            Buffer.BlockCopy(BitConverter.GetBytes(axes[i]), 0, buf, aBase + i * 4, 4);
                    }
                }
                fs.Seek(0, SeekOrigin.Begin);
                fs.Write(buf, 0, buf.Length);
                fs.Flush();
                Thread.Sleep(8);
            }
        }
    }
}
"@
    Add-Type -TypeDefinition $code -Language CSharp
    [XInputBridge]::Run($StatePath)
} -ArgumentList $PadStateFile

Write-Step "Starting WSL2 environment and launching OpenRaven..."

# Restart WSL first: a stale WSL/WSLg session (GPU, audio, display) stops the game from opening.
Write-Host "[OpenRaven] Restarting WSL (wsl --shutdown)..." -ForegroundColor Cyan
& wsl.exe --shutdown 2>$null
Start-Sleep -Seconds 3

# Detect the primary monitor's real (DPI-unscaled) resolution for fullscreen.
$HaloRes = ""
try { $HaloRes = (& (Join-Path $PSScriptRoot "detect_res.ps1") | Select-Object -Last 1).Trim() } catch {}
if ($HaloRes) { Write-Host "[OpenRaven] Monitor resolution: $HaloRes (fullscreen)" -ForegroundColor Cyan }

$VkWinProc = $null
try {
    # Convert Windows paths to WSL paths (/mnt/...)
    $WslRepoRoot = (& wsl.exe wslpath -a ($RepoRoot -replace "\\", "/")).Trim()
    $WslPadState = "$WslRepoRoot/openraven/run_logs/winpad.bin"

    # Build native Windows Vulkan worker (ravenvk_win.exe) inside WSL2 if not built yet
    $VkWinExe = Join-Path $RepoRoot "openraven\vk\ravenvk_win.exe"
    if (-not (Test-Path $VkWinExe)) {
        Write-Host "[OpenRaven] Preparing native Windows Vulkan worker (ravenvk_win.exe) for FP16 matrix cores..." -ForegroundColor Cyan
        & wsl.exe --cd $WslRepoRoot bash -c "if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then sudo apt-get update && sudo apt-get install -y gcc-mingw-w64-x86-64 libvulkan-dev; fi; x86_64-w64-mingw32-gcc -O2 -Wall -o openraven/vk/ravenvk_win.exe openraven/vk/ravenvk_win_daemon.c openraven/vk/ravenvk.c -lws2_32 2>/dev/null || true"
    }

    if (Test-Path $VkWinExe) {
        $VkWinProc = Start-Process -FilePath $VkWinExe -ArgumentList "28765" -WindowStyle Hidden -PassThru
    }

    if (-not [string]::IsNullOrWhiteSpace($WinZipPath)) {
        $WslZipPath = (& wsl.exe wslpath -a ($WinZipPath -replace "\\", "/")).Trim()
        & wsl.exe --cd $WslRepoRoot bash -c "sed -i 's/\r$//' start_linux.sh openraven/*.sh openraven/fsr2/*.sh openraven/vk/*.sh 2>/dev/null || true; chmod +x start_linux.sh openraven/*.sh; command -v wmctrl >/dev/null 2>&1 || sudo apt-get install -y wmctrl >/dev/null 2>&1 || true; HOST_IP=\$(ip route show default 2>/dev/null | awk '{print \$3; exit}'); HALO_RES=`"$HaloRes`" HALO_FULLSCREEN=1 RIO2PAD_STATE_FILE=`"$WslPadState`" RAVENVK_WIN_SERVER=`"\${HOST_IP:-127.0.0.1}:28765`" ./start_linux.sh `"$WslZipPath`" $($GameArgs -join ' ')"
    } else {
        & wsl.exe --cd $WslRepoRoot bash -c "sed -i 's/\r$//' start_linux.sh openraven/*.sh openraven/fsr2/*.sh openraven/vk/*.sh 2>/dev/null || true; chmod +x start_linux.sh openraven/*.sh; command -v wmctrl >/dev/null 2>&1 || sudo apt-get install -y wmctrl >/dev/null 2>&1 || true; HOST_IP=\$(ip route show default 2>/dev/null | awk '{print \$3; exit}'); HALO_RES=`"$HaloRes`" HALO_FULLSCREEN=1 RIO2PAD_STATE_FILE=`"$WslPadState`" RAVENVK_WIN_SERVER=`"\${HOST_IP:-127.0.0.1}:28765`" ./start_linux.sh $($GameArgs -join ' ')"
    }
} finally {
    if ($VkWinProc -and -not $VkWinProc.HasExited) {
        Stop-Process -Id $VkWinProc.Id -Force -ErrorAction SilentlyContinue
    }
    Stop-Job $XInputJob -ErrorAction SilentlyContinue
    Remove-Job $XInputJob -Force -ErrorAction SilentlyContinue
}
