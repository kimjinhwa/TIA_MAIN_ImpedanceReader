# ESP AP(192.168.11.1)에 PC WiFi 연결 후 실행
$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "basicInfo.html"
$url = "http://192.168.11.1/basicInfo.html"

Write-Host "Downloading $url ..."
curl.exe -f --connect-timeout 15 $url -o $out
if ($LASTEXITCODE -ne 0) {
    Write-Error "Download failed (exit $LASTEXITCODE). ESP AP에 연결되어 있는지 확인하세요."
}
$size = (Get-Item $out).Length
if ($size -lt 100) {
    Remove-Item $out -Force
    Write-Error "Downloaded file too small ($size bytes). SPIFFS에 basicInfo.html이 없을 수 있습니다."
}
Write-Host "OK: $out ($size bytes)"
