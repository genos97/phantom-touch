# Downloads the pinned official Genymobile/scrcpy Windows 64-bit release (v5.0.1) into .\scrcpy\
$ErrorActionPreference = "Stop"

$scrcpyTag = "v5.0.1"
$expectedSha256 = "b12a2c4ee8be317422451fc7dcf8ee20a71b5ea7ef9ad73ddd825a25316227a5"
$destDir = Join-Path $PSScriptRoot "scrcpy"
Write-Host "Fetching official scrcpy $scrcpyTag release metadata from GitHub..."
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/Genymobile/scrcpy/releases/tags/$scrcpyTag" -Headers @{ "User-Agent" = "scrcpy-keymapper-setup" }

$asset = $release.assets | Where-Object { $_.name -match "scrcpy-win64-.*\.zip$" } | Select-Object -First 1
if (-not $asset) {
    throw "Could not find scrcpy-win64 zip in release $scrcpyTag."
}

$zipPath = Join-Path $PSScriptRoot $asset.name
Write-Host "Downloading $($asset.name) ($($release.tag_name))..."
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath

$actualHash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedSha256) {
    Remove-Item -Force $zipPath -ErrorAction SilentlyContinue
    throw "SHA-256 verification failed for $($asset.name)! Expected $expectedSha256, got $actualHash."
}
Write-Host "Verified SHA-256: $actualHash"

$tmpExtract = Join-Path $PSScriptRoot "_scrcpy_tmp"
if (Test-Path $tmpExtract) { Remove-Item -Recurse -Force $tmpExtract }
Expand-Archive -Path $zipPath -DestinationPath $tmpExtract -Force

$innerDir = Get-ChildItem -Path $tmpExtract -Directory | Select-Object -First 1
if (Test-Path $destDir) { Remove-Item -Recurse -Force $destDir }
Move-Item -Path $innerDir.FullName -Destination $destDir

Remove-Item -Recurse -Force $tmpExtract
Remove-Item -Force $zipPath

Write-Host ""
Write-Host "Done! Installed verified scrcpy $($release.tag_name) into: $destDir"
Write-Host "Now plug in your Android phone (USB Debugging ON) and run: .\PhantomTouch.exe"
