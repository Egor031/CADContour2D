param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
function Save-Json([object]$Value, [string]$Path) {
    $jsonText = ($Value | ConvertTo-Json -Depth 10) -replace "`r`n", "`n"
    [IO.File]::WriteAllText($Path, $jsonText + "`n", [Text.UTF8Encoding]::new($false))
}
$root = (Get-Location).Path
$matrixRoot = [IO.Path]::GetFullPath($OutputDirectory)
$evidenceRoot = [IO.Path]::GetFullPath($EvidenceDirectory)
if ((Test-Path -LiteralPath $matrixRoot) -or (Test-Path -LiteralPath $evidenceRoot)) { throw 'Both directories must be new' }
New-Item -ItemType Directory -Path $matrixRoot,$evidenceRoot | Out-Null
New-Item -ItemType Directory -Path (Join-Path $evidenceRoot 'raw'),(Join-Path $evidenceRoot 'tests') | Out-Null
@('*.json -text','*.csv -text','*.xml -text') | Set-Content -LiteralPath (Join-Path $evidenceRoot '.gitattributes') -Encoding utf8NoBOM
$exe = Join-Path $root 'build/windows-release/density_construction_benchmark.exe'
$prepare = Join-Path $root 'build/windows-release/density_construction_prepare.exe'
$label = 'D: Hitachi HDS721050CLA362 SATA HDD NTFS; verified Get-Partition/Get-Disk/Get-PhysicalDisk'
$matrix = @(
    @{name='small'; inputs='build/density-06a-smoke/inputs.json'; tile=2; group=1},
    @{name='distributions-g1'; inputs='prepared/inputs.json'; tile=4; group=1},
    @{name='distributions-g4'; inputs='prepared/inputs.json'; tile=4; group=4},
    @{name='1m-g1'; cache='build/task052-resource-1m/source.pc2dcache'; tile=64; group=1; cell='0.1'},
    @{name='1m-g4'; cache='build/task052-resource-1m/source.pc2dcache'; tile=64; group=4; cell='0.1'},
    @{name='1m-g8'; cache='build/task052-resource-1m/source.pc2dcache'; tile=64; group=8; cell='0.1'},
    @{name='1m-shift'; cache='build/task052-resource-1m/source.pc2dcache'; tile=64; group=4; cell='0.1'; padX='0.375'; padY='0.625'},
    @{name='42m-g4'; cache='build/task052-resource-42m/source.pc2dcache'; tile=64; group=4; cell='0.1'},
    @{name='42m-g8'; cache='build/task052-resource-42m/source.pc2dcache'; tile=64; group=8; cell='0.1'}
)
Save-Json $matrix (Join-Path $evidenceRoot 'matrix-planned.json')
Save-Json @(Get-PhysicalDisk | Select-Object FriendlyName,MediaType,Size) (Join-Path $evidenceRoot 'disks.json')
Save-Json (Get-Partition -DriveLetter D | Get-Disk | Select-Object Number,FriendlyName,BusType) (Join-Path $evidenceRoot 'volume.json')
Save-Json (Get-CimInstance Win32_OperatingSystem | Select-Object FreePhysicalMemory,TotalVisibleMemorySize) (Join-Path $evidenceRoot 'resources-before.json')
$actual = @()
foreach ($item in $matrix) {
    if ($item.inputs -eq 'prepared/inputs.json') {
        $prepared = Join-Path $matrixRoot 'prepared'
        if (!(Test-Path -LiteralPath $prepared)) {
            & $prepare $prepared
            if ($LASTEXITCODE) { throw 'Fixture preparation failed' }
            Copy-Item -LiteralPath (Join-Path $prepared 'inputs.json') -Destination (Join-Path $evidenceRoot 'prepared-inputs.json')
        }
        $inputPath = Join-Path $matrixRoot $item.inputs
    } elseif ($item.inputs) { $inputPath = Join-Path $root $item.inputs }
    $argsList = @('--output-dir',(Join-Path $matrixRoot $item.name),'--tile-size',[string]$item.tile,'--group-tiles',[string]$item.group,
        '--repeats','4','--storage-label',$label,'--ram-mib','512','--temp-mib','1024','--output-mib','1024')
    if ($item.cache) {
        $argsList += @('--cache',(Join-Path $root $item.cache),'--cell',$item.cell,'--regular-fixture')
        if ($item.padX) { $argsList += @('--pad-x',$item.padX,'--pad-y',$item.padY) }
    } else { $argsList += @('--inputs',$inputPath) }
    Write-Output ('Running '+$item.name)
    & $exe @argsList
    if ($LASTEXITCODE) { throw ('Matrix run failed: '+$item.name) }
    foreach ($extension in @('json','csv')) {
        Copy-Item -LiteralPath (Join-Path $matrixRoot ($item.name+'/results.'+$extension)) -Destination (Join-Path $evidenceRoot ('raw/'+$item.name+'.'+$extension))
    }
    $actual += $item
    Save-Json $actual (Join-Path $evidenceRoot 'matrix.json')
}
