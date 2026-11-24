param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$QtPrefix,
    [Parameter(Mandatory=$true)][string]$FFmpegDirectory,
    [string]$VisualStudio='C:\Program Files\Microsoft Visual Studio\2022\Community'
)
$ErrorActionPreference='Stop'
$outputPath=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $outputPath | Out-Null
$temporaryPath=Join-Path $outputPath 'deploy-temp'
New-Item -ItemType Directory -Force $temporaryPath | Out-Null
$env:TEMP=$temporaryPath
$env:TMP=$temporaryPath
$deploy=Join-Path $QtPrefix 'tools\qt5\bin\windeployqt.exe'
& $deploy --release --no-translations --no-compiler-runtime --no-angle --no-opengl-sw --no-system-d3d-compiler (Join-Path $outputPath 'wpsrecorder.exe')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }
Copy-Item -LiteralPath (Join-Path $FFmpegDirectory 'ffmpeg.exe'),(Join-Path $FFmpegDirectory 'ffprobe.exe') -Destination $outputPath -Force
$compiler=Get-ChildItem -LiteralPath (Join-Path $VisualStudio 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$dumpbin=Join-Path $compiler.FullName 'bin\Hostx64\x64\dumpbin.exe'
$redist=Get-ChildItem -LiteralPath (Join-Path $VisualStudio 'VC\Redist\MSVC') -Filter Microsoft.VC143.CRT -Directory -Recurse | Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
$openmp=Get-ChildItem -LiteralPath (Join-Path $VisualStudio 'VC\Redist\MSVC') -Filter Microsoft.VC143.OpenMP -Directory -Recurse | Where-Object { $_.FullName -match '\\x64\\' -and $_.FullName -notmatch '\\onecore\\' } | Select-Object -First 1
$sources=@{}
foreach ($source in @((Join-Path $QtPrefix 'bin'),$redist.FullName,$openmp.FullName) | Where-Object { $_ }) {
    Get-ChildItem -LiteralPath $source -Filter '*.dll' | ForEach-Object { $sources[$_.Name.ToLowerInvariant()]=$_.FullName }
}
$queue=[Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $outputPath -Recurse -File | Where-Object { $_.Extension -in '.exe','.dll' } | ForEach-Object { $queue.Enqueue($_.FullName) }
$seen=@{}
while ($queue.Count -gt 0) {
    $binary=$queue.Dequeue()
    if ($seen.ContainsKey($binary)) { continue }
    $seen[$binary]=$true
    foreach ($line in (& $dumpbin /dependents $binary)) {
        if ($line -notmatch '^\s*([\w.-]+\.dll)\s*$') { continue }
        $dependency=$Matches[1].ToLowerInvariant()
        $dependencyDirectory=if ($binary.StartsWith((Join-Path $outputPath 'whisper')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { Join-Path $outputPath 'whisper' } else { $outputPath }
        $destination=Join-Path $dependencyDirectory $dependency
        if ($sources.ContainsKey($dependency) -and !(Test-Path -LiteralPath $destination)) {
            Copy-Item -LiteralPath $sources[$dependency] -Destination $destination
            $queue.Enqueue($destination)
        }
    }
}
Write-Output "Deployed $($seen.Count) binaries including transitive Qt and VC runtime dependencies."
@('[Paths]','Prefix=.','Plugins=.','Libraries=.') | Set-Content -LiteralPath (Join-Path $outputPath 'qt.conf') -Encoding ascii
