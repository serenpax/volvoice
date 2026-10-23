# build.ps1 - build vi PC_Server (MinGW-w64)
# usage: .\build.ps1

$ErrorActionPreference = "Stop"

# refresh PATH (same approach as C_code/build.ps1)
$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")

Write-Host "=== Building vi PC_Server (C++) ===" -ForegroundColor Cyan

try {
    $gccver = & g++ --version 2>&1 | Select-Object -First 1
    Write-Host "Compiler: $gccver" -ForegroundColor Green
} catch {
    Write-Host "[ERROR] g++ not found. Install MinGW-w64." -ForegroundColor Red
    exit 1
}

$src = @(
    "main.cpp",
    "json.cpp",
    "log.cpp",
    "config.cpp",
    "http_client.cpp",
    "asr.cpp",
    "text_out.cpp",
    "http_server.cpp"
)

$libs = @(
    "-lws2_32",
    "-lwinhttp",
    "-lole32",
    "-loleaut32",
    "-luiautomationcore",
    "-luuid",
    "-lcrypt32",
    "-lshell32",
    "-lgdi32",
    "-luser32"
)

$flags = @(
    "-O2",
    "-s",
    "-std=c++17",
    "-mwindows",
    "-municode",
    "-DUNICODE",
    "-D_UNICODE",
    "-Wall",
    "-Wno-unused-parameter"
)

$out = "PC_Server.exe"

Write-Host "`nCompiling..." -ForegroundColor Yellow
& g++ @flags @src @libs -o $out 2>&1

if ($LASTEXITCODE -eq 0) {
    $size = (Get-Item $out).Length / 1KB
    Write-Host "`n[OK] Build succeeded: $out ($([math]::Round($size, 1)) KB)" -ForegroundColor Green
    Write-Host "`nRun:" -ForegroundColor Cyan
    Write-Host "  .\$out"
    Write-Host "  Then from the phone open http://<PC-LAN-IP>:30686/ping to test."
} else {
    Write-Host "`n[FAIL] Build failed" -ForegroundColor Red
    exit 1
}
