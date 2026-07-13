$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = Join-Path $Root ".build\esp32"
$OutDir = Join-Path $Root "dist"
New-Item -ItemType Directory -Force -Path $BuildDir, $OutDir | Out-Null
arduino-cli compile `
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" `
  --build-path $BuildDir `
  --output-dir $OutDir `
  --jobs 2 `
  --warnings default `
  (Join-Path $Root "firmware\Nexstar_Protocol_Converter")
