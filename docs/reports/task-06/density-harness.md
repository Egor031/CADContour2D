# 06.0A — экспериментальный стенд Density Map

2026-10-27. Вердикт: **READY WITH LIMITATIONS**. Кандидаты 06.0B/C не реализованы
и не сравнивались; задача 06 целиком не выполнена и не принята.

## Baseline

- Branch `main`, HEAD `c6f7c2a771aff60fce5b883c449e6f579faa94cb`.
  `git status --porcelain=v1` перед изменениями пустой.
- Прочитаны AGENTS, README, относящиеся к density/coordinates/resources/tests
  разделы requirements/architecture/algorithms, пункт 06 implementation-plan,
  Point Cache v1 и reader contracts. Предыдущих исследований 06 в репозитории нет.
- Windows 10 22H2 x64, MSVC **19.51.36260.0**, toolset directory 14.51.36231,
  Windows SDK **10.0.26100.0**, Qt **6.12.0**, CMake **3.30.5**, Ninja **1.12.1**,
  C++20; отдельные Debug/Release Ninja directories.
- RAM 17 135 476 736 bytes, свободно при первом обследовании примерно 6.55 GiB.
  D: NTFS, Hitachi HDS721050CLA362, SATA HDD, диск 500 105 736 192 bytes,
  свободно сначала 303 099 461 632 bytes. Model/media проверены через
  Get-PhysicalDisk и Get-Partition/Get-Disk, явно переданы в `--storage-label`.

Использован существующий локальный `build/task02-env.cmd`: штатный vcvars64,
UTF-8 code page, process-local PATH/QT_ROOT/VCPKG_ROOT. Глобальное окружение и
dependencies не менялись. Configure показал прежние необязательные
Qt6TaskTree/Vulkan diagnostics и завершился успешно; новых compiler warnings нет.

## Реализация и подключение кандидатов

Инструмент: [tools/density_benchmark](../../../tools/density_benchmark/README.md).
Подробные контракты форматов, метрик и ресурсных ограничений находятся в его README.

| Файл | Ответственность |
|---|---|
| tools/density_benchmark/CMakeLists.txt | opt-in library/executable, без новых dependencies |
| dataset.h/.cpp | точные тайлы, Spec, общие ReadPlan, input interchange, fingerprints |
| oracle.h/.cpp | ручные record→cell labels и независимый малый интервальный oracle |
| measurement.h/.cpp | preflight, измерительные/проверочные callbacks, OS metrics, JSON/CSV/statistics |
| main.cpp | reuse generator/CacheWriter/CacheReader, общие входы, fresh workers |
| README.md инструмента | проверенные команды, методика, интерфейсы 06.0B/C |
| tests/density_benchmark_tests.cpp | 20 unit/integration cases, label density_experiment |

Корневой CMake подключает tools subdirectory; tests/CMake добавляет отдельный
target/label при CADCONTOUR2D_BUILD_TOOLS=ON. `src/`, QML, production formats,
CacheReader и существующие fixtures не изменены.

06.0B передаёт прямые calls своего writer/reader в `measure()`, используя
одинаковые Spec/Tile inputs и `readPlans()` для write/full/tiles/ROI. `verify`
выполняется после замера. Нет plugin registry, dummy candidate, compressed
container или TIFF adapter. `.counts` — только несжатый input interchange.
06.0C получает `inputs.json → rasters`: read-only cache path, grid, point count,
point digest и малые expectedCounts. Будущий kernel запускается через тот же
measure; scanAll/event-spool/grouped-spatial algorithms здесь не написаны.
Кандидат должен предоставить собственную оценку Plan для общего preflight.

## Данные и oracle

12 scenarios: empty/uniform/dense/sparse, horizontal/vertical, clustered, overlap,
u32/u64, entropy, huge-sparse. Default seed **6001**, tile **8×8**, обычный raster
**23×21**, 9 tiles; последний **7×5**. Huge-sparse имеет
**1 000 000 003 × 1 000 000 001** cells, четыре явных corner tiles, остальное
implicit zero; есть zero-tile и empty-ROI requests. Память и итерация не зависят
от пустой логической площади.

Проверены независимые формулы простых sums: uniform **483**, dense **1 978 368**.
u32 maximum **4 294 967 295**, u64 control **4 294 967 297**; codec проверен также
на UINT64_MAX и отказ при overflow суммы. Entropy first-row controls и сумма
**1 080 383 497 533** независимо рассчитаны uint64 modular arithmetic в JavaScript;
golden SHA-256 закреплён тестом. Hash дополняет проверки значений, размеров и sums.

Пять малых point cases имеют вручную заданную expected cell **для каждой записи**:
negative coordinates, ±0/nextafter, internal/max boundaries, duplicates,
degenerate point/line bounds, partial cell, empty cloud. Oracle сравнивает
координату с интервалами всех cells, без floor и addressing будущего kernel.
RecordId→cell повторно проверяется после production cache sorting.

Переиспользованы `rectangle.json` + `clean_horizontal.json`, generator **0.4.0**,
seed **1001**: **87 230** points. Генерация/запись/чтение потоковые, batch 1024,
полного массива облака нет. Fixture hashes и canonical point digests сохранены.
Малый oracle ограничен 4096 cells/100 000 points; для большого external cache
записывается oracleAvailable=false, count/digest не доказывают растеризацию.

Временная конвенция: min origin, Y вверх, размер max(1,ceil(span/cell)), внутренние
cells полуоткрыты, крайняя включает точный максимум; degenerate axis — одна cell.
Это **экспериментальное допущение**, Open decision algorithms §6 не закрыт.
Единицы — мм, исходные координаты binary64, универсального epsilon нет.

## Методика и ограничения

- elapsedMs — wall time операции; verificationMs — отдельная проверка;
  totalElapsedMs включает обе фазы и сбор метрик. Whole-operation time включает
  подготовку/dispatch/агрегацию до финального сохранения отчёта. Все времена в ms.
- PeakWorkingSetSize — resident maximum **за жизнь процесса**, не phase peak
  и не peak system commit. Workers новые; их предыдущие verification/фазы могут
  определять максимум. Working set/private bytes before/after — snapshots.
- FileIo — фактические bytes/attempted calls QIODevice fixture wrappers;
  GetProcessIoCounters — logical I/O всего процесса; CacheReader statistics
  для open и scan раздельны. CacheWriter complete logical I/O недоступен.
- Размеры файлов — логические bytes, не allocated clusters. Scratch после
  операции известен; scratch high-water mark, physical disk I/O недоступны (null).
- First и subsequent workers разделены. **Cold OS cache не заявляется**:
  подготовка и correctness reads могли прогреть файлы. Есть сырые повторы,
  n/min/median/max/mean/sample stddev. Для выводов о кандидатах нужны более
  длинные фазы, больше повторов, согласованный порядок и cache policy.

Preflight проверяет checked arithmetic, доступные RAM/disk и configurable
RAM/temp/output budgets. Checkpoints контролируют working set/cancellation,
writer получает production budgets. Это **cooperative limits**, не OS hard cap;
оценка подготовки не является оценкой будущего кандидата. Реальное заполнение
диска/исчерпание RAM и нажатие Ctrl+C не проверялись. Cancellation проверена
детерминированно, включая verification callback. Full CacheReader validate
не запускается: проверяются прочитанные pages, count/digest и малый oracle.

Все данные — в новом явно заданном каталоге. Стенд не делает recursive cleanup,
не перезаписывает существующие каталоги и не меняет source/project/Recovery.
CacheWriter чистит только свой scratch. Незавершённые собственные inputs остаются
с диагностикой, повторное использование каталога запрещено.

## Фактические проверки

Из корня репозитория, через локальный environment launcher:

```bat
build\task02-env.cmd cmake --preset windows-debug -DCADCONTOUR2D_BUILD_TOOLS=ON
build\task02-env.cmd cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=ON
build\task02-env.cmd cmake --build --preset windows-debug --target density_benchmark density_benchmark_tests --parallel 4
build\task02-env.cmd cmake --build --preset windows-release --target density_benchmark density_benchmark_tests --parallel 4
build\task02-env.cmd ctest --preset windows-debug -L density_experiment --output-on-failure --output-junit build/density-06a-debug-tests-final.xml
build\task02-env.cmd ctest --preset windows-release -L density_experiment --output-on-failure --output-junit build/density-06a-release-tests-final.xml
```

| Проверка | Результат |
|---|---|
| Debug / Release experimental build, /W4 | PASS / PASS |
| Финальный CTest density_experiment | **20/20 / 20/20 PASS**, 0 skipped |
| Generation, exact codec, sums, manual cells | PASS |
| uint64 JSON/CSV, escaping, statistics, units/I/O | PASS |
| Invalid parameters, overflow, corruption, cancellation | PASS |
| RAM/temp/output budgets и refusal до datasets | PASS |
| Existing directory/data overwrite refusal | PASS |
| Сквозные workers и read-only external cache | PASS в обеих конфигурациях |
| Release smoke, JSON и независимый PowerShell Import-Csv | **230/230 rows ok** |
| Input digests двух отдельных запусков | **12/12 count + 6/6 point совпали** |
| git diff --check и просмотр итогового diff | PASS |

JUnit реально записан относительно binary directory:
`build/windows-debug/build/density-06a-debug-tests-final.xml` и аналогичный Release.
Первоначальные compile errors нового кода и dangling QJsonValueRef в тестовом
helper устранены. Первый Debug CTest был 17 PASS / 1 FAIL; затем 20/20 в обеих
конфигурациях, финально повторено после изменений ROI/metadata. Production CTest,
GUI и qmllint не повторялись: production код не затронут. Исторические результаты
не считаются новым подтверждением.

## Smoke evidence

Выполненная команда Release x64 (Qt environment из Build README):

```bat
build\windows-release\density_benchmark.exe --output-dir build\density-06a-smoke --repeats 4 --storage-label "D: Hitachi HDS721050CLA362 SATA HDD"
```

Для повторения задайте **другой новый** output-dir. Настоящие результаты:

- `D:/ZherlitsynEE/CADContour2D/build/density-06a-smoke/results.json`;
- `D:/ZherlitsynEE/CADContour2D/build/density-06a-smoke/results.csv`;
- рядом inputs.json, 12 dataset directories, 6 caches, repeat-0..3.json.

Начало **2026-10-27 10:09:18.833 Europe/Moscow**. HEAD ещё baseline, изменения
незакоммичены, что записано в environment.gitStatus. SHA-256 exe:
`ab9efee028fc50d0938cb4e7c7ceca64c00d6b0cc46c21d35d42b3a616930c85`.

Preflight: free RAM **6 478 856 192**, disk **301 490 135 040**; plan RAM
**50 335 744**, scratch **16 777 216**, retained output **75 965 952** bytes;
все три budgets **134 217 728** bytes. Пройдено 30 фаз подготовки, 50 первого
worker, 150 трёх subsequent workers. Наибольший зафиксированный process peak
**17 977 344** bytes. Scratch после запуска **0**; свои файлы до final reports
**3 127 835** bytes. Whole-operation time **1960.3799 ms** — только инфраструктура.

Контроль единиц: каждый uniform full read — **4152 bytes / 9 calls**, 483 cells.
Три subsequent values 0.9421/0.8948/0.7341 ms; median 0.8948, sample stddev
0.1090304 ms. Это пример разброса, не сравнение кандидатов.

results.json: **409 279 bytes**, SHA-256
`03160b4fead7e071b264aebdf2ce888a6251c66fa42891c9bf0f7451b00231ba`.
results.csv: **124 950 bytes**, SHA-256
`78acbe49c5c25c5ba98317496b913e24d5ec80decb35b5546d2f6df2c19b400e`.
Raw runs/datasets сохраняются в отдельном ignored build directory; большие caches
и временные файлы в Git не добавляются.

## Готовность

**READY WITH LIMITATIONS**: воспроизводимые входы, независимые малые эталоны,
общие access plans, раздельные measurement/verification, fresh-process повторы,
метрики и resource refusals проверены. Подключение реального кандидата требует
прямых callbacks и собственного resource plan, без переделки измерительного слоя.
Не подтверждены large real scans, physical I/O/cold cache, scratch peak, OS hard
memory cap и full cache integrity. Большие grids имеют только count/digest reference.
Production coordinate/storage/PNG decisions остаются открытыми. Ограничения не
мешают началу 06.0B с описанной методикой. К 06.0B/C в этой работе не переходили.
