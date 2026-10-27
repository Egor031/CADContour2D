# 06.0D/P1 — масштабирование экспериментального C1

2026-10-27. **P1 CLOSED WITH LIMITATIONS**. Рекомендован **C1 как единственный
основной алгоритм** для архитектурного рассмотрения. Адресная проверка после
06.0C. Production Density Map, формат D,
coordinate convention и переход к 06.1 этим документом не утверждаются.

## Baseline и источники

- `main`, `af9f44cc61711c33ddeba77a341a7ef7b3436b8e`; исходный Git status чистый.
- Прочитаны AGENTS, README, density/coordinates/resources/testing contracts
  requirements/architecture/algorithms, implementation-plan 06, C1/C2,
  CacheReader, общий Output/preflight, fixtures и raw JSON/CSV/audit 06.0C.
  Локального отчёта/исходного cache независимого 06.0D не найдено; основание P1
  и его численные результаты получены из приложенного задания.
- Исходный C1 собран отдельно из `git archive af9f44c` в
  `build/density-p1-baseline-{source,release}`. Архив применяется без редактирования
  sources; сравнение с Git учитывает только CRLF/LF, добавляемые archive attributes.
  [Baseline/source SHA-256](../../benchmarks/density-p1/baseline.json).
- Release/Debug x64: MSVC 19.51.36260.0, Qt 6.12.0, CMake 3.30.5/Ninja 1.12.1;
  штатные установленные зависимости. D: NTFS, Hitachi HDS721050CLA362 SATA HDD,
  [physical disks](../../benchmarks/density-p1/disks.json),
  [volume mapping](../../benchmarks/density-p1/volume.json),
  [resources](../../benchmarks/density-p1/resources.json).

## Причина и изменение

C2 вызывает `scanBox()` для каждой группы прямоугольного raster, очищает count
buffer и формирует/проверяет даже полностью нулевые тайлы. Число запросов равно
`ceil(width/side)*ceil(height/side)`. Пространственный index устраняет часть
чтения пустых областей, но не сам обход, повторные index/point pages и работу
над нулевыми group buffers.

Старый C1 уже читает cache один раз и создаёт spool только для занятых групп,
но после scan проверяет membership для **всех** logical groups. Его прежний
`maxGroups` также связывает логическую площадь, metadata и число файлов.
Это особенно мешает sparse datasets: preflight оценивает output через
`min(totalTiles,N)`, даже если занято всего четыре группы.

Исправление использует существующий `std::set<uint64_t>` напрямую: после одного
`scanAll()` C1 обходит только его элементы. ID `gy*columns+gx` упорядочены по Y/X.
Отдельного списка/sorter/sharding/container нет. Один bounded event chunk,
один group count buffer, LE uint32 local indices, checked uint64 counts и общий
sparse output сохранены.

| Файлы | Изменение и причина |
|---|---|
| construction.h/.cpp | Occupied traversal; отдельный maxOccupiedGroups; bounded metadata/output plan; visited/occupied/phase counters; allocation accounting; work guard для boundaries |
| benchmark.cpp | Передача новых guards в worker; Output reserve ограничен тем же планом |
| tests.cpp | 15+ млрд logical groups с двумя occupied, uint64 IDs, partial final tile, independent literal counts, empty raster, occupied refusal, boundary-work/event/area overflow |
| prepare.cpp | `--p1`: bounded CacheWriter fixtures; schema/generator 06.0A/C не меняются |
| run-p1.py / audit-p1.py | Адресная orchestration существующих workers/measure, exact sparse oracle и extension прежнего stream audit |
| README эксперимента, этот отчёт/evidence, краткие README/algorithms ссылки | Контракты экспериментальных guards и итоговая рекомендация |

## Корректность и ресурсная политика

Grid 06.0A/C не изменён: min-origin, Y-up, half-open внутренние интервалы,
clamped maximum. Binary64 boundaries, nextafter, отрицательные координаты,
дубликаты, signed zero, degenerate axes и partial tiles проверяются прежними
тестами. Полного raster allocation в C1 нет. Огромный regression test дополнительно
проверяет literal first/last tile counts и отсутствие других тайлов; последний
group ID превышает uint32. Empty cache посещает ноль групп.

`makeGrid` проверяет representable dimensions/area, layout — group products,
uint32 local range и размер event chunk. При `cell()` оба group coordinates
лежат внутри layout, поэтому линейный ID меньше проверенного `rows*columns`.
Последняя группа ограничивается grid bounds; канонический Output сортирует
nonzero tiles независимо от группировки. Успешный marker создаётся только после
полного build, finish, sum checks и readback; отмена/ошибка/resource refusal
не публикуют успешное завершение. RAII очищает только собственный event directory.

Пусть `N` — measurements, `G` — occupied groups, `E` — event capacity, `S` — group
side, `T` — реально сохранённые tiles. RAM имеет вид `O(E + G + S² + T)` плюс
bounded Reader/runtime reserve; нет члена для пустой площади. План включает
80 bytes на разрешённую occupied group и 64 bytes на output descriptor; это
консервативные allowances, не точный allocation trace. Runtime working set
проверяется на checkpoints, OS hard cap/system commit peak не измеряются.

- `maxGroups` default 1 000 000 ограничивает только C2 queries.
- `maxOccupiedGroups` default 1 000 000 — экспериментальный верхний guard,
  **не подтверждённое практически приемлемое число файлов**. Preflight использует
  `min(N,logicalGroups,maxOccupiedGroups)` и ограничивает descriptor/output plan
  числом тайлов этих групп. Runtime отказывает до создания лишнего spool файла;
  C2 также соблюдает occupied limit общего плана.
- Sparse trials явно ограничены четырьмя occupied groups. Это проверяемое
  ограничение входного профиля с безопасным отказом, а не автоматическая оценка
  будущего распределения. Default plan для произвольного huge sparse input может
  по-прежнему консервативно отказать; пользователь стенда может задать меньший
  occupied budget. Никакой resource check не отключён.
- Логический scratch peak точно `4*N`; дополнительно планируется 4096 bytes/file.
  Измеряется максимум суммы reported allocation после flush, через borrowed
  native handle и [FILE_STANDARD_INFO::AllocationSize](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_standard_info).
  Источник descriptor — [QFileDevice::handle() Qt 6.12](https://doc.qt.io/qt-6/qfiledevice.html#handle),
  преобразование — [_get_osfhandle](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-osfhandle?view=msvc-170).
  Это не весь объём NTFS MFT/directory metadata и не physical I/O. Allocation
  проверяется против scratch budget; свободный диск после preflight не гарантирован.
- Один spool QFile открыт одновременно; вместе с Reader/Output — до трёх
  принадлежащих эксперименту dataset handles. Allocation query не открывает
  дополнительный dataset. Process-wide/transient Qt handles peak не измерен.
  Число opens = сумма distinct groups по chunks плюс `G` read opens; worst case
  не больше `min(N,G*ceil(N/E))+G`. Файлы удаляются по мере накопления.
- Проверка всех внутренних boundaries сохраняет `O(width+height)` time / `O(1)`
  RAM. Новый `maxBoundaryChecks`, default 100 000 000 внутренних boundaries,
  даёт ранний resource refusal для непрактичного обхода осей. Это настраиваемый
  work guard эксперимента, не предел production координат и не лимит RAM.

## Методика и воспроизведение

Исходные файлы/evidence 06.0A–C не перезаписывались. Новый каталог данных —
`build/density-p1-matrix`, evidence — `docs/benchmarks/density-p1`.
Первый подготовительный запуск остановлен до основной матрицы при добавлении
work guard; его собственные каталоги сохранены только в ignored `build/density-p1-pilot-*`
и не входят в результаты. Финальная матрица не выполнялась параллельно со сборками,
тестами или нагрузочным audit.

Каждый trial — новый процесс/каталог. First сохраняется отдельно; subsequent
n=3, ротация кандидатов и обратный порядок последнего повтора. Приведены
median и min–max; при таком n tail-поведение не подтверждено. `elapsedMs` включает
build/preflight/output finish, но исключает source SHA, подготовку/oracle и
readback. `open` измеряется отдельно; audit также сохраняет median open+build.
Вложенные callback/scan timers не складываются. Working-set peak — lifetime
до verification. OS cache не контролируется; `first` **не cold-cache**, process/
CacheReader bytes/operations логические, physical HDD/network I/O не измерен.

Общие budgets для сравнения трёх кандидатов: RAM 512 MiB, scratch 1024 MiB,
output 4096 MiB, event capacity 65 536. Больший output budget нужен исходному
C1/C2 plan на 16383²; фактический output sparse trials значительно меньше.
В reference preparation 42M используются прежние defaults: это проверка входа,
не строка сравнительной performance-таблицы.

В штатной CMD-среде из корневого README (`build/task02-env.cmd` локально задаёт её):

```bat
git archive --format=zip --output=build/density-p1-baseline.zip af9f44c
```

После Expand-Archive в новый `build/density-p1-baseline-source`:

```bat
cmake -S build/density-p1-baseline-source -B build/density-p1-baseline-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCADCONTOUR2D_BUILD_TOOLS=ON -DBUILD_TESTING=OFF "-DCMAKE_TOOLCHAIN_FILE=D:/Program Files/Microsoft Visual Studio/18/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64 -DVCPKG_INSTALLED_DIR=D:/ZherlitsynEE/CADContour2D/build/windows-release/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF
cmake --build build/density-p1-baseline-release --target density_construction_benchmark --parallel 4
cmake --build --preset windows-debug --target density_construction_tests density_construction_prepare density_benchmark_tests --parallel 4
cmake --build --preset windows-release --target density_construction_tests density_construction_prepare density_benchmark_tests --parallel 4
ctest --preset windows-debug -L density_ --output-on-failure
ctest --preset windows-release -L density_ --output-on-failure
cmake --build build/density-06b-debug --target density_formats_tests --parallel 4
cmake --build build/density-06b-release --target density_formats_tests --parallel 4
ctest --test-dir build/density-06b-debug --output-on-failure
ctest --test-dir build/density-06b-release --output-on-failure
python tools/density_benchmark/construction/run-p1.py build/density-p1-matrix docs/benchmarks/density-p1 --baseline build/density-p1-baseline-release/density_construction_benchmark.exe
python tools/density_benchmark/construction/audit-p1.py docs/benchmarks/density-p1
```

Для audit нужны JUnit из этих CTest запусков в `tests/core-{debug,release}.xml`
и `tests/formats-{debug,release}.xml`. Пути SDK/dependencies машинозависимы;
первый configure с ошибочным installed-dir исправлен на фактический cache path.
Известные optional Qt TaskTree/Vulkan configure diagnostics сохраняются.
Первая попытка передать `|` через CMD wrapper не запустила tests; фактические
успешные команды используют label `density_`. Compiler warnings не подавлялись.

### Входы и независимая проверка

Четыре кластера содержат по 256×256 integer points: координаты каждой оси —
`0..255` либо `16128..16383`. Всего 262 144 measurements; один и тот же cache
используется при cell 2/1 и при росте upper bounds до 32767, 131071, 1048575.
Таким образом точно воспроизведены размеры raster, N, число кластеров, storage
tile 256 и construction group 512 из задания. Байтовая идентичность исходному
review fixture не заявляется: его точные координаты/cache не предоставлены.
В частности, объём повторных чтений C2 зависит от полученного Morton index.

Many-occupied fixtures: 64, 1024, 4096 групп по 32×32 cells, tile side 16,
cell 1, по одной позиции `(32*gx+.5,32*gy+.5)` в группе, повторённой `262144/G`
раз. N постоянно, оси групп 8/32/64, logical groups полностью заняты. Меньший
group side позволяет адресно измерить файловые издержки без гигабайтов нулевого
output. Влияние группы 512 независимо покрывают sparse и 42M сценарии.

Python audit строит integer interval oracle из формулы кластеров, с отдельным
clamped max; для many-occupied использует literal labels и multiplicities.
Каждая сохранённая cell, включая нули, сравнивается с oracle; множество nonzero
cells обязано совпасть полностью, поэтому все omitted tiles тоже проверены как
нулевые. Для 42M новый `--regular-fixture` проверяет каждый RecordId/coordinate
и строит independent separable histograms; audit проверяет все dense expected
counts, включая нули. Ни candidate binary search, ни его spool/mapping в Python
oracle не используются. Общий прежний decoder/digest audit переиспользован.

| Фактически выполненные проверки | Debug | Release |
|---|---:|---:|
| Build benchmark/construction tests/prepare + 06.0A tests | PASS | PASS |
| 06.0C + два новых regression tests | 14/14 PASS | 14/14 PASS |
| 06.0A tests | 22/22 PASS | 22/22 PASS |
| 06.0B build + tests | 13/13 PASS | 13/13 PASS |

JUnit и build logs находятся в [tests](../../benchmarks/density-p1/tests).
Проверены отмена в scan и после выданного префикса, source preservation,
реальный spool open failure, sink I/O failure, corrupt/truncated cache,
tiny RAM/scratch/output, overflow/collapsed intervals и существующий output
directory. Старый тест `layout(maxGroups=2)` изменён согласно новому смыслу
guard: layout допустим для C1, а C2 отказывает до scratch; это дополнительно
проверяет новый sparse regression. Корректные oracle checks не ослаблены.

## Измерения

Все времена ниже — ms, subsequent median [min–max], n=3; first и каждый raw
повтор сохранены отдельно в [evidence](../../benchmarks/density-p1/README.md).
Во всех сравниваемых столбцах одинаковые cache, grid, tile/group и budgets.

| Сценарий | Исходный C1 | Обновлённый C1 | C2 |
|---|---:|---:|---:|
| 8192², cell 2, group 512, четыре кластера | 191.4 [190.1–221.6] | 224.4 [195.3–343.0] | 804.7 [790.8–805.5] |
| 16383², cell 1, group 512, четыре кластера | 197.9 [195.9–199.7] | 198.0 [197.3–200.6] | 2782.1 [2701.5–2803.3] |
| 64 occupied groups, side 32 | 245.9 [240.2–278.5] | 297.9 [242.4–305.5] | 266.5 [263.0–306.8] |
| 1024 occupied groups, side 32 | 1021.6 [790.4–1152.4] | 1001.1 [966.9–1098.4] | 2662.4 [2625.1–2750.2] |
| 4096 occupied groups, side 32 | 2895.6 [2551.9–3189.8] | 2952.2 [2371.7–3021.7] | 10701.1 [10642.5–10766.7] |
| 42M measurements, cell .1, group 512, tile 256 | 27985.7 [27657.5–30122.7] | 27483.0 [27458.6–27719.0] | 26559.6 [25100.0–26758.2] |

На 8192² и малом many-occupied обновлённый C1 имеет больший median и заметный
разброс; фазовые timings и n=3 не доказывают устойчивого ускорения от этого fix.
На 16383², 1024/4096 occupied и большом cache существенного ухудшения не выявлено.
Ценность изменения — устранение зависимости обхода/плана от пустой площади и
проверяемый ресурсный профиль. На узком 42M cache C2 сохраняет небольшое
преимущество; этого недостаточно для оправдания двух production алгоритмов.

C1 читает sparse cache один раз: **97 point pages, 6.0625 MiB, 97 reads/seeks**;
spatial queries = 0. C2 выполняет соответственно **256 / 1024 queries**,
возвращаясь к index/point pages: **55.8125 / 201.8125 MiB** логического чтения.
Различие с 153.38 MiB из задания относится к восстановленному fixture, точные
исходные review bytes отсутствуют. Причина P1 воспроизведена: counts одинаковы,
а число spatial queries и нулевая работа растут вместе с logical groups.

### Рост пустой площади

N=262 144, occupied=4, group=512, cell=1; один неизменный cache, bounds расширены
только вверх/вправо. C1 во всех случаях выдаёт четыре nonzero tiles, читает те же
97 pages / 6.0625 MiB и использует 1 MiB logical/reported allocation scratch.

| Raster | Logical groups | Visited / occupied C1 | Median ms | Peak WS MiB |
|---|---:|---:|---:|---:|
| 16383² | 1024 | 4 / 4 | 198.0 | 13.95 |
| 32767² | 4096 | 4 / 4 | 203.3 | 13.94 |
| 131071² | 65536 | 4 / 4 | 235.2 | 13.98 |
| 1048575² | 4194304 | 4 / 4 | 412.0 | 13.98 |

Рост total объясняется сохранённой axis validation: около 3 ms на 16383² и
200 ms на 1048575². Сам выбор четырёх occupied groups/checkpoints занимает
около 0.01 ms; полного обхода logical groups больше нет. RAM, spool files и
measurement I/O остаются постоянными. Новый regression независимо проверяет
**15 625 250 001 logical groups / 2 visited / 2 occupied**, без full-raster oracle.

Исходный C1 на 32767² безопасно отказывает по консервативному output envelope
в отдельной refusal пробе с default output budget 1024 MiB; сам envelope
превышает 8192 MiB, то есть также больше сравнительного budget 4096 MiB.
На 1048575² исходный C1 отказывает по maxGroups.
Эти случаи сохранены как refusals; времени несуществующего successful build
им не приписано. На больших growth jobs C2 не запускался: известный полный
обход не нужен для подтверждения sparse traversal C1 и был бы несоразмерен.

### Файлы, RAM, scratch и накопление

| C1 scenario | Files / opens | Metadata allowance | Logical / reported allocated peak MiB | Peak WS MiB | Output tiles |
|---|---:|---:|---:|---:|---:|
| Четыре кластера | 4 / 12 | 320 B | 1 / 1 | ≤13.98 | 4 |
| 64 occupied | 64 / 128 | 5 KiB | 1 / 1 | 9.97 | 64 |
| 1024 occupied | 1024 / 2048 | 80 KiB | 1 / 4 | 10.09 | 1024 |
| 4096 occupied | 4096 / 8192 | 320 KiB | 1 / 1 | 10.38 | 4096 |
| 42M | 20 / 1367 | 1600 B | 160.217 / 160.234 | 34.16 | 40 |

Spool read/write bytes совпали с `4*N` в каждом successful C1 trial; final logical
и reported allocation равны нулю, `.events` после завершения отсутствуют.
Metadata allowance отражает план, фактический размер set/node не измерен.
Reported allocation для маленьких файлов не включает полный MFT/directory cost;
меньшая сумма на 4096 файлах сама по себе не означает меньшую нагрузку на FS.

Фазовый пример r1 C1: при 1024 группах write/open/flush/allocation — 282 ms,
read-side open/close/remove — 404 ms, accumulation+tiles — 37 ms, selection/
checkpoints — 2.1 ms. При 4096: 1704 / 780 / 54 / 8.5 ms соответственно.
Таким образом главный фактор роста — файловая система, а не set traversal.
При 42M: write phase около 665 ms, read lifecycle 34 ms, accumulation+tiles
3838 ms; основное время остаётся в scan/mapping. Все повторы/фазы доступны в raw;
фазовый пример не подменяет median total.

4096 файлов и 8192 opens практически выполнимы на данном HDD за несколько секунд
при bounded RAM. Усложнение spool сейчас не подтверждено необходимостью. Однако
линейная файловая стоимость реальна: экстраполяция до миллиона файлов не является
приёмкой. При существенно большем G требуется отдельная адресная проверка; если
file lifecycle станет неприемлемым, следующим минимальным исследованием будет
ограниченное укрупнение spool-файлов, с тем же single scan и output. В P1 оно
не реализовано и не требуется для проверенного диапазона.

Для 42M C1 неизменно читает **15 408 pages / 963 MiB / 15 408 reads**, C2 —
19 554 point + 178 index pages / 1233.25 MiB / 19 732 reads, 20 queries.
Все три кандидата дают **42 000 000**, 420 000 nonzero cells, maximum 121,
40 tiles / 3 361 280 bytes и canonical SHA-256
`26890e30ef0628958b9816b11b55dffcae1677399a9b51549b5a54c8f3a8f982`.
Это digest при tile 256; исторический tile 64 digest напрямую не сравнивается.

## Audit, отказы и рекомендация

[Independent audit](../../benchmarks/density-p1/audit.json) проверяет **84 exact
oracle outputs**, JSON/CSV, canonical streams и SHA-256, пять source caches
до/после, отсутствие event leftovers, counters и JUnit. 42M source SHA также
совпадает с 06.0C. Отдельный regular-reference JSON/CSV содержит проверку входа
по всем records и четыре успешных build/readback; он исключён из performance
statistics. [Семь CLI refusals](../../benchmarks/density-p1/refusals.json):
исходные logical/output guards, новый occupied limit, boundary work, tiny RAM,
scratch и output. Все имеют `resource_refused`, без successful completion marker
и без `.events`. Отмена покрыта Debug/Release checkpoint tests; реальный
disk-full и управляемый OS hard-limit injection не выполнялись.

**P1 CLOSED WITH LIMITATIONS.** C1 устраняет обязательный обход пустых групп,
сохраняет exact counts, bounded RAM и управляемый spool на проверенных sparse,
many-occupied и большом Point Cache. Рекомендовать его как единственный основной
алгоритм обоснованно; поддержка гибрида/автовыбора C1/C2 не требуется.

Границы вывода: synthetic fixtures, до 4096 реально занятых spool groups,
42M measurements на регулярной узкой полосе, uncontrolled OS cache на локальном
HDD. Cold physical HDD/network I/O, representative real scans, million-file
workloads, system commit peak и полный filesystem metadata footprint отсутствуют.
Axis validation `O(width+height)` и консервативный tunable occupied plan остаются
явными ограничениями эксперимента; ускорение одного synthetic case не является
основанием заключения.

До 06.1 требуется архитектурное согласование C1 и отдельно хранения D/coordinate
policy, а также выбор production resource policy с безопасным refusal вместо
переноса experimental maxOccupiedGroups=1M как испытанного значения. При запросе
выхода за проверенный file-count profile нужна целевая проверка, а не второй
основной алгоритм. Production CacheReader/ProjectState/persistent formats,
Save/Open/Recovery, GUI и Rough Stage не изменены. Переход к 06.1 не выполнен,
окончательное D/C1 решение за пользователем. Push не выполнялся.
