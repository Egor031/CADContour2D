# Persistent Point Cache — проверка 5/5

## A. Baseline

17 октября 2026 г., ветка `main`, исходный HEAD
`2509a7a134e478dc28273dd19c3aa5f9292134dd`. Рабочая копия была чистой:
tracked/staged/untracked изменений не было. Изучены AGENTS, README, задача 03
implementation-plan, требования к measurements/cache, architecture §§12–14,
39–44, численные/ресурсные правила algorithms, point-cache-format,
cache-reader-check, production codecs/file/sorter/writer/reader и их tests/CMake.
Файловые примитивы находятся в `src/cache`; отдельного `src/io` пока нет.

## B. Reliability Verification

Инвентаризация до добавления проверок:

| Область | Оценка существующего покрытия |
|---|---|
| Codec, binary64, layout, checksums | Достаточно: независимые golden bytes/SHA, overflow, reserved/padding, finite extrema, signed zero/subnormal |
| Ingest и sorter | Достаточно: независимый численный oracle (старший различающийся IEEE bit), random finite bits, разные batches, совпадения, multi-generation |
| Writer/index/sealing | Достаточно: независимый DFS, реальные page boundaries, искусственные глубокие уровни, rehashed semantic mutations |
| I/O, cancel, resource limits | Достаточно: production fault seams перед transfer/seek/flush, real truncation/open/rename errors, ошибки после prefix и всего sink |
| Cleanup и ownership | Достаточно: move/Discard/Accept/repeated transfer, native locks и destructor retry, запрет overwrite, сохранение первой ошибки |
| Reader/union/validation | Достаточно для запросов, strict ID coverage и отказов; не хватало проверки канонического порядка |
| Abrupt process exit | Отсутствовало; добавлены четыре изолированные контрольные точки |
| Большие данные/HDD/ресурсы | Предварительного height=2 integration недостаточно; добавлен явный benchmark |

Существующие `FileFaultsDuringRunMergeAndOutput` проверяют initial run write/read,
intermediate merge read/write/seek/flush и final merge read; Writer tests — point
page/descriptor/index/sealing I/O, реальный staged flush, существующий target и
native rename failure. Cancellation проходит ingest/sort/run write/merge/output,
point pages/index/sealing/readback/ID sort. Недостаточные RAM/disk/handle budgets,
недоступный parent, terminal/reentrant state, reader index/point read errors,
callback Status/exception и повторное использование уже проверены. Эти сценарии
не дублировались. Физические disk-full/OOM не имитируются заполнением диска/RAM.

Добавлены две регрессии Morton validation: перестановка полных записей с разными
ключами и перестановка ID совпадающих координат внутри/между point pages.
Checksums пересчитываются; BBox/counts/ID coverage сохраняются. Есть ±0,
очистка scratch и успешная повторная validation после восстановления файла.

`cache_crash_probe` — собственный дочерний процесс. Родитель ждёт READY и вызывает
`QProcess::kill()` до sealing, после завершения записи/flush до rename, после
rename до release и после `releaseToOwner()`. Проверяются исходные hashes
предыдущего принятого dataset и постороннего sentinel, возможность повторно
открыть/проверить старый cache, отсутствие final target до rename, непригодность
неполного staging и структурная пригодность завершённых orphan files.
Завершённый orphan может читаться явно по пути, но не передаётся владельцу проекта
автоматически: project/recovery слоя здесь нет. Destructor cleanup после kill
не ожидается. Изолированный каталог удаляется родителем после проверки.
Это **не проверка power-loss durability**.

Подтверждённых дефектов production lifecycle не обнаружено. Известный пробел
каноничности полной validation устранён; Writer и модель владения сохранены.

## C. Resource Verification

Оба больших dataset: N=42 000 000, writer/validation disk budget=8192 MiB,
maxOpenFiles=4; Reader queries=1 MiB / 0 scratch / 1 file. Внутренний budget
validation включает Reader, measurement batch и ID sorter. Входной batch harness
(98 304 bytes), Qt/runtime и monitor не относятся к writer working allocations.

| Метрика | 8 MiB | 128 MiB |
|---|---:|---:|
| Наблюдённый максимум рабочих массивов Writer+sorter, bytes | 8255400 | 134084520 |
| Peak Working Set процесса при build, MiB | 19.324 | 139.324 |
| Lifetime Peak Working Set к концу validation, MiB | 19.520 | 139.508 |
| Sampled peak PrivateUsage при build, bytes | 10788864 | 136806400 |
| Sampled peak owned disk при build, bytes | 3360006784 | 2689818560 |
| Sampled peak owned disk при validation, bytes (включая cache) | 4370907936 | 2690901408 |
| Conservative preflight bound, bytes (не измерение) | 4372393472 | 4372380992 |
| Initial runs / intermediate generations / fan-in cap | 208 / 1 / 64 | 13 / 0 / 64 |
| Run capacity / block records | 202087 / 1024 | 3347815 / 1024 |
| Наблюдённый максимум dataset file owners при build | 4 | 3 |
| Sorter максимум одновременно открытых файлов | 2 | 1 |
| Sampled process HandleCount, build (initial=174) | 179 | 178 |
| Sorter logical read / write bytes | 5040006912 / 3360013568 | 1680000416 / 1680000832 |
| Sorter read / write calls / logical seeks | 123403 / 104238 / 123827 | 41037 / 41050 / 41063 |
| Process build ReadTransferCount / WriteTransferCount | 5046196992 / 4372397824 | 1686190496 / 2692385088 |

Каждый profile запускался в новом процессе. Переход от 1.2 млн к 42 млн records
при 8 MiB оставляет размер рабочих массивов **8255400 bytes** неизменным; peak
процесса остаётся около 20 MiB. При 128 MiB массивы увеличиваются на 120 MiB,
а не до размера облака. Это согласуется с bounded моделью; process memory не
приравнивается к заданному budget. Reader arrays height=2 — 245440 bytes, 2 frames.

Рабочие массивы + writer control/path reserve + sorter fixed reserve укладываются
в M; проверены фактические capacities. Приватные scratch generations содержат
40-byte entries. Малый budget требует 208 runs → 4 runs → final output и
одного verification readback промежуточного поколения. Большой — 13 runs → final
output без второго scratch-файла. Measured scratch read volume снижается
с 5.04 до 1.68 GB, write volume с 3.36 до 1.68 GB.

Process counters включают также descriptors/index/sealing и вспомогательные I/O;
они не подменяют счётчики sorter. Windows samples и file-owner observations имеют
разную область измерения. Для validation допустимы один Reader + до двух sorter
handles; её sampled process peak=179/178. Существующие native regression tests
проверяют возврат handle count после закрытия и отсутствие scratch после success,
cancellation/failure. Полное прочтение validation: 15408 point + 17 index pages,
envelope 8192 bytes, всего **1010900992 bytes**, 15427 read/seek calls; ID sorter I/O
считается отдельно в Windows validation resources JSON.

## D. Performance Methodology

- Windows 11 Pro 10.0.26200 x64; Intel Xeon E5-2690 v4, 14 cores / 28 logical CPUs;
  доступная ОС RAM 33 455 132 KiB (около 31.91 GiB), при инвентаризации свободно
  около 18.8 GiB. Release, MSVC **19.51.36260.0** по CMake compiler detection
  (toolset directory 14.51.36231, VS 18.11.0), C++20, `/O2 /W4
  /permissive- /fp:precise /utf-8`, Qt 6.12.0, CMake 3.30.5.
- `F:` — NTFS на SATA **TOSHIBA HDWD110**, 1 TB, MediaType=HDD подтверждён
  `Get-PhysicalDisk` и сопоставлением `Get-Partition -DriveLetter F | Get-Disk`.
  Перед началом свободно 307 560 517 632 bytes. Все новые данные создаются в
  собственных уникальных `F:\CADContour2D-benchmark\cache05-XXXXXX`.
- `cache_benchmark` — отдельный opt-in tool под `CADCONTOUR2D_BUILD_TOOLS`,
  не CTest. Источник вычисляет measurement по ID без полного облака в RAM;
  batch=4096, seed=314159. Это ограниченный controlled measurement source,
  без ASC/XYZ parser и без нового полноценного scan generator.
- Horizontal: `(id % 8192, id / 8192)`; vertical меняет оси; multi чередует
  горизонтальный/вертикальный проходы для `j=id/2`; repeated повторяет каждое
  horizontal measurement дважды. Clustered/sparse используют фиксированную
  stateless integer permutation и четыре квадрата со стороной 124.875 мм,
  шагом 0.125 мм, расстоянием между началами 1200/100000 мм. Z flag проверяется
  на measurements с ID кратным 997. Все X/Y в мм, без квантования Writer.
- Семь запросов: local `[10,15]²`, horizontal `y=100`, vertical `x=100`
  (полосы до 1 000 000 мм), separated union из трёх rectangles, overlapping
  union с повторным rectangle, large `[0,4096]²`, scanAll. Точные boxes находятся
  в `queries()` harness. Для каждого — три timed passes с counting sink;
  затем отдельный oracle pass сверяет каждый ID, биты X/Y, membership,
  отсутствие повторов/полноту с независимым проходом исходного источника.
- Первый запрос выполняется после build/readback. OS cache не сбрасывается;
  **first-pass не считается cold-cache**, два следующих — warm repeats.
  Другие запросы также прогревают часть данных. Нет изоляции от обычной нагрузки
  Windows; минимумы/максимумы трёх проходов сохраняются в JSON.
- Baseline использует те же point/page codecs, SHA-256, размеры страниц, writer
  descriptor/index builder, sealing и production Reader. Только sorter обходится
  отдельным friend adapter в benchmark executable, ID идут в исходном порядке.
  Это экспериментальный `.baseline`, не канонический v1 dataset; полная validation
  к нему не применяется. Его startup включает allocation и немедленное освобождение
  обычного sorter; это небольшой консервативный overhead baseline, без run I/O.
- Время фаз получено на cancellation checkpoints без production timers:
  `phase_4` ingest (включая generation source), `5` run sort, `6` run write,
  `7` intermediate merge **вместе с verification**, `8` final merge **вместе
  с point-page encoding/write**; после удаления sorter `110` partial page tail,
  `111` index, `112` header/footer/sealing/readback/rename. `generation_N` — число
  уже завершённых intermediate generations; финальный merge не считается
  дополнительным scratch generation. Последовательность phase intervals
  агрегируется, а не измеряет исключительно время системных I/O calls.
- `GetProcessMemoryInfo`: lifetime PeakWorkingSetSize — ближайшая доступная
  метрика к peak RSS. Дополнительно 100-ms samples WorkingSetSize/PrivateUsage.
  Включает runtime/Qt/monitor; не включает отдельно весь OS file cache.
  Phase samples могут пропустить короткий пик; lifetime peak после build
  наследуется validation/baseline и не приписывается этим фазам заново.
- Диск: сумма EOF собственных файлов с шагом 100 ms через `CreateFileW` с
  FILE_READ_ATTRIBUTES/share READ|WRITE|DELETE + `GetFileSizeEx`. Размер directory
  entry Windows может отставать от открытого writer, поэтому не используется.
  Это sampled logical lengths, нижняя оценка временного максимума, без allocation
  granularity/NTFS metadata. Отдельно сохраняются conservative planned bound и
  budget high-water sorter/writer; они **не измерения** фактического диска.
- Handles: внутренние checkpoint observations file owners и ранее существующие
  native handle tests; отдельно sampled process HandleCount, включающий monitor,
  runtime и directory enumeration. Reader хранит один dataset handle. Счётчики
  sorter/Reader и Windows GetProcessIoCounters — логические transfers/seeks,
  **не physical HDD bytes, не перемещения головок**. Physical I/O не измерен.
- До данных проверяются ResourceLimits, checked layout, размер generations/cache/
  descriptors и достаточность split budgets; на каждом volume сохраняется
  минимум 10 GiB сверх выбранного полного disk budget. Morton cache удаляется
  перед созданием baseline; одновременно двух гигабайтных caches нет.

## E. Benchmark Results

Большой multi-pass, 42 млн records, 8 MiB. Медиана и min..max трёх timed запросов;
один build каждого варианта. Cache одинакового размера: **1 010 900 992 bytes**,
15 408 point pages, 17 index pages (1 114 112 bytes), height=2. Исходные point
records занимают 1 008 000 000 bytes; текстовый XYZ здесь не создавался.

| Запрос | Morton point/index pages | Baseline point/index pages | Morton read bytes | Baseline read bytes | Morton ms, median (range) | Baseline ms, median (range) |
|---|---:|---:|---:|---:|---:|---:|
| Local, 72 records | 3 / 2 | 2672 / 17 | 327680 | 176226304 | 4.45 (4.45–4.51) | 2201.47 (2120.47–2235.10) |
| Horizontal, 10756 | 91 / 4 | 3249 / 17 | 6225920 | 214040576 | 84.30 (73.87–90.47) | 2557.93 (2503.61–2608.95) |
| Vertical, 10756 | 64 / 2 | 3249 / 17 | 4325376 | 214040576 | 51.32 (49.89–51.41) | 2510.48 (2503.27–2602.44) |
| Separated, 556 | 17 / 4 | 2707 / 17 | 1376256 | 178520064 | 16.11 (16.01–16.60) | 2152.88 (2096.47–2235.73) |
| Overlapping, 1522 | 8 / 2 | 2844 / 17 | 655360 | 187498496 | 7.49 (7.40–7.74) | 2229.70 (2205.82–2264.35) |
| Large, 21009030 | 8119 / 16 | 15408 / 17 | 533135360 | 1010892800 | 6424.03 (6285.96–6433.71) | 12052.67 (11980.07–12094.85) |
| scanAll, 42000000 | 15408 / 0 | 15408 / 0 | 1009778688 | 1009778688 | 11987.46 (11886.45–12048.78) | 11987.16 (11827.24–12309.60) |

Логические read/seek counts равны сумме point/index pages; open отдельно от этих
чисел. Local читает **0.0195%** point pages Morton против **17.34%** baseline.
Узкие полосы — 0.59%/0.42% против 21.09%. scanAll одинаков по объёму и времени:
сортировка сама по себе не ускоряет полный последовательный проход.

Стоимость построения Morton **105.970 s**, baseline **19.433 s**; дополнительные
**86.537 s** компенсируются в сценарии примерно 40 local, 35 horizontal,
36 vertical, 41 separated, 39 overlapping или 16 large запросов. Это деление
измеренной разницы build times на разницу медианных query times, без гипотез
о реальной частоте пользовательских операций. Для scanAll компенсации не показано.

| Условное число одинаковых local queries | Morton build + queries, s | Baseline build + queries, s |
|---:|---:|---:|
| 10 | 106.01 | 41.45 |
| 50 | 106.19 | 129.51 |
| 100 | 106.42 | 239.58 |

Это сценарии в условиях измеренного OS cache, не прогноз cold HDD latency.
Уменьшение bytes/seeks подтверждает выборочность, но не доказывает пропорциональное
уменьшение времени физических HDD перемещений. Преимущество зависит от query shape,
распределения и количества повторных обращений.

Фазы Writer и полная validation (секунды):

| Фаза | 8 MiB | 128 MiB |
|---|---:|---:|
| Ingest, включая source generation | 17.914 | 17.860 |
| Initial run sorts, сумма | 7.305 | 6.829 |
| Initial run writes, сумма | 5.641 | 5.861 |
| Intermediate merge generation + verification | 43.636 | отсутствует |
| Final merge + point-page write | 31.200 | 31.797 |
| Partial point-page tail | 0.0007 | 0.0007 |
| Index build | 0.133 | 0.174 |
| Sealing/readback/publication | 0.135 | 0.195 |
| Build всего, включая setup | 105.970 | 62.761 |
| Source-order baseline build | 19.433 | 20.032 |
| Full validation | 107.451 | 64.445 |

Высокий budget убрал intermediate generation, что объясняет основное ускорение.
Reader/query buffers при этом одинаковы. В отдельном 128-MiB запуске local:
4.52 ms против 2168.48 ms baseline (медианы), те же 3 против 2672 point pages;
разница build times окупается примерно за 20 таких запросов. Это сценарная
оценка внутри второго запуска, а не сравнение запросов между разными budget runs.

На 1.2 млн records каждый файл — 28 975 104 bytes, 441 point page, height=1.
Все семь классов queries/полная validation/побитовый oracle прошли на каждом
распределении. Ниже local query (медиана трёх passes); полный набор и разброс —
в [raw JSON reports](../../benchmarks/cache05).

| Распределение | Build Morton / baseline, s | Validation, s | Local point pages Morton / baseline | Local ms Morton / baseline |
|---|---:|---:|---:|---:|
| Horizontal | 1.620 / 0.520 | 1.696 | 3 / 7 | 3.64 / 5.94 |
| Vertical | 1.680 / 0.511 | 1.850 | 1 / 7 | 1.42 / 6.05 |
| Clustered | 1.827 / 0.536 | 1.852 | 4 / 441 | 4.52 / 338.83 |
| Sparse clusters | 1.885 / 0.602 | 1.806 | 4 / 441 | 6.62 / 340.11 |
| Repeated measurements | 1.701 / 0.625 | 1.789 | 3 / 7 | 3.63 / 5.90 |

На уже локализованных line scans дополнительная сортировка даёт скромный выигрыш:
около 480 horizontal local queries до условной компенсации build cost.
Для перемешанных скоплений — примерно 4 queries. Morton/BBox сохраняется:
точность/identity подтверждены, рабочие ресурсы bounded, selective reads полезны
на всех проверенных распределениях, без предположения универсального превосходства.
Compression, mapping, дополнительные threads и новый backend не понадобились.

Для большого build sampled owned disk Morton=3.360/2.690 GB (8/128 MiB),
baseline=1.012 GB в обоих запусках. Конечный размер одинаков. Полная стоимость
пространственной организации оплачивается во время ingest/sort, а не скрывается
увеличением постоянного индекса. На HDD физически cold-cache не получен;
одной модели HDD и synthetic распределений недостаточно для нормативов latency
на других машинах или произвольных scans. Это ограничение интерпретации,
не неподтверждённая корректность запросов.

## F. Morton Validation Decision

Полная `CacheReader::validate()` теперь проверяет строгое возрастание
`(Morton128, RecordId)` в физическом порядке. Предыдущие ключ и ID сохраняются
между всеми callback batches/pages. Нарушение — `CorruptCache`, phase Validation,
operation Decode. Signed zero канонизируется только в ключе; исходные биты
координат не меняются. Совпадения координат допустимы, ID служит tie-breaker.
Строгость согласуется с глобально уникальными ID; ID validation внешней сортировкой
остаётся необходимой: координаты разных записей не доказывают уникальность ID.

Дополнительное состояние O(1): два uint64 ключа, RecordId и bool, без массива,
дополнительного I/O или изменения fast open. Переиспользуется тот же `mortonKey`,
проверенный независимыми golden words и численным oracle/random finite patterns.
Большой benchmark отдельно сравнивает три пары scanAll с/без order-check и
измеряет всю настоящую validation, включая ID scratch sort. Парный scan — оценка
добавочного CPU/scan времени, а не отдельный прогон старого validate.

Для 42 млн / 8 MiB медианы трёх пар: обычный scan 11.966 s, с проверкой порядка
22.362 s; добавка **10.396 s**, около **9.7%** полной validation 107.451 s.
Checksums и decode страниц включены в оба прохода, ID sort в этой парной оценке
отсутствует. Стоимость считается приемлемой для явной полной проверки; переносить
её в fast open оснований нет. Существенного изменения бюджета памяти не требуется.

## G. Production Changes

- `src/cache/cache_reader.cpp`: глобальная проверка канонического порядка в
  полной validation; закрывает принятие переставленных rehashed records.
- `src/cache/point_stream_sort.h/.cpp`: приватные числовые счётчики успешных
  logical transfers/bytes/seeks для benchmark observations через friend adapter.
  Порядок сортировки, buffers, budgets и scratch framing не изменены.
- `tests/cache_reader_tests.cpp`, `cache_reliability_tests.cpp`,
  `cache_crash_probe.cpp`, tests CMake: две содержательные регрессии и четыре
  process-exit cases.
- `tools/cache_benchmark`, root CMake: opt-in headless измеритель и ограниченный
  source-order adapter, без второго production backend. Зависимость только от
  уже используемого Qt Core и системной Windows Psapi, новых сторонних пакетов нет.

Binary offsets, encoding/checksums, Writer output и ownership contract не менялись.

## H. Build and Test Results

CMD-инициализация (как в исходном запросе, только для текущего процесса):

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
chcp 65001 >nul
set "VCPKG_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg"
set "QT_ROOT=C:\Qt\6.12.0\msvc2022_64"
set "PATH=C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;%QT_ROOT%\bin;%PATH%"
set "VCPKG_MAX_CONCURRENCY=4"
cd /d C:\dev\CADContour2D
cmake --preset windows-debug -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-debug --target cadcontour_cache cache_tests cache_benchmark project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
cmake --build --preset windows-release --target cadcontour_cache cache_tests cache_benchmark project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
ctest --preset windows-debug -L cache_integration -V
ctest --preset windows-release -L cache_integration -V
```

| Проверка | Debug | Release |
|---|---|---|
| Configure и перечисленные build targets | PASS | PASS |
| Production CTest, включая 166 cache tests и 6 новых regression cases | 298/298 PASS, 61.54 s | 298/298 PASS, 37.01 s |
| Отдельный cache integration | 1/1 PASS, 86.35 s | 1/1 PASS, 12.86 s |
| Явная большая HDD матрица | Не запускалась | 7/7 PASS |

До итоговой матрицы отдельно выполнен Release `cache_tests
--gtest_filter=-CacheReaderIntegration.*`: 166/166 PASS. В итоговую production
suite входят те же cache fault/cancel/cleanup/crash/Morton tests. Heights 3/4/5
проверены небольшими структурными моделями; минимальный layout height=6 отвергнут
checked arithmetic/qint64. Файлы высоты 3–5 не создавались.

Новый production/test/tool C++ собран с действующим `/W4`, без предупреждений.
Configure сохранил известные необязательные Qt6TaskTree/WrapVulkanHeaders
diagnostics. `--help` переведён на `parse()/helpText()`:
`process()` при pipes на Windows показывал native message box; повторный запуск
вывел справку в stdout и завершился с кодом 0 без окна.

`git diff --check` и просмотр итогового diff выполнены; случайных правок вне
cache/tests/benchmark и соответствующей документации нет. Исходная working copy
была чистой, чужие изменения не затронуты. Большие generated datasets в Git не входят.

Большие команды, реально выполненные в Release (каждому `--report` нужен новый путь):

```bat
build\windows-release\cache_benchmark.exe --parent F:\CADContour2D-benchmark --report docs\benchmarks\cache05\multi-8m-42m.json --points 42000000 --memory-mib 8 --distribution multi --repeats 3
build\windows-release\cache_benchmark.exe --parent F:\CADContour2D-benchmark --report docs\benchmarks\cache05\multi-128m-42m.json --points 42000000 --memory-mib 128 --distribution multi --repeats 3
```

Затем в `.cmd` последовательно выполнена малая матрица:

```bat
for %%D in (horizontal vertical clustered sparse repeated) do (
  build\windows-release\cache_benchmark.exe --parent F:\CADContour2D-benchmark --report docs\benchmarks\cache05\%%D-8m-1200k.json --points 1200000 --memory-mib 8 --distribution %%D --repeats 3
  if errorlevel 1 exit /b 1
)
```

JSON сохраняет параметры, все phase intervals, successful logical I/O counters,
process samples, каждый query pass и отдельную validation. Query timings сняты
без параллельных сборок/CTest; остальные процессы Windows не изолировались.
Отдельная стоимость order-check при 128 MiB: median scan=12.347 s,
scan+order=22.645 s, добавка 10.298 s (16.0% validation); это тот же O(N) CPU pass,
а не зависимость Reader от writer budget.

Не выполнялись гарантированный cold-cache, physical HDD tracing, power-loss,
реальное исчерпание диска/RAM, real scanner datasets, GUI manual acceptance и
полная regression отдельного Synthetic Point Cloud Generator. Эти компоненты/
гарантии не менялись; production application/QML tests выполнены. Новый source
stream не является полноценным ASC parser задачи 04.

## I. Documentation and Format v1

Обновлены point-cache-format, architecture §§12/14/45, algorithms §39,
implementation-plan, README; cache-reader-check помечен как исторический отчёт 4/5
со ссылкой на эту проверку. Формат v1 **зафиксирован**, byte layout и writer order
сохранены. Полная validation теперь действительно проверяет каноничность помимо
структурной пригодности/ID coverage. Несовместимое дальнейшее изменение потребует
нового version identifier; миграции несуществующих опубликованных версий не созданы.

Для задачи 04 готовы streaming Measurement API с finite X/Y/Z и точным binary64,
metadata/Z flag, bounded budgets, PendingCache с явным Accept/Discard, быстрый
Reader и full validation. Application должна сохранить applicability gate и
внешнего владельца immutable dataset; успешный `finish()` сам по себе не означает
принятие проекта. Consumer должен учитывать допустимый prefix перед failure.
Parser, GUI предупреждения Z, project serialization/recovery и OperationRunner
integration не входят в текущую headless задачу.

Все семь успешных benchmark runs удалили свои большие данные штатным RAII cleanup;
в Git сохранены только небольшие JSON reports. От остановленного pilot (до
исправления измерителя размера открытых Windows files) остался собственный каталог
`F:\CADContour2D-benchmark\cache05-xUjGyG`, около 2.12 GB. Автоматическая проверка
отклонила как recursive cleanup, так и удаление перечисленных файлов, ответ
`blocked by policy` без дополнительного объяснения. Удаление не обходилось другим
механизмом. Это операционный остаток текущей проверки, не принятый dataset и не
пользовательские данные. Неизвестный прежний каталог benchmark не изменялся.

## J. Final Verdict

**READY FOR REVIEW.** Reliability, bounded construction/validation, сохранение ID
и битов X/Y, spatial selectivity, стоимость Morton относительно source order и
ресурсы подтверждены описанными проверками. Оснований менять layout/index strategy
нет; v1 зафиксирован. Нормативы производительности для произвольного оборудования
не вводятся. Заключительное независимое ревью и окончательное принятие пользователем
остаются отдельным заданием; **ACCEPTED не установлен**.

## Исправление R1/P2 после независимого ревью

На исходном `dd3be9f` подтверждён поздний resource preflight в benchmark harness:
полный oracle выполнялся до checked layout и проверки split disk budget.
Ограниченный запуск `--points 18446744073709551615 --memory-mib 8 --repeats 2`
за 1.2 s не завершился, потребил 1.14 s CPU и не создал данных/отчёта; собственный
диагностический процесс остановлен. Это дефект обработки невозможных параметров,
не production cache и не причина пересматривать семь успешных измерений выше.

Теперь до oracle создаётся и уничтожается пустой Writer с теми же paths/limits:
он проверяет реальные budgets и предоставляет фактический sorter run capacity,
без копирования ресурсных формул. Общий `preflight()` проверяет checked page counts,
layout/qint64, размеры generations/descriptors и split disk budget. До отказа
возможны лишь bounded buffers и пустые scratch files/4-KiB staging header, без
измерений и record I/O. Ранний отказ очищает собственные каталоги; сохранение
диагностики неполного build после его начала осталось прежним.

После probe выполняется oracle; затем, как прежде, создаются Observer/Monitor,
стартует build timer и создаётся новый измеряемый Writer. Проверки повторяются
внутри прежнего timed interval, JSON success schema, query oracle и resource
monitoring не менялись. Probe не перекрывается с измеряемыми buffers/handles,
не входит в phase samples/buildMs. Lifetime PeakWorkingSet по своему прежнему
определению остаётся максимумом всего процесса, включая подготовку/probe; это
не отдельная метрика фазы build и не утверждение cold-start условий.

Добавлен opt-in CTest label `cache_benchmark`: **7/7 PASS в Debug и Release**.
Проверены UINT64_MAX, самостоятельный qint64 layout failure без uint64 overflow,
overflow двух scratch generations, недостаточный disk budget, нулевые count и
memory, успешные 12000 records с обеими организациями, oracle/full validation,
timings/resources и сравнением JSON schema с прежним report. Negative cases
проверяют exit=1, конкретную диагностику, JSON FAIL без build/query results и
пустые data/scratch roots, а не только ограничение времени.

```bat
cmake --build --preset windows-debug --target cache_benchmark_tests --parallel 4
cmake --build --preset windows-release --target cache_benchmark_tests --parallel 4
ctest --preset windows-debug -L cache_benchmark -V
ctest --preset windows-release -L cache_benchmark -V
build\windows-debug\cache_tests.exe --gtest_filter=-CacheReaderIntegration.*
build\windows-release\cache_tests.exe --gtest_filter=-CacheReaderIntegration.*
```

Обе сборки с `/W4` без новых предупреждений; существующие cache tests —
**166/166 PASS** в каждой конфигурации. `git diff --check` чистый. Гигабайтная
матрица не повторялась, её JSON не менялись. Все новые test datasets/каталоги
удалены, прежние заблокированные каталоги на F: и в Temp не затрагивались.
Production API, ресурсные контракты и зафиксированный v1 сохранены.
**FIXED — READY FOR RE-REVIEW** по замечанию R1; ACCEPTED не установлен.
