# 06.0C benchmark evidence

Девять Release x64 серий на D: Hitachi SATA HDD. В `raw/` — точные копии
aggregated JSON/CSV из `build/density-06c-matrix/<name>/results.*`.
Source caches, generated datasets, count streams и scratch в Git не включены.

- [Исходный план](matrix-planned.json) и [выполненная матрица](matrix.json).
  План выполнен полностью, сокращений нет.
- [Малые случаи](raw/small.json), [CSV](raw/small.csv).
- [Spatial distributions, группы 4×4](raw/distributions-g1.json),
  [группы 16×16](raw/distributions-g4.json).
- 1 млн points: [64×64](raw/1m-g1.json), [256×256](raw/1m-g4.json),
  [512×512](raw/1m-g8.json), [shifted grid](raw/1m-shift.json).
- 42 млн points: [256×256](raw/42m-g4.json), [CSV](raw/42m-g4.csv),
  [512×512](raw/42m-g8.json), [CSV](raw/42m-g8.csv).
- [Prepared input metadata / oracle / effective generator scenarios](prepared-inputs.json).
- [Disk models/media](disks.json), [D: mapping](volume.json), [initial resources](resources-before.json).
- [Core Debug](tests/core-debug.xml), [Core Release](tests/core-release.xml): 12 C1/C2 + 22 06.0A cases.
- [Formats Debug](tests/formats-debug.xml), [Formats Release](tests/formats-release.xml): 13 existing 06.0B cases.
- [Независимый audit, exact stream comparisons, controls, sources и evidence SHA-256](audit.json).

Четыре fresh-process пары, чередование C1/C2 и C2/C1. `first` исключён из
subsequent statistics (n=3). Median/min/max/p95/raw сохранены; p95 при n=3 —
максимум маленькой выборки. `open` отдельно от `build_counts`.
Verification не входит в elapsedMs. Общий output несжатый; формат D не измерялся.

`processPeakWorkingSetBytes` — lifetime working set worker до verification.
`scratchLogicalPeakBytes` — точный peak суммы EOF event files; allocation clusters
и process-wide handle peak не измерены. `fileIo` покрывает wrapped spool/output,
CacheReader statistics отдельно: open и сумма всех queries, repeated pages учтены.
Process I/O и seeks логические, physical disk I/O отсутствует. Подготовка,
reference и source SHA reads прогревают OS cache: `first` не cold-HDD run.

Все performance datasets synthetic; representative real scans недоступны.
Measured executable hash и baseline/runtime Git сохранены в каждом raw JSON.
`.gitattributes` отключает text conversion evidence, чтобы сохранить hashes.
Manifest audit охватывает JSON/CSV/XML; line endings новых JUnit нормализованы
в LF до расчёта hashes, содержимое test results сохранено.

[Отчёт и рекомендация](../../reports/task-06/density-construction.md).
[Реализация, определения и воспроизведение](../../../tools/density_benchmark/construction/README.md).
