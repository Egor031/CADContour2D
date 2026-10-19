# Save resource probe

Opt-in `save_acceptance` builds with `CADCONTOUR2D_BUILD_TOOLS=ON`. It creates a
bounded synthetic Point Cache, installs a test-only domain fixture and measures the
real application Save API through native completion, then metadata-only Save after
`cell = 0.5`. It does not implement Open or add a GUI command.

`--help` documents the arguments. Workload directory and JSON report must be new.
Reports include elapsed milliseconds, Windows process memory, logical read/write
bytes, sampled new-file EOF totals, final project EOF size and dataset copy count.
Fixture creation precedes the measurements; process lifetime memory peaks include
that creation. Sampling is every 100 ms and can miss short-lived files; EOF is not
allocated disk space, logical I/O is not physical traffic, OS caches are not flushed.
No benchmark is registered with CTest. Workload files remain in the explicit root.

Verified invocations from the repository root with the configured MSVC/Qt environment:

```bat
build\task02-env.cmd cmake --build --preset windows-release --target save_acceptance --parallel 4
build\task02-env.cmd build\windows-release\save_acceptance.exe --help
build\task02-env.cmd build\windows-release\save_acceptance.exe --points 1000000 --root D:/ZherlitsynEE/CADContour2D/build/task052-resource-1m --report D:/ZherlitsynEE/CADContour2D/build/task052-resource-1m.json
build\task02-env.cmd build\windows-release\save_acceptance.exe --points 42000000 --root D:/ZherlitsynEE/CADContour2D/build/task052-resource-42m --report D:/ZherlitsynEE/CADContour2D/build/task052-resource-42m.json
```

Choose fresh paths when repeating. Results and limits are recorded in
[the 05.2 report](../../docs/reports/task-05/save-implementation.md#g-resource-and-performance).
