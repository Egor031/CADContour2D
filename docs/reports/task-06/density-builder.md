# 06.3A — Headless Density Builder

## A. Baseline

Checkout `D:/ZherlitsynEE/CADContour2D`, branch `main`, исходный HEAD
`20a74277f3b077a8e52348264d876d266908af04`, исходный status чистый.
Родительские `D:/AGENTS.md` и `D:/ZherlitsynEE/AGENTS.md` отсутствуют;
прочитаны корневой AGENTS, README/docs index, plan 06, requirements §§7–8,
architecture §15/operations/errors/resources, algorithms §6, D format,
отчёты 06.1/06.2/P1, source retention, CacheReader/Writer, mapping/storage и тесты.

06.1 и 06.2 сохраняют **ACCEPTED WITH LIMITATIONS**. Recovery P3
`085a5f1d8c1abd0dd85368dd88e2803454c45c7b` сохраняет **FIXED AND VERIFIED**.
Их исторические проверки не являются проверками нового полного pipeline.
Experimental construction benchmark не изменён, его ceil/linear ordinals,
allowances и descriptor lists не перенесены.

Windows 10 Pro 10.0.19045 x64, i5-7500 (4 cores), 17 135 476 736 bytes RAM.
Использован существующий `build/task02-env.cmd`: vcvars64 и process-local PATH,
без установки зависимостей или изменения глобальной среды.
Фактические toolchain/media и результаты — в [evidence](../../benchmarks/density063a/README.md).

## B. Реализация

`cadcontour_density` предоставляет синхронный `DensityBuilder::build(BuildRequest)`.
Request содержит копию `ImportedDataset` с shared immutable `DatasetLocation`,
cell, tile/group/chunk profile, output/scratch paths, единую policy и callbacks.
Builder копирует request до callbacks. Physical source path, output и scratch
parent обязаны быть абсолютными; относительные пути отклоняются без разрешения
через изменяемый current working directory.
Внутренняя копия включает только используемые source metadata/binding: provenance
ASC/XYZ (`sourcePath` и другие ancillary поля) не создаёт лишних глубоких копий.
Собственный read-only reader сверяет CacheId, count и исходные binary64 bounds.
Пустой source отклоняется, вырожденные оси поддержаны. Mutable project/GUI state
и threading отсутствуют.

Mapping создаётся только factory 06.1 с явным budget/cancel; каждое измерение
проходит `locate`. Никаких floor/ceil/clamp/epsilon или coordinate rounding.
Один scanAll обрабатывает borrowed batches на месте. Ограниченный event chunk
сортируется по паре uint64 group coordinates; LE uint32 local events дописываются
только в occupied spools. Полного linear group ordinal или списка output tiles нет.
После scan reader освобождается; обходятся только зарегистрированные группы.
Повторно используются bounded counts/read/tile buffers; partial shapes и X-fast
порядок проверяются. Counts и totals checked uint64, storage width выбирает D Writer.

Успех содержит move-only `BuildOutput` с завершённым `PendingDataset` и scalar
statistics. D metadata: новый non-null DatasetId, фактический source CacheId/count,
`CompletePointCache`, неизменный mapping и tileSide 256/512. Finish использует
штатный sort/seal/readback D; дополнительного полного readback builder не делает.
На успехе source count = scanned = events written/read = accumulated = tile total
= D total. До возврата завершены cleanup и последний cancellation checkpoint.

## C. Correctness

`tests/density_builder_tests.cpp`: штатный Point Cache Writer → production builder
→ штатный D Reader. Oracle не вызывает production locate/addressing/accumulation:
малые fixtures перебирают интервалы с раздельными RN operations, numerical fixtures
используют существующие integer/rational golden boundary bits. Проверяются каждое
значение physical tile, точные keys/shapes и implicit zeros, а не только digest/total.

Матрица включает singleton/duplicates >65535, обе degenerate axes, partial tiles/groups,
tile sides 256/512, group neighbours и повторные chunks, negative/cross-zero/signed zero,
decimal/nextafter/six-decimal/subnormal/large-coordinate golden fixtures, sparse area
growth и pair keys за uint32. Boundary overflow проверяется адресно, без миллиардов
records. Missing/wrong/null binding, metadata mismatch и corrupt/truncated cache
не дают success. Operation-local fault seams имитируют отказы spool create/append/
reopen/read/remove, corrupt length/index/extra events и late cancellation.
Private transfer seam выполняет реальную запись/чтение неполного prefix и проверяет
ветки short transfer; это управляемая имитация, не физический отказ устройства.
Отмена проверена также внутри D ingest/sort/seal/readback, включая несколько
последовательных readback checkpoints, и после finish. Отдельно проверены точные
RAM/metadata/disk/files/handles/work ceilings и одна недостающая единица.

## D. Ресурсы и lifecycle

Policy ограничивает owned RAM reservations (metadata — отдельный sub-ceiling),
reader reserve, logical scratch/output/combined bytes, одновременно существующие
files и open handles, cumulative work и отдельную проверку mapping axes.
Ноль разрешает нулевой расход ресурса, unlimited нет. Ресурсы компонентов выделяются
из общего ceiling по фазам, а не получают каждый полный лимит. Metadata зависит
от фактической occupancy; пустая logical area не резервирует worst-case groups.
Это accounting заявленных allocations/file lengths, не process/OS hard cap,
physical allocation или гарантия свободного места.

Первоначальный профиль вызывающей стороны (не production maximum):

| Ресурс | Ceiling |
|---|---:|
| Working RAM / occupied metadata sub-ceiling | 32 / 8 MiB |
| CacheReader reservation | 1 MiB |
| D buffer / codec / sort reservations | 8 / 1 / 1 MiB |
| Scratch / output / combined logical disk | 4 / 4 / 8 GiB |
| Existing files (включая source) / handles | 16384 / 5 |
| Общие work checkpoints / mapping boundary checks на factory call | 100 млн / 10 млн |

Профиль выбирается явно через `initialBuildResourcePolicy`, request по умолчанию
содержит нулевые ceilings. На фактическом 16 GiB офисном host RAM-профиль оставляет
запас сверх group 2 MiB, tile 0.5 MiB и D reservations, не претендуя на общий process
limit. Disk ceilings — caller safety envelope при наличии примерно 244 GiB свободного
D:, не обещание свободного места. Files ceiling ограничивает цену one-file-per-group;
испытаны меньшие occupancy, поддержка 16384 occupied files не заявляется. Общая work
policy допускает адресные million-point профили; mapping 10 млн наследует измеренный
профиль 06.1. Все ceilings изменяемы caller, оптимальность значений не утверждается.

Фазы не получают общий лимит каждая: scan резервирует base/path bookkeeping,
reader, `eventCapacity*sizeof(Event)` и 64 KiB wire buffer; после освобождения reader
и events accumulation получает D reservations, `8*groupSide²`, `8*tileSide²` и 64 KiB
wire buffer; finish оставляет D reservations. Exact STL allocation requests metadata
заряжаются custom allocator до allocation, включая sentinel/node capacity.
Working peak — максимум консервативных phase reservations плюс metadata.
Файлы: retained source + actual occupied spools + четыре D files; readback добавляет
handle существующего candidate, а не ещё один file. До пяти handles одновременно.

Disk reservation полного обязательного event payload `4*N` консервативно закреплён
до D finish. D получает остаток scratch/combined ceilings; уже удалённые spools не
расширяют его policy. Итоговые disk peaks — reservation bounds с этой удержанной
суммой, не sampled maxima совпадающих во времени lengths. Это может безопасно
отказать раньше динамического переиспользования budget; occupancy/compression ratio
заранее не угадываются.

Cancel/progress callbacks noexcept, context заимствован до возврата build.
Diagnostic context передаётся shared ownership и удерживается самим PendingDataset,
даже после отдельного move из BuildOutput. Для этого D API получил optional retained
context без изменения wire format и прежних вызовов. Diagnostic bridge не хранит
ссылок на builder/request stack. Cleanup errors не стирают первопричину; cleanup
failure после вычисления результата исключает success. Собственный scope уникален,
cleanup не удаляет parent/source/чужие datasets. Existing target отклоняется.
`releaseToOwner` вызывается только явным владельцем после возврата результата.

## E. Проверки

Сборки выполнялись в штатной среде `cmd /d /c build\task02-env.cmd ...`.
В Debug/Release собраны `density_builder_tests density_storage_tests raster_mapping_tests`
и широкий headless набор: `project_tests cache_tests persistence_tests asc_xyz_tests
operation_tests import_application_tests nonzero_z_confirmation_tests save_tests
open_tests recovery_tests dependency_smoke`. Release benchmark собран отдельно.
После последнего production изменения builder/storage tests и benchmark перелинкованы.

Discovery `ctest --preset windows-debug --show-only=json-v1`, затем та же команда
для Release выполнены последовательно. В каждой конфигурации inventory содержит
1170 entries / 1170 уникальных имён. Предварительная PowerShell-группировка без
учёта регистра ошибочно сочла варианты `nan` / `NaN` одним GUI-именем.
Проверка case-sensitive имён независимым JSON parser сняла это наблюдение;
тесты и их имена не изменялись. Исходное наблюдение и исправление сохранены в evidence.
Выборка по executable basename перечисленных targets и label production содержит
783 уникальных имени; Debug/Release выборки совпадают. Большой `cache_integration`
с отдельным label не включён.

Фактические финальные команды после сборки:

```bat
ctest --preset windows-debug -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/density063a-evidence/headless-debug-complete-tests.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063a-evidence/headless-debug-verified.xml
ctest --preset windows-release -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/density063a-evidence/headless-release-complete-tests.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063a-evidence/headless-release-verified.xml
python tools/density_builder_benchmark/audit.py docs/benchmarks/density063a/release.json
```

| Проверка | Debug | Release |
|---|---|---|
| Перечисленные headless targets build | PASS | PASS |
| Builder (включён в широкий набор) | 84 PASS | 84 PASS |
| DensityStorage / RasterMapping (включены в широкий набор) | 115 / 14 PASS | 115 / 14 PASS |
| Широкая production headless regression | 782 PASS / 0 FAIL / 1 SKIP | 782 PASS / 0 FAIL / 1 SKIP |
| Измеренная матрица 9×1 млн / audit | N/A | PASS |

SKIP — только `Persistence.StoragePath.FileSymlinkEscape`, Windows privilege 1314.
Recovery P3 проходит, исключений для его failures не вводилось. Предварительные
Debug-прогоны: targeted 197 PASS; headless 766 PASS / 0 FAIL / 1 SKIP из 767;
после расширения 778 PASS / 0 FAIL / 1 SKIP из 779 и 782 PASS / 0 FAIL / 1 SKIP
из 783 до последней оптимизации snapshot. Последующий PASS не заменяет историю.
Первый Debug build завершился ошибками Qt macro `emit` и volatile test value;
они исправлены. Одна shell-команда с regex pipe завершилась до запуска CTest;
выборка через tests-from-file устранила quoting issue. Исходные FAIL records,
команды, hashes и финальные JUnit — [verification.json](../../benchmarks/density063a/verification.json).

`dumpbin /DEPENDENTS` Release benchmark: Qt6Core, zlib и Windows/CRT; GUI/QML/Quick,
OpenCV и TIFF DLL dependencies отсутствуют. `git diff --check` — PASS.
GUI/lint и отдельный большой cache integration — **NOT RUN**, эти подсистемы
не изменены. Исторические FAIL/SKIP принятых 06.1/06.2/P3 остаются в их отчётах.

## F. Измерения

Opt-in `density_builder_benchmark` измеряет полный production pipeline отдельно
от подготовки cache, с независимой spatial verification каждого успешного trial.
Representative irregular64 выполняется трижды; остальные профили — samples.
Nested phase timings не суммируются: scan включает grouping, grouping включает
sort/event writes, accumulation включает zeroing/event reads/counting; tile-copy
и D ingest измерены отдельно. Event I/O пересекается с grouping/accumulation.
D finish включает sort/copy/readback. addTile не называется
чистым временем Deflate. Win process CPU и logical I/O — build-only deltas;
working set — before/after snapshots, process-lifetime peak включает preparation/oracle.
Source I/O объединяет open и scan. Event payload calls не включают EOF probe/flush/
size/remove; D operations — успешные logical exact-transfer requests и opens, не
OS syscall counts. Для этого Statistics D дополнена scalar instrumentation;
byte accounting D readback уже включал metadata open и не учитывается повторно.

Финальная synthetic матрица: 9 builds по 1 млн measurements, каждый source
24 125 440 bytes, 367 point pages. Representative64 — три wall times
1803.4480 / 1704.6610 / 1614.9463 ms, медиана **1704.661 ms**, process CPU median
1656.25 ms. Source scanAll=1, spatial queries=0; каждый раз event payload ровно
4 000 000 bytes записан/прочитан, totals=1 000 000. 64 groups → 256 tiles,
D=1 625 129 bytes. При увеличении обеих logical axes с 4096 до 524288 остаются
те же occupied groups/tiles/D bytes; axis validation учитывается отдельно.
Capacity 1024 вместо 65536 даёт то же распределение, spool opens 1349 вместо 232,
sample wall 1951.1354 ms. Это sample comparison, не универсальное отношение скорости.

Irregular256 sample: 256 groups / 1024 tiles, 4608.027 ms и D=2 661 772 bytes.
Максимумы всей матрицы: owned working reservation 13 309 568 bytes, exact metadata
16 448 bytes, scratch/combined reservation 6 743 308 / 9 405 080 bytes, files 261,
handles 5. Process-lifetime working set до 110 891 008 bytes включает preparation
и oracle; это не peak builder. Compressible profile содержит основную повторную
ячейку и два anchor measurements: всего три nonzero cells / два tiles / одна group.

Функциональные smoke 9×4096 и первая million-point матрица прошли до последнего
изменения selective snapshot; после него выполнена заново вся matrix 9×1 млн.
Предыдущие PASS artifacts/hashes сохранены отдельно. Во время измерений сборки
и тесты не выполнялись. Полные phase CPU/time/I/O/resources, compression ratios,
точная среда, source/executable hashes и ограничения —
[benchmark evidence](../../benchmarks/density063a/README.md).

## G. Статус и границы

**READY FOR INDEPENDENT REVIEW.** Это самостоятельный builder 06.3A, не
приёмка 06.3 целиком. 06.3B (OperationRunner/application publication), 06.4
(каталог версий/выбор/Undo), Save/Open/Recovery карт, Canvas/LOD/ROI, Rough,
threshold/morphology и PNG не реализованы. Форматы Point Cache/D/manifest не менялись.

Representative real scans, cold-HDD/UNC, physical disk-full/flush faults,
power-loss/hostile filesystem mutation — **NOT RUN**. Synthetic tests не подтверждают
качество Rough или эксплуатационные пределы числа occupied files.
