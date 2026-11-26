# Downloads the pinned official Genymobile/scrcpy Windows 64-bit release (v5.0.1) into .\scrcpy\
$ErrorActionPreference = "Stop"

$scrcpyTag = "v5.0.1"
$destDir = Join-Path $PSScriptRoot "scrcpy"
Write-Host "Fetching official scrcpy $scrcpyTag release metadata from GitHub..."
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/Genymobile/scrcpy/releases/tags/$scrcpyTag" -Headers @{ "User-Agent" = "scrcpy-keymapper-setup" }

$asset = $release.assets | Where-Object { $_.name -match "scrcpy-win64-.*\.zip$" } | Select-Object -First 1
if (-not $asset) {
    throw "Could not find scrcpy-win64 zip in latest release."
}

$zipPath = Join-Path $PSScriptRoot $asset.name
Write-Host "Downloading $($asset.name) ($($release.tag_name))..."
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath

$tmpExtract = Join-Path $PSScriptRoot "_scrcpy_tmp"
if (Test-Path $tmpExtract) { Remove-Item -Recurse -Force $tmpExtract }
Expand-Archive -Path $zipPath -DestinationPath $tmpExtract -Force

$innerDir = Get-ChildItem -Path $tmpExtract -Directory | Select-Object -First 1
if (Test-Path $destDir) { Remove-Item -Recurse -Force $destDir }
Move-Item -Path $innerDir.FullName -Destination $destDir

Remove-Item -Recurse -Force $tmpExtract
Remove-Item -Force $zipPath

Write-Host ""
Write-Host "Done! Installed scrcpy $($release.tag_name) into: $destDir"
Write-Host "Now plug in your Android phone (USB Debugging ON) and run: .\PhantomTouch.exe"
