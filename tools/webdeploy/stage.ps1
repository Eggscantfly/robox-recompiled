<#
    Stages the four shippable files out of build-web/ under the names the build
    actually asks for at runtime.

    The build emits mixed casing: CMake sets OUTPUT_NAME "ROBOX", but three of
    the files on disk kept lowercase names from an earlier build because NTFS
    preserves the original casing when a file is overwritten. So robox.html
    asks for ROBOX.js and robox.js asks for ROBOX.data, and neither exists
    under that name. On NTFS nobody notices. On R2 -- or any case-sensitive
    host -- both are a 404 and the page hangs on "loading...".

    Renaming here rather than in CMake keeps the next rebuild from undoing it.

    Usage:  pwsh tools/webdeploy/stage.ps1
#>

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $repo 'build-web'
$dst = Join-Path $repo 'build-web-deploy'

# Source name on disk -> name the runtime fetches.
#
# Read the UPPERCASE ROBOX.* the build actually emits (OUTPUT_NAME "ROBOX").
# This used to read lowercase robox.html/js/data, which on a case-sensitive
# build-web/ were STALE leftovers from an old build sitting beside the fresh
# uppercase files -- so every deploy shipped a stale JS+data with a fresh wasm,
# an old-glue/new-wasm mix that presented as corrupt audio. Assert-fresh below
# guards against it coming back.
$map = [ordered]@{
    'ROBOX.html' = 'index.html'
    'ROBOX.js'   = 'ROBOX.js'
    'ROBOX.wasm' = 'ROBOX.wasm'
    'ROBOX.data' = 'ROBOX.data'
}

if (-not (Test-Path $src)) { throw "no build-web/ at $src -- build the web target first" }

# Refuse to stage anything older than the wasm: the four files are emitted
# together by one link step, so a mismatched mtime means a stale leftover.
$wasmTime = (Get-Item (Join-Path $src 'ROBOX.wasm')).LastWriteTimeUtc
foreach ($from in $map.Keys) {
    $f = Join-Path $src $from
    if (-not (Test-Path $f)) { throw "missing $from in build-web/ -- rebuild the web target" }
    if ((Get-Item $f).LastWriteTimeUtc -lt $wasmTime.AddMinutes(-5)) {
        throw "$from is older than ROBOX.wasm by >5 min -- stale leftover, rebuild clean"
    }
}

if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
New-Item -ItemType Directory $dst | Out-Null

foreach ($from in $map.Keys) {
    $in = Join-Path $src $from
    if (-not (Test-Path $in)) { throw "missing $from in build-web/" }
    Copy-Item $in (Join-Path $dst $map[$from]) -Force
}

# Content-hash the payload filenames.
#
# Without this, a returning visitor can pair a freshly-fetched index.html with
# a ROBOX.js still in their browser cache from a previous deploy. That does not
# degrade -- it aborts at boot with an error about the older build, and it took
# the live site down once. Serving no-cache fixes it only for fetches made
# AFTER the change; entries already cached keep poisoning boots until they age
# out. Hashed names make the pairing impossible instead of unlikely: new bytes
# mean a new URL, and index.html is never cached.
#
# Order matters. Hash the leaves first, rewrite the references, then hash the
# file that now contains them.
function Short-Hash([string]$path) {
    (Get-FileHash $path -Algorithm SHA256).Hash.Substring(0, 10).ToLower()
}

$rename = [ordered]@{}
foreach ($leaf in 'ROBOX.wasm', 'ROBOX.data') {
    $p = Join-Path $dst $leaf
    $h = Short-Hash $p
    $new = $leaf -replace '^ROBOX\.', "ROBOX.$h."
    Move-Item $p (Join-Path $dst $new) -Force
    $rename[$leaf] = $new
}

# ROBOX.js names the wasm and the data, so it has to be rewritten before it is
# hashed -- otherwise its hash would not cover what it actually loads.
$jsPath = Join-Path $dst 'ROBOX.js'
$js = Get-Content $jsPath -Raw
foreach ($k in $rename.Keys) { $js = $js.Replace($k, $rename[$k]) }
Set-Content $jsPath $js -NoNewline -Encoding utf8
$jsHash = Short-Hash $jsPath
$rename['ROBOX.js'] = "ROBOX.$jsHash.js"
Move-Item $jsPath (Join-Path $dst $rename['ROBOX.js']) -Force

$htmlPath = Join-Path $dst 'index.html'
$html = (Get-Content $htmlPath -Raw).Replace('ROBOX.js', $rename['ROBOX.js'])
Set-Content $htmlPath $html -NoNewline -Encoding utf8

# Guard the thing this script exists to prevent: every name referenced by the
# html and the js must be present in the staging directory.
$refs = @{}
foreach ($f in @('index.html', $rename['ROBOX.js'])) {
    $text = Get-Content (Join-Path $dst $f) -Raw
    foreach ($m in [regex]::Matches($text, 'ROBOX\.[0-9a-f.]*(js|wasm|data)')) { $refs[$m.Value] = $f }
}
if ($refs.Count -eq 0) { throw "no ROBOX.* references found -- the rewrite silently did nothing" }
foreach ($ref in $refs.Keys) {
    if (-not (Test-Path (Join-Path $dst $ref))) {
        throw "$($refs[$ref]) references $ref but it is not in the staged output"
    }
}

Get-ChildItem $dst | Select-Object Name, @{n = 'MB'; e = { '{0:N2}' -f ($_.Length / 1MB) } } | Format-Table
Write-Host "staged $($map.Count) files -> $dst"
Write-Host "verified refs: $($refs.Keys -join ', ')"
