# Headless Density Builder 06.3A — evidence

Полный production pipeline: штатный Point Cache Writer (подготовка), затем
`DensityBuilder` → один `CacheReader::scanAll` → C1 → D Writer → PendingDataset.
После измеряемого build штатный D Reader независимо сверяет все physical tile
keys/shapes/counts и implicit zeros с test-only spatial oracle. Проверка суммы
не заменяет эту сверку. UUID/file CRC могут отличаться между builds.

Воспроизводимый harness и определения метрик:
[tools/density_builder_benchmark](../../../tools/density_builder_benchmark/README.md).
Исследования D/E и C1/C2 не повторяются. Этот benchmark не является CTest.

## Артефакты

- `release.json`: исходные строки полного pipeline, три representative repeats,
  отдельные samples, exact spatial verification и paired comparisons.
- `environment.json`: фактическая среда, физический носитель, hashes compiled inputs
  и executable; дополняет environment внутри `release.json`.
- `verification.json`: компактный протокол реально выполненных проверок и hashes
  полных локальных logs/XML; исходные большие журналы остаются в `build/`.
- `tests/`: compressed финальные JUnit Debug/Release и одинаковый список 783
  headless tests; первоначальный compile failure, pre-CTest shell failure и DLL inventory.

Финальные Debug/Release: **782 PASS / 0 FAIL / 1 SKIP** из 783 в каждой конфигурации,
включая builder **84/84**, DensityStorage **115/115**, RasterMapping **14/14**.
SKIP — symlink privilege 1314. Case-sensitive discovery: **1170/1170 уникальных**
entries; первоначальное наблюдение PowerShell о `nan`/`NaN` было ложным из-за
группировки без учёта регистра. Его исправление сохранено в verification.json.

## Профили и границы измерений

Один миллион measurements на профиль; tile 256, construction group 512, cell 1 мм,
event capacity 65536. Representative irregular64 — три измеренных повтора без
исключения первого. Отдельный sample irregular64/capacity1024 использует тот же
source. Compact повторяет одну основную ячейку и содержит две отдельные bound
anchors: всего 3 nonzero cells, 2 tiles и 1 occupied group. Irregular64/256 и fixed-area16/128 дают
менее регулярные counts. Sparse64 увеличивает обе logical axes с 4096 до 524288,
сохраняя occupied groups, physical tiles и ожидаемые counts с переносом последней
группы. Fixed-area16/128 используют одинаковую область 16384×16384.

Source preparation/oracle и последующая D verification вынесены из build time.
`scanMs` включает grouping; grouping включает event sorting/writes. Accumulation
включает group zeroing, event read и counting; tile copy и D ingest измерены отдельно.
Event I/O пересекается с grouping/accumulation. Finish включает D sort/copy/readback.
Эти времена нельзя суммировать; D ingest не является измерением одного Deflate.

## Фактические результаты 2026-11-05

Final Release build, измеренная матрица 9×1 000 000 и `audit.py` — **PASS**.
Functional smoke 9×4096 прошёл до последнего изменения selective source snapshot;
после него полностью повторена матрица 9×1 000 000. Во время измеряемой матрицы параллельные
сборки и тесты не выполнялись. MSVC 19.51.36260.0 (`_MSC_FULL_VER=195136260`), C++20 x64,
Qt 6.12.0, CMake 3.30.5, Ninja 1.12.1, Windows 10.0.19045, i5-7500 @3.40GHz,
RAM 17 135 476 736 bytes. Source/scratch/output — D: NTFS,
Hitachi HDS721050CLA362 SATA HDD. Exact executable/source hashes — в environment.json.

| Profile | Event capacity | Groups / tiles | Wall ms | Process CPU ms | Final D bytes | Physical u32 tile bytes / final D |
|---|---:|---:|---:|---:|---:|---:|
| compact, sample | 65536 | 1 / 2 | 468.45 | 421.88 | 1111 | 471.91 |
| irregular64, median of 3 | 65536 | 64 / 256 | 1704.66 | 1656.25 | 1625129 | 41.29 |
| irregular64, small-chunk sample | 1024 | 64 / 256 | 1951.14 | 1828.13 | 1625129 | 41.29 |
| irregular256, sample | 65536 | 256 / 1024 | 4608.03 | 4046.88 | 2661772 | 100.85 |
| sparse64, sample | 65536 | 64 / 256 | 1725.26 | 1703.13 | 1625129 | 41.29 |
| fixed-area16, sample | 65536 | 16 / 64 | 1051.85 | 1031.25 | 958018 | 17.51 |
| fixed-area128, sample | 65536 | 128 / 512 | 2444.48 | 2375.00 | 2106779 | 63.71 |

Representative wall times — 1803.45 / 1704.66 / 1614.95 ms; mean 1707.69 ms,
sample standard deviation 94.29 ms. CPU median 1656.25 ms, range 1578.13–1750.00 ms.
Три повтора не дают confidence/performance guarantee. Independent verification
медианно 238.59 ms, отдельно от build; для irregular256 sample — 946.40 ms.

Медианы nested phase timers representative профиля (ms): mapping 0.0516,
source open 0.7368, scan 466.0229, grouping 65.5073, event sort 14.4163,
event I/O 35.0416, accumulation 60.1691, tile copy 77.0201,
D ingest 841.7743, finish 242.8682. Внутри D: sort 10.4574,
payload/index copy 2.8839, readback 204.7377. Медианы отдельных фаз получены
независимо и не обязаны соответствовать одному повтору. D ingest заметен в этом
профиле, но не отделяет codec от packing/checksum/I/O.

Каждый source содержит 1 000 000 записей и занимает 24 125 440 bytes:
367 point pages + одна index page + header/footer. Каждый build прочитал source
24 125 440 bytes за 370 read requests/seeks (open+scan), scanAll=1, spatial queries=0.
Event payload ровно 4 000 000 bytes записан и прочитан; events=count totals=1 000 000.
У representative: 104 payload writes, 64 reads, 232 spool opens; D read/write
3 311 314 / 3 270 674 bytes, 1794 / 1283 exact logical requests и 5 opens.
При capacity1024 spool opens выросли до 1349, exact spatial counts и D size совпали.

При росте обеих пустых logical axes в 128 раз остались 64 spools/visited groups,
256 output tiles и те же 1 625 129 final D bytes. Mapping validation отдельно
проверяет более длинные axes; остальные потенциальные причины стоимости, включая
storage open/readback mapping, не объявляются O(N). Fixed-area sample показывает
рост фактической occupancy и D tiles: 16/64 → 128/512 при неизменных axes.

Owned working peak — 13 293 248–13 309 568 bytes; metadata — 128–16 448 bytes.
Maximum scratch/combined reservations — 6 743 308 / 9 405 080 bytes; они включают
весь обязательный event payload и компонентные D peaks как консервативные резервы,
а не замер строго одновременных физических длин файлов. Maximum file reservation
261 (source + 256 occupied spools + 4 D files), handles 5. Representative working
peak 13 297 280, metadata 4160, scratch/combined 5 645 225 / 7 270 354 bytes.
Reader buffer 188 200 bytes, D actual buffer reserve 1 638 573,
codec peak 268 104, sort reserve 18 432 bytes; это разные части accounting,
их нельзя повторно суммировать с общим working peak.

Sampled process-lifetime peak working set вырос до 110 891 008 bytes; в него входят
source writer и oracle с capacity на миллион sparse expected records. Он не является
пиком builder. Все counters по каждому повтору, process logical I/O и policy ceilings
сохранены в release.json. Начальные policies не являются утверждёнными production limits.

Предыдущий PASS-прогон до selective source snapshot fix сохранён в
`build/density063a-evidence/release-before-snapshot.json` (SHA-256
`9cc72108274d4849b48db94a98b22f739d07c12ae82a5b107bb925d3abe3b9b9`)
вместе с прежней environment. Это отдельный успешный исторический прогон, а не FAIL.
Tracked release.json — копия нового `build/density063a-release-final.json`;
его executable SHA-256 и hashes окончательных compiled inputs согласованы в environment.json.

Source I/O объединяет metadata open и единственный scan. Event operations считают
payload read/write calls и успешные spool opens; EOF probes, flush/size/remove
в эту узкую статистику не входят. D operations — logical exact read/write requests,
четыре writer scratch/staged opens и один readback-reader open; readback-open bytes
включены. OS process I/O имеет другую гранулярность и указан отдельно.

Owned working/metadata/disk/files/handles peaks — внутренний accounting/reservations,
не process/OS cap и не физическая NTFS allocation. Windows working-set snapshots
сняты до/после build; PeakWorkingSetSize — high-water mark всего процесса, включая
source preparation/oracle. Per-phase/process-build peaks этим способом не измерены.
CPU — разность GetProcessTimes (kernel+user); logical process I/O — разность
GetProcessIoCounters; физический disk I/O не измерен.

OS cache не контролировался; source только что подготовлен/повторно использован.
Носитель HDD сам по себе не делает прогон cold-HDD. **NOT RUN:** physical cold-HDD,
UNC, physical disk faults, representative real scans и 42M pipeline. Проверенные
synthetic counts не подтверждают качество Rough/Precise методов. Результаты не
задают универсальные limits/нормативы и не доказывают O(N) всему pipeline.
