# 06.3A — Headless Density Builder

**ACCEPTED WITH LIMITATIONS** на
`bc28195971ddbe43ef36a12f38b5d49453bd85d6`; B1/P1 и B2/P2 независимо закрыты.
[Решение пользователя от 2026-11-06 и основания](#i-независимое-принятие).
Разделы A–H сохраняют историю реализации и исправлений до принятия.

Последующие исправления блокеров независимого ревью B1/P1 и B2/P2 приведены
в [разделе H](#h-исправления-блокеров-независимого-ревью).
Разделы A–G сохраняют исходные результаты реализации на `03fd6e2`.

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

## H. Исправления блокеров независимого ревью

### A. HEAD, diff и status

Проверенный checkout `D:/ZherlitsynEE/CADContour2D`, ветка `main`, HEAD
`03fd6e2480710e6a959411d6623621710ec209f5`, исходный status чистый.
Baseline исходного этапа `20a74277f3b077a8e52348264d876d266908af04` не менялся.
Прочитаны действующий AGENTS, README и относящиеся contracts requirements §§7–8,
architecture §15 и algorithms §6; родительских AGENTS на D:/ и D:/ZherlitsynEE нет.
Локальных независимых report/probes 06.3A в task-06 и верхних build review directories
не найдено; сценарии пользователя воспроизводятся адресными tests.

### B. Причины и исправления B1/B2

B1: прежние length/count/index/EOF/total invariants не обнаруживали замену local
index другим допустимым значением. D readback проверял согласованность уже неверного
распределения и потому тоже проходил. Нужен независимый fingerprint точного event
stream между scan/grouping и accumulation, до передачи tiles обработанной группы.
CRC предназначен для обнаружения случайного повреждения, не аутентификации.
`Group::fingerprint` хранит CRC32 последовательности точных LE32 bytes. CRC очередного
bounded write chunk вычисляется по encoded buffer до transfer seam и фиксируется
только после полного успешного write. Accumulation независимо считает CRC реально
прочитанных chunks и сверяет его после length/count/index/EOF checks, до tile copy/ingest.
Несовпадение даёт `InconsistentEvents`, phase `Accumulation` и group key; completed
dataset/pending не возвращается. Short read/write остаются самостоятельными I/O errors.

B2: Qt `QFileInfo::isAbsolute` не доказывает независимость Windows-пути от CWD и
current drive. Кроме того, preflight progress/cancel вызывались раньше проверки
путей. Требуется единая синтаксическая проверка fully qualified source/output/scratch
до callbacks и filesystem queries, без преобразования относительного ввода через CWD.
Проверка выполняется по копии request до создания Engine. Поддержаны ASCII drive
letter + colon + root separator, UNC с непустыми server/share, а также canonical
backslash namespace aliases полностью квалифицированных drive/UNC paths.
Drive-relative, root-relative, QRC/resource, embedded NUL, malformed UNC и прочие
device namespaces не принимаются. Unicode body не перекодируется через local codepage:
source использует прежнее native-wide → QString преобразование; output/scratch
проверяются как borrowed UTF16 views. Пути не trim/clean/canonicalize и не разрешаются
через current directory/current drive.

### C. Воспроизведение до и после

Новые 44 regressions сначала собраны и запущены с неизменённым production `03fd6e2`:
**12 PASS / 32 FAIL / 0 SKIP**. B1 — оба expected failures: build завершался success
при same-length valid-index corruption. Первый byte flip менял counts `[1,2]` на
`[0,3]`; полная подмена valid stream — на `[2,1]`. Point Cache содержит три measurements
0/1/2, `cell=1`, original maximum относится к последней cell; eventCapacity=1 делает
исходные LE events `[0,1,1]` детерминированными через три append chunks.

B2 — 15 вариантов output/scratch/source × ordinary relative/drive-relative/drive-only/
slash-root-relative/backslash-root-relative плюс 15 malformed/resource/device/NUL
syntax cases не соблюдали требование отказа до callbacks. 12 positive cases проходили:
девять drive/UNC/extended syntax probes с cancellation до I/O и три Unicode CWD builds.
После исправления адресный Debug GREEN: **44 PASS / 0 FAIL / 0 SKIP**.

B1 дополнительно проверяет ноль D ingest callbacks для повреждённой группы, отсутствие
result/остатков scratch, неизменность source SHA-256 и ранее завершённого D dataset,
его независимые spatial counts. B2 fixtures/CWD находятся внутри собственных temporary
directories на D:, CWD восстанавливается RAII и при assertions. Valid absolute builds
с callback-сменой CWD сохраняют точные paths, output counts и source bytes. UNC probes
не обращаются к share: это validation/cancellation tests, не сетевой I/O.

### D. Команды и результаты Debug/Release

Debug/Release headless targets собраны тем же toolchain/локальным `task02-env.cmd`.
После финального production изменения перелинкованы builder/storage/mapping tests
и Release benchmark. Configure/dependency changes не понадобились. Discovery Debug,
затем Release выполнены последовательно: **1214/1214 case-sensitive уникальных имён**
в каждой конфигурации; совпадающая production headless выборка — **827/827**.
В ней builder **128**, DensityStorage **115**, RasterMapping **14**, Recovery **68**.

Фактические команды (CMD-среда `build\task02-env.cmd`):

```bat
cmake --build --preset windows-debug --target density_builder_tests density_storage_tests raster_mapping_tests project_tests cache_tests persistence_tests asc_xyz_tests operation_tests import_application_tests nonzero_z_confirmation_tests save_tests open_tests recovery_tests dependency_smoke --parallel 4
cmake --build --preset windows-release --target density_builder_tests density_storage_tests raster_mapping_tests project_tests cache_tests persistence_tests asc_xyz_tests operation_tests import_application_tests nonzero_z_confirmation_tests save_tests open_tests recovery_tests dependency_smoke density_builder_benchmark --parallel 4
build\windows-debug\density_builder_tests.exe --gtest_filter=EventIntegrity.*:WindowsForms/*:WindowsSyntax/*:DriveAbsolute/* --gtest_output=xml:build/density063a-review-fixes/red-debug.xml
ctest --preset windows-debug --show-only=json-v1
ctest --preset windows-release --show-only=json-v1
ctest --preset windows-debug -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/density063a-review-fixes/headless-debug.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063a-review-fixes/headless-debug.xml
ctest --preset windows-release -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/density063a-review-fixes/headless-release.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063a-review-fixes/headless-release.xml
build\windows-debug\open_tests.exe --gtest_filter=Atomicity/OpenLifecycle.CancelInvalidateShutdownAndPreparationFailureNeverReplacePrevious/3 --gtest_repeat=10 --gtest_output=xml:build/density063a-review-fixes/open-debug-repeat.xml
ctest --preset windows-debug -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/density063a-review-fixes/headless-debug.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063a-review-fixes/headless-debug-rerun.xml
```

RED-команда запущена до production исправлений; её повтор после исправлений записан
как `green-debug.xml` с тем же filter. Первый compile новых tests завершился ошибкой
GTest macro parsing inline lambda с comma-initializers; generator вынесен в функцию,
assertions не ослаблялись. Эта ошибка и **32 RED failures** сохранены в evidence.

| Проверка | Debug | Release |
|---|---|---|
| Final build headless targets | PASS | PASS |
| B1/B2 targeted tests | 44 PASS | Включены в полный PASS |
| Builder / Storage / Mapping в широком наборе | 128 / 115 / 14 PASS | 128 / 115 / 14 PASS |
| Первый широкий headless прогон | 825 PASS / 1 FAIL / 1 SKIP | 826 PASS / 0 FAIL / 1 SKIP |
| Повтор широкого Debug после расследования | 826 PASS / 0 FAIL / 1 SKIP | N/A |
| Fresh full-pipeline smoke 9×100k / audit | N/A | PASS |

Единственный SKIP — `Persistence.StoragePath.FileSymlinkEscape`, privilege 1314.
Recovery **68/68 PASS**; исключений для P3 не вводилось. Debug FAIL — отдельный
`OpenApplication.Atomicity/OpenLifecycle.CancelInvalidateShutdownAndPreparationFailureNeverReplacePrevious/3`:
строка 590 увидела Closing вместо Ready. Проверка только этого case с `--gtest_repeat=10`
дала **10 PASS / 0 FAIL**. Предыдущий FAIL этим не отменяется.

Причинная граница проверена: Open executable link rule не содержит cadcontour_density,
application/Open/helper source и CMake не изменялись. `finish()` ждёт только
`!operation.active`; controller может после этого держать retired recovery sessions
и запланированный `finishRecoverySessions()`. Это согласуется с наблюдением Closing;
точный порядок в неуспешном запуске отдельно не инструментировался. Это наблюдение
вне B1/B2, не исправление и не допустимый заранее Recovery P3 failure. Open assertions
и production lifecycle не изменены.

Старые проверки A–G не считаются подтверждением нового кода. История всех FAIL/SKIP,
case names, commands, source hashes и compressed XML/logs —
[новый evidence](../../benchmarks/density063a-review-fixes/).

### E. Ресурсы, lifecycle и ограничения

Сохраняются один scanAll, occupied-only grouping, immutable Mapping, checked counts,
bounded buffers, retained diagnostic context и PendingDataset cleanup/ownership.
Checksum входит в `Group`, а custom allocator до allocation проверяет фактический
`sizeof` tree node с новым полем. Отдельных arrays/containers/passes нет; ожидаемый CRC
накапливается в scan/grouping, фактический — в существующем чтении spools по ≤64 KiB.
CRC work входит в bounded I/O chunks и их существующие timers/checkpoints. Сохранены
проверки length, partial-group indices, count, EOF и всех totals. Fingerprint metadata
не сериализуется: event payload по-прежнему ровно `4*N`, D bytes/layout не меняются.
Fresh Release smoke — **9 builds по 100 000 points**, с независимой сверкой всех
tile keys/shapes/counts и implicit zeros. Каждый source — 2 498 560 bytes,
37 point pages, 40 read/seek operations; каждый build scanAll=1/spatial=0,
event write/read по 400 000 bytes и D opens=5. При одинаковой occupancy metadata
совпала с историческим allocator accounting: 128 / 1088 / 4160 / 8256 / 16448 bytes
для 1 / 16 / 64 / 128 / 256 групп. На MSVC node остался 64 bytes: CRC занял padding,
а не исключён из accounting. Максимумы smoke: working 13 309 568, metadata 16 448,
scratch/combined 1 138 624 / 1 795 712 bytes, files 261 / handles 5.

Historical audit старого million-point JSON повторён PASS отдельно; он не является
новым измерением. Исходные artifacts/harness не менялись. Размер 100k выбран для
нескольких event flushes и spool read chunks, count >65535 и source paging;
новой million-point performance серии не запускалось. Отдельный CRC timer/call
counter не вводился; CRC fault-proof дают dedicated regressions. Сравнение старого
1M и нового 100k используется только для metadata при равной occupancy, не CPU/I/O.
Wire formats Point Cache/D, FP semantics и application publication не меняются.
UNC syntax validation не означает проверку сетевого I/O; physical UNC/cold-HDD,
physical device faults и representative real scans требуют отдельного окружения.

### F. Готовность к повторному ревью

**B1/P1 — FIXED; B2/P2 — FIXED. READY FOR INDEPENDENT REVIEW.** Финальный код прошёл
128 builder / 115 storage / 14 mapping tests в обеих конфигурациях, новый full-pipeline
smoke и независимый audit. Фактический первый Open FAIL и его расследование сохранены
как отдельное наблюдение; повторный широкий Debug и Release прошли с единственным
symlink SKIP. Новых изменений Open lifecycle или ослабления assertions нет.

Код готов к повторному независимому ревью двух блокеров и к будущему adapter 06.3B;
сам adapter, версии/GUI/persistence не реализованы. Самостоятельная проверка не является
принятием 06.3A или всего этапа 06.3. Перед commit просмотрен diff и выполнен
`git diff --check`; push не выполнялся.

## I. Независимое принятие

2026-11-06 пользователь подтвердил **06.3A — ACCEPTED WITH LIMITATIONS** на
`bc28195971ddbe43ef36a12f38b5d49453bd85d6` по повторному независимому ревью
от 2026-11-05. **B1/P1 и B2/P2 — FIXED AND INDEPENDENTLY VERIFIED**;
новых подтверждённых замечаний P0–P3 к builder и блокеров принятия нет.
[Первый и повторный независимые отчёты сохранены в Git](review063a-independent/README.md).
Прежний **NEEDS FIXES** на `03fd6e2480710e6a959411d6623621710ec209f5`
остаётся исторически верным; раздел H и его RED/FAIL/SKIP не переписаны.

Повторное ревью независимо проверило in-range/same-length corruption event stream
до D ingest, short transfers и сохранение их первопричин, fully qualified Windows
paths до callbacks/I/O, immutable source, literal D counts, source/previous dataset,
один scanAll, ресурсы, cancel/cleanup и retained PendingDataset diagnostics.

| Независимая проверка на `bc28195` | Debug | Release |
|---|---|---|
| Штатная incremental build/dependency state и hashes | PASS, no work | PASS, no work |
| Headless regression, 827 cases | 826 PASS / 0 FAIL / 1 SKIP | 826 PASS / 0 FAIL / 1 SKIP |
| Builder / storage / Mapping внутри headless | 128 / 115 / 14 PASS | 128 / 115 / 14 PASS |
| Собственные integrity/resource probes | 134 PASS | 134 PASS |
| Собственные path/lifecycle probes | PASS | PASS |
| Literal full-pipeline fixture scenarios | 9 PASS | 9 PASS |

Поднаборы пересекаются и не суммируются. Собственные probes были заново собраны;
clean rebuild всех production targets в том ревью **не выполнялся**. Подробности
binary provenance, первых ошибок собственного harness и нового Release smoke
9×100k сохранены в независимом отчёте; synthetic результаты не считаются real scans.
SKIP — Windows file-symlink privilege 1314.

Первый Open `Closing` вместо `Ready` из раздела H не отменён последующими PASS.
Повторное ревью подтвердило неизменность Open executable/dependency graph и
отделило наблюдение от B1/B2; точный interleaving не доказан. Это **не исправление**
Open lifecycle и не повторное открытие/закрытие прежнего Recovery P3.

Representative real scans, cold-HDD/UNC и реальное network I/O, physical disk/
flush/power-loss faults и эксплуатационные maxima ресурсов остаются **NOT RUN**.
Reservations/logical bytes не являются process/OS cap или гарантией свободного места;
CRC не является аутентификацией. Качество Rough этими проверками не подтверждено.
Форматы Point Cache/D/manifest и численный метод не пересматривались.

При документальном закрытии текущий checkout был уже на
`42147bb3c10df8008e1f8485255d8370bd02a7bf`; он не откатывался к `bc28195`.
Сборки и тесты повторно не запускались. Вместе с
[принятым 06.3B](density-build-operation.md#h-независимое-ревью-и-принятие)
**согласованный объём 06.3 завершён — ACCEPTED WITH LIMITATIONS**.
Следующий шаг — проектирование 06.4. Каталог/VersionId/selection/Undo, persistence
карт и полный пользовательский workflow ещё не готовы; вся задача 06 — **IN PROGRESS**.
