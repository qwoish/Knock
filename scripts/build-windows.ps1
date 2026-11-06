param([string]$OutputDirectory = "out/Knock")
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
    cmake -S . -B build -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
    cmake --build build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed." }
    # The GUI tests run off the desktop, so a running Knock is safe.
    & build\Release\isolated_run.exe -- ctest --test-dir build -C Release --output-on-failure -E '^hotkey$'
    if ($LASTEXITCODE -ne 0) { throw "Tests failed; the release was not packaged." }
    & build\Release\isolated_run.exe --desktop -- ctest --test-dir build -C Release --output-on-failure -R '^hotkey$'
    if ($LASTEXITCODE -ne 0) { throw "Tests failed; the release was not packaged." }
    cmake --install build --config Release --prefix $OutputDirectory
    if ($LASTEXITCODE -ne 0) { throw "Packaging failed." }
    Write-Host "Built Knock in $OutputDirectory"
} finally {
    Pop-Location
}
