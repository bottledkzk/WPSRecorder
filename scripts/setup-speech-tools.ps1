param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [Parameter(Mandatory=$true)][string]$ToolsDirectory,
    [string]$Audio8RuntimeDirectory
)
$ErrorActionPreference='Stop'
$runtime=[IO.Path]::GetFullPath($RuntimeDirectory)
$cache=[IO.Path]::GetFullPath($ToolsDirectory)
New-Item -ItemType Directory -Force $runtime,$cache | Out-Null
function Get-Verified([string]$Url,[string]$Name,[string]$Sha256) {
    $file=Join-Path $cache $Name
    if(!(Test-Path -LiteralPath $file)){Invoke-WebRequest -Uri $Url -OutFile $file}
    if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $Sha256){throw "SHA256 mismatch: $file"}
    return $file
}
$whisper = Join-Path $runtime 'whisper'
$asr = Get-Verified 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin' 'ggml-small-q5_1.bin' 'AE85E4A935D7A567BD102FE55AFC16BB595BDB618E11B2FC7591BC08120411BB'
New-Item -ItemType Directory -Path $whisper -Force | Out-Null
Copy-Item -LiteralPath $asr -Destination $whisper -Force

if ($Audio8RuntimeDirectory) {
    $audio8Source = (Resolve-Path -LiteralPath $Audio8RuntimeDirectory).ProviderPath
    $audio8Target = Join-Path $runtime 'tts\audio8'
    foreach ($required in @('python\python.exe', 'runtime\arktts_runtime\runtime.py',
                            'model\runtime_manifest.json', 'voices\default\codes.npy', 'voices\default\meta.json')) {
        if (!(Test-Path -LiteralPath (Join-Path $audio8Source $required) -PathType Leaf)) {
            throw "Incomplete portable Audio8 0.1B runtime: $required"
        }
    }
    if ($audio8Source -ne $audio8Target) {
        New-Item -ItemType Directory -Path $audio8Target -Force | Out-Null
        foreach ($entry in @('python', 'runtime', 'model', 'voices', 'LICENSE', 'NOTICE', 'README.md', 'download.json')) {
            $asset = Join-Path $audio8Source $entry
            if (Test-Path -LiteralPath $asset) {
                Copy-Item -LiteralPath $asset -Destination $audio8Target -Recurse -Force
            }
        }
    }
}
