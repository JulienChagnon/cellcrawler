# Downloads prebuilt SDL3 and FFmpeg (LGPL, shared) into pc/third_party.
# Usage (from the pc folder):  powershell -ExecutionPolicy Bypass -File fetch_deps.ps1
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$SdlVersion = '3.4.16'
$FfmpegBranch = '8.1'

$root = Join-Path $PSScriptRoot 'third_party'
New-Item -ItemType Directory -Force $root | Out-Null
$tmp = Join-Path $root 'download'
New-Item -ItemType Directory -Force $tmp | Out-Null

function Fetch($url, $zip, $dest) {
    if (Test-Path $dest) { Write-Host "$dest already present"; return }
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $staging = "$dest.staging"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    Expand-Archive -Path $zip -DestinationPath $staging
    # Both archives contain a single top-level folder.
    $inner = Get-ChildItem $staging | Select-Object -First 1
    Move-Item $inner.FullName $dest
    Remove-Item -Recurse -Force $staging
}

Fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SdlVersion/SDL3-devel-$SdlVersion-mingw.zip" `
      (Join-Path $tmp 'sdl3.zip') (Join-Path $root 'SDL3')
Fetch "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n$FfmpegBranch-latest-win64-lgpl-shared-$FfmpegBranch.zip" `
      (Join-Path $tmp 'ffmpeg.zip') (Join-Path $root 'ffmpeg')

Remove-Item -Recurse -Force $tmp
Write-Host "Done. Dependencies are in $root"
