# ASC/XYZ acceptance measurements

Opt-in tool for task 04.4B, built only with CADCONTOUR2D_BUILD_TOOLS=ON.
It has no CTest registration. It uses production ProjectController,
ProjectViewModel, Main.qml and CacheReader; production behavior is unchanged.

Verified build in the initialized CMD environment:

    cmake --build --preset windows-release --target import_acceptance --parallel 4
    build\windows-release\import_acceptance.exe --help

The Debug target is also built by final checks. New report/sample paths are
required; existing files are not overwritten. The caller creates the report
parent and checks available storage capacity before running.

## Import and native presentation

Default storage: production QStandardPaths::AppLocalDataLocation/projects.
The --storage option selects an isolated test root, without adding a production
GUI setting. Accepted caches persist. Unaccepted cleanup remains production work.

The --gui option loads real Main.qml on the current native platform, suppresses
the OS FileDialog and delivers QUrl/confirmed replacement through the adapter.
It measures actual window/event-loop behavior, but does not replace manual
native FileDialog, replacement or Z-dialog acceptance tests.

Verified example (retained 04.4B data; use a new report path for a repeat):

    build\windows-release\import_acceptance.exe --source build\ascxyz-044b-20261018-f1f2b934\large.xyz --oracle build\ascxyz-044b-20261018-f1f2b934\large-oracle.json --gui --report build\ascxyz-044b-20261018-f1f2b934\large-gui.json

Scenarios: complete, cancel-parsing, cancel-cache, cancel-z, shutdown-parsing,
shutdown-cache. Cancellation is queued after an actual application phase,
not an arbitrary elapsed delay; the worker mailbox phase at request is recorded.
Z cancellation waits for the actual waitingForZ state and a current pending request,
then answers Cancel through the adapter; it does not estimate Z-wait by elapsed time.
Shutdown calls the production adapter. The --baseline option first imports a
small flat source and prepares both history branches. Cancellation must preserve
its complete metadata/history snapshot and leave no new files in storage.

The --retry-source option actually imports another flat source on the same
open headless controller after cancellation measurement stops. It requires a
non-shutdown scenario without --gui. Setup/retry caches also persist.
Setup/retry have a ten-minute harness deadline; this does not add a timeout to
application Z confirmation. Instrumentation callbacks use a scoped QObject context.
Complete checks Accept, cell=1 and empty history, then validates the cache unless
--no-validate was supplied for an auxiliary run.

## Measurement boundaries

- A background thread samples every nominal 100 ms **plus sampling work**.
  Actual timestamps are in the report's .samples.csv file.
- GetProcessMemoryInfo supplies OS lifetime PeakWorkingSetSize and
  PeakPagefileUsage, including a final reading after join. Current WorkingSetSize
  and PrivateUsage peaks are sampled. Setup, GUI/runtime and monitor are included;
  the internal 128 MiB budget is not a process memory limit.
- GetProcessIoCounters deltas are logical process I/O, including telemetry/log
  overhead, not physical HDD traffic.
- Shared native attribute handles/GetFileSizeEx supply EOF sizes, excluding
  pre-existing files. Sampled sums are lower bounds on logical file high-water,
  not allocated NTFS clusters.
- Report timeline starts before monitor/QML setup; importStartRequestedMs marks
  selected-file delivery. importToNativeCompletionMs ends at the controller
  notification after native completion/join. CSV has a nearby separate
  monitor-construction clock, not an exactly synchronized per-phase trace.
- Existing runner hooks only timestamp Ready publication, worker observation of
  publication decision and product cleanup. No barriers are introduced. Cleanup
  hook precedes final release of the worker callable's captures.
- Phase transitions are observed mailbox phases, including poll delay.
  They are not isolated sorting or OS-I/O timings.
- A 10 ms precise GUI-thread timer records tick gaps and bounded outlier details.
  Window descendants are sampled every 100 ms. Startup, system menus, resizing,
  scheduling and rendering can affect observations.
- Reader validation runs synchronously **after destruction of the GUI engine/window**,
  outside import timing/resources. Total limits: 128 MiB/8 GiB scratch/32 handles;
  Reader open/scans reserve 1 MiB/one handle. Unique validation scratch is beside
  the JSON report, even if the imported cache is on another volume.

## Reader and independent controls

The --cache option opens an accepted cache directly, without opening ASC/XYZ:

    build\windows-release\import_acceptance.exe --cache <accepted-cache-path> --oracle <oracle-path> --report <new-report-path>

It runs full production validation: all checksums, hierarchy, canonical order,
external exact ID coverage; then scanAll, two scanBox and one scanBoxes query.
Index results match independent scanAll membership using counts and ordered
SHA-256 over full RecordId/X/Y records, including overlapping/repeated boxes.
Binary64 controls compare bits by RecordId.

source_oracle.py streams known finite decimal test sources, counts physical lines
and measurements, hashes the entire file, and retains at most 74 selected records.
Exact decimal Fraction and CPython integer true division convert controls to
binary64 independently of MSVC from_chars. Signed zero is handled explicitly.
This is an oracle for known acceptance inputs, not another ASC/XYZ parser.
Its conversion agrees with all 37 existing integer-generated golden pairs.

Verified invocation:

    python tools/import_acceptance/source_oracle.py build/ascxyz-044b-20261018-f1f2b934/large.xyz --count 41984566 --output build/ascxyz-044b-20261018-f1f2b934/large-oracle.json

Six-decimal generator output does not cover arbitrary source precision.
Unchanged production numeric tests separately cover 6/9/12/15 places, finite
extrema, subnormals, signed zero and rounding boundaries.

See [04.4B results](../../docs/reports/task-04/asc-xyz-e2e-performance-check.md) for actual
measurements, commands, storage placement and retained artifacts.
