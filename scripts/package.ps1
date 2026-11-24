param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [Parameter(Mandatory=$true)][string]$Sample,
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$FFmpegRoot,
    [Parameter(Mandatory=$true)][string]$QtPrefix
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repository=[IO.Path]::GetFullPath((Join-Path $project '..\..'))
$destinationPath=[IO.Path]::GetFullPath($Destination)
if ($destinationPath.StartsWith($repository+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Package binaries outside the Git repository.' }
if (Test-Path -LiteralPath $destinationPath) { throw 'Destination already exists; choose a fresh package directory.' }
$executables=@('wpsrecorder.exe','wpsrecorder_cli.exe','ffmpeg.exe','ffprobe.exe')
foreach ($required in ($executables + @('platforms\qwindows.dll','studio\index.html'))) {
    if (!(Test-Path -LiteralPath (Join-Path $RuntimeDirectory $required) -PathType Leaf)) { throw "Incomplete portable runtime: $required" }
}
$sampleFile=Get-Item -LiteralPath $Sample
$sampleExtension=$sampleFile.Extension.ToLowerInvariant()
if ($sampleFile.PSIsContainer -or $sampleExtension -notin '.mp4','.mkv','.mov','.avi','.flv','.webm','.m4v') { throw 'Provide a recorded video sample.' }
$probe=& (Join-Path $RuntimeDirectory 'ffprobe.exe') -v error -select_streams 'v:0' -show_entries 'stream=codec_type:format=duration' -of json $sampleFile.FullName
if ($LASTEXITCODE -ne 0) { throw 'Could not read the recorded sample.' }
$media=($probe -join "`n") | ConvertFrom-Json
$duration=[double]$media.format.duration
$video=$media.streams | Select-Object -First 1
if ($video.codec_type -ne 'video' -or !($duration -gt 0 -and $duration -le 180)) { throw 'The sample must be a playable video no longer than three minutes.' }
New-Item -ItemType Directory -Force $destinationPath | Out-Null
$package=Join-Path $destinationPath 'gongweichao'
$runtime=Join-Path $package 'exe_cli'
New-Item -ItemType Directory -Force $runtime,(Join-Path $package 'sample'),(Join-Path $runtime 'licenses') | Out-Null
Get-ChildItem -LiteralPath $RuntimeDirectory -File | Where-Object { $_.Name -in $executables -or $_.Extension -eq '.dll' } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $runtime }
foreach ($plugin in @('audio','bearer','iconengines','imageformats','mediaservice','platforms','playlistformats','studio','styles','whisper')) {
    $path=Join-Path $RuntimeDirectory $plugin
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $runtime -Recurse }
}
$tts = Join-Path $runtime 'tts'
New-Item -ItemType Directory -Path $tts -Force | Out-Null
foreach ($asset in @('audio8_worker.py', 'audio8', 'voices')) {
    $path = Join-Path $RuntimeDirectory "tts\$asset"
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $tts -Recurse }
}
@('[Paths]','Prefix=.','Plugins=.','Libraries=.') | Set-Content -LiteralPath (Join-Path $runtime 'qt.conf') -Encoding ascii
Copy-Item -LiteralPath $sampleFile.FullName -Destination (Join-Path $package "sample\screen-recording$sampleExtension")
Copy-Item -LiteralPath (Join-Path $FFmpegRoot 'LICENSE'),(Join-Path $FFmpegRoot 'README.txt') -Destination (Join-Path $runtime 'licenses')
foreach ($component in @('qt5-base','qt5-multimedia','qt5-svg','qt5-translations','openssl')) {
    $copyright=Join-Path $QtPrefix "share\$component\copyright"
    if (Test-Path -LiteralPath $copyright) { Copy-Item -LiteralPath $copyright -Destination (Join-Path $runtime "licenses\$component.txt") }
}
$files=Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName
$excluded=@('settings.ini','ui-settings.ini','speech-services.ini','credentials.json','.env','Install.cmd','Install.ps1','Uninstall.ps1')
foreach ($file in $files) {
    if ($file.Name -in $excluded -or $file.Name -like 'recorder_*.exe' -or $file.FullName.Substring($package.Length+1) -match 'kokoro') { throw "Unexpected submission file: $($file.FullName)" }
}
$manifest=$files | ForEach-Object { [PSCustomObject]@{path=$_.FullName.Substring($package.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()} }
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $destinationPath 'manifest.json') -Encoding utf8
$archive=Join-Path $destinationPath 'gongweichao.zip'
Compress-Archive -LiteralPath (Join-Path $package 'exe_cli'),(Join-Path $package 'sample') -DestinationPath $archive
Get-Item -LiteralPath $archive | Select-Object FullName,Length
Get-FileHash -LiteralPath $archive -Algorithm SHA256
