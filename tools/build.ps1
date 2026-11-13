param(
    [string]$Config,
    [switch]$Run,
    [switch]$SmokeTest,
    [string]$Name,
    [string]$Python,
    [ValidateSet('all','fps','range')][string]$Profile = 'range',
    [switch]$Protected,
    [switch]$Signed,
    [switch]$Hardened,
    [string]$PublicKey,
    [string]$SigningKey,
    [string]$CMake,
    [string]$Dxc,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($Config) {
    if (-not $Python) {
        foreach ($candidate in @('python','py')) {
            $pythonCommand = Get-Command $candidate -ErrorAction SilentlyContinue
            if ($pythonCommand -and $pythonCommand.Source -notlike '*\WindowsApps\*') {
                $Python = $pythonCommand.Source
                break
            }
        }
        # Optional fallback for this desktop environment; never install anything implicitly.
        if (-not $Python) {
            $bundledPython = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
            if (Test-Path -LiteralPath $bundledPython) { $Python = $bundledPython }
        }
        if (-not $Python) { throw 'Python 3.9+ is required. Pass -Python <python.exe>.' }
    }
    if ($Hardened -or $ConfigureOnly -or $Dxc) { throw 'Selected runtime: use -Config/-Run/-SmokeTest/-Name/-Python/-CMake. Use -Signed/-SigningKey/-PublicKey for selected signing; hardened profiles use the existing build path.' }
    $selectedArgs = @('-B',(Join-Path $PSScriptRoot 'advisor/build_runtime.py'),'--config',(Resolve-Path -LiteralPath $Config).Path)
    if ($Signed) { $selectedArgs += '--signed' }
    if ($SigningKey) { $selectedArgs += @('--signing-key',(Resolve-Path -LiteralPath $SigningKey).Path) }
    if ($PublicKey) { $selectedArgs += @('--public-key',(Resolve-Path -LiteralPath $PublicKey).Path) }
    if ($Run) { $selectedArgs += '--run' }
    if ($SmokeTest) { $selectedArgs += '--smoke-test' }
    if ($Name) { $selectedArgs += @('--name',$Name) }
    if ($CMake) { $selectedArgs += @('--cmake',$CMake) }
    & $Python @selectedArgs
    if ($LASTEXITCODE -ne 0) { throw 'Selected runtime build failed.' }
    return
}
if ($Run -or $SmokeTest -or $Name -or $SigningKey) { throw '-Run/-SmokeTest/-Name/-SigningKey require -Config.' }
if ($Signed) { $Protected = $true }
if ($Profile -eq 'range' -and $Hardened) { throw 'range does not contain local defense services.' }
if (-not $CMake) {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { $CMake = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $CMake = & $vswhere -latest -products '*' -find 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' | Select-Object -First 1
        }
    }
}
if (-not $CMake -or -not (Test-Path -LiteralPath $CMake)) { throw 'Install VS2022 C++/CMake tools, or pass -CMake <cmake.exe>.' }
if (-not $Dxc) {
    $command = Get-Command dxc -ErrorAction SilentlyContinue
    if ($command) { $Dxc = $command.Source }
    else {
        $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
        $Dxc = Get-ChildItem -Path (Join-Path $sdk '*/x64/dxc.exe') -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $Dxc -or -not (Test-Path -LiteralPath $Dxc)) { throw 'Install Windows SDK with dxc.exe or pass -Dxc <dxc.exe>.' }
$buildName = switch ($Profile) { 'all' {'build-event'} 'fps' {'build-fps'} 'range' {'build-range'} }
if ($Signed) { $buildName = if ($Profile -eq 'all') {'build-signed'} else {"build-$Profile-signed"} }
elseif ($Protected) { $buildName = if ($Profile -eq 'all') {'build-protected'} else {"build-$Profile-protected"} }
$buildPath = Join-Path $projectRoot $buildName
$protectFlag = if ($Protected) {'ON'} else {'OFF'}
$verifyFlag = if ($Signed) {'ON'} else {'OFF'}
$hardenFlag = if ($Hardened) {'ON'} else {'OFF'}
$configArgs = @('-S',$projectRoot,'-B',$buildPath,'-G','Visual Studio 17 2022','-A','x64',
    "-DGAME_PROFILE=$Profile","-DHARDENED=$hardenFlag","-DPROTECT_DISTRIBUTION=$protectFlag",
    "-DVERIFY_DISTRIBUTION=$verifyFlag","-DDXC_EXE=$Dxc")
if ($Signed) {
    if (-not $PublicKey) { $PublicKey = Join-Path $projectRoot 'output/release-identity-lab/release-public.bin' }
    $PublicKey = (Resolve-Path -LiteralPath $PublicKey).Path
    $configArgs += "-DDISTRIBUTION_PUBLIC_KEY_FILE=$PublicKey"
}
& $CMake @configArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
if (-not $ConfigureOnly) {
    & $CMake --build $buildPath --config Release --parallel 2
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
}
Write-Host "Build directory: $buildPath"
