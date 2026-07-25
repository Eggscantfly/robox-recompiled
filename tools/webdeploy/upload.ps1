<#
    Pushes the staged payload into the R2 bucket.

    Uploads whatever stage.ps1 produced rather than a fixed list, because the
    payload filenames now carry a content hash and change every time the build
    does.

    Content types are passed explicitly: R2 stores whatever the upload declared,
    and a .wasm served as anything other than application/wasm loses streaming
    compilation (the browser buffers the whole 16 MB first).

    Usage:  pwsh tools/webdeploy/upload.ps1            # upload
            pwsh tools/webdeploy/upload.ps1 -CreateBucket   # first run
#>

param(
    [string]$Bucket = 'robox-web',
    [switch]$CreateBucket
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$dst = Join-Path $repo 'build-web-deploy'

if (-not (Test-Path $dst)) { throw "no build-web-deploy/ -- run tools/webdeploy/stage.ps1 first" }

$types = @{
    '.html' = 'text/html; charset=utf-8'
    '.js'   = 'text/javascript; charset=utf-8'
    '.wasm' = 'application/wasm'
    '.data' = 'application/octet-stream'
}

if ($CreateBucket) {
    Write-Host "creating bucket $Bucket ..."
    npx --yes wrangler r2 bucket create $Bucket
    if ($LASTEXITCODE -ne 0) { throw "bucket create failed (already exists? drop -CreateBucket)" }
}

$files = Get-ChildItem $dst -File
if (-not $files) { throw "build-web-deploy/ is empty" }

foreach ($f in $files) {
    $ct = $types[$f.Extension.ToLower()]
    if (-not $ct) { $ct = 'application/octet-stream' }
    $mb = '{0:N1}' -f ($f.Length / 1MB)
    Write-Host "-> $Bucket/$($f.Name)  ($mb MB)"
    npx --yes wrangler r2 object put "$Bucket/$($f.Name)" --file $f.FullName --content-type $ct --remote
    if ($LASTEXITCODE -ne 0) { throw "upload failed for $($f.Name)" }
}

Write-Host ""
Write-Host "uploaded $($files.Count) objects to $Bucket"
