# 06.0D/P1 evidence

Новые изолированные Release x64 trials: исходный C1 из `af9f44c`, обновлённый
C1, C2. [Сценарии](matrix.json), [порядок и пути trials](trials.json),
[executable SHA-256](executables.json), [исходники baseline](baseline.json),
[source SHA до/после](source-hashes.json), [resource refusals](refusals.json).

В `raw/` — точные worker `results.json` / `results.csv`, две строки open/build
для каждого successful trial. `baseline-C1` в имени файла обозначает исходную
сборку; внутри worker candidate по прежней схеме называется `C1`. Сценарий
`sparse-16383-c2` имеет cell **2**, `sparse-16383-c1` — cell **1**; суффикс здесь
относится к cell, а candidate идёт после repetition.

`first` (r0) отделён от subsequent r1–r3; порядок ротируется. Physical HDD I/O
и cold-cache не заявляются. Canonical count streams, caches и jobs/oracles
находятся в ignored `build/density-p1-matrix`, а не в Git. Они воспроизводятся
через [run-p1.py](../../../tools/density_benchmark/construction/run-p1.py).

При аудите переиспользован decoder/digest/CSV слой 06.0C. Новый sparse oracle
проверяет каждую stored cell и полное множество nonzero cells, включая
implicit-zero complement без полного raster allocation. 42M reference заново
проверяет RecordId/coordinates и separable interval counts. [Audit](audit.json)
содержит first/subsequent timings, min/median/max, open+build, resources, counters,
fingerprints и SHA-256 manifest. `oracleAvailable=false` внутри sparse worker
относится к встроенному dense readback: независимый sparse oracle выполняется
Python audit после измерений.

[JUnit/build logs](tests): 36 tests 06.0A/C и 13 tests 06.0B в каждой конфигурации.
JUnit и служебные JSON нормализованы в LF до финального manifest; raw worker
JSON/CSV сохранены побайтно.
Reported allocation — сумма `FILE_STANDARD_INFO::AllocationSize` на flush
checkpoints; MFT/directory metadata и process-wide handle peak отсутствуют.

[Итог и ограничения](../../reports/task-06/density-construction-p1.md).
