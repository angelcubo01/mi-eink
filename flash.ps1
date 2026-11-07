[CmdletBinding()]
param(
  [ValidatePattern('^COM\d+$')]
  [string]$Port
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$environment = 'trmnl_og_diy'

function Find-PlatformIo {
  $candidates = @(
    (Join-Path $projectRoot '.venv\Scripts\pio.exe'),
    (Get-Command pio -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
    (Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe')
  ) | Where-Object { $_ -and (Test-Path $_) }

  if ($candidates.Count -eq 0) {
    throw 'No se encontró PlatformIO. Instala PlatformIO IDE o ejecuta: pip install platformio'
  }
  return $candidates[0]
}

function Find-XiaoPort {
  $devices = Get-CimInstance Win32_PnPEntity | Where-Object {
    $_.PNPDeviceID -match 'VID_303A&PID_1001' -and $_.Name -match '\(COM\d+\)'
  }
  if ($devices.Count -ne 1) {
    if ($devices.Count -gt 1) {
      throw "Hay varios XIAO conectados: $($devices.Name -join ', '). Ejecuta .\flash.ps1 -Port COMx"
    }
    throw 'No se detectó el XIAO ESP32-S3. Comprueba el cable USB-C de datos. Si sigue sin aparecer, mantén BOOT, pulsa RESET, suelta BOOT y vuelve a ejecutar el script.'
  }
  return ([regex]::Match($devices[0].Name, '\((COM\d+)\)')).Groups[1].Value
}

try {
  $secrets = Join-Path $projectRoot 'include\secrets.h'
  if (-not (Test-Path $secrets)) {
    throw 'Falta include\secrets.h. Copia include\secrets.example.h a include\secrets.h antes de flashear.'
  }

  $secretContent = Get-Content -LiteralPath $secrets -Raw
  $apPasswordMatch = [regex]::Match($secretContent, '(?m)^\s*#define\s+MI_EINK_WIFI_PASSWORD\s+"([^"]*)"')
  if (-not $apPasswordMatch.Success -or $apPasswordMatch.Groups[1].Value.Length -lt 8 -or $apPasswordMatch.Groups[1].Value.Length -gt 63) {
    throw 'MI_EINK_WIFI_PASSWORD debe tener entre 8 y 63 caracteres en include\secrets.h.'
  }
  $weatherMatch = [regex]::Match($secretContent, '(?m)^\s*#define\s+MI_EINK_OPENWEATHER_API_KEY\s+"([^"]*)"')
  if (-not $weatherMatch.Success -or [string]::IsNullOrWhiteSpace($weatherMatch.Groups[1].Value)) {
    Write-Warning 'MI_EINK_OPENWEATHER_API_KEY está vacía: el firmware funcionará, pero el widget Tiempo no podrá consultar OpenWeather.'
  }

  $pio = Find-PlatformIo
  if (-not $Port) { $Port = Find-XiaoPort }

  Write-Host "MiEInk: compilando y cargando en $Port..." -ForegroundColor Cyan
  Push-Location $projectRoot
  try {
    & $pio run -e $environment -t upload --upload-port $Port
    if ($LASTEXITCODE -ne 0) { throw "PlatformIO terminó con código $LASTEXITCODE." }
  } finally {
    Pop-Location
  }

  Write-Host "`nListo. Conéctate al AP definido por MI_EINK_WIFI_SSID y abre http://192.168.4.1" -ForegroundColor Green
} catch {
  Write-Host "`nERROR: $($_.Exception.Message)" -ForegroundColor Red
  exit 1
}
