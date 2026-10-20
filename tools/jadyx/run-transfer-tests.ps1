# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Production-library runner adapted from run-depth-copy-tests.ps1.
param([Parameter(Mandatory=$true)][string]$JadyxRoot,
      [Parameter(Mandatory=$true)][string]$RuntimeLayout,
      [ValidateSet('Pool','Depth','Bindings','Rows','Readback','FrameChain','PartialSubmit','ContinuousSubmit','FirmwareSync')][string]$Mode='Pool')
$ErrorActionPreference='Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Use Windows PowerShell 5.1 for Appx APIs' }
$env:PSModulePath=[Environment]::GetEnvironmentVariable('PSModulePath','Machine')
$taskRoot=(Resolve-Path -LiteralPath $JadyxRoot).Path
$taskLayout=(Resolve-Path -LiteralPath $RuntimeLayout).Path
$taskBuild=Join-Path $taskRoot 'build-uwp'
$taskNinja=Get-Content -LiteralPath (Join-Path $taskBuild 'build.ninja')
$taskObject=($taskNinja | Select-String -SimpleMatch 'build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_image_copy_blit.cpp.obj:').LineNumber
if (!$taskObject) { throw 'Configured D3D12 object missing' }
$taskIncludes=($taskNinja[$taskObject..($taskObject+6)] | Where-Object {$_ -match '^  INCLUDES ='}) -replace '^  INCLUDES = ',''
$taskLibs=($taskNinja | Where-Object {$_ -match '^  LINK_LIBRARIES = .*video_core.lib'} | Select-Object -Last 1) -replace '^  LINK_LIBRARIES = ',''
if (!$taskIncludes -or !$taskLibs) { throw 'Configured includes/libraries missing' }
$taskVs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw 'MSVC missing' }
$taskRelative='seriesx-transfer-gate-'+$Mode.ToLowerInvariant()
$taskGate=New-Item -ItemType Directory -Force -Path (Join-Path $taskBuild $taskRelative)
$taskSource=Join-Path $PSScriptRoot 'tests\ordered-copy-pool.cpp'
if ($Mode -eq 'Readback') { $taskSource=Join-Path $PSScriptRoot 'tests\readback-gpu.cpp' }
if ($Mode -eq 'FrameChain') { $taskSource=Join-Path $PSScriptRoot 'tests\frame-chain-gpu.cpp' }
if ($Mode -eq 'PartialSubmit') { $taskSource=Join-Path $PSScriptRoot 'tests\partial-submit-gpu.cpp' }
if ($Mode -eq 'ContinuousSubmit') { $taskSource=Join-Path $PSScriptRoot 'tests\continuous-submit-gpu.cpp' }
if ($Mode -eq 'FirmwareSync') { $taskSource=Join-Path $PSScriptRoot 'tests\firmware-sync-gpu.cpp' }
if ($Mode -eq 'Bindings') { $taskSource=Join-Path $PSScriptRoot 'tests\incremental-uniform-bindings.cpp' }
if ($Mode -eq 'Rows') { $taskSource=Join-Path $PSScriptRoot 'tests\partial-texture-rows.cpp' }
if ($Mode -eq 'Depth') {
    $taskOriginal=(Join-Path $taskRoot 'tools\xbox\tests\depth-region-copy.cpp').Replace('\','/')
    $taskSource=Join-Path $taskGate.FullName 'depth-ordered-wrapper.cpp'
    $taskWrapper=@'
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#define main OriginalCopyGate
#include "UPSTREAM_TEST"
#undef main
int main() {
    D3D12::SetOrderedCopyReuse(true);
    D3D12::SetCopyPlanCache(true);
    return OriginalCopyGate();
}
'@
    Set-Content -LiteralPath $taskSource -Value ($taskWrapper.Replace('UPSTREAM_TEST',$taskOriginal)) -Encoding utf8
}
$taskArgs='/nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK '+
    $taskIncludes+' /I"'+$taskRoot+'" "'+$taskSource+'" src\jadyx\CMakeFiles\jadyx.dir\app\vma_impl.cpp.obj '+
    '/Fo:'+$taskRelative+'\ /Fe:'+$taskRelative+'\ordered-copy-pool.exe /link /OPT:REF /NODEFAULTLIB:libcmt d3dcompiler.lib '+$taskLibs
Set-Content -LiteralPath (Join-Path $taskGate.FullName 'test.rsp') -Value $taskArgs -Encoding utf8
$taskCommands=@('@echo off',('call "'+$taskVs+'\VC\Auxiliary\Build\vcvarsall.bat" x64'),'if errorlevel 1 exit /b 1',
    ('cd /d "'+$taskBuild+'"'),('cl @'+$taskRelative+'\test.rsp'),'exit /b %errorlevel%')
$taskCommand=Join-Path $taskGate.FullName 'build.cmd'
Set-Content -LiteralPath $taskCommand -Value $taskCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c $taskCommand *> (Join-Path $taskGate.FullName 'build.log')
if ($LASTEXITCODE -ne 0) { throw ('Transfer test build failed: '+$taskGate.FullName) }
Get-ChildItem -LiteralPath $taskLayout -Filter '*.dll' -File | Copy-Item -Destination $taskGate.FullName -Force
$taskOldPath=$env:PATH
try {
    $taskCrt=Get-AppxPackage 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64' | Select-Object -First 1 -ExpandProperty InstallLocation
    if (!$taskCrt) { throw 'Store CRT runtime missing' }
    $env:PATH=$taskCrt+';'+$env:PATH
    $taskCases=if ($Mode -ne 'Depth') { @('inline','async') } else { @('inline') }
    foreach ($taskCase in $taskCases) {
        $taskStart=@{FilePath=(Join-Path $taskGate.FullName 'ordered-copy-pool.exe');WorkingDirectory=$taskGate.FullName;
            Wait=$true;PassThru=$true;WindowStyle='Hidden';
            RedirectStandardOutput=(Join-Path $taskGate.FullName ($taskCase+'.stdout.txt'));
            RedirectStandardError=(Join-Path $taskGate.FullName ($taskCase+'.stderr.txt'))}
        if ($taskCase -eq 'async') { $taskStart.ArgumentList='--async' }
        $taskProcess=Start-Process @taskStart
        if ($taskProcess.ExitCode -ne 0) { throw ('Transfer GPU gate failed: '+$taskCase+' exit '+$taskProcess.ExitCode+' '+$taskGate.FullName) }
        $taskOutput=Get-Content -LiteralPath $taskStart.RedirectStandardOutput -Raw
        if (!$taskOutput.Contains('D3D12 debug clean')) { throw 'Transfer test completion missing' }
        $taskOutput
    }
} finally {
    $env:PATH=$taskOldPath
}
