param(
    [Parameter(Mandatory=$true)][string]$InputDirectory,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
$auditRoot = (Resolve-Path -LiteralPath $InputDirectory).Path
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$auditPath = Join-Path $evidenceRoot 'audit.json'
if (Test-Path -LiteralPath $auditPath) { throw 'Audit must be new' }
$matrix = Get-Content (Join-Path $evidenceRoot 'matrix.json') -Raw | ConvertFrom-Json
$manifest = @()
$successful = 0
$unsupported = 0
$peak = 0L
$allDigests = @()
foreach ($experiment in $matrix) {
    $runDirectory = Join-Path $auditRoot $experiment.name
    $run = Get-Content (Join-Path $runDirectory 'results.json') -Raw | ConvertFrom-Json
    if ($run.status -ne 'ok') { throw "Failed run: $($experiment.name)" }
    $csv = @(Import-Csv (Join-Path $runDirectory 'results.csv'))
    if ($csv.Count -ne $run.measurements.Count) { throw 'JSON/CSV row count mismatch' }
    for ($i=0; $i -lt $csv.Count; ++$i) {
        $r = $run.measurements[$i]
        $c = $csv[$i]
        foreach ($field in @('dataset','candidate','phase','status','repetition','runClass')) {
            if ([string]$r.$field -cne [string]$c.$field) { throw "CSV field mismatch: $field" }
        }
        if ($r.status -eq 'unsupported') {
            if ($r.candidate -ne 'E' -or $r.dataset -ne 'huge-sparse' -or $r.phase -ne 'capability') {
                throw 'Unexpected unsupported result'
            }
            ++$unsupported
            continue
        }
        if ($r.status -ne 'ok' -or $r.correctness -ne $true -or $c.correctness -ne 'true') { throw 'Incorrect result' }
        $csvTime = [double]::Parse($c.elapsedMs,[Globalization.CultureInfo]::InvariantCulture)
        if ($csvTime -ne $r.elapsedMs -or $csvTime -lt 0 -or [double]::IsNaN($csvTime) -or [double]::IsInfinity($csvTime)) {
            throw 'Invalid elapsedMs or CSV numeric mismatch'
        }
        $peak = [math]::Max($peak,[long]$r.processPeakWorkingSetBytes)
        ++$successful
    }
    foreach ($group in ($run.measurements | Where-Object phase -eq 'open_index' | Group-Object dataset)) {
        $digests = @($group.Group.details.fingerprint.sha256 | Select-Object -Unique)
        if ($digests.Count -ne 1) { throw 'Candidate/repetition fingerprints differ' }
        $allDigests += [pscustomobject]@{run=$experiment.name; dataset=$group.Name; sha256=$digests[0]}
    }
    foreach ($phase in @('read_full','read_tiles','read_roi','read_roi-large','read_random-128')) {
        $groups = $run.measurements | Where-Object { $_.phase -eq $phase -and $_.dataset -ne 'huge-sparse' } |
            Group-Object { $_.dataset + '/' + $_.repetition }
        foreach ($group in $groups) {
            if ($group.Count -ne 2) { throw 'Missing D/E paired read' }
            foreach ($field in @('returnedCells','returnedTiles','sum')) {
                if ([string]$group.Group[0].details.$field -cne [string]$group.Group[1].details.$field) {
                    throw "D/E read results differ: $phase/$field"
                }
            }
            $a = $group.Group[0].details.request | ConvertTo-Json -Depth 10 -Compress
            $b = $group.Group[1].details.request | ConvertTo-Json -Depth 10 -Compress
            if ($a -cne $b) { throw 'D/E ReadPlans differ' }
        }
    }
    foreach ($extension in @('json','csv')) {
        $source = Join-Path $runDirectory "results.$extension"
        $destination = Join-Path $evidenceRoot "raw/$($experiment.name).$extension"
        $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
        if (!(Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath $source -Destination $destination }
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) {
            throw 'Evidence differs from original; refusing overwrite'
        }
        $manifest += [pscustomobject]@{file="raw/$($experiment.name).$extension";
            bytes=[string](Get-Item -LiteralPath $source).Length; sha256=$hash}
    }
}
[pscustomobject]@{status='ok'; runs=$matrix.Count; successfulRows=$successful; unsupportedRows=$unsupported;
    maximumObservedProcessPeakBytes=[string]$peak; generatedUtc=[DateTimeOffset]::UtcNow.ToString('o');
    definitions='Independent PowerShell JSON/CSV parser; exact paired read coverage/sums/requests; fingerprints across candidates/repetitions; byte-identical raw evidence';
    digests=$allDigests; files=$manifest} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $auditPath -Encoding utf8
Write-Output "Audit PASS: $successful successful rows; $unsupported expected unsupported; $($matrix.Count) runs"
