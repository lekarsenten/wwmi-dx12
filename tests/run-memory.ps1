$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Install the Visual Studio Desktop development with C++ workload.' }
Import-Module (Join-Path $vsRoot 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$output = Join-Path $repoRoot 'build_addon/tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
Push-Location $output
try {
    $engine = Join-Path $repoRoot 'build_addon/Release/migoto_engine.lib'
    if (-not (Test-Path $engine)) { throw 'Build the Release x64 solution before running resource memory tests.' }
    $includes = @('reshade_addon/migoto', 'build_addon/3dm_src', 'third_party/3Dmigoto',
        'third_party/3Dmigoto/DirectXTK/Inc', 'third_party/3Dmigoto/DirectXTK/Src',
        'third_party/3Dmigoto/HLSLDecompiler', 'third_party/3Dmigoto/D3D_Shaders',
        'third_party/3Dmigoto/crc32c-hw-1.0.5/include', 'third_party/3Dmigoto/pcre2') |
        ForEach-Object { '/external:I' + (Join-Path $repoRoot $_) }
    $definitions = @('/DUNICODE', '/D_UNICODE', '/D_HAS_STD_BYTE=0', '/DMIGOTO_DX=11',
        '/DPCRE2_CODE_UNIT_WIDTH=8', '/DPCRE2_STATIC', '/DCRC32C_STATIC',
        '/D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1', '/D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES_COUNT=1')
    & cl.exe /nologo /c /std:c++17 /EHsc /MT /W4 /WX /external:W0 /O2 @includes @definitions (Join-Path $PSScriptRoot 'resource_memory.cpp')
    if ($LASTEXITCODE -ne 0) { throw "Resource memory build failed: $LASTEXITCODE" }
    $objects = Get-ChildItem (Join-Path $repoRoot 'build_addon/Release/addon') -Filter '*.obj'
    $linkInputs = @('resource_memory.obj', $engine) + @($objects.FullName) + @(
        (Join-Path $repoRoot 'third_party/3Dmigoto/pcre2/pcre2-8-64.lib'),
        (Join-Path $repoRoot 'third_party/3Dmigoto/Nektra/NktHookLib64.lib'))
    $linkArgs = @('/OUT:resource_memory.exe', '/SUBSYSTEM:CONSOLE') +
        @($linkInputs | ForEach-Object { '"' + $_ + '"' }) +
        @('user32.lib', 'gdi32.lib', 'ole32.lib', 'advapi32.lib', 'd3d12.lib', 'd3d11.lib',
        'd3dcompiler.lib', 'dxgi.lib', 'xinput.lib', 'dbghelp.lib', 'shlwapi.lib', 'psapi.lib')
    $linkArgs | Set-Content -Encoding UTF8 memory-link.rsp
    & link.exe /nologo '@memory-link.rsp'
    if ($LASTEXITCODE -ne 0) { throw "Resource memory link failed: $LASTEXITCODE" }
    & ./resource_memory.exe
    if ($LASTEXITCODE -ne 0) { throw "Resource memory tests failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
