# Density Builder 06.3A — B1/B2 review-fix evidence

Baseline: `03fd6e2` (`implement headless density builder through C1`).
Новые результаты хранятся отдельно от исторического
[density063a](../density063a/README.md); старые raw data не изменяются.

## Regression results и история

На исходном production `03fd6e2` новые 44 cases дали **12 PASS / 32 FAIL**.
После исправлений targeted Debug — **44/44 PASS**; полный набор включает
builder **128**, DensityStorage **115**, RasterMapping **14**, Recovery **68**.
Case-sensitive inventory обеих конфигураций — **1214/1214 unique**, совпадающая
production headless выборка — **827/827 unique**.

Release и финальный повтор Debug: **826 PASS / 0 FAIL / 1 SKIP** из 827.
Первый широкий Debug: **825 PASS / 1 FAIL / 1 SKIP**. Сохранён Open lifecycle
FAIL `CancelInvalidateShutdownAndPreparationFailureNeverReplacePrevious/3`
(Closing вместо Ready); отдельные 10 повторов PASS не отменяют его.
Предполагаемая fixture race и проверенная граница зависимостей описаны в
`open-lifecycle-observation.json`; Open/application code не менялся.
Это не исключение для Recovery P3. SKIP — только symlink privilege 1314.

`verification.json` содержит все результаты, failure names, команды, source hashes
и ссылки на lossless compressed JUnit/logs в `tests/`. Первоначальная ошибка
компиляции test macro также сохранена. Подробный отчёт A–F —
[раздел H основного отчёта](../../reports/task-06/density-builder.md#h-исправления-блокеров-независимого-ревью).

## Область проверки

- B1: изменение корректного LE32 local cell index с сохранением длины и общего
  count не должно незаметно изменить spatial distribution. Нужна независимая
  сверка per-group CRC записанных и прочитанных event bytes до передачи tiles D.
- B2: source/output/scratch обязаны быть fully qualified Windows paths до
  callbacks и I/O; drive-relative и root-relative paths не разрешаются через CWD.

Fault regressions и тесты path validation выполняются отдельно. Обычный успешный
pipeline smoke сам по себе не доказывает обнаружение injected corruption или
отклонение неверного path. CRC не заявляется криптографической аутентификацией.

## Исторический audit

Повторён существующий `tools/density_builder_benchmark/audit.py` над историческим
`docs/benchmarks/density063a/release.json`: **PASS**.
Команда, UTC, HEAD, hashes и exit status — в
[historical-audit.json](historical-audit.json), stdout — в
[historical-audit.log](historical-audit.log).

Это повторная проверка сохранённых JSON counts/invariants, **не новые измерения**
и не доказательство исправления B1/B2. Поиск имён в корне `build/` и
`build/density063a-evidence/` не нашёл отдельных prior B1/B2 probes;
существующие artifact logs относятся к исходной реализации 06.3A.

## Новый full-pipeline smoke — PASS

Используется существующий production harness без изменения oracle или historical
benchmark: штатный Point Cache Writer → DensityBuilder → D Reader → exact spatial
keys/shapes/counts/implicit zeros. После production freeze выполнены девять
Release runs по 100 000 measurements; runtime и независимый audit — **PASS**.
Тесты/сборки параллельно с измеряемыми runs не выполнялись.

100k выбран вместо минимального 4096, чтобы основной event capacity 65536
потребовал несколько flushes, compact cell count превысил 65535, а source занимал
несколько point pages. Сравнение capacities 65536/1024 проверяет накопление CRC
между append chunks; unchanged occupied-group/total invariants проверяются audit.
Это адресный correctness/resource smoke, без новой performance-рекомендации.

Фактические команды после process-local MSVC/Qt setup из основного README:

```bat
cmake --build --preset windows-release --target density_builder_benchmark --parallel 4
build\windows-release\density_builder_benchmark.exe D:/ZherlitsynEE/CADContour2D/build D:/ZherlitsynEE/CADContour2D/docs/benchmarks/density063a-review-fixes/smoke-release-100k.json 100000
python tools/density_builder_benchmark/audit.py docs/benchmarks/density063a-review-fixes/smoke-release-100k.json
```

Build — PASS (`ninja: no work to do`, уже собранный frozen target). Повтор запуска
требует нового имени JSON: harness отклоняет существующий output без overwrite.
CTest discovery в этом workflow не запускался.

Артефакты:

- [smoke-release-100k.json](smoke-release-100k.json) — свежие raw rows, spatial
  verification, counters и runtime environment;
- [environment.json](environment.json) — source/object/library/executable hashes,
  фактическая среда, commands и сохранность старых artifacts;
- [summary.json](summary.json) — инварианты и сравнение metadata при равной occupancy;
- [smoke-audit.log](smoke-audit.log), [build-release-benchmark.log](build-release-benchmark.log).

MSVC 19.51.36260.0 (`_MSC_FULL_VER=195136260`), Qt 6.12.0, Windows 10.0.19045 x64,
i5-7500, RAM 17 135 476 736 bytes. D: NTFS, Hitachi HDS721050CLA362 SATA HDD.
Source/scratch/output paths fully qualified drive paths; реальные UNC I/O этим
smoke не проверялись. Executable SHA-256:
`82fcb3bacb4f66776f3285c3697c6f2e165a622d156e6e4bdf316081b86e9064`;
он совпал с hash, записанным harness при запуске. Builder source SHA-256:
`511146ef5670ea2580dae4d74d9dc1a0ca7698f5b5ad9771c666655e436500d3`.

## Наблюдаемые counters и CRC metadata

В каждом успешном run: source count = scanned = events written/read = accumulated
= tile totals = D total = 100 000; scanAll=1, spatial queries=0, visited=occupied.
Каждый source — 2 498 560 bytes, 37 point pages + одна index page + header/footer;
open+scan прочитали ровно 2 498 560 bytes за 40 read requests/seeks.
Event payload — ровно 400 000 bytes записано и 400 000 bytes прочитано, без
дополнительного payload pass для CRC. D opens=5 во всех runs.

| Profile / capacity | Occupied groups | Physical tiles | Metadata peak bytes | Final D bytes | Spool opens |
|---|---:|---:|---:|---:|---:|
| compact / 65536 | 1 | 2 | 128 | 1109 | 4 |
| irregular64 / 65536 (все три runs) | 64 | 256 | 4160 | 354326 | 193 |
| irregular64 / 1024 | 64 | 256 | 4160 | 354326 | 378 |
| irregular256 / 65536 | 256 | 1024 | 16448 | 657088 | 770 |
| sparse64 / 65536 | 64 | 256 | 4160 | 354326 | 193 |
| fixed-area16 / 65536 | 16 | 64 | 1088 | 228601 | 49 |
| fixed-area128 / 65536 | 128 | 512 | 8256 | 459030 | 385 |

Compact fixture содержит 3 nonzero cells, 2 tiles и одну group. В основную cell
попали 99 998 measurements; ещё два measurements — bound anchors в двух других cells.
Compact сделал 7
payload writes/7 reads; одна группа дополнялась между event chunks. При capacity
1024 тот же irregular64 source потребовал 250 payload writes вместо 65; reads=64.
Exact spatial counts и D bytes сохранились. Рост обеих пустых axes с 4096 до
524288 также сохранил 64 occupied/visited groups, 256 tiles и 354 326 D bytes.

Новый uint32 fingerprint хранится в occupied-group metadata; allocator проверяет
весь `sizeof(T)` node до allocation. Измеренный metadata peak при равном числе
groups остался таким же, как в историческом 1M run: для 1/16/64/128/256 groups
это 128/1088/4160/8256/16448 bytes, наблюдаемо по 64 bytes на node, включая head.
Таким образом, в этом MSVC layout CRC поместился в уже учитываемый footprint;
универсальный ABI/node size из этих значений не следует.

Old 1M и fresh 100k отличаются N: сравнивается только metadata при равной occupancy,
а не CPU, disk size или производительность до/после исправления. Отдельного
CRC-call counter и изолированного CRC timer нет. В production CRC обновляется в
тех же bounded payload write/read loops, а mismatch проверяется до tile submission.
Защиту от in-range bit flip/same-length corruption доказывают отдельные
`EventIntegrity.InRangeBitFlipCannotChangeSpatialCounts` и
`EventIntegrity.SameLengthValidEventsCannotChangeSpatialCounts` regressions;
их RED→GREEN и Windows path cases описаны в
[основном отчёте](../../reports/task-06/density-builder.md).

Maximum owned working reservation — 13 309 568 bytes; metadata — 16 448;
scratch/combined disk reservations — 1 138 624 / 1 795 712 bytes;
existing files — 261, handles — 5. Process-lifetime peak working set —
82 268 160 bytes, включая source preparation и sparse test oracle. Это разные
меры, не OS/process hard cap и не per-build/per-phase memory peak.

`git diff --exit-code HEAD -- docs/benchmarks/density063a tools/density_builder_benchmark`
вернул 0: прежние artifacts и harness не менялись. Исторический raw SHA-256
остался `2d793c03a71b9724afb32bfdb3775effaba1ef52e36ad5c464b093070f087563`.
Свежий raw SHA-256:
`f0c9230083c22440c1d38e90822ccc5eab4acee8ab70864fb33c9b3750228683`.

Математический oracle, подготовка source вне build time, nested phase timing,
logical I/O, accounting reservations и process-lifetime memory имеют тот же
[ограниченный охват](../../../tools/density_builder_benchmark/README.md).
Physical cold-HDD, UNC I/O, physical disk faults, representative real scans и
42M pipeline в этот smoke не входят.
