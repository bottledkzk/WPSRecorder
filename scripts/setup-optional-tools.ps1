param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [Parameter(Mandatory=$true)][string]$ToolsDirectory,
    [switch]$CompatibleFFmpeg,
    [string]$VisualStudio='C:\Program Files\Microsoft Visual Studio\2022\Community'
)
$ErrorActionPreference='Stop'
$runtime=[IO.Path]::GetFullPath($RuntimeDirectory)
$cache=[IO.Path]::GetFullPath($ToolsDirectory)
New-Item -ItemType Directory -Force $runtime,$cache | Out-Null
function Get-VerifiedFile([string]$Url,[string]$Path,[string]$Sha256) {
    if (!(Test-Path -LiteralPath $Path)) { Invoke-WebRequest -Uri $Url -OutFile $Path }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Sha256) { throw "SHA256 mismatch: $Path" }
}
$archive=Join-Path $cache 'whisper-1.9.5.zip'
Get-VerifiedFile 'https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.9.5.zip' $archive '7A7A73071939F16BC028AD2F713F16F26E37AFB6CC155D0F19E3D65705D2B1D9'
$model=Join-Path $cache 'ggml-base.bin'
Get-VerifiedFile 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin' $model '60ED5BC3DD14EEA856493D334349B405782DDCAF0028D4B5DF4088345FBA2EFE'
$source=Join-Path $cache 'whisper.cpp-1.9.5'
if (!(Test-Path -LiteralPath $source)) { Expand-Archive -LiteralPath $archive -DestinationPath $cache }
$build=Join-Path $cache 'whisper-build'
& cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 -DGGML_NATIVE=OFF -DGGML_CUDA=OFF -DGGML_AVX2=ON -DGGML_AVX512=OFF -DWHISPER_BUILD_TESTS=OFF -DWHISPER_CURL=OFF
if ($LASTEXITCODE -ne 0) { throw 'Whisper configure failed' }
& cmake --build $build --config Release --target whisper-cli --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Whisper build failed' }
$destination=Join-Path $runtime 'whisper'
New-Item -ItemType Directory -Force $destination | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'bin\Release\whisper-cli.exe'),$model,(Join-Path $source 'LICENSE') -Destination $destination -Force
Get-ChildItem -LiteralPath (Join-Path $build 'bin\Release') -Filter '*.dll' | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $destination -Force }
$redist=Get-ChildItem -LiteralPath (Join-Path $VisualStudio 'VC\Redist\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
foreach ($component in @('Microsoft.VC143.CRT','Microsoft.VC143.OpenMP')) {
    Get-ChildItem -LiteralPath (Join-Path $redist.FullName "x64\$component") -Filter '*.dll' | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $destination -Force }
}
@('whisper.cpp v1.9.5 - https://github.com/ggml-org/whisper.cpp','Multilingual base GGML model - https://huggingface.co/ggerganov/whisper.cpp','Model SHA256: 60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe','CPU build requires AVX2; no network is used during transcription.') | Set-Content -LiteralPath (Join-Path $destination 'SOURCES.txt') -Encoding utf8
if ($CompatibleFFmpeg) {
    $ffmpegArchive=Join-Path $cache 'ffmpeg-7.1.1.zip'
    Get-VerifiedFile 'https://github.com/GyanD/codexffmpeg/releases/download/7.1.1/ffmpeg-7.1.1-essentials_build.zip' $ffmpegArchive '04861D3339C5EBE38B56C19A15CF2C0CC97F5DE4FA8910E4D47E5E6404E4A2D4'
    $ffmpegRoot=Join-Path $cache 'ffmpeg-7.1.1-essentials_build'
    if (!(Test-Path -LiteralPath $ffmpegRoot)) { Expand-Archive -LiteralPath $ffmpegArchive -DestinationPath $cache }
    Copy-Item -LiteralPath (Join-Path $ffmpegRoot 'bin\ffmpeg.exe'),(Join-Path $ffmpegRoot 'bin\ffprobe.exe') -Destination $runtime -Force
}
Write-Output "Offline subtitle tools prepared at $destination"
