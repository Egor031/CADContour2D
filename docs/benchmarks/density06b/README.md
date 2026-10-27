# 06.0B benchmark evidence

Исходные aggregated results каждого завершённого Release x64 запуска находятся
в `raw/<run>.json` и `raw/<run>.csv`. Это точные копии из новых каталогов
`build/density-06b-matrix/<run>/results.*`; .gitattributes сохраняет их bytes.
Большие count datasets и per-worker files в Git не включены.

- [Выполняемый набор и порядок матрицы](matrix.json),
  [первоначальный более широкий план](matrix-planned.json).
- [Малая серия: JSON](raw/small.json), [CSV](raw/small.csv).
- [3 млн: JSON](raw/3m-all-t256.json), [CSV](raw/3m-all-t256.csv).
- [12 млн entropy / 256: JSON](raw/12m-entropy-t256.json), [CSV](raw/12m-entropy-t256.csv).
- [48 млн sparse: JSON](raw/48m-sparse-t512.json), [CSV](raw/48m-sparse-t512.csv).
- [Аудит всех 12 серий и SHA-256 raw files](audit.json).
- [Финальный Debug 06.0A JUnit](tests/density-06b-06a-debug.xml).
- [Финальный Release 06.0A JUnit](tests/density-06b-06a-release.xml).
- [Финальный Debug D/E JUnit](tests/density-06b-debug-tests-final.xml).
- [Финальный Release D/E JUnit](tests/density-06b-release-tests-final.xml).

[Отчёт, сравнительные таблицы, рекомендация и ограничения](../../reports/task-06/density-formats.md).
[Реализация и проверенные команды](../../../tools/density_benchmark/formats/README.md).

В JSON `subsequentStatistics`: median=p50; p95 nearest-rank, first исключён.
Все raw measurements сохранены. Unsupported E/huge-sparse — явное ограничение,
а не успешное чтение. `first` не означает cold cache. Trials создают новые файлы;
repeat N не является открытием того же filename из repeat N-1.

FileIo покрывает unbuffered QFile обоих адаптеров, processIo — Windows logical
process I/O, physical disk I/O отсутствует. Peak RAM — process-lifetime working
set high-water mark, не отдельная phase peak. Counts/sizes — decimal strings,
times — числовые milliseconds. Точный allocation trace отсутствует.

Остальные 12m JSON/CSV: `12m-{entropy,sparse,uniform}-t{256,512,1024}` в raw/.
Первоначальный более широкий launcher остановлен после 12m серий по ресурсной
целесообразности; [запись](curtailment.json). Все выбранные 12 серий завершены.
300m uniform и 48m entropy не включены. Архив JUnit использует LF в Git;
исходные отчёты CTest в build/ сохранены без изменения.
