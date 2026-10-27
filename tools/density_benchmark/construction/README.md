# Density count construction experiment — 06.0C

Изолированные C1 (один `CacheReader::scanAll` + event spool) и C2 (один
`CacheReader::scanBox` на raster-группу). Production не зависит от этих targets.
Новых зависимостей, page cache, изменений CacheReader или persistent formats нет.
PNG, GUI, ProjectState и 06.0D сюда не входят. Сжатие D не включено в замеры:
оба кандидата выдают один несжатый проверяемый поток count-тайлов.

## Общий контракт

`Parameters` задаёт Grid 06.0A, tile side, квадратную группу в тайлах, bounded
event capacity и одинаковые RAM/temp/output budgets. Все counts — checked uint64.
Grid — экспериментальная min-origin/Y-up/half-open/clamped-max конвенция 06.0A.
Окончательная coordinate policy проекта остаётся открытой.

Boundary вычисляется ровно как в интервальном oracle: `origin + double(i)*cell`.
Mapping использует binary search по этим binary64 boundaries, без subtraction/floor
и epsilon. Перед обработкой проверяются все внутренние boundaries обеих осей:
конечность, строгое возрастание и containment. Стоимость O(width+height), память O(1).
Индексы больше 2^53 и collapsed intervals отклоняются явно. Это ограниченный
экспериментальный профиль, а не решение для любых экстремальных координат.

C2 строит bounds из тех же выражений, расширяет их на один представимый binary64
шаг наружу и ограничивает metadata bounds сетки. Это консервативный запрос,
а не геометрический допуск. Inclusive queries могут вернуть соседние точки;
общий mapping заново вычисляет cell и единственного владельца группы.
Повторные measurements сохраняются. Morton-порядок не используется как raster-порядок.

## C1 и ресурсы

В RAM только один group count buffer и до `eventCapacity` событий `{group,local}`.
Chunk сортируется по group, затем каждая подпоследовательность дописывается
в соответствующий `.events` файл. На диске событие — LE uint32 local cell index;
размер группы проверяется до выделения. Один spool handle открыт одновременно.
Никакого полного external sort нет; порядок событий внутри группы несущественен.

После успешного scan группы читаются по Y/X. По завершении чтения удаляется только
собственный event file; при ошибке RAII удаляет собственный `QTemporaryDir`.
После P1 группы перечисляются непосредственно из ordered occupied set, без
цикла по пустым logical groups. `visitedGroups == occupiedGroups` для C1;
линейный uint64 group ID `y*columns+x` даёт порядок Y/X.
Количество event files ограничено `maxOccupiedGroups` (CLI
`--max-occupied-groups`, default 1 000 000). `maxGroups` / `--max-groups`
ограничивает только число spatial queries C2. Это экспериментальные guards,
не универсальная политика production.
Occupied-group set и output descriptors учтены в RAM plan. Число occupied groups
в плане — `min(N,logicalGroups,maxOccupiedGroups)`. Output descriptors дополнительно
ограничены числом тайлов в разрешённых occupied groups. Превышение occupied
лимита даёт resource refusal до создания лишнего spool файла. Для sparse jobs
можно явно задать небольшой occupied limit и тем самым уменьшить консервативный
preflight; это ограничение trial, а не предсказание распределения точек.

Точный peak суммы логических EOF event files равен 4*N: первая фаза только
дописывает, вторая только читает и удаляет. Отдельно фиксируются read/write bytes,
operations, opens, buffers и peak одновременно открытых **dataset file handles**
(C1 — до трёх вместе с Reader и output; C2 — два). Peak всех process handles
не измеряется. P1 дополнительно фиксирует `FILE_STANDARD_INFO::AllocationSize`
для открытого spool через borrowed CRT/native handle: максимум суммы после
flush, и ноль после cleanup. MFT/directory metadata этим не измеряются.
Сумма allocation также проверяется против scratch budget. Preflight резервирует 4096
bytes на spool file. Budgets cooperative, без OS hard cap и гарантии свободного
диска после preflight; process working set проверяется на checkpoints.

## Output и независимая проверка

`Output` пишет existing `encode(Tile)` из 06.0A в один `counts.partial`.
Нулевые тайлы implicit, полного raster allocation нет. В RAM — ограниченные
descriptors непустых тайлов; они сортируются для canonical verification.
Digest: SHA-256 компактного Spec JSON, затем nonzero encoded tiles в Y/X-порядке.
Он не зависит от группировки/порядка выдачи. Tile size входит в Spec.
Это sparse digest 06.0C; его нельзя напрямую сравнивать с dense digest 06.0A.

`complete.json` появляется только после успешного build, count/sum checks и
полного readback, включая expected counts при наличии oracle. Ошибка оставляет
диагностику и staged output, без completion marker. Sink может получить префикс;
успехом является только успешное завершение build и проверки. Файлы `.partial`
не переименовываются: marker отделяет успешный trial от незавершённого.

Малые случаи используют ручные record labels и интервальный oracle 06.0A.
`density_construction_prepare` использует существующий generator 0.4.0 и fixtures,
накапливая oracle counts во время генерации до Morton sorting. Дополнительный
`dense-square` меняет только pointStep=.02 / lineStep=.2 существующего clean
Horizontal: около 1.5 млн points; effective Scan JSON и hashes сохранены.

`--regular-fixture` проверяет готовые Save acceptance caches: каждый RecordId
встречается один раз и имеет `X=(id%100000)*.01`, `Y=(id/100000)*.01`.
Expected counts строятся независимыми separable interval histograms по исходным
координатным рядам через STL lower/upper_bound, без candidate cell mapping.
Reference ограничен 2 млн cells, размер fixture — 100 млн records.
Для произвольного внешнего cache без reference совпадение counts/digest/суммы
само по себе не является независимым доказательством корректности.

## Измерения

Общий `measure()` 06.0A: `open` отдельно, затем `build_counts`, затем нетаймируемая
проверка. Open statistics сохраняются до первого scan; C2 суммирует **все** query
statistics, включая повторные страницы. `scanInclusiveMs` включает callbacks;
`scanExcludingCallbacksMs` вычитает callback durations. Эти вложенные тайминги
нельзя складывать с inclusive scan как независимые фазы. У C1 callback включает
mapping, chunk sort и spool writes; заключительный flush также входит в build.
`countAndTileMs` включает накопление и формирование тайлов; у C2 — mapping в sink.
Output/finalization и spool grouping/write/read дополнительно выделены.

Каждый candidate trial — новый процесс и новые output files. Четыре пары:
C1/C2, C2/C1, C1/C2, C2/C1. `first` исключён из statistics; subsequent n=3,
median/min/max/nearest-rank p95 сохранены. n=3 мало для устойчивой tail оценки.
Fingerprint/source checks и подготовка прогревают OS cache. `first` не cold.
PeakWorkingSetSize — lifetime working set процесса, не phase delta; processIo
и CacheReader I/O — логические, physical HDD I/O отсутствует. Параллельные
нагрузочные проверки во время матрицы не запускались.

## Воспроизведение

В x64 MSVC CMD со штатным Qt/CMake/Ninja PATH из корневого README:

```bat
cmake --build --preset windows-debug --target density_construction_tests density_construction_prepare density_benchmark_tests --parallel 4
ctest --preset windows-debug -L "density_construction|density_experiment" --output-on-failure
cmake --build --preset windows-release --target density_construction_tests density_construction_prepare density_benchmark_tests --parallel 4
ctest --preset windows-release -L "density_construction|density_experiment" --output-on-failure
```

Полная матрица из PowerShell при Qt bin в PATH; оба каталога должны отсутствовать:

```powershell
& tools/density_benchmark/construction/run-matrix.ps1 -OutputDirectory build/density-06c-matrix -EvidenceDirectory docs/benchmarks/density06c
python tools/density_benchmark/construction/audit-results.py build/density-06c-matrix docs/benchmarks/density06c
```

Матрица переиспользует `build/density-06a-smoke/inputs.json` и существующие
`build/task052-resource-{1m,42m}/source.pc2dcache`; при отсутствии подготовьте
подходящие inputs отдельно. Стенд не генерирует 42 млн measurements повторно.
Для другого одиночного входа используйте `--help`, `--inputs` либо `--cache`.
Параметры `--pad-x/--pad-y` расширяют минимум bounds для проверки смещения сетки.
Audit Python использует только stdlib: JSON/CSV equality, самостоятельный decode,
каждый count относительно reference, точное сравнение canonical tile streams,
SHA-256 и контрольные cells, scratch invariants, source hashes и evidence manifest.

P1 сохраняет проверку boundaries `O(width+height)`. Отдельный
`--max-boundary-checks` (default 100 000 000) ограничивает число внутренних
axis boundaries **до** их обхода; это настраиваемый экспериментальный work guard.
Он не зависит от площади карты и не определяет coordinate convention.
`groupTraversalMs` включает выбор следующей группы и checkpoint, исключая
её накопление/output; `spoolOpenRemoveMs` — read-side open/allocation/close/remove.
Write-side opens, allocation queries и flush входят в `spoolWriteMs`.

Адресная матрица P1, после сборки исходного baseline в отдельном build tree,
при Qt bin в PATH (оба каталога должны отсутствовать):

```powershell
python tools/density_benchmark/construction/run-p1.py build/density-p1-matrix docs/benchmarks/density-p1 --baseline build/density-p1-baseline-release/density_construction_benchmark.exe
python tools/density_benchmark/construction/audit-p1.py docs/benchmarks/density-p1
```

Матрица вызывает существующие `prepare --p1`, workers и `measure()`; first +
три subsequent trials каждого кандидата, с ротацией порядка. Входные caches
создаются заново; 42M cache переиспользуется и проверяется по каждой записи.
Output budget 4096 MiB общий для трёх кандидатов: исходный C1 резервирует до
2 GiB для 16383² даже при четырёх nonzero tiles. RAM 512 / scratch 1024 MiB.
Для корректной интерпретации отказов и воспроизведения baseline см.
[отчёт P1](../../../docs/reports/task-06/density-construction-p1.md).

Исторические результаты: [отчёт 06.0C](../../../docs/reports/task-06/density-construction.md).
