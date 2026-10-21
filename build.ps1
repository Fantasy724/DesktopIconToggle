$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$build = Join-Path $root "build"
$dist = Join-Path $root "dist"
$tmp = Join-Path $build "tmp"

New-Item -ItemType Directory -Force $build, $dist, $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp
$env:TMPDIR = $tmp

function Find-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $root "..\tools\w64devkit\bin\$name"),
        (Join-Path $root "tools\w64devkit\bin\$name"),
        "C:\msys64\mingw64\bin\$name",
        "C:\mingw64\bin\$name"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "Cannot find $name. Install MinGW-w64 or add its bin directory to PATH."
}

$cxx = Find-Tool "g++.exe"
$windres = Find-Tool "windres.exe"
$strip = Find-Tool "strip.exe"

Write-Host "C++:      $cxx"
Write-Host "windres:  $windres"

$res = Join-Path $build "app.res"
$out = Join-Path $dist "DesktopIconToggle.exe"

& $windres (Join-Path $src "app.rc") -O coff -o $res
if ($LASTEXITCODE -ne 0) { throw "windres failed" }

$cxxArgs = @(
    "-std=c++17", "-Os", "-s", "-municode", "-mwindows",
    "-ffunction-sections", "-fdata-sections", "-fno-exceptions", "-fno-rtti",
    "-static", "-static-libgcc", "-Wl,--gc-sections",
    (Join-Path $src "main.cpp"), $res,
    "-o", $out,
    "-loleacc", "-lole32", "-loleaut32", "-luuid",
    "-lshell32", "-ladvapi32", "-luser32", "-lgdi32"
)
& $cxx @cxxArgs
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

& $strip --strip-all $out
if ($LASTEXITCODE -ne 0) { throw "strip failed" }

Copy-Item (Join-Path $root "README.md") $dist -Force
Copy-Item (Join-Path $root "LICENSE") $dist -Force

$item = Get-Item $out
Write-Host ""
Write-Host ("Output: {0} ({1:N0} bytes)" -f $item.FullName, $item.Length)
