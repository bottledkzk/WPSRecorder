param(
    [string]$Destination,
    [switch]$NoShortcuts,
    [switch]$NoDesktopShortcut,
    [switch]$Launch
)
$ErrorActionPreference='Stop'
$source=Join-Path $PSScriptRoot 'exe_cli'
$sourceApp=Join-Path $source 'wpsrecorder.exe'
$sourceCli=Join-Path $source 'wpsrecorder_cli.exe'
if (!(Test-Path -LiteralPath $sourceApp -PathType Leaf) -or !(Test-Path -LiteralPath $sourceCli -PathType Leaf)) {
    throw 'Run this installer from the complete extracted package.'
}
$versionString=& $sourceCli --version
if ($LASTEXITCODE -ne 0 -or $versionString -notmatch '^wpsrecorder (\d+\.\d+\.\d+)$') { throw 'Could not identify the recorder package version.' }
$version=$Matches[1]
if (!$Destination) { $Destination=Join-Path $env:LOCALAPPDATA "Programs\WPSRecorder-$version" }
$target=[IO.Path]::GetFullPath($Destination)
$marker=Join-Path $target '.wpsrecorder-install.json'
$app=Join-Path $target 'wpsrecorder.exe'
$cli=Join-Path $target 'wpsrecorder_cli.exe'
function Get-ExecutableDigest([string]$Path) {
    $file=[IO.File]::OpenRead($Path)
    $sha=[Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($file)) }
    finally { $sha.Dispose(); $file.Dispose() }
}
$appHash=Get-ExecutableDigest $sourceApp
$cliHash=Get-ExecutableDigest $sourceCli
# Create new files so they inherit the destination's integrity label.
function Write-InstalledFile([string]$From, [string]$To) {
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($To)) | Out-Null
    $inputStream=[IO.File]::OpenRead($From)
    try {
        $outputStream=[IO.File]::Open($To,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
    } finally { $inputStream.Dispose() }
}
$shortcuts=@()
if (Test-Path -LiteralPath $marker -PathType Leaf) {
    $existing=Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
    if ($existing.application -ne 'wpsrecorder-course-v1' -or $existing.install_path -ne $target -or
        $existing.version -ne $version -or !(Test-Path -LiteralPath $app -PathType Leaf) -or
        !(Test-Path -LiteralPath $cli -PathType Leaf) -or (Get-ExecutableDigest $app) -ne $appHash -or
        (Get-ExecutableDigest $cli) -ne $cliHash) {
        throw 'Destination contains a different installation. Choose a fresh install directory.'
    }
    $shortcuts=@($existing.shortcuts | Where-Object { $_ })
} else {
    if (Test-Path -LiteralPath $target) {
        if (!(Test-Path -LiteralPath $target -PathType Container) -or @(Get-ChildItem -LiteralPath $target -Force).Count -gt 0) {
            throw 'Destination is not empty. Choose a fresh install directory.'
        }
        Remove-Item -LiteralPath $target
    }
    $parent=Split-Path -LiteralPath $target
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $stage=Join-Path $parent ('.wpsrecorder-install-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    try {
        Get-ChildItem -LiteralPath $source -Force -Recurse -File | ForEach-Object {
            Write-InstalledFile $_.FullName (Join-Path $stage $_.FullName.Substring($source.Length+1))
        }
        Write-InstalledFile (Join-Path $PSScriptRoot 'Uninstall.ps1') (Join-Path $stage 'Uninstall.ps1')
        @{application='wpsrecorder-course-v1';install_path=$target;version=$version;shortcuts=@()} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage '.wpsrecorder-install.json') -Encoding utf8
        # Directory.Move fails atomically if another installer has already created the destination.
        [IO.Directory]::Move($stage, $target)
    } finally {
        if ((Test-Path -LiteralPath $stage -PathType Container) -and (Resolve-Path -LiteralPath $stage).Path -eq $stage) {
            Remove-Item -LiteralPath $stage -Recurse -Force
        }
    }
}
$devices=& $cli --devices | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $devices.process_integrity_rid -lt 8192 -or $devices.process_app_container) {
    throw 'Windows still runs this installation with restricted permissions. Use a normal user folder such as LocalAppData\Programs and start Install.cmd in Explorer.'
}
if (!$NoShortcuts) {
    $shell=New-Object -ComObject WScript.Shell
    $folders=@([Environment]::GetFolderPath('Programs'))
    if (!$NoDesktopShortcut) { $folders+=@([Environment]::GetFolderPath('Desktop')) }
    foreach ($folder in $folders) {
        $path=Join-Path $folder 'WPS Recorder.lnk'
        if (Test-Path -LiteralPath $path) {
            $oldTarget=$shell.CreateShortcut($path).TargetPath
            $oldInstall=$null
            if ([IO.Path]::GetFileName($oldTarget) -eq 'wpsrecorder.exe') {
                $oldFolder=Split-Path -LiteralPath $oldTarget
                $oldMarker=Join-Path $oldFolder '.wpsrecorder-install.json'
                if (Test-Path -LiteralPath $oldMarker) {
                    try { $oldInstall=Get-Content -LiteralPath $oldMarker -Raw | ConvertFrom-Json } catch { }
                }
            }
            if ($oldInstall.application -ne 'wpsrecorder-course-v1' -or $oldInstall.install_path -ne $oldFolder -or $oldInstall.shortcuts -notcontains $path) {
                Write-Warning "Existing shortcut was retained: $path"
                continue
            }
        }
        & $cli --create-shortcut --output $path
        if ($LASTEXITCODE -ne 0) { throw "Could not register the shortcut: $path" }
        $shortcuts+=$path
    }
}
@{application='wpsrecorder-course-v1';install_path=$target;version=$version;shortcuts=@($shortcuts | Select-Object -Unique)} |
    ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding utf8
Write-Output "Installed WPS Recorder to $target"
if ($Launch) { Start-Process -FilePath $app -WorkingDirectory $target }
