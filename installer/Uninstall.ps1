$ErrorActionPreference='Stop'
$target=[IO.Path]::GetFullPath($PSScriptRoot)
$marker=Join-Path $target '.wpsrecorder-install.json'
if (!(Test-Path -LiteralPath $marker)) { throw 'Installation marker is missing; nothing was removed.' }
$installation=Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
if ($installation.application -ne 'wpsrecorder-course-v1' -or [IO.Path]::GetFullPath($installation.install_path) -ne $target) { throw 'Installation identity or path mismatch; nothing was removed.' }
if (!(Test-Path -LiteralPath (Join-Path $target 'wpsrecorder.exe'))) { throw 'Application identity check failed.' }
if (Get-Process wpsrecorder -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $target 'wpsrecorder.exe') }) { throw 'Close the recorder interface before uninstalling.' }
$cli=Join-Path $target 'wpsrecorder_cli.exe'
$status=& $cli --status | ConvertFrom-Json
if ($status.state -eq 'recording' -or $status.scheduled_start) { throw 'Stop the recording and cancel scheduled tasks before uninstalling.' }
& $cli --service-exit 2>$null | Out-Null
Start-Sleep -Milliseconds 200
if ((Read-Host "Remove WPS Recorder from $target ? Type REMOVE") -ne 'REMOVE') { exit 0 }
$shell=New-Object -ComObject WScript.Shell
foreach ($path in $installation.shortcuts) {
    if ((Test-Path -LiteralPath $path) -and $shell.CreateShortcut($path).TargetPath -eq (Join-Path $target 'wpsrecorder.exe')) { Remove-Item -LiteralPath $path }
}
# The absolute target is validated against this install's marker and executable above.
Remove-Item -LiteralPath $target -Recurse -Force
Write-Output 'Removed WPS Recorder. Recording files outside the installation directory were retained.'
