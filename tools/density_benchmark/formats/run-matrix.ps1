param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$StorageLabel = "unspecified"
)
$ErrorActionPreference = 'Stop'
$experimentExe = (Resolve-Path -LiteralPath $Executable).Path
$experimentRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $experimentRoot) { throw 'OutputDirectory must not exist' }
if (!(Test-Path -LiteralPath (Split-Path -Parent $experimentRoot))) { throw 'Parent must exist' }
New-Item -ItemType Directory -Path $experimentRoot | Out-Null
$experiments = @(
    @{name='small'; scenario='all'; side=16; width=47; height=45; repeats=2},
    @{name='3m-all-t256'; scenario='all'; side=256; width=1733; height=1731; repeats=5}
)
foreach ($experimentSide in @(256,512,1024)) {
    foreach ($experimentScenario in @('entropy','sparse','uniform')) {
        $experiments += @{name="12m-$experimentScenario-t$experimentSide"; scenario=$experimentScenario;
            side=$experimentSide; width=3465; height=3463; repeats=5}
    }
}
$experiments += @{name='48m-sparse-t512'; scenario='sparse'; side=512; width=6929; height=6927; repeats=5}
$experiments | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $experimentRoot 'matrix.json') -Encoding utf8
foreach ($experiment in $experiments) {
    Write-Output ("{0:o} START {1}" -f [DateTimeOffset]::Now,$experiment.name)
    # --width/--height affect ordinary scenarios; huge-sparse keeps its 06.0A dimensions.
    $experimentArgs = @('--output-dir',(Join-Path $experimentRoot $experiment.name),
        '--scenario',$experiment.scenario,'--tile-size',"$($experiment.side)",
        '--width',"$($experiment.width)",'--height',"$($experiment.height)",
        '--repeats',"$($experiment.repeats)",'--ram-mib','512','--output-mib','8192',
        '--storage-label',$StorageLabel)
    & $experimentExe @experimentArgs
    if ($LASTEXITCODE -ne 0) { throw "Failed: $($experiment.name), exit $LASTEXITCODE" }
    Write-Output ("{0:o} DONE {1}" -f [DateTimeOffset]::Now,$experiment.name)
}
