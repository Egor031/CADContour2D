# Headless Density Builder benchmark (06.3A)

Opt-in `density_builder_benchmark` measures the production synchronous
Point Cache → C1 → D pipeline. It is separate from the historical D/E and C1/C2
experiments and is not part of CTest. No additional libraries are required.

The executable takes an existing scratch parent, a **new** JSON output path,
and the number of measurements per profile (at least 4096). It creates a unique
temporary directory for each source, retains the source through all builds,
and discards each completed D dataset after verification. The JSON output is
never silently overwritten. Large point caches, D files and event spools are
temporary and are not evidence artifacts.

Profiles use tile side 256, construction group side 512 and nominal cell 1 mm.
Source coordinates are exact binary rationals: origin `.25`, prescribed interior
samples at integer + `.5`, and the exact original maximum at extent + `.25`.
Thus the independent expected cell indices are known before the production
builder runs. A sparse test-only list of expected counts is sorted and reduced;
it does not call production mapping, group addressing or accumulation. The
regular Point Cache Writer builds every source.

| Profile | Occupied groups | Logical width/height | Repetitions |
|---|---:|---:|---:|
| compact repeated cell + two bound anchors (3 nonzero cells, 2 tiles) | 1 | 512 | 1 sample |
| irregular64 | 64 | 4096 | 3 measured repeats |
| irregular64, event chunk 1024 | 64 | 4096 | 1 sample, same source |
| irregular256 | 256 | 16384 | 1 sample |
| sparse64 | 64 | 524288 | 1 sample |
| fixed-area-16 | 16 | 16384 | 1 sample |
| fixed-area-128 | 128 | 16384 | 1 sample |

The default measured event capacity is 65536. The same source is reused for all
three representative repeats and the small-chunk comparison. Each successful
build is checked through the production D Reader, including every physical tile
key, shape and count, zeros inside those tiles, provenance, total, and an omitted
tile when one is available. No output is accepted on a total/digest check alone.
The sparse-area comparison checks that spools, visited groups and output tiles
do not grow when only the empty area grows. UUIDs and file CRCs can differ.

Preparation and the independent oracle are outside measured build time.
Verification is also timed separately. JSON contains exact builder phase
counters, reservations, logical file lengths and source/event I/O counters.
Source counters combine metadata open and the single scan. Event transfer
operations count payload read/write calls and successful spool opens; EOF probes,
flush/size queries and removal calls are outside those payload counters.
Nested phase timers overlap: scan includes grouping, grouping includes sorting
and event writes, accumulation includes group zeroing/event reads/counting,
and D finish includes sort/copy/readback. Tile copy and D ingest are separate;
event I/O overlaps grouping/accumulation. They must not be summed. `addTile`
time is not Deflate-only.
D counters include exact logical read/write requests and successful opens: four
writer scratch/staged files plus the readback reader (five in each successful
build). Readback-open bytes are included. Windows process logical I/O deltas have
different OS-call granularity and are reported separately.

Windows `GetProcessTimes` measures build-only kernel + user CPU. Logical I/O
deltas come from `GetProcessIoCounters`, not a physical disk monitor. Memory is
sampled immediately before/after build with `GetProcessMemoryInfo`; Windows peak
working set is a process-lifetime high-water mark and includes source preparation
and the oracle. It cannot identify a per-build or per-phase memory peak. Builder
owned reservations are a separate measure, not an OS memory cap.

OS file cache is uncontrolled; sources are freshly prepared or reused. No
cold-cache, physical HDD throughput, UNC, physical disk-fault or representative
real-scan claim follows from these synthetic measurements. The harness does
not assess Rough detection quality.

Verified Release build and functional smoke on 2026-11-05, in the x64 MSVC/Qt
environment described by the repository README (`CADCONTOUR2D_BUILD_TOOLS=ON`):

```bat
cmake --build --preset windows-release --target density_builder_benchmark --parallel 4
build\windows-release\density_builder_benchmark.exe D:/ZherlitsynEE/CADContour2D/build D:/ZherlitsynEE/CADContour2D/build/density063a-smoke-final.json 4096
python tools/density_builder_benchmark/audit.py build/density063a-smoke-final.json
build\windows-release\density_builder_benchmark.exe D:/ZherlitsynEE/CADContour2D/build D:/ZherlitsynEE/CADContour2D/build/density063a-release-final.json 1000000
python tools/density_builder_benchmark/audit.py build/density063a-release-final.json
```

The final smoke and measured million-point matrix each completed nine builds and
passed the spatial/invariant/resource audit, including five D opens per build.
After the final selective source snapshot fix, the million-point matrix was
repeated against the final executable; the 4096-point smoke predates that fix.
The audited final result is copied to `docs/benchmarks/density063a/release.json`.
[Raw measurements and interpretation](../../docs/benchmarks/density063a/README.md).
A rerun needs a new output filename because the executable refuses an existing
output file; the commands above record the actual runs.
