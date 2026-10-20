param([Parameter(Mandatory=$true)][ValidateSet('both-on','both-off','zero-only','frame-only','draw-plan-on','draw-plan-off','cache-pressure','readback-on','readback-off','partial-submit-on','partial-submit-off','supply-control','supply-continuous','supply-coalesced','supply-combined','sync-control','sync-nv-events','sync-consumer-fences','sync-callbacks','sync-combined')][string]$Variant,
      [Parameter(Mandatory=$true)][string]$VerificationDir,
      [Parameter(Mandatory=$true)][int]$ExpectedProcessId)
$ErrorActionPreference='Stop'
$taskProcess=Get-Process -Id $ExpectedProcessId
if ($taskProcess.ProcessName -ne 'jadyx') { throw 'Expected Jadyx process' }
$taskExeHash=(Get-FileHash -LiteralPath $taskProcess.Path).Hash.ToLowerInvariant()
$taskRuntime=Split-Path -Parent $taskProcess.Path
$taskConfig=Get-Content -LiteralPath (Join-Path $taskRuntime 'boot.cfg')
$taskZero=$Variant -in @('both-on','zero-only')
$taskFrame=$Variant -in @('both-on','frame-only')
foreach ($taskPair in @(@('jit_zero_upper',$taskZero),@('jit_stable_frame',$taskFrame))) {
    if (!($taskConfig -contains ($taskPair[0]+'='+[int]$taskPair[1]))) { throw 'Unexpected runtime JIT option' }
}
if ($taskConfig -match '^profile=(1|full)$|^gpu_profile=1$') { throw 'Disable detailed profiling before measuring' }
$taskDrawPlan=$Variant -eq 'draw-plan-on'
if ($Variant -like 'draw-plan-*') {
    if (!($taskConfig -contains ('draw_plan_reuse='+[int]$taskDrawPlan))) { throw 'Unexpected draw-plan option' }
    if ($taskConfig -contains 'draw_plan_validate=1') { throw 'Disable reference validation before measuring' }
} elseif ($taskConfig -contains 'draw_plan_reuse=1') { throw 'Use a draw-plan variant for this executable' }
if ($Variant -eq 'cache-pressure' -and !($taskConfig -contains 'draw_plan_reuse=0')) {
    throw 'The cache-pressure candidate must leave DrawPlan disabled'
}
if ($Variant -like 'readback-*') {
    foreach ($taskOption in @('readback_reuse','readback_copy_coalesce','gc_readback_batch','pool_trim')) {
        if (!($taskConfig -contains ($taskOption+'='+[int]($Variant -eq 'readback-on')))) {
            throw ('Unexpected readback option: '+$taskOption)
        }
    }
    if (!($taskConfig -contains 'draw_plan_reuse=0')) { throw 'Readback comparison requires DrawPlan off' }
}
$taskOutput=(New-Item -ItemType Directory -Path $VerificationDir -Force).FullName
if ($Variant -like 'partial-submit-*') {
    if (!($taskConfig -contains ('partial_submit='+[int]($Variant -eq 'partial-submit-on')))) {
        throw 'Unexpected partial submission option'
    }
    foreach ($taskOption in @('readback_reuse','readback_copy_coalesce','gc_readback_batch','pool_trim')) {
        if (!($taskConfig -contains ($taskOption+'=1'))) { throw ('Keep readback recovery on: '+$taskOption) }
    }
}
if ($Variant -like 'supply-*') {
    if (!($taskConfig -contains 'partial_submit=1')) { throw 'Supply comparison requires the .28 partial-submit base' }
    $taskContinuous=$Variant -in @('supply-continuous','supply-combined')
    $taskCoalesced=$Variant -in @('supply-coalesced','supply-combined')
    foreach ($taskPair in @(@('continuous_submit',$taskContinuous),@('coalesce_fence_flush',$taskCoalesced))) {
        if (!($taskConfig -contains ($taskPair[0]+'='+[int]$taskPair[1]))) { throw 'Unexpected supply comparison option' }
    }
    foreach ($taskOption in @('readback_reuse','readback_copy_coalesce','gc_readback_batch','pool_trim')) {
        if (!($taskConfig -contains ($taskOption+'=1'))) { throw ('Keep readback recovery on: '+$taskOption) }
    }
    if ($taskConfig -contains 'async_record=1') { throw 'Supply comparison requires inline recording' }
}
if ($Variant -like 'sync-*') {
    foreach ($taskOption in @('partial_submit','continuous_submit','coalesce_fence_flush',
                              'readback_reuse','readback_copy_coalesce','gc_readback_batch','pool_trim')) {
        if (!($taskConfig -contains ($taskOption+'=1'))) { throw ('Keep the .29 base enabled: '+$taskOption) }
    }
    $taskSyncFlags=@{
        nonblocking_nv_events=($Variant -in @('sync-nv-events','sync-combined'))
        consumer_release_fences=($Variant -in @('sync-consumer-fences','sync-combined'))
        detached_syncpoint_actions=($Variant -in @('sync-callbacks','sync-combined'))
    }
    foreach ($taskOption in $taskSyncFlags.Keys) {
        if (!($taskConfig -contains ($taskOption+'='+[int]$taskSyncFlags[$taskOption]))) {
            throw ('Unexpected synchronization option: '+$taskOption)
        }
    }
    if ($taskConfig -contains 'async_record=1') { throw 'Synchronization comparison requires inline recording' }
}
$taskLog=Join-Path $env:LOCALAPPDATA 'Packages\SeriesX.JadyxBinaryProbe_s0hs1mdp2yrec\LocalState\jadyx\log\jadyx_log.txt'
$taskUtc=[DateTime]::UtcNow.ToString('o')
$taskLast=Get-Content -LiteralPath $taskLog -Tail 30 |
    Where-Object {$_ -match '^\[\s*\d+\.\d+\]'} | Select-Object -Last 1
if ($taskLast -notmatch '^\[\s*(?<elapsed>\d+\.\d+)\]') { throw 'No current log timestamp' }
$taskMinimum=[double]::Parse($Matches.elapsed,[Globalization.CultureInfo]::InvariantCulture)+2
$taskMarker=[ordered]@{utc=$taskUtc;minimum_complete_window_start_s=$taskMinimum;
    pid=$ExpectedProcessId;variant=$Variant;controls_sent=$false}
$taskMarker | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskOutput ($Variant+'-start.json'))
$taskMarker | ConvertTo-Json
$taskDeadline=(Get-Date).AddSeconds(120)
do {
    $taskProcess=Get-Process -Id $ExpectedProcessId
    if ($taskProcess.HasExited) { throw 'Jadyx exited while measuring' }
    $taskWindows=@(Get-Content -LiteralPath $taskLog -Tail 2200 | ForEach-Object {
        if ($_ -match '^\[\s*(?<end>\d+\.\d+)\].*D3D12 perf over (?<frames>\d+) frames \((?<ms>\d+) ms\)') {
            $taskEnd=[double]::Parse($Matches.end,[Globalization.CultureInfo]::InvariantCulture)
            $taskMs=[int]$Matches.ms
            $taskFrames=[int]$Matches.frames
            if ($taskFrames -eq 300 -and $taskMs -gt 0 -and ($taskEnd-$taskMs/1000) -ge $taskMinimum) {
                [pscustomobject]@{elapsed_end_s=$taskEnd;elapsed_start_s=($taskEnd-$taskMs/1000);
                    frames=$taskFrames;wall_ms=$taskMs;mean_frame_ms=$taskMs/$taskFrames;
                    fps=$taskFrames*1000/$taskMs;raw=$_.ToString()}
            }
        }
    } | Select-Object -First 4)
    if ($taskWindows.Count -lt 4) { Start-Sleep -Seconds 2 }
} while ($taskWindows.Count -lt 4 -and (Get-Date) -lt $taskDeadline)
if ($taskWindows.Count -ne 4) { throw 'Four complete windows not available before timeout' }
$taskFrames=($taskWindows | Measure-Object frames -Sum).Sum
$taskMs=($taskWindows | Measure-Object wall_ms -Sum).Sum
$taskPacing=@(Get-Content -LiteralPath $taskLog -Tail 2200 | ForEach-Object {
    if ($_ -match '^\[\s*(?<end>\d+\.\d+)\].*D3D12 frame times: .*p50 (?<p50>[\d.]+) ms, p95 (?<p95>[\d.]+) ms, p99 (?<p99>[\d.]+) ms') {
        [pscustomobject]@{
            elapsed_s=[double]::Parse($Matches.end,[Globalization.CultureInfo]::InvariantCulture);
            p50_ms=[double]::Parse($Matches.p50,[Globalization.CultureInfo]::InvariantCulture);
            p95_ms=[double]::Parse($Matches.p95,[Globalization.CultureInfo]::InvariantCulture);
            p99_ms=[double]::Parse($Matches.p99,[Globalization.CultureInfo]::InvariantCulture)}
    }
})
foreach ($taskWindow in $taskWindows) {
    $taskQuantiles=@($taskPacing | Where-Object {[Math]::Abs($_.elapsed_s-$taskWindow.elapsed_end_s) -lt 0.1})
    if ($taskQuantiles.Count -ne 1) { throw 'Missing or ambiguous frame-window percentiles' }
    foreach ($taskField in @('p50_ms','p95_ms','p99_ms')) {
        $taskWindow | Add-Member -NotePropertyName $taskField -NotePropertyValue $taskQuantiles[0].$taskField
    }
}
$taskResult=[ordered]@{variant=$Variant;exe_sha256=$taskExeHash;jit_zero_upper=$taskZero;
    jit_stable_frame=$taskFrame;draw_plan_reuse=$taskDrawPlan;partial_submit=($taskConfig -contains 'partial_submit=1');profiler=$false;view_confirmed_utc=$taskUtc;
    continuous_submit=($taskConfig -contains 'continuous_submit=1');
    coalesce_fence_flush=($taskConfig -contains 'coalesce_fence_flush=1');
    nonblocking_nv_events=($taskConfig -contains 'nonblocking_nv_events=1');
    consumer_release_fences=($taskConfig -contains 'consumer_release_fences=1');
    detached_syncpoint_actions=($taskConfig -contains 'detached_syncpoint_actions=1');
    minimum_complete_window_start_s=$taskMinimum;frames=$taskFrames;wall_ms=$taskMs;
    mean_frame_ms=$taskMs/$taskFrames;fps=$taskFrames*1000/$taskMs;
    windows=$taskWindows;percentile_scope='per 300-presented-interval window; not pooled';controls_sent=$false}
$taskResult | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskOutput ($Variant+'-fixed-view.json'))
Copy-Item -LiteralPath $taskLog -Destination (Join-Path $taskOutput ($Variant+'-fixed-view-log.txt'))
$taskResult | ConvertTo-Json -Depth 8
