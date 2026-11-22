# Reproducible, out-of-tree Windows Release build of the Slugia runtime,
# Figma development plugin, AOT compiler and Vulkan demo EXE.
param(
  [string]$BuildDirectory = "",
  [string]$OutputDirectory = "",
  [string]$VulkanSdk = "",
  [switch]$FullTests
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDirectory) {
  $BuildDirectory = Join-Path (Split-Path $root -Parent) "slugia-verified-build"
}
if (-not $OutputDirectory) {
  $OutputDirectory = Join-Path $root "dist"
}
if (-not $VulkanSdk) {
  $VulkanSdk = $env:VULKAN_SDK
}
if (-not $VulkanSdk -or -not (Test-Path (Join-Path $VulkanSdk "Include\vulkan\vulkan.h"))) {
  $VulkanSdk = "D:\Developer\VulkanSDK"
}
if (-not (Test-Path (Join-Path $VulkanSdk "Include\vulkan\vulkan.h"))) {
  throw "Vulkan SDK not found. Pass -VulkanSdk 'path\to\VulkanSDK'."
}
function Invoke-Checked {
  param([string]$Executable, [string[]]$Arguments)
  & $Executable @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$Executable failed with exit code $LASTEXITCODE."
  }
}
Invoke-Checked cmake @(
  "-S", $root, "-B", $BuildDirectory, "-G", "Visual Studio 17 2022", "-A", "x64",
  "-DSLUGIA_ENABLE_3D=ON", "-DSLUGIA_BUILD_TESTS=ON", "-DSLUGIA_BUILD_GPU_TESTS=ON",
  "-DSLUGIA_INSTALL=ON",
  "-DVulkan_INCLUDE_DIR=$VulkanSdk/Include",
  "-DVulkan_LIBRARY=$VulkanSdk/Lib/vulkan-1.lib",
  "-DVulkan_GLSLC_EXECUTABLE=$VulkanSdk/Bin/glslc.exe"
)
Invoke-Checked cmake @("--build", $BuildDirectory, "--config", "Release", "--parallel", "8")
if ($FullTests) {
  Invoke-Checked ctest @("--test-dir", $BuildDirectory, "-C", "Release",
                         "--output-on-failure", "--parallel", "4")
} else {
  Invoke-Checked ctest @("--test-dir", $BuildDirectory, "-C", "Release",
                         "-R", "slugia_core|slugia_gpu$|slugia_figma_colorpicker_v5|slugia_figma_gui_import_smoke|slugui_compiler|slugui_figma",
                         "--output-on-failure")
}
Invoke-Checked cmake @("--install", $BuildDirectory, "--config", "Release",
                       "--prefix", $OutputDirectory)
$bin = Join-Path $OutputDirectory "bin"
New-Item -Path $bin -ItemType Directory -Force | Out-Null
# The developer often keeps the previous GUI EXE open. Never terminate it
# or report an old locked EXE as the newly built one.
$sourceExample = Join-Path $BuildDirectory "Release\slugia-example.exe"
$preferredExample = Join-Path $bin "slugia-example.exe"
$launchExample = $preferredExample
try {
  Copy-Item -LiteralPath $sourceExample -Destination $preferredExample -Force -ErrorAction Stop
} catch {
  $sha = (Get-FileHash -LiteralPath $sourceExample -Algorithm SHA256).Hash.Substring(0, 12)
  $launchExample = Join-Path $bin "slugia-example-$sha.exe"
  $alreadyIdentical = (Test-Path $launchExample) -and
    ((Get-FileHash -LiteralPath $launchExample -Algorithm SHA256).Hash -eq
     (Get-FileHash -LiteralPath $sourceExample -Algorithm SHA256).Hash)
  if (-not $alreadyIdentical) {
    Copy-Item -LiteralPath $sourceExample -Destination $launchExample -Force -ErrorAction Stop
  }
  Write-Warning "The previous slugia-example.exe is locked; new Release EXE: $launchExample"
}
foreach ($demo in @("slugia-3d.exe", "slugia-vector.exe", "slugia-vector-baseline.exe")) {
  $builtDemo = Join-Path $BuildDirectory "Release\$demo"
  if (Test-Path $builtDemo) {
    Copy-Item $builtDemo (Join-Path $bin $demo) -Force
  }
}
Set-Content -LiteralPath (Join-Path $bin "slugui-compiler.cmd") -Encoding Ascii -Value @('@echo off', 'python "%~dp0slugui_compiler.py" %*')
$plugin = Join-Path $OutputDirectory "share\slugia\figma-slugui"
if (-not (Test-Path (Join-Path $plugin "manifest.json")) -or
    -not (Test-Path (Join-Path $plugin "code.js")) -or
    -not (Test-Path (Join-Path $bin "slugui_compiler.py"))) {
  throw "Release staging is incomplete."
}
Invoke-Checked node @("--check", (Join-Path $plugin "code.js"))
Invoke-Checked powershell @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                              (Join-Path $root "tools\update_figma_dev_plugin.ps1"),
                              "-OutputDirectory", $OutputDirectory)
Push-Location $root
try {
  Invoke-Checked python @((Join-Path $bin "slugui_compiler.py"),
                           (Join-Path $root "examples\figma_sample.slugui"), "--check")
  Invoke-Checked $launchExample @("--smoke")
} finally {
  Pop-Location
}
Write-Host "Slugia Release verified:" -ForegroundColor Green
Write-Host "  EXE:      $launchExample"
Write-Host "  Compiler: $(Join-Path $bin 'slugui-compiler.cmd')"
Write-Host "  Figma:    $(Join-Path $OutputDirectory 'share\slugia\figma-slugui-v6\manifest.json')"
