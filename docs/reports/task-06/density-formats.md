# 06.0B — экспериментальное сравнение Density Map storage

2026-10-27. **READY WITH LIMITATIONS**. Рекомендация: **D (count-tile/zlib)**.
Изолированное исследование D и E (TIFF/BigTIFF).
Production форматы, ProjectState, Save/Open/Recovery, GUI и PNG не изменены.
06.0C не выполнялась. Рекомендация требует отдельного согласования.

## Baseline и воспроизводимость

- Branch **main**, HEAD **cc199618ff6e07a8f00348e69113896a4d3f2235** (06.0A).
  Исходный `git status --short` пустой; baseline commit и его изменения изучены.
- Прочитаны AGENTS, README, инструменты/тесты/отчёт 06.0A, density/coordinate/
  memory/error/testing contracts requirements/architecture/algorithms и пункт 06
  implementation-plan. Исследование не утверждает замену обязательной PNG.
- Windows 10 22H2 x64, MSVC **19.51.36260.0**, toolset **14.51.36231**,
  SDK **10.0.26100.0**, C++20, Qt **6.12.0**, CMake **3.30.5**, Ninja **1.12.1**.
- zlib headers/runtime **1.3.2**, vcpkg **1.3.2#1**; libtiff **4.7.2**,
  только `core,zip`, без libdeflate; GoogleTest **1.17.0#3**.
  Версии подтверждены configure, установленными headers, port recipes и runtime.
- D: **Hitachi HDS721050CLA362, SATA HDD, NTFS**; model/media проверены через
  Get-PhysicalDisk. Physical RAM **17 135 476 736 bytes**. Preflight выполнен
  перед каждой серией и trial; в агрегированном JSON сохранён серийный preflight,
  в open row — оценка плана trial. Budgets: RAM 512 MiB, temp 128 MiB, output 8192 MiB.
- Изменения ещё не закоммичены во время замеров; environment хранит build/runtime
  baseline, Git status, executable SHA-256, UTC start, CPU/OS/RAM/filesystem.
  Основной Release exe SHA-256:
  `fcf8fe016f601c819c6bc63c3dc21e92510bcbaf2f1d3fe1f7c6c10ff949f14b`.
- Production manifest и toolchain не менялись. Зависимости эксперимента находятся
  в `build/density-06b-deps`. Bundled vcpkg дважды пытался скачивать registry HEAD;
  fetch остановлены. Установка выполнена из неизменённых port recipes **того же**
  pinned baseline, экспортированных из локального registry, без смены версий.

## Реализация и API

Подробности форматов, byte layout, измерений и проверенные команды —
[README эксперимента](../../../tools/density_benchmark/formats/README.md).

| Файл в tools/density_benchmark/formats | Назначение |
|---|---|
| CMakeLists.txt, vcpkg.json | Отдельная opt-in конфигурация, Qt Core + zlib + libtiff |
| formats.h, format_support.cpp | Узкие общие helpers, FileIo и ресурсная оценка |
| count_container.cpp | D: immutable sparse index, Deflate, checksums, writer/reader |
| tiff_adapter.cpp | E: libtiff client I/O, writer, проверка metadata/striles и строгий reader |
| benchmark.cpp | Прямые callbacks в прежний measure, отдельные candidate workers |
| tests.cpp | Малые round-trip, independent controls, ROI, corruption, CLI tests |
| run-matrix.ps1 | Только порядок/параметры запусков прежнего измерительного слоя |
| audit-results.ps1 | Независимая проверка JSON/CSV, paired plans/digests и SHA-256 evidence |

В общем стенде предел side 256 расширен до 1024 с прежним schema и defaults;
добавлены candidate CSV column, отдельные statistical groups и nearest-rank p95.
Тесты 06.0A сохранены, добавлены две regression checks. Второй инфраструктуры
измерения, registry форматов, production dependency или raster builder нет.

D: LE uint32/uint64, independently compressed partial tiles, 96-byte header,
40-byte sorted index entries, header/index/decoded CRC32; omission всех нулевых
тайлов, binary search, RAM O(сохранённые тайлы). Header/index bounds и арифметика
проверяются до allocations. zlib `compressBound/compress2/uncompress2/crc32_z`.

E: BigTIFF `w8l`, uint32 dimensions, uint64 offsets, uint32 tile numbering,
unsigned 32/64-bit single samples, predictor 1, Deflate level 6/zlib subcodec,
zero padding крайних тайлов. `TIFFClientOpenExt` с per-handle diagnostics и
single/cumulated allocation limits, `TIFFWriteEncodedTile/WriteDirectory/Flush/Close`.
Классический TIFF дополнительно проверен, основные замеры — BigTIFF.

Обнаружено воспроизводимое ограничение **native `TIFFReadEncodedTile`**: при
byteCount, укороченном на один байт Deflate trailer, полный decoded buffer может
вернуться без ошибки. До исправления адаптера regression test FAIL: exception
не выброшено. Причина в `tif_zip.c::ZIPDecode`: завершение по заполнению output
не обязательно требует Z_STREAM_END. E теперь использует `TIFFReadRawTile` +
`uncompress2` и проверяет consumed length/output length/checksum. Тест PASS;
это узкий profile, а не универсальный TIFF decoder. UInt64 native codec отдельно
проверен на UINT64_MAX; BigTIFF сам по себе такого свойства не доказывает.

API сверены с локальными исходниками **4.7.2** и headers **1.3.2**, дополнительно
с официальными [TIFFOpenOptions](https://libtiff.gitlab.io/libtiff/functions/TIFFOpenOptions.html),
[TIFFReadEncodedTile](https://libtiff.gitlab.io/libtiff/functions/TIFFReadEncodedTile.html),
[zlib manual](https://zlib.net/manual.html). Веб-версия zlib manual при проверке
описывала 1.3.1, поэтому доказательство доступности API — установленные 1.3.2 headers.

## Корректность и пределы

| Проверка | Результат |
|---|---|
| 06.0A Debug / Release, включая новые checks | **22/22 / 22/22 PASS** |
| 06.0B Debug / Release | **13/13 / 13/13 PASS** |
| 12 сценариев, точные values/shapes/canonical fingerprints | D PASS; E PASS для 11, huge-sparse UNSUPPORTED |
| uint32 maximum, uint64 >32 bits и UINT64_MAX | PASS, без float/lossy conversion |
| Tile 256/512/1024, partial edges, explicit duplicate/random requests | PASS |
| Независимые literal/formula/entropy controls, ROI crop и Y/X order | PASS |
| D: 14 header/index/offset/length/payload/truncation mutations | PASS: отказ |
| E: 12 header/tag/offset/length/payload/truncation mutations + short trailer | PASS: отказ строгого адаптера |
| Zero vs invalid offset/payload; sparse omission | PASS |
| Resource budget, arithmetic overflow, existing path/overwrite refusal | PASS |
| Cancellation и сохранность соседнего user.data | PASS; partial не читается как успешный dataset |
| Console --help без нового GUI окна | PASS в CLI test |

Первый Debug large-tile test превысил 90 s из-за многократного повторения общих
fingerprint/ROI проходов. В нём сохранена однократная полная проверка каждого
значения каждого размера; общие fingerprints/ROI независимо проверяются другими
cases. Финальный Debug — PASS за 45.54 s целиком; Release — PASS за 5.61 s.
Первоначальная неоднозначная QByteArray overload исправлена. Одно ожидающее окно
Qt --help мешало relink; собственный help-процесс закрыт, CLI исправлен.

Для TIFF отсутствующий strile — ошибка, **не нулевая область**. Dense arrays и
uint32 tile IDs не представляют huge-sparse (даже при side 1024). D хранит его
четыре source corner tiles или меньше, если corner полностью нулевой. Полный
логический обход ~10^18 cells не запускался: ReadPlan Full/ROI здесь пропускает
zero complement. Это не результат скорости полного Rough scan.

TIFF metadata/IFD не имеют общего checksum. Структурная проверка ловит выбранные
повреждения, но не любое превращение допустимой metadata в другую допустимую.
D CRC32 тоже не является криптографической аутентификацией. Fuzzing, hostile-file
security audit и доказательство обнаружения всех возможных повреждений не проводились.
Aggregate sums стенда — checked uint64: непредставимая общая сумма означает отказ,
а не wraparound. Хранение/чтение отдельного UINT64_MAX подтверждено вне такого
агрегирования; этот предел totals не является дополнительным ограничением samples.

## Методика

Общие inputs — прежний deterministic `generate`, seed **6001**; малые серии
47×45, ordinary sizes **1733×1731**, **3465×3463**, **6929×6927**.
Huge-sparse всегда исходный **1 000 000 003 × 1 000 000 001**. Side 256/512/1024
сравниваются на **одинаковых dimensions**, count type основного сравнения uint32;
u64 — отдельная capability/control серия. Код генератора и координатные решения
06.0A не менялись.

Основные серии Release x64, отдельный процесс на dataset/candidate/repetition,
пять повторений (первый + четыре subsequent), малая серия — два. Порядок D,E / E,D
чередуется. Каждый trial пишет новый файл, затем читает его; subsequent — новые
trials, не тот же physical filename. Cold cache не заявляется; verification и
предыдущие фазы прогревают данные. Debug/сборки не запускались параллельно матрице.

Write включает генерацию, conversion, compression, index/metadata/finalize и
QFile flush/close; durability до platter при power loss не проверяется. Source
generation измеряется отдельно. Reads включают unpack в общий Tile<uint64> и
sum/cell/tile sink. Verification выполняется вне operation timer/process I/O:
полная проверка каждой возвращённой величины/формы/адреса и canonical digest
write-round-trip. Это стоимость streaming adapter, не чистого memcpy/codec.

Full обычных datasets обходит все логические tiles, включая zero; physical
обходит только payloads. Small ROI возвращает до четырёх whole tiles, large ROI —
центральную половину каждой оси; random — 128 deterministic tile requests с
повторами. Для D/E одного side ROI в cells одинаковы. Large ROI одинаков и между
side; прежний small ReadPlan имеет bounds 0..side+1, поэтому сравнение его времени
между side не означает одинаковую cell area. Whole-tile cells записаны в rows.
E хранит zero tiles, D их omits; различие
составляет часть измеряемого поведения.

FileIo — successful bytes/attempted calls unbuffered QFile обоих adapters, включая
TIFF client callbacks. QIODevice::Unbuffered отключает Qt buffering, **не OS cache**.
Process I/O — GetProcessIoCounters logical transfers,
не physical disk. RAM peak — PeakWorkingSetSize за жизнь worker, может включать
предыдущие operation/verification; private bytes/working-set snapshots не peaks.
Allocation trace, scratch high-water mark, physical I/O — **не измерены**.
Известны buffer/index sizes и conservative plan, но точное число дополнительных
allocations неизвестно. p95 — nearest rank; при n=4 равно max, при n=2 также max.
Оба reader материализуют общий Tile<uint64> и отдельные packed/compressed buffers
на запрос; D zero tile требует только output buffer. Это ограничение текущих
адаптеров, влияющее на tile-size/RAM/throughput; reusable buffers и native uint32
processing в production здесь не измерялись. Из сравнения нельзя вывести
теоретическую максимальную скорость Deflate или любого TIFF implementation.

Limits кооперативные, не OS hard cap. Проверены RAM/disk перед нагрузкой;
ненужных больших копий/full-raster allocations нет. Dataset files остаются только
в новом ignored `build/density-06b-matrix`, прежний 06.0A evidence не менялся.

После оценки фактической цены source generation/fingerprint матрица сокращена:
необязательные 48m entropy и 300m uniform не доводились до benchmark результата.
[Первоначальный план](../../benchmarks/density06b/matrix-planned.json) сохранён;
[выполненный набор](../../benchmarks/density06b/matrix.json) содержит 12 серий.
Исходный launcher остановлен после завершения всех 12m series; короткий старт
48m entropy исключён. [Запись остановки](../../benchmarks/density06b/curtailment.json).
48m sparse запущен отдельной той же CLI командой/Release binary. Это сознательное
ограничение стоимости эксперимента, не failure какого-либо формата. Текущий
`run-matrix.ps1` воспроизводит выбранный сокращённый набор.

## Измерения: 3 млн ячеек

Серия 1733×1731, side 256, seed 6001, пять trials. Времена — p50 четырёх
subsequent trials, ms; размеры — точные bytes. Write включает генерацию.
Исходные [JSON](../../benchmarks/density06b/raw/3m-all-t256.json) и
[CSV](../../benchmarks/density06b/raw/3m-all-t256.csv).

| Dataset (uint32) | D bytes | E bytes | D write | E write | D full | E full |
|---|---:|---:|---:|---:|---:|---:|
| empty | 96 | 14 474 | 1206.03 | 1323.66 | 6.83 | 38.59 |
| uniform | 14 903 | 21 358 | 440.48 | 462.39 | 31.45 | 31.66 |
| dense | 14 952 | 21 406 | 501.27 | 501.22 | 32.68 | 30.28 |
| sparse | 194 880 | 196 144 | 643.64 | 644.24 | 35.35 | 37.42 |
| horizontal | 28 067 | 27 782 | 873.16 | 807.41 | 39.52 | 39.68 |
| vertical | 31 136 | 33 040 | 983.36 | 923.43 | 28.55 | 24.74 |
| clustered | 417 | 14 478 | 1017.96 | 1016.06 | 6.97 | 36.79 |
| overlap | 64 183 | 66 494 | 1122.55 | 1238.39 | 29.60 | 26.66 |
| u32 | 14 860 | 14 522 | 1128.64 | 1163.34 | 38.86 | 37.16 |
| entropy | 11 947 965 | 11 962 264 | 1604.01 | 1630.57 | 37.13 | 40.41 |

Uint64 control отдельно: D/E **26 529 / 27 018 bytes**, write p50 **957.63 /
984.43 ms**, full p50 **61.48 / 55.70 ms**. Это не часть uint32 ranking.
Huge-sparse D **4570 bytes**, full source-corner p50 **1.08 ms**; E UNSUPPORTED.

При 49 physical tiles metadata/index D **2056 bytes**, E около **900 bytes**.
D empty — 96 bytes / 0 tiles, clustered — 136 bytes metadata / 1 tile. Для
clustered logical full D читает **281 bytes / 1 call**, E **13 577 / 49 calls**;
для empty — **0 / 0** против **13 573 / 49**. Эти calls не являются HDD seeks.

PeakWorkingSetSize этой серии около **10.9–11.1 MiB** у обоих. Open p50 порядка
**0.14–0.39 ms**; различия слишком малы для устойчивого HDD latency вывода.
Random-128 p50 на entropy D/E **99.53 / 97.35 ms**, small ROI **3.83 / 3.96 ms**.
Full entropy p95 **41.44 / 94.96 ms** против p50 37.13/40.41: один tail trial
существенно меняет вывод о скорости. Sparse random p50 **92.88 / 85.64 ms** —
победа D по всем access patterns не наблюдается.

Compression ratio определяется как logical cells × sample bytes / file bytes,
включая metadata. Для entropy он близок к 1 у обоих; для пустых областей размер
D сокращается прежде всего за счёт omission, а не преимущества Deflate codec.
При uniform разница размера включает обязательный zero padding TIFF на краях.

## Измерения: размер тайла на 12 млн entropy cells

3465×3463 = **11 999 295 cells**, uint32, пять trials. Все времена — ms;
full указан как **p50 / p95**, остальные timings — p50. RAM — максимальный
process-lifetime working set во всех rows кандидата. Raw:
[256 JSON](../../benchmarks/density06b/raw/12m-entropy-t256.json),
[512 JSON](../../benchmarks/density06b/raw/12m-entropy-t512.json),
[1024 JSON](../../benchmarks/density06b/raw/12m-entropy-t1024.json), CSV рядом.

| Side | Candidate | File bytes | Metadata bytes | Write | Full p50/p95 | Large ROI | Random-128 | Peak MiB |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 256 | D | 47 902 250 | 7936 | 5591.73 | 107.84 / 112.12 | 37.39 | 63.27 | 10.99 |
| 256 | E | 47 933 780 | 2665 | 5578.48 | 113.53 / 115.04 | 34.91 | 62.63 | 11.07 |
| 512 | D | 47 971 097 | 2056 | 5748.04 | 155.33 / 239.82 | 105.33 | 430.26 | 15.91 |
| 512 | E | 48 013 810 | 900 | 5680.20 | 165.55 / 180.89 | 82.38 | 415.87 | 16.17 |
| 1024 | D | 48 004 942 | 736 | 5731.66 | 127.80 / 133.64 | 102.66 | 1051.59 | 33.90 |
| 1024 | E | 48 095 064 | 505 | 6076.36 | 172.51 / 305.41 | 97.39 | 1461.11 | 34.05 |

Compression ratio около **1** во всех шести случаях. Экономия metadata от больших
тайлов измерима, но составляет лишь несколько KiB относительно 48 MB payload.
Larger tiles существенно увеличивают decoded bytes и RAM выборочного чтения.
Для side 1024 raw TIFF tiles содержат **16 777 216** samples на logical
11 999 295 cells; padding приходится декодировать. У D краевые samples упакованы
без padding. Random-128 возвращает **95 994 360** logical cells у обоих, но E
распаковывает **134 217 728** samples. Это физически значимое различие реализации
форматов, а не потеря/добавление counts. Его вклад в latency — инженерная
интерпретация; hardware profiling не проводился.

Эти результаты поддерживают **256 как исходный экспериментальный side** для
дальнейшей проверки count storage/ROI на этом workload, но не закрепляют его
production default. 1024 не дал общего выигрыша. Большая ROI одинакова в cells,
но возвращаемое число whole tiles/cells и padding зависят от side. Random-128 —
нагрузочный access plan, а не одинаковый объём декодирования между side.

## Измерения: другие 12 млн сценарии

p50 четырёх subsequent trials; в каждой паре сначала D, затем E. Raw JSON/CSV
в [каталоге evidence](../../benchmarks/density06b/README.md).

| Dataset | Side | D/E file bytes | D/E write p50, ms | D/E full p50, ms | D/E random-128 p50, ms |
|---|---:|---:|---:|---:|---:|
| sparse | 256 | 781282 / 783170 | 2365.04 / 2334.20 | 107.65 / 106.89 | 69.51 / 67.80 |
| sparse | 512 | 765976 / 773734 | 2531.65 / 2469.36 | 180.03 / 132.16 | 370.08 / 345.38 |
| sparse | 1024 | 763087 / 792612 | 2378.08 / 2466.75 | 122.62 / 156.42 | 1031.43 / 1294.59 |
| uniform | 256 | 59344 / 71730 | 1927.95 / 1928.24 | 105.82 / 96.70 | 62.27 / 58.17 |
| uniform | 512 | 49831 / 66440 | 1893.18 / 1931.12 | 122.11 / 123.78 | 325.73 / 321.08 |
| uniform | 1024 | 47647 / 83002 | 1790.69 / 1776.22 | 119.50 / 145.87 | 954.46 / 1174.04 |

Sparse здесь имеет ненулевые cells почти в каждом tile: большого omission-выигрыша
нет. На 512 E быстрее по full-read median, на 1024 — D; это противоречит выводу
об универсальном победителе по throughput. Для uniform файл D меньше, но обе
записи занимают примерно 1.8–1.9 s с генерацией. Размер TIFF metadata меньше,
однако обязательное padding/явные zero tiles могут перекрывать эту экономию.

## Измерения: масштаб 48 млн cells

**6929×6927 = 47 997 183 cells**, sparse, uint32, side 512, пять trials;
196 logical/physical tiles у обоих. Raw [JSON](../../benchmarks/density06b/raw/48m-sparse-t512.json)
и [CSV](../../benchmarks/density06b/raw/48m-sparse-t512.csv).

| Метрика | D | E |
|---|---:|---:|
| File bytes | 3 064 205 | 3 086 042 |
| Metadata/index bytes | 7936 | 2664 |
| Write p50 / p95, ms | 10090.34 / 10525.33 | 9644.13 / 10247.76 |
| Full p50 / p95, ms | 526.14 / 568.92 | 548.46 / 587.11 |
| Open p50, ms | 0.55 | 0.93 |
| Small ROI p50, ms | 12.04 | 11.28 |
| Large ROI p50, ms | 181.29 | 176.08 |
| Random-128 p50 / p95, ms | 354.59 / 374.04 | 343.83 / 352.07 |
| Maximum observed process peak, MiB | 15.94 | 16.09 |
| Full-read FileIo bytes / calls, trial 1 | 3 056 269 / 196 | 3 083 378 / 196 |

Compression ratio (191 988 732 logical raw bytes / file bytes) примерно **62×**.
Memory остаётся близкой к 12m/512, а не растёт пропорционально raster area.
Открытие загружает metadata/index без полного raster. В указанном full-read trial
process logical counters совпали с FileIo bytes/operations у обоих; физическое HDD
I/O отсюда не выводится. Source generation p50 D/E **4028.98 / 3963.28 ms**.
Первые write trials **9830.77 / 9817.12 ms**, первые full reads **539.58 / 631.53 ms**;
они сохранены отдельно и не считаются cold-cache.

Preflight последней серии: available RAM **4 454 703 104**, disk **299 093 237 760**,
plan RAM **58 745 344**, retained output **2 148 387 208**, scratch **0** bytes.
План консервативно учитывает несжимаемые streams всех trials, а не будущий ratio 62×.
Максимальный process peak всей выбранной матрицы **35 704 832 bytes (34.05 MiB)**.

## Evidence audit и готовность

**READY WITH LIMITATIONS.** Реализованы оба адаптера, общие операции проверены,
сопоставимое измерение завершено, рекомендация сформулирована. Это не приёмка
production формата и не переход к 06.0C.

Независимый [PowerShell audit](../../../tools/density_benchmark/formats/audit-results.ps1)
успешно проверил **12 серий, 2363 ok rows и 7 ожидаемых E/huge-sparse unsupported**:
JSON/CSV row identity/числовое время, correctness flags, paired ReadPlan/coverage/
sums и единый fingerprint между кандидатами/повторами. Проверены SHA-256 и bytes
**24 raw JSON/CSV**, всего **10 343 470 bytes**. [Результат и manifest хэшей](../../benchmarks/density06b/audit.json).
Сами datasets остались вне Git. JUnit 22/22 и 13/13 в обеих конфигурациях
сохранён в [evidence](../../benchmarks/density06b/README.md).

## Фактически выполненные команды

Из корня, локальный `build/task02-env.cmd` — прежний vcvars64/Qt launcher:

```bat
build\task02-env.cmd cmake --build --preset windows-debug --target density_benchmark density_benchmark_tests --parallel 4
build\task02-env.cmd cmake --build --preset windows-release --target density_benchmark density_benchmark_tests --parallel 4
build\task02-env.cmd ctest --preset windows-debug -L density_experiment --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density-06b-06a-debug.xml
build\task02-env.cmd ctest --preset windows-release -L density_experiment --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density-06b-06a-release.xml
build\task02-env.cmd cmake --build build/density-06b-debug --parallel 4
build\task02-env.cmd cmake --build build/density-06b-release --parallel 4
build\task02-env.cmd ctest --test-dir build/density-06b-debug -L density_formats --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density-06b-debug-tests-final.xml
build\task02-env.cmd ctest --test-dir build/density-06b-release -L density_formats --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density-06b-release-tests-final.xml
build\task02-env.cmd powershell -NoProfile -ExecutionPolicy Bypass -File tools/density_benchmark/formats/run-matrix.ps1 -Executable build/density-06b-release/density_formats_benchmark.exe -OutputDirectory build/density-06b-matrix -StorageLabel "D: Hitachi HDS721050CLA362 SATA HDD"
powershell -NoProfile -ExecutionPolicy Bypass -File tools/density_benchmark/formats/audit-results.ps1 -InputDirectory build/density-06b-matrix -EvidenceDirectory docs/benchmarks/density06b
```

Точные configure/install commands — в README инструмента. Production CTest,
GUI/qmllint не повторялись: production код, CMake и dependencies не затронуты.
После завершения собственные JUnit/диагностические logs из build root перенесены
без изменения содержимого в новый `build/density-06b-evidence`; команды выше
сохраняют фактически использованные при запуске paths. Промежуточные таблицы
удалены после переноса чисел в отчёт; raw evidence сохранён.

## Стоимость сопровождения: инженерная оценка

D требует собственного контракта index/version/checksums/validation и поддержки
corruption tests. Узкий контейнер небольшой, использует одну зависимость zlib,
имеет естественную sparse omission и прямой bounded selective access. Для
production всё ещё нужны identity/provenance, безопасная публикация immutable
dataset через существующий persistence, изучение paged index при реально большом
числе сохранённых tiles и политика ресурсов. Эти механизмы здесь не создавались.

E даёт стандартный внешний контейнер и диагностические инструменты, готовую запись
IFD/strile arrays и проверенный lossless integer codec. Цена — libtiff и её build/
distribution/security maintenance, dense logical tile arrays/uint32 addressing,
узкий принимаемый profile и дополнительная строгая Deflate validation. Sparse
convention с absent striles, внешняя integrity/identity либо sharding/subIFDs для
огромных sparse dimensions потребовали бы отдельного протокола/адаптера и тестов;
это не бесплатная возможность BigTIFF и не реализованное решение эксперимента.

## Рекомендация

**D — основной рекомендуемый вариант для последующего рассмотрения count storage.**
Основание — точные uint32/uint64, selective access с ограниченными tile buffers,
естественная sparse omission, checksummed index и одна codec dependency. На
обычных заполненных maps D/E близки, местами E быстрее; это не победа D по всем
измерениям. Но E не даёт устойчивого преимущества, компенсирующего дополнительную
зависимость, dense logical index/uint32 tile numbering и строгий codec/profile
adapter. Малый собственный D-контейнер в данном эксперименте проще этой комбинации.

TIFF остаётся разумной альтернативой при приоритете внешней interoperability и
заранее ограниченных raster dimensions/sparsity. Другая конфигурация predictors/
codec может изменить числа; в этом эксперименте она не подтверждена. Standard
format сам по себе не гарантирует всю требуемую integrity policy.

Для будущего Rough оба адаптера дают точные counts, из которых можно построить
выбранное PNG encoding; это **пригодность хранения**, не реализация или подтверждение
Rough algorithm. Huge-sparse подтверждает только bounded storage/selective reads,
а не возможность полного PNG/Rough прохода по 10^18 cells. Такой запрос всё ещё
нуждается в ресурсном отказе либо отдельно согласованном workflow.

Интеграция любого варианта потребует временной записи/validation и публикации
immutable dataset через существующий persistence transaction, source/build identity,
cell/origin/units metadata и ссылок версии. D дополнительно требует поддержки
собственного version/validation contract; E — принимаемого TIFF profile и внешней
integrity/identity policy. Эта оценка архитектурной стоимости не является проверенной
реализацией Save/Open/Recovery. **Production format не выбран автоматически.**

Экспериментально подтверждены только свойства/ограничения adapters, тесты и числа
в таблицах/raw. Оценка стоимости сопровождения, перспективы интеграции и причины
части latency-различий — инженерные оценки. Рекомендация D и исходный side 256
не закрывают Open decisions и требуют отдельного согласования перед интеграцией.

## Ограничения применимости

Все maps синтетические; representative real scans, будущий Rough algorithm и
production raster/PNG mapping не проверены. Нет рекомендаций о rough/precise
качестве. One HDD/OS/toolchain, unmanaged cache/antivirus/background load,
малое число повторов; субмиллисекундные open/ROI timings ненадёжны для выбора.
Не проверены file offsets >4 GiB на фактическом dataset, network storage,
power-loss durability/OS disk-full/OOM injection, paged D index, big-endian files и
другие TIFF codecs/predictors. Resource refusals проверены через budgets, а не заполнением
пользовательского диска. Проверка counts не утверждает production coordinate policy.
