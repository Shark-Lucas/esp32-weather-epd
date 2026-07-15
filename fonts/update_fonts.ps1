param(
    [switch]$Check,
    [switch]$Apply,
    [switch]$Build,
    [switch]$NoInstall,
    [string]$Font,
    [int[]]$Sizes,
    [string]$Environment,
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"
$Python = (Get-Command python -ErrorAction Stop).Source
$Script = Join-Path $PSScriptRoot "update_fonts.py"
$Requirements = Join-Path $PSScriptRoot "requirements.txt"

if (-not $NoInstall) {
    & $Python -c "import fontTools, PIL" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Installing font conversion dependencies..."
        & $Python -m pip install --user -r $Requirements
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to install font conversion dependencies."
        }
    }
}

$Arguments = @($Script)
if ($Apply) {
    $Arguments += "--apply"
}
if ($Build) {
    $Arguments += "--build"
}
if ($Font) {
    $Arguments += @("--font", $Font)
}
if ($Sizes) {
    $Arguments += "--sizes"
    $Arguments += $Sizes
}
if ($Environment) {
    $Arguments += @("--environment", $Environment)
}
if ($OutputDir) {
    $Arguments += @("--output-dir", $OutputDir)
}

# With no mutation flag the Python tool performs a read-only missing-glyph check.
& $Python @Arguments
exit $LASTEXITCODE
