# 06.0C — экспериментальное сравнение построения Density Map

2026-10-27. **READY WITH LIMITATIONS**. Предварительно рекомендован **C2,
группы 512×512 cells**, после независимого ревью и согласования.
Изолированные C1 Sequential Scan + Event Spool и C2 Grouped Spatial Queries.
Production-контракты, CacheReader, persistent formats, GUI, ProjectState
и Save/Open/Recovery не изменены. 06.0D не выполнялась.

## Baseline и среда

- Branch `main`, HEAD `fd6581f67d422785daec9b8afb268841ecdaf3e0` (06.0B).
  Исходный `git status --short` пустой. Изучены AGENTS, README, относящиеся
  density/coordinates/cache/resources/testing contracts, implementation-plan 06,
  point-cache-format, инструменты и evidence 06.0A/B.
- Windows x64, C++20, MSVC 19.51.36260.0 / toolset 14.51.36231, Qt 6.12.0,
  CMake 3.30.5, Ninja 1.12.1; основной CMake/vcpkg baseline сохранён.
  Новые зависимости не устанавливались. Известные optional Qt TaskTree/Vulkan
  configure diagnostics сохраняются.
- D: NTFS, SATA **Hitachi HDS721050CLA362**, **HDD**. Модель/media и связь тома
  проверены Get-PhysicalDisk и Get-Partition → Get-Disk, raw сохранён.
  Physical RAM 17 135 476 736 bytes; при исходном обследовании свободно около
  3.9 GB, на диске около 299 GB. Перед сериями/trials выполняется live preflight.
- Budgets обоих кандидатов: RAM 512 MiB, scratch 1024 MiB, output 1024 MiB.
  Reader budget 8 MiB / один cache handle. Source cache открыт read-only.
- Основной Release executable SHA-256:
  `f6d692c98aba7bb4c57ddd476414daff685b04aa51414bc4127df04bda1e3a67`.
  Build/runtime HEAD в evidence — baseline; работа была незакоммичена во время
  измерений. Финальный commit следует после проверок и review diff.

## Реализация и контракты

Подробные параметры, API, измерительные определения и команды —
[README эксперимента](../../../tools/density_benchmark/construction/README.md).

| Файл в tools/density_benchmark/construction | Ответственность |
|---|---|
| construction.h/.cpp | Общий mapping, plan, C1/C2, staged sparse count stream |
| benchmark.cpp | Прямые callbacks в measure(), workers, общий preflight, reference, fingerprints |
| prepare.cpp | Дополнительные caches из существующего generator 0.4.0 / fixtures |
| tests.cpp | Manual/oracle, boundaries, duplicates, resources, cancellation, failures, CLI |
| run-matrix.ps1 | Воспроизводимые параметры и последовательный порядок серий |
| audit-results.py | Независимый decode всех count streams, JSON/CSV, reference, hashes |
| CMakeLists.txt | Только opt-in experimental targets, существующие dependencies |

Переиспользованы Grid/Spec/Tile, `encode/decode`, checked arithmetic, oracle,
`measure()`, JSON/CSV и resource preflight. 06.0A/B interchange и defaults не
изменены. Нового измерительного слоя или альтернативы CacheReader нет.

Общий mapping сохраняет временную конвенцию 06.0A: min-origin, Y вверх,
полуоткрытые внутренние интервалы и включённый максимум последней ячейки.
Binary search сравнивает точку с теми же rounded binary64 boundaries, что oracle.
Он корректно обрабатывает случай, когда subtraction/floor округлил бы точку
`nextafter(boundary,-inf)` в соседнюю ячейку. Все внутренние boundaries обеих осей
проверяются на конечность/строгое возрастание; collapsed intervals и индексы >2^53
отклоняются. Это экспериментальный профиль, не закрытие coordinate Open decision.

C1 делает ровно один scanAll. Bounded chunk до 65 536 событий сортируется по
группе, local index записывается как LE uint32. Append/read используют максимум
один spool handle; group buffer один. File count ограничен maxGroups=1 000 000
и числом occupied groups, память event chunk/set/descriptors включена в plan.
После scan группы последовательно накапливаются; temporary files освобождаются.

C2 получает inclusive boxes из общего mapping, расширенные на один representable
binary64 шаг наружу. Это консервативный query bound без epsilon; точный mapping
определяет единственного владельца. Ни scanBoxes box ID, ни совпадение Morton
и raster порядка не предполагаются. Дополнительного page cache нет.

Оба кандидата выдают одинаковые nonzero Tile через общий sink; нули implicit.
Один несжатый `counts.partial` содержит existing encoded tiles, дескрипторы
позволяют читать их канонически по Y/X. Output budget и уникальность тайлов
проверяются, counts/sum — checked uint64. Marker `complete.json` появляется только
после успешного build и полного readback. Ошибка/отмена оставляет диагностику,
но не успешный dataset. Нет публикации в ProjectState.

## Корректность и независимость

- Пять малых 06.0A случаев: ручные record→cell labels, независимый interval oracle,
  отрицательные координаты, signed zero, повторы, degenerate point/line и partial edges.
- Дополнительные unit cases: различные cell/origin/group/chunk sizes, все точки
  до/на/после cell и group boundaries через nextafter, decimal .1/.3, большие
  абсолютные координаты, reverse input, horizontal/vertical/crossing/shifted passes,
  dense groups, clusters и пустые области. Миллион ячеек между двумя точками
  проверен без full-raster allocation кандидатов.
- Отдельный spatial test подтверждает возврат одной boundary-точки несколькими
  запросами и ровно один count после ownership filtering; статистика запросов
  суммируется с repeated point pages.
- Дополнительные fixture caches получают expected counts от interval oracle
  **во время генерации**, до Morton sorting, отдельно от candidate kernel.
- Готовые caches 1 млн / 42 млн measurements взяты из Save acceptance без
  повторной генерации. Каждый RecordId/coordinate проверен по исходной формуле;
  expected raster построен независимыми separable interval histograms. Все counts
  сравниваются, включая нули; проверка не ограничена совпадением C1/C2.
- Проверены tiny RAM/temp/output budgets, group/index limits, collapsed intervals,
  cancellation во время scan и после выданного префикса, реальный отказ открытия
  spool-файла, sink I/O failure, missing output parent, повреждение point page,
  truncated cache, сохранность source и existing output directory.

Полная production regression не требуется и не запускалась: production sources
и dependencies не менялись. Проверки и итоговая матрица приведены ниже.

## Методика измерений

Все performance trials — **Release x64** на D: HDD. Девять серий,
четыре пары на каждый dataset: C1/C2 → C2/C1 → C1/C2 → C2/C1.
Каждый candidate — новый процесс и новый изолированный output.
`first` отделён; subsequent n=3, median/min/max/mean/sample stddev/nearest-rank p95
сохранены. Малое n не позволяет устойчиво оценивать tail latency.

План сохранён до запуска в matrix-planned.json. Он выполнен без сокращения:
малые cases; семь spatial distributions при двух group sizes; 1 млн при трёх
group sizes и shifted grid; 42 млн при двух group sizes. Новая плотная квадратная
fixture содержит около 1.5 млн points; остальные дополнительные cases — прежние
scan fixtures. Крупные пары начались после PASS малых tests и средних серий.

Open измерен отдельно с сохранением initial statistics. Основное `build_counts`
включает preflight/проверку mapping, scan, grouping/spool, накопление, общий output
и finalization. Verification и source hashing — за пределами этой фазы.
Сжатие/формат D не участвуют; end-to-end D не выполнялся.

`scanInclusiveMs` содержит callback, `scanExcludingCallbacksMs` его вычитает.
У C1 callback включает mapping/sort/spool write, а final chunk flush входит
в build после scan. `spoolReadMs` — время read calls, open/close/remove также
входят в общее build time, но отдельно для read-фазы не выделены.
Вложенные timers не являются независимыми слагаемыми total.

RAM — Windows process-lifetime PeakWorkingSetSize каждого свежего worker до
verification; previous open также может определять peak. Scratch peak — точно
измеренная сумма EOF event files: монотонные записи до обработки, затем удаления.
Это не allocated clusters; консервативный preflight добавляет 4096 bytes/file.
Известен peak одновременно открытых dataset handles (3/2); process-wide handle
high-water mark не измерен.

Все reads/seeks/bytes — логические. C2 учитывает повторные index/point pages,
open не смешан с scans. GetProcessIoCounters не physical HDD I/O; logical seek
не означает перемещение головки. Источники/reference/SHA-256 прогревают OS cache,
его сброса нет. Поэтому это измерения **на HDD при неконтролируемом OS cache**,
а не доказательство cold-HDD throughput. Параллельные builds/tests не выполнялись
во время матрицы.

## Измеренные результаты

Все девять серий завершены: **416 ok rows**, из них **208 count outputs** и
208 open rows. Независимый Python audit — **PASS**: 208 самостоятельных decode /
canonical fingerprints, **182 точных сравнений tile streams**, все expected
counts, cross-group digests, JSON/CSV, executable SHA-256 и **15 source hashes**.
Source SHA-256 до, после серий и при audit совпали. Scratch после trials — 0 bytes.
Raw, JUnit и audit доступны в [evidence index](../../benchmarks/density06c/README.md).

Основные median build times subsequent n=3, **секунды**; open отдельно,
около 1–2 ms, не влияет на эти различия. У 1 млн/42 млн cell=.1 mm,
output tile=64, группы — 64/256/512 cells; raster 10000×1 / 10000×42 cells.
Источник 42 млн — 1 010 900 992 bytes. Shifted grid добавляет .375/.625 cell
к нижним X/Y bounds без изменения исходных измерений.

| Dataset / группа | C1, s | C2, s | Cache logical read MiB C1 / C2 | Max worker WS MiB C1 / C2 | C1 scratch MiB |
|---|---:|---:|---:|---:|---:|
| 1 млн / 64 | 1.217 | 2.253 | 22.94 / 189.25 | 10.64 / 9.60 | 3.815 |
| 1 млн / 256 | .857 | 1.002 | 22.94 / 64.56 | 11.15 / 10.07 | 3.815 |
| 1 млн / 512 | .836 | .805 | 22.94 / 43.81 | 12.79 / 11.59 | 3.815 |
| 1 млн shifted / 256 | .848 | .911 | 22.94 / 64.44 | 11.47 / 10.36 | 3.815 |
| 42 млн / 256 | 31.939 | 28.871 | 963.00 / 1504.25 | 34.19 / 34.24 | 160.217 |
| 42 млн / 512 | 27.316 | 25.580 | 963.00 / 1233.25 | 34.25 / 34.20 | 160.217 |

WS — максимум по четырём workers данного кандидата, включает общий JSON/reference
и предыдущий open. Он не является оценкой одного group buffer. Непосредственные
group buffers для сторон 64/256/512 — 32 KiB / 512 KiB / 2 MiB; C1 event buffer
1 MiB плюс encoded chunk до 256 KiB, C2 event buffer 0.

Сравнение пространственных распределений, median **ms**:

| Generator scenario | C1 / C2, группа 4×4 | C1 / C2, группа 16×16 |
|---|---:|---:|
| Vertical | 100.55 / 157.94 | 69.32 / 53.34 |
| Horizontal + Vertical | 197.07 / 246.31 | 122.97 / 104.46 |
| Shifted passes | 100.88 / 156.03 | 70.77 / 53.31 |
| Overlapping passes | 161.15 / 167.28 | 75.31 / 60.77 |
| Local repeated scan | 135.47 / 187.81 | 84.52 / 72.01 |
| Outside grid cloud / gap | 106.62 / 166.43 | 73.14 / 54.16 |
| Dense square, 1 505 301 points | 1243.49 / 1688.86 | 1013.80 / 919.79 |

Output tile=4; для шести fixtures cell=5 mm, raster=35×14 с ненулевым отрицательным
origin. Для dense-square cell=2 mm, raster=50×30. Малые manual cases занимают
около 2–9 ms: это overhead/correctness checks, не устойчивый speed ranking.

Повторное чтение и filesystem cost на большом cache:

| Метрика | C1 / 256 | C2 / 256 | C1 / 512 | C2 / 512 |
|---|---:|---:|---:|---:|
| Queries | 1 scanAll | 40 scanBox | 1 scanAll | 20 scanBox |
| Point pages, с повторениями | 15408 | 23722 | 15408 | 19554 |
| Index pages, scans без open | 0 | 346 | 0 | 178 |
| Logical read/seek operations | 15408 | 24068 | 15408 | 19732 |
| Spool opens | 2079 | 0 | 1367 | 0 |
| Spool files | 40 | 0 | 20 | 0 |
| Spool write + read bytes | 168M + 168M | 0 | 168M + 168M | 0 |
| Peak одновременно открытых dataset handles | 3 | 2 | 3 | 2 |

Здесь M=10^6 bytes. C2 / 256 отклонил 16 380 соседних boundary records после
точного mapping; итоговая сумма ровно 42 000 000. Для каждого trial соответствующие
значения есть в raw details. Canonical controls большого raster, например:
cell(0,0)=100, cell(5000,21)=100, cell(9999,41)=81. Последнее значение следует
binary64 interval convention и независимо проверено, а не подогнано к идеальным
десятичным шагам.

## Анализ и стоимость реализации

**Измеренный факт:** мелкие spatial groups увеличивают repeated reads и число
queries. На 1 млн / 64 C2 читает 189.25 MiB против 22.94 MiB C1 и проигрывает.
При группе 512 C2 читает 43.81 MiB и практически сравнивается с C1; на 42 млн
медиана C2 ниже при обоих tested group sizes. На пространственных fixtures
укрупнение группы с 4 до 16 cells меняет преимущество в пользу C2.

**Инженерная интерпретация:** Morton-packed pages локальны, но их BBox пересекает
несколько узких raster groups; C2 платит за повторные traversal/read/CRC checks.
Большая группа уменьшает эту цену. C1 избегает повторного cache scan, но добавляет
event encoding, chunk sorting, множество append opens, 4*N scratch writes/reads
и второй проход накопления. Например, в одном raw 42 млн / 256 C1 trial:
scan inclusive 23.41 s, callback 13.82 s, spool write .83 s, spool read 2.77 s,
count/tile 4.45 s; output/finalization .009 s. Вложенные времена перекрываются.
Это объяснение согласуется с метриками; долю физических HDD ожиданий оно не
устанавливает.

Общий mapping, counts/output verification и cancellation нужны обоим вариантам.
Поверх них C2 имеет простой group loop и один Reader query. C1 дополнительно
требует event codec/local-index contract, bounded buffering, chunk grouping,
append lifetime, disk/file budgets, rollback/cleanup и проверки spool integrity.
Его сложность оправдана только подтверждённой выгодой, которой при выбранных
крупных группах на доступной матрице нет.

Для очень разреженной карты с удалёнными кластерами C2 может обходить множество
пустых групп и перечитывать страницы с широкими BBox. Unit case с большим gap
корректен, но representative large sparse performance не измерена. C1 также
не является универсально масштабируемым решением: scratch растёт как 4*N,
occupied group metadata/file opens могут стать существенными.

## Рекомендация, ограничения и готовность

**Рекомендовать C2 для следующего архитектурного рассмотрения.** Начальный
рабочий group side — **512 cells** (2 MiB uint64 buffer). Фактически проверенная
конфигурация основного сравнения — tile=64, groupTiles=8, Reader=8 MiB,
четыре balanced pairs. Для raster из 50×30 cells достаточно небольших групп:
в этой матрице 16 cells оказались предпочтительнее 4. Универсального процента
выигрыша или автоматической политики выбора размера не устанавливается.

Выбор опирается на корректность, практически достаточное время на доступных
synthetic HDD workloads, отсутствие scratch и меньшую сложность. C1 сохраняется
как экспериментальный результат сравнения; поддерживать оба метода в production
без новой подтверждённой необходимости не предлагается. Размер output tile и
end-to-end накладные расходы D требуют отдельного согласования: формат D из
06.0B остаётся рекомендацией, не утверждённым persistent format.

Существенные ограничения:

- **Только synthetic data.** Representative real scans недоступны; реальные
  аппаратные задержки/шум/распределения не подтверждены. Большой источник — узкая
  регулярная полоса; квадратный medium case проверен отдельно, но это не полный
  sweep cell/bounds/distributions.
- **OS cache uncontrolled.** Все серии физически размещены на подтверждённом
  HDD, но cold-cache и physical I/O не измерены. Преимущество на cold HDD или
  network storage не доказано; repeated reads C2 могут изменить баланс.
- Preflight использует одинаковый консервативный C1-sized envelope для обоих
  кандидатов, включая 4*N scratch reserve. Поэтому предел N, доступный одному
  C2 при меньшем temp budget, здесь не исследован. Его фактический event scratch
  равен нулю; budgets cooperative, без hard cap и real disk-full injection.
- Измерен logical EOF scratch peak; filesystem allocation, system commit memory
  и process-wide handle peak отсутствуют. Read-side spool open/remove overhead
  входит в total, но не выделен отдельным timer.
- Coordinate convention, collapsed-interval refusals, maxGroups и bounded
  descriptors — экспериментальные решения; production контракт ими не заменён.

Фактические проверки:

| Проверка | Debug | Release |
|---|---:|---:|
| Build C1/C2 benchmark/tests/prepare + общий 06.0A target | PASS | PASS |
| Новые C1/C2 unit/integration tests | 12/12 PASS | 12/12 PASS |
| Существующие 06.0A tests | 22/22 PASS | 22/22 PASS |
| Существующие 06.0B tests, без изменения adapters | 13/13 PASS | 13/13 PASS |

Independent audit и полная Release matrix — PASS. Первые compiler errors
экспериментального scaffolding (неоднозначные CLI initializers / две GTest
throw-macros на одной строке) исправлены до измерений. Корректные tests не
ослаблялись; финальных failures/skips нет.

**06.0C — READY WITH LIMITATIONS**, оба алгоритма реализованы и проверены,
HDD-матрица и независимое evidence завершены. Рекомендация требует независимого
ревью и согласования; production алгоритм не утверждён. 06.0D не начата.
Изменения фиксируются одним локальным commit
`add experimental density map construction comparison`, parent `fd6581f`;
точный итоговый commit ID сообщается при завершении. Build datasets и scratch
в Git не входят; diff ограничен экспериментом, отчётом/evidence и кратким README
статусом. Push не выполняется.
