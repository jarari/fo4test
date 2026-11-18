$ErrorActionPreference = 'Stop'
$sdkPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'extern/XeSS'
$sdkRevision = '8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0'
if (-not (Test-Path -LiteralPath $sdkPath)) {
    & git clone --depth 1 --branch v3.0.2 https://github.com/intel/xess.git $sdkPath
    if ($LASTEXITCODE -ne 0) { throw 'XeSS SDK download failed.' }
}
$sdkHead = & git -C $sdkPath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $sdkHead -ne $sdkRevision) {
    throw "Existing XeSS checkout is not v3.0.2 ($sdkRevision). It was not changed."
}
foreach ($sdkFile in @('inc/xess/xess_d3d12.h', 'inc/xess_fg/xefg_swapchain_d3d12.h',
        'inc/xell/xell_d3d12.h', 'bin/libxess.dll', 'bin/libxess_fg.dll', 'bin/libxell.dll', 'LICENSE.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $sdkPath $sdkFile))) { throw "SDK file missing: $sdkFile" }
}
Write-Host "XeSS SDK v3.0.2 ready: $sdkPath"
