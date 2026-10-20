param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('before','after','check','export','recover')]
    [string]$Action
)
$ErrorActionPreference = 'Stop'

function Test-ProjectPython {
    param([string]$Executable, [string[]]$PrefixArgs = @())
    try {
        $projectProbe = & $Executable @PrefixArgs -c "import sys; print('STM32_PYTHON_OK' if sys.version_info >= (3, 10) else 'STM32_PYTHON_TOO_OLD')" 2>$null
        return ($LASTEXITCODE -eq 0 -and $projectProbe -contains 'STM32_PYTHON_OK')
    } catch {
        return $false
    }
}

$projectPython = $env:STM32_PYTHON
if ($projectPython) {
    if (-not (Test-ProjectPython -Executable $projectPython)) {
        Write-Error 'STM32_PYTHON must point to a working Python 3.10+ interpreter.'
        exit 1
    }
    & $projectPython (Join-Path $PSScriptRoot 'project_sync.py') $Action
    exit $LASTEXITCODE
}
$projectPyLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
if ($projectPyLauncher -and (Test-ProjectPython -Executable $projectPyLauncher.Source -PrefixArgs @('-3'))) {
    & $projectPyLauncher.Source -3 (Join-Path $PSScriptRoot 'project_sync.py') $Action
    exit $LASTEXITCODE
}
$projectPythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
if ($projectPythonCommand -and (Test-ProjectPython -Executable $projectPythonCommand.Source)) {
    & $projectPythonCommand.Source (Join-Path $PSScriptRoot 'project_sync.py') $Action
    exit $LASTEXITCODE
}
Write-Error 'Python 3.10+ is required. Set STM32_PYTHON to the full path of python.exe.'
exit 1
