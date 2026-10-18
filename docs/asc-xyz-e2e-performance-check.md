# 04.4B — сквозная и нагрузочная проверка ASC/XYZ

## A. Baseline

18 октября 2026, ветка `main`, исходный HEAD
`0453d2aed0b74c10f9fe99b7680fd3c76e03496a`. Working tree и index были чистыми.
Baseline совпал с заданием; повторное независимое ревью R1–R3 принято как указанный
пользователем предшествующий результат, без повторного полного аудита 04.1–04.4A.

Изучены AGENTS, README, задача 04 implementation-plan, requirements §§4–7/35,
architecture import/cache/resources/worker/publication/testability, algorithms §5,
Point Cache v1, отчёты 04.1–04.4A, GUI review fixes, cache-performance-report,
спецификация/README генератора и соответствующие APIs, fault seams и tests.

Windows 11 Pro 10.0.26200 x64; Xeon E5-2690 v4, 14 cores/28 threads;
RAM 33 455 132 KiB (31.91 GiB), свободно при preflight 20 553 024 KiB.
MSVC 19.51.36260.0 (toolset 14.51.36231), Qt 6.12.0 msvc2022_64,
CMake 3.30.5, Ninja 1.12.1, C++20. Окружение — предоставленная CMD-инициализация
через сохранённый `%TEMP%\cadcontour-044a.cmd`; глобальные настройки не менялись.

Repository/executables: `C:\dev\CADContour2D` и
`build\windows-debug|windows-release`. C: — NVMe SSD KINGSTON SNV3S1000G;
F: — SATA HDD TOSHIBA HDWD110, NTFS. Сопоставление drive→disk и MediaType сохранено
в [drive-map.json](benchmarks/ascxyz044b/drive-map.json) и [disks.json](benchmarks/ascxyz044b/disks.json).

Qt сообщает штатный storage `C:\Users\Egor31\AppData\Local\CADContour2D\projects`.
В данной среде его resolved path:
`C:\Users\Egor31\AppData\Local\Packages\OpenAI.Codex_2p2nqsd0c76g0\LocalCache\Local\CADContour2D\projects`.
Logical/resolved paths указывают на тот же file identity; это подтверждено
`Path.resolve()`/`os.path.samefile()`, а не предположением о размещении.
Оба на C:. [Storage resolution](benchmarks/ascxyz044b/storage-resolution.json)
содержит конкретный большой cache. Explicit storage в build/F: не перенаправлялся.

## B. Test Plan and Preflight

Порядок: preflight → native small E2E/Reader → medium/Reader/cancel/shutdown →
один large Release/Reader → целевой HDD → controlled failures и полная регрессия.

Уникальные roots:

- SSD: `C:\dev\CADContour2D\build\ascxyz-044b-20261018-f1f2b934`.
- HDD: `F:\CADContour2D-benchmark\ascxyz-044b-20261018-f1f2b934`.

F: проверен на запись собственным маленьким write-probe. Старые datasets/cache
не удалялись и не перезаписывались. В найденных C:\dev, F:\CADContour2D-benchmark,
Temp, Documents/Downloads подходящего большого ASC/XYZ не было; исторические
build/clean-performance и final-generator-audit отсутствовали. Старые binary
cache benchmarks не являются текстовым source для задачи 04.

| Input | Точки | Bytes | Подготовка и проверка |
|---|---:|---:|---|
| Small rectangle ASC | 87 230 | 1 896 098 | Existing build/audit/rectangle-release.asc; manifest/hash совпали. Для GUI создана собственная копия «малое облако.asc» |
| Z XYZ | 3 | 24 | Собственный малый source: первая строка пустая, первый nonzero Z=1e-100 в физической строке 3, затем Z=2 |
| Medium XYZ | 4 202 892 | 103 065 900 | Generator 0.4.0, large_plate geometry, pointStep=0.7, lineStep=1, maxLineLength=200 |
| Large XYZ | 41 984 566 | 1 029 576 096 | Канонические performance geometry/clean_horizontal inputs, pointStep=0.07; один новый output |

SHA-256:

- Small: `538684d0cb1130be7f9441db3fcf0a0e1289fd4704633abed5a897ef188e41f8`.
- Medium: `2826e64b54211ab051e8c62ca37415d70d6e631d2bd5115429285d8d98274b60`.
- Large: `75810e6e316d4ef4a0dafc4c8220b3749abe3d97f34d923a029c82ac3e4fdd8c`.

Large size/count/hash совпали с историческим case и новым manifest. Фактически
проверены generator version 0.4.0, hashes обоих входов, physical line count и
72 детерминированных control records. Source hashes повторно проверены после
испытаний — неизменны. Копии [manifest](benchmarks/ascxyz044b/large.xyz.manifest.json),
[oracle](benchmarks/ascxyz044b/large-oracle.json) и [post hashes](benchmarks/ascxyz044b/source-hashes-after.json)
сохраняют исходный path context. Для measured medium.scan.json локальный attribute
отключает EOL normalization: его байты и hash совпадают с manifest также в Git blob.
Generation больших данных не входит в import time:
32.743 s large и 7.295 s medium включают CMD initialization/process launch.

### Resource preflight

GUI profile: 128 MiB controlled working memory, 32 owned file handles; worker
вычисляет disk budget по доступному месту с резервом 64 MiB и 5% остатка.
Точное значение snapshot T каждого запуска не экспортируется. Parser arrays —
229 376 bytes; reserve 294 912 + 16×source-path-length. Остаток передаётся Writer.
Writer reserve и sorter capacities определяются действующими формулами
[Point Cache](point-cache-format.md#совместный-ресурсный-бюджет), без массива N точек.

Для N=41 984 566:

- point pages = ceil(N/2726) = 15 402; index pages = 16+1 = 17;
- final cache = 8192 + (15402+17)×65536 = **1 010 507 776 bytes**;
- descriptors bound = (15402+17)×64 = 986 816 bytes;
- conservative two-generation scratch = 2×(40×N + 512×32) = 3 358 798 048 bytes;
- combined owned upper bound = **4 370 292 640 bytes**, отдельно от source.

Header allowance допускает до 512 runs; по текущему 128 MiB/path reserve плану
ожидается 13 initial runs и fan-in до 64. Это расчёт, не измеренный run counter.
Консервативная оценка включает два поколения, хотя intermediate merge не ожидается.
Разделение disk budget пополам требует, чтобы каждая часть вмещала свой bound;
доступный GUI budget намного больше. До large generation свободно
115 691 659 264 bytes на C: и 305 439 133 696 на F:.
Source, scratch, cache и validation scratch совместно безопасно помещались.
Оценка owned handles ≤5 консервативна относительно source + Writer/Sorter, при
лимите 32; это не измеренный общий HandleCount Qt-процесса.

Validation отдельно имеет total limits 128 MiB/8 GiB/32, Reader open/scans —
1 MiB/one handle. Её scratch расположен рядом с report на C:, даже при cache на F:.
План/начальное место сохранены в [resource plan](benchmarks/ascxyz044b/large-resource-plan.json)
и [free-before-large](benchmarks/ascxyz044b/free-before-large.json).

## C. End-to-End Verification

**Настоящий Windows GUI, Release CADContour2D.exe, через computer-use:**

1. Startup FileDialog → Cancel: NoProject, повторный выбор через «Новый проект».
2. Native selection собственного Unicode/space ASC → реальный import 87 230 точек;
   card 0…100 × 0…60 мм, cell=1, Point Cache готов, Density Map отсутствует.
3. Apply cell=2, затем 3, Undo → cell=2, обе ветви history доступны.
4. Отказ от replacement сохранил identity=1/revision=3, cell и обе history branches.
5. Confirm malformed replacement: русская ошибка, ParseError, line=2/column=3,
   coordinate Y/token bad. Прежние dataset/cell/history остались.
6. Z Cancel кнопкой и отдельно закрытие Z-dialog через Escape завершили операцию
   без замены. Показаны физическая строка 3 и 1e-100.
7. Continue: все 3 точки, без повторного предупреждения на следующем nonzero Z=2.
   Новая identity=2, cell=1, Undo/Redo пусты, hasNonZeroZ=true, bounds [1,2]…[5,6].
8. Нормальное закрытие production button, exit 0. Первый cache остался; его SHA-256
   до/после failed/cancelled/successful replacements совпал.

Оба native-accepted caches затем проверены отдельным production Reader: strict
validation, scanAll, spatial queries и 72/3 bit controls — PASS. Для первого source
собственная копия была временно переименована, Reader прошёл при её отсутствии,
имя восстановлено в finally. Оригинальный build/audit input не менялся.

**Native Main.qml в измерительном tool:** medium, large и HDD complete, плюс
shutdown на BuildingCache среднего импорта с прежним проектом. OS FileDialog
здесь подавлен; QUrl/confirmation переданы штатному adapter. Это отдельно от
ручного native workflow и offscreen CTest. Для large вручную проверены maximize,
restore и system-menu Move/Right во время Parsing/BuildingCache. Фаза/индикатор
обновлялись, Cancel был доступен, второй импорт запрещён. До Accept показывался
NoProject. Количество window descendant QObjects: medium 277→304, large 277→305,
HDD 277→304; sampled counts, без объектов на каждую точку.

Malformed/coordinate-count/overflow/nonfinite/empty/missing/unreadable source,
stale callbacks, source growth/truncation, Ready gate и dialog shutdown дополнительно
подтверждены текущими automated parser/application/QML tests. Все QQmlEngine warnings
остаются failures. GUI читает только accepted ProjectState; WorkerOutcome не стал
presentation state. Это также проверяют существующие deterministic Ready/Discard tests.

## D. Large Import Correctness

[Large raw result](benchmarks/ascxyz044b/large-gui.json):

| Поле | Source / accepted state / Reader |
|---|---|
| Count / sourceLines | 41 984 566 / 41 984 566 |
| sourceBytes | 1 029 576 096 |
| Bounds, мм | X 0…3000; Y 0…1000 |
| Z flag | false |
| CacheId | 80a256fb-c135-4ffc-86b0-f7045da52ba5, state и Reader совпали |
| Format / encoding | 1 / 1 |
| Cache size | 1 010 507 776 bytes, совпадает с layout formula |
| Index | height=2; 17 index pages; 15 402 point pages |
| Publication | Succeeded / Accept; cell=1, history empty |

Full strict validation — **53.722 s**: envelope/checksums, layout, все pages,
hierarchy/counts/bounds, canonical Morton/RecordId order и exact external ID sort.
Таким образом подтверждены достижимость всех pages и полный диапазон IDs 0…N−1
без missing/duplicate; слабые XOR/sum не использовались как доказательство.

Отдельный scanAll — 41 984 566 records, 15 402 pages, **13.183 s**.
scanBox [10,15]² вернул 11, вертикальная полоса [99,101]×[-1,1001] — 28 981,
scanBoxes с overlapping/repeated rectangles — 22 857. Count и ordered SHA-256
полных ID/X/Y совпали с независимым membership из scanAll. Всего Reader проверка,
control comparison и queries — **67.017 s**, отдельно от import.

72 X/Y controls по RecordId побитово совпали с exact decimal Fraction → CPython
binary64, независимо от MSVC from_chars. Oracle conversion сверена со всеми 37
существующими integer-generated goldens. Unchanged production tests отдельно
проверили 6/9/12/15 decimal places, signs/scales, ±0, subnormal, finite extrema,
ties и round-trip ASC/XYZ. Шестизначный generator не объявляется покрытием всей
десятичной grammar. Полное побитовое сравнение всех 42 млн координат не выполнялось;
его не требовал подтверждённый риск.

Reader после GUI shutdown получает только cache path и bounded oracle, source
не открывает; source QFile уже закрыт worker. После завершения процесса cache
остался и потоково хэширован: SHA-256
`f6ff1a3d23ca9815df2dcbfdc186e587caa25d2130e80918bd494a2d7b43ed58`.
Физический путь и hashes остальных принятых файлов — в [inventory](benchmarks/ascxyz044b/summary.json).
Save/Open .pc2dproj для этой проверки не требовался.

## E. Performance Measurements

Все полные прогоны — Release. A: SSD source → штатный cache на C:.
C: SSD source → isolated cache/runs на HDD F:. ASC/XYZ не копировался на HDD.
Каждый measured import — новый процесс; setup baseline, если указан, предшествует
measurement. Начало duration — adapter selected-file delivery, конец —
controller observation подтверждённых native completion/join. Generation, hash/oracle,
Reader validation и retry в эти elapsed/I/O samples не входят.

| Run / layout | Input points / bytes | Import, s | OS lifetime Peak WS, MiB | Sampled Private, MiB |
|---|---|---:|---:|---:|
| medium-gui / A | 4 202 892 / 103 065 900 | 8.0916 | 220.008 | 222.699 |
| large-gui / A | 41 984 566 / 1 029 576 096 | 69.1781 | 222.871 | 233.379 |
| hdd-gui / C | 4 202 892 / 103 065 900 | 8.0857 | 216.473 | 223.059 |

| Run | New files sampled EOF peak, bytes | Logical read / write bytes | Final cache bytes | Strict Reader, s |
|---|---:|---|---:|---:|
| medium-gui | 267 420 864 | 272 240 667 / 269 720 271 | 101 261 312 | 6.5078 |
| large-gui | 2 685 656 976 | 2 717 104 812 / 2 692 990 524 | 1 010 507 776 | 53.7220 |
| hdd-gui | 265 058 208 | 272 230 760 / 269 720 080 | 101 261 312 | 5.4783 |

GetProcessMemoryInfo final reading captures lifetime PeakWorkingSetSize;
PrivateUsage maxima are sampled. PeakPagefileUsage отдельно сохранён в JSON,
не выдан за RAM/Working Set. Runtime, GUI/rendering и monitor включены.
OS file cache не считается памятью хранящего cloud application.
Lifetime counters cancelled runs также включают более ранний baseline setup peak;
они не являются peak только отменяемого target. Короткие Z allocations могли
не попасть в sampled PrivateUsage.
Номинальный sample sleep=100 ms + работа: observed median/max intervals
119.811/127.284 ms medium, 120.175/427.681 ms large, 102.217/673.579 ms HDD.
Короткий private/disk peak мог быть пропущен. Три полных CSV сохранены рядом с
[summary](benchmarks/ascxyz044b/summary.json).

EOF high-water — sampled lower bound логических длин новых owned files, не NTFS
allocated clusters. Обход файлов неатомарен и добавляет погрешность на переходах;
цифры не используются как точный моментальный максимум. GetProcessIoCounters — logical transfers/calls всего процесса,
включая небольшие telemetry/log writes; **не physical HDD traffic**.
Reader validation resources не входят в import samples: для неё записаны
elapsed/read statistics и действующие бюджеты, disk high-water отдельно не измерялся.

Рост N почти в 10 раз увеличил GUI Peak WS только с 220.0 до 222.9 MiB;
working arrays ограничены profile, их размер не N. Сравнение не изолирует все
GUI allocations: large window также разворачивалось на 1920×1032. Вместе с кодовыми
контрактами и постоянным QObject count результаты подтверждают streaming/external
storage, а не хранение исходного cloud целиком. 128 MiB не объявляется process RSS cap.

Наблюдаемые large application phase intervals: Preparing ≈0.214 s,
Parsing ≈42.668 s, BuildingCache ≈26.184 s. Parsing включает append/spill,
initial run sort/write; BuildingCache — merge/pages/index/sealing/readback.
Poll lag входит в границы; «чистое время сортировки» не вычислялось.

Large milestones относительно selected-file start:
Ready published hook 69.0607 s; application Accept 69.0737 s;
worker observed decision/product cleanup 69.0739 s;
native completion/join observed 69.1781 s. Последний интервал включает observation
delay, не только время native worker. Точного kernel exit timestamp нет.

### GUI responsiveness

10 ms precise timer: medium max gap 350.654 ms (место в первой версии trace
не записано); large 357.022 ms при startup/Preparing и 71.010 ms при Finalizing;
HDD 314.540 ms при startup/Preparing. Large startup gap начался на 453.097 ms,
launch был на 807.513 ms, конец gap на 810.119 ms: почти весь разрыв **до**
запуска импорта, а не 357 ms внутри parser. На large 6802 ticks;
кроме двух указанных разрывов gaps >50 ms не было. Maximize/restore/move,
индикация и доступность Cancel проверены на живом native window.

Длительного GUI freeze на время обработки не обнаружено. Результат не означает
идеальные 10 ms или отсутствие стартовой/финальной задержки. Причина startup gap
между Qt/window initialization и внешним scheduling отдельно не изолировалась;
подтверждённого дефекта import из него не следует.
OS cache не сбрасывался, cold-cache проходов и изолированной hardware throughput
оценки нет. Равные SSD/HDD times не доказывают равную физическую скорость носителей.
Обычная Windows/background нагрузка не изолировалась. Полные конфигурации выполнены
по одному разу; статистические интервалы производительности не оценивались.

## F. Reliability and Cancellation

Запросы привязаны к наблюдаемой фазе и actual worker mailbox; Z Cancel —
к waitingForZ=true и текущему request, без угадывания задержки.
Для каждого non-shutdown cancel реально выполнен последующий import на том же
controller после confirmed completion. Old metadata/cell/history совпали;
remaining new files=0, cleanupWarning=false, publication Discard.

| Сценарий | Input | До native completion, ms | Итог |
|---|---|---:|---|
| Parsing cancel, SSD, headless | Medium | 220.134 | Old project preserved, cleanup, retry PASS |
| BuildingCache cancel, SSD, headless | Medium | 219.442 | То же |
| BuildingCache shutdown, SSD, headless | Medium | 218.130 | Old project preserved, operation completed |
| BuildingCache shutdown, native Main.qml | Medium | 109.551 | Окно завершило shutdown, QML warnings=0, old project preserved |
| BuildingCache cancel, HDD storage | Medium | 219.038 | Cleanup, old project, retry PASS |
| Z-wait Cancel, Release | 3 points | 140.011 | Actual waitingForZ, line=3, cleanup, retry PASS |
| Z-wait Cancel, Debug | 3 points | 194.589 | То же |

Raw memory/I/O/disk/timing всех этих прогонов — в
[summary](benchmarks/ascxyz044b/summary.json) и отдельных JSON. Input N не является
измерением числа обработанных records при отмене; оно не экспортируется.
Нулевой sampled EOF peak короткого run не означает отсутствие temporary files.

Например, SSD BuildingCache: request→Ready hook 83.355 ms,
request→worker decision/product cleanup 109.188 ms, request→native completion
observation 219.442 ms. Это разделяет cooperative stop, cleanup и observation/join.
Отдельные OS button dispatch и atomic flag timestamp не измерялись: начало —
непосредственно перед синхронным adapter cancellation call. Poll delay входит
в верхнюю практическую оценку latency. Блокирующий OS read не обязан отменяться мгновенно.

Четыре baseline caches длительных cancel/shutdown дополнительно прошли strict
Reader/bit controls после отмены — [preserved results](benchmarks/ascxyz044b/preserved-medium-cancel-cache.json).
Native first cache также сохранил SHA-256. Принятые targets остаются после
Close/Shutdown; unaccepted attempts очищает worker. Ready/Discard, late publication,
lease/native lifetime проверены существующими deterministic regressions.
Отдельная [проверка каталогов](benchmarks/ascxyz044b/cleanup-check.json) подтвердила
ровно 17 ожидаемых accepted project directories и отсутствие scratch directories
во всех новых storage roots; новых пустых unaccepted attempts в штатном storage нет.

### Controlled failures

Использованы действующие безопасные production tests/fault seams; реальные тома
не заполнялись, RAM не исчерпывалась, ACL/диски не менялись:

| Категория | Проверка |
|---|---|
| Memory / temporary disk / maxOpenFiles | AscImport.BudgetsIncludeParserAndSourceHandle; CacheWriter InvalidAndInsufficientBudgets, separate writer/sorter disk accounting; application ResourceAndStorageFailuresDoNotCreateProject |
| Open/missing source | AscImport.EmptyMalformedZCancelAndOpenFailureCleanUp, application/UI failure cases |
| Read failure / partial read | Parser injected device, CacheFileIO failure-after-prefix, sorter run/merge fault seams |
| Write failure | AscImport.InjectedAppendAndFinishIoFailuresThroughParserPipeline, CacheWriter/file hooks |
| Publication | Existing target/rename/flush failures; application candidate preparation/Discard tests |
| Source change/disappearance | DetectableSourceGrowthAndTruncation; missing-after-selection GUI/application cases |

Эти tests проверяют structured root cause, failure/cleanup и сохранение предыдущих
данных на соответствующих границах. Application failed replacement/history,
generic failure transport и фактический cancel/retry дополняют module fault tests.
Это не полная GUI-матрица каждой OS failure × history × retry: для недоступных
сквозных OS faults использованы существующие seams и анализ общего failure path.
Новых production fault hooks ради испытаний не добавлено.

## G. Findings and Fixes

**Подтверждённых дефектов production не обнаружено; production sources не менялись.**
R1–R3 regressions и весь прежний набор сохранены. Parser, Writer/Sorter, format v1,
threading, consumer protocol, ResourceLimits и atomic Accept не перерабатывались.

Добавлен ограниченный opt-in [import_acceptance](../tools/import_acceptance/README.md)
и source oracle, без новых third-party dependencies и CTest workloads.
Runner hooks только фиксируют время, не останавливают worker. В процессе проверки
самого инструмента исправлены modal --help (используется parse/helpText в console)
и сохранение signed zero в oracle; conversion сверена с goldens. Instrumentation
callbacks привязаны к scoped QObject, setup/retry учитывает ранний terminal failure.
Отложенный Busy probe не отправляется после завершения короткой операции;
подготовка обеих baseline history branches проверяется явно.
Это не defects
CADContour2D и не основания для изменения production.

Resolved AppData storage и startup timer gaps задокументированы как фактические
особенности среды/измерений; требование persistent cache и отзывчивости ими не
нарушено по выполненным сценариям. Число production tests не увеличивалось ради отчёта.

## H. Debug/Release Verification

Обе конфигурации: CADContour2D, перечисленные tests/tools и all_qmllint — PASS.
Production CTest: **452/452 Debug (119.14 s)** и **452/452 Release (53.06 s)**.
Это включает GUI/UI, ViewModel, QML integration, Z confirmation, import application,
operation, cache, parser и project regressions. CTest timeout/assertions не менялись.
После добавления только opt-in Z latency scenario helper повторно собран и проверен
в Debug/Release; production/test code при этом не менялся. Финальная версия helper
дополнительно прошла NoProject Z-cancel в обеих конфигурациях и expected missing-baseline
failure (exit=1/status=FAIL, без попыток/данных) —
[tool checks](benchmarks/ascxyz044b/tool-final-checks.json). Финальные NoProject Z/Busy
checks и expected ParseError без false success/retained attempts также прошли —
[reviewed checks](benchmarks/ascxyz044b/tool-reviewed-checks.json). Полные нагрузки после
этих изменений lifetime/setup/probe инструментального кода не повторялись;
production libraries, clocks, sampler и вычислительный pipeline остались прежними.

Фактически выполненные основные команды после штатной CMD-инициализации:

```bat
cmake --build --preset windows-release --target import_acceptance --parallel 4
build\windows-release\synthetic-cloud-generator.exe --geometry tests\fixtures\synthetic\performance\large_plate.geometry.json --scan build\ascxyz-044b-20261018-f1f2b934\medium.scan.json --output build\ascxyz-044b-20261018-f1f2b934\medium.xyz
build\windows-release\synthetic-cloud-generator.exe --geometry tests\fixtures\synthetic\performance\large_plate.geometry.json --scan tests\fixtures\synthetic\performance\clean_horizontal.scan.json --output build\ascxyz-044b-20261018-f1f2b934\large.xyz
python tools/import_acceptance/source_oracle.py build/ascxyz-044b-20261018-f1f2b934/large.xyz --count 41984566 --output build/ascxyz-044b-20261018-f1f2b934/large-oracle.json
build\windows-release\import_acceptance.exe --source build\ascxyz-044b-20261018-f1f2b934\large.xyz --oracle build\ascxyz-044b-20261018-f1f2b934\large-oracle.json --gui --report build\ascxyz-044b-20261018-f1f2b934\large-gui.json
build\windows-release\import_acceptance.exe --source build\ascxyz-044b-20261018-f1f2b934\medium.xyz --storage F:\CADContour2D-benchmark\ascxyz-044b-20261018-f1f2b934\complete-storage --oracle build\ascxyz-044b-20261018-f1f2b934\medium-oracle.json --gui --report build\ascxyz-044b-20261018-f1f2b934\hdd-gui.json
cmake --build --preset windows-debug --target CADContour2D import_acceptance synthetic-cloud-generator gui_import_tests view_model_tests qml_integration_tests nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests cache_tests dependency_smoke operation_probe all_qmllint --parallel 4
cmake --build --preset windows-release --target CADContour2D import_acceptance synthetic-cloud-generator gui_import_tests view_model_tests qml_integration_tests nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests cache_tests dependency_smoke operation_probe all_qmllint --parallel 4
ctest --preset windows-debug -L production
ctest --preset windows-release -L production
git diff --check
```

Остальные medium cancellation/shutdown команды использовали тот же medium source,
`--baseline build\audit\rectangle-release.asc`, scenario из таблицы F,
уникальный storage/report с именем сценария; cancel headless также
`--retry-source build\audit\rectangle-release.asc`. Native shutdown добавляет
`--gui`, HDD cancel использует F:\…\cancel-storage. Z runs используют
собственный z.xyz, `--scenario cancel-z`, baseline/retry, отдельные
z-debug-storage/z-release-storage и соответствующий executable.
Точные sources/storage/result fields сохранены в JSON.

Existing cache validation выполнялась через `--cache` с original small/z oracle,
новым report path; native GUI отдельно запускался CADContour2D.exe.
CLI help проверен; independent Python conversion согласована с 37 goldens
и 219 сохранёнными source controls — [check](benchmarks/ascxyz044b/oracle-conversion-check.json).

Logs в run root: final-debug/release-build.log, final-debug/release-ctest.log,
final-debug/release-tool-build.log и отдельные run logs. Известное optional
Qt6TaskTree configure warning осталось; compiler/QML lint failures не было.
Cache subsystem не менялся, поэтому дорогой исторический cache_integration и
полная benchmark-матрица задачи 03 не повторялись.

## I. Limitations and Artifacts

Representative real scans и реальный UNC server не использовались: соответствующие
inputs/адрес сервера для испытаний не предоставлены. HDD проверен сценарием C
(Writer/Sorter/storage), не отдельным HDD-source→SSD прогоном. OS cache не
контролировался; physical disk transfers, power-loss durability, worst-case
blocked/network I/O latency и жёсткий OS RAM/job limit не измерялись.
Source same-size rewrite с сохранённым mtime имеет прежнее ограничение detection.
Нет полной DPI/theme/accessibility/Windows hardware matrix.
Эти результаты synthetic acceptance не обобщаются на все промышленные scans.

| Сохранённый artifact | Bytes / статус | Использование |
|---|---|---|
| SSD run root / large.xyz | 1 029 576 096; retained synthetic source | Повторное чтение/воспроизведение, hash подтверждён |
| SSD run root / medium.xyz | 103 065 900; retained synthetic source | Cancellation/HDD/GUI повторные целевые проверки |
| Default resolved storage / project-a047c35c-2c2a-4b72-ae25-bad2e78ad223-AmHTfA/datasets/e38e1d57-62d6-43a5-8875-350e44a1d15f.pc2dcache | 1 010 507 776; accepted persistent | Reader validation/queries без source |
| Default resolved storage / project-db3921c8-2163-4d48-a909-7533620c1dd7-XGzFaD/datasets/1daf2bfd-0474-46f5-a394-e3c284678846.pc2dcache | 101 261 312; accepted persistent | Средний verified cache |
| HDD root / complete-storage/project-3437e2f3-c737-4483-bc95-a218caddab08-ctkYlu/datasets/af19aeb0-d0c3-42f7-88fc-1509650bb011.pc2dcache | 101 261 312; accepted persistent | HDD cache, Reader |

Абсолютные SSD/HDD roots указаны в B, resolved default storage — в A.
Полные пути, размеры и SHA-256 всех **17 принятых caches**, включая small/baseline/retry,
содержит [inventory](benchmarks/ascxyz044b/summary.json). Они намеренно остаются,
не являются abandoned worker results. SSD run root ≈1.157 GB; HDD root ≈105.6 MB
на момент inventory. Копии compact JSON/manifests и три CSV закоммичены;
большие XYZ/cache и локальные build/run logs в Git не входят.

Старые внешние artifacts, включая ранее заблокированные cleanup, не удалялись.
Автоматическая проверка разрешений отклонила удаление созданного
`tools/import_acceptance/__pycache__/source_oracle.cpython-313.pyc`:
`blocked by policy`. Bytecode оставлен как локальный generated artifact,
исключён локальным .gitignore; повторной попытки очистки не было.

На финальном независимом ревью следует проверить соответствие этих измерений
кодовым гарантиям и достаточность указанной матрицы. Save/Open (05), Density Map
(06) и последующие этапы не выполнялись. Существующие статусы задач 01–03 сохранены.

## J. Final Verdict

**READY FOR INDEPENDENT REVIEW.**

Обязательные small GUI E2E, medium cancellation/shutdown, один большой Release,
bounded-memory/resource measurements, cache correctness и целевой HDD сценарий
завершены. Подтверждённых production defects нет; Debug/Release regression passed.
Задача 04 готова к финальному независимому ревью, **принятой не объявляется**.

Отчёт, opt-in tool и compact evidence фиксируются одним commit после diff review.
Конечный HEAD и состояние working tree/index сообщаются после commit в итоговом
ответе; самоссылочный hash здесь не записывается. Push не выполняется.
