# Density Map experiment harness — 06.0A

Изолированный C++20 стенд подготовки входов, независимых малых эталонов и измерений
для 06.0B/06.0C. Production targets не зависят от него. Собственных compressed
count containers, TIFF, rasterization candidates, PNG, GUI и project persistence
здесь нет. Числа smoke run характеризуют только инфраструктуру.

## Сборка и запуск

Проверено в x64 Developer Command Prompt с Qt 6.12.0 `bin`, CMake и Ninja в PATH,
`QT_ROOT` и `VCPKG_ROOT`, как в корневом README:

```bat
cmake --preset windows-debug -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-debug --target density_benchmark density_benchmark_tests --parallel 4
ctest --preset windows-debug -L density_experiment --output-on-failure
cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-release --target density_benchmark density_benchmark_tests --parallel 4
ctest --preset windows-release -L density_experiment --output-on-failure
build\windows-release\density_benchmark.exe --output-dir build\density-06a-smoke --repeats 4 --storage-label "D: Hitachi HDS721050CLA362 SATA HDD"
```

`--output-dir` обязателен: конечный каталог должен отсутствовать, его родитель —
существовать. Для повторения всей подготовки задайте **другой новый каталог**.
Внутри одного запуска все workers читают одни и те же файлы. Первый worker и
последующие workers учитываются отдельно. `--help` описывает параметры;
`--scenario` принимает `all` или имя из таблицы ниже. По умолчанию tile side = 8,
grid side = 3, seed = 6001, четыре повторения, RAM/temp/output по 128 MiB.

Дополнительный уже существующий Point Cache можно подготовить для 06.0C:

```bat
build\windows-release\density_benchmark.exe --output-dir build\density-06a-external --cache build\density-06a-smoke\boundaries.pc2dcache --cell 1 --scenario empty --repeats 1
```

Он открывается read-only через production CacheReader. Исходный cache не копируется,
не переписывается и не удаляется. `--cell` относится только к дополнительному cache;
у встроенных малых сценариев собственные фиксированные grids. Команды external
workflow проверены также сквозным тестом с временным исходным cache.

## Компоненты и подключение кандидатов

| Файл | Назначение |
|---|---|
| `dataset.h/.cpp` | Спецификации, точные тайлы, общий input interchange, ReadPlan, проверки и digest |
| `oracle.h/.cpp` | Экспериментальная координатная конвенция, ручные record→cell labels, интервальный oracle |
| `measurement.h/.cpp` | Preflight, измерительный callback, память/I/O, JSON/CSV, статистика повторов |
| `main.cpp` | Подготовка общих входов, production CacheWriter/CacheReader, отдельные workers |
| `tests/density_benchmark_tests.cpp` | Малые unit и сквозные проверки; label `density_experiment` |

06.0B подключает обычные прямые вызовы своего writer/reader к `measure()`:
writer получает `Spec` и поток `Tile` через `readInputs()` либо `generate()`;
reader получает тот же `Spec`, `ReadPlan` из `readPlans()` и отдаёт `TileSink`.
Должны измеряться отдельные write / full read / selected tiles / ROI, включая
close/flush, если они входят в сравниваемую операцию. Открытие reader нужно явно
включать в фазу или выделять отдельно одинаково для всех кандидатов.
`FileIo` обновляется только по фактическим результатам обёрнутых I/O calls;
неинструментированные bytes/operations следует обозначать null в details.

Проверка результата передаётся отдельным callback `verify`: время и process I/O
операции заканчиваются **до** проверки. Сравнивайте каждое значение и форму тайла
с общим входом, порядок/покрытие выбранных тайлов, суммы и канонический digest,
включая implicit zero. Для больших результатов проверку выполняйте потоково.
`measure()` не выбирает метод, не регистрирует plugins и не содержит фиктивного
кандидата. Измеряемое сейчас чтение `.counts` — проверка входного interchange,
его нельзя выдавать за кандидата 06.0B.

06.0C получает `inputs.json → rasters`: read-only `cachePath`, grid, count,
pointDigest и малый `expectedCounts`. Запускайте кандидат через тот же `measure()`
и production CacheReader с одинаковыми reader budgets; open и scan statistics
учитывайте раздельно, поскольку CacheReader сбрасывает статистику перед scan.
Oracle не должен попадать в измеряемый raster kernel. Большой исходный cache
проходит только потоковый scan/digest; полного массива точек в памяти нет.
`Plan` нового кандидата должен учитывать его реальные RAM/scratch/output размеры
до вызова; существующий preflight подготовки не является оценкой его потребностей.

## Данные и независимость эталонов

Для обычных count scenarios размеры равны `(gridSide*tileSide-1) ×
(gridSide*tileSide-3)`: по умолчанию 23×21, 9 тайлов, крайний тайл 7×5.

| Scenario | Содержимое |
|---|---|
| `empty`, `uniform`, `dense` | Counts 0, 1, 4096 |
| `sparse` | Детерминированная редкая occupancy с count 7 |
| `horizontal`, `vertical` | Полосы с периодом 4 ячейки |
| `clustered` | Локальный кластер 17 в одном тайле |
| `overlap` | Сумма горизонтальных и вертикальных измерений 5+7 |
| `u32`, `u64` | UINT32_MAX и UINT32_MAX+2 в начале каждого тайла |
| `entropy` | Полные псевдослучайные uint32 counts, низкая ожидаемая сжимаемость |
| `huge-sparse` | 1 000 000 003 × 1 000 000 001 ячеек, только четыре corner tiles |

Сжимаемость — свойство входов, фактический compression ratio ещё не измерялся.
Все остальные тайлы huge-sparse — implicit zero. Full/ROI перечисляют сохранённые
тайлы в порядке Y/X; explicit tile request внутри пустоты возвращает полный
нулевой тайл без файлового I/O. Пустой ROI не выдаёт тайлы. ROI задаётся
полуоткрытым прямоугольником в **целых ячейках**, reader возвращает целые
пересекающиеся тайлы. Пиксели за частичным краевым тайлом не существуют.

Interchange не является production format: manifest JSON и отдельные несжатые
`.counts` с magic `D06AIN01`, LE uint64 tile X/Y, LE uint32 width/height, затем
row-major LE uint64 counts. Все количества сохраняются точно, включая UINT64_MAX
в unit codec test. 32-bit scenarios проверяют диапазон явно. Суммы накапливаются
с проверкой uint64 overflow; полный UINT64_MAX вместе с другими counts может
потребовать отказа вместо непредставимой суммы.

Digest: SHA-256 компактного Qt JSON `Spec`, затем всех **явных** encoded tiles в
порядке Y/X (внутри — row-major). Schema, Qt version, размеры, seed и count width
зафиксированы. PRNG использует явно заданные uint64 modular arithmetic constants,
а не implementation-dependent `uniform_int_distribution`. Помимо digest есть
сумма, максимум, nonzero count, literal controls, независимые формулы простых
сценариев и golden entropy sum 1 080 383 497 533 для default grid/seed.

Raster inputs: отрицательные координаты, signed zero, точки по обе стороны от
нуля через `nextafter`, внутренние и крайние границы, точные повторы измерений,
вырожденные point/line bounding boxes, частичная ячейка и пустое облако.
У каждого из пяти малых случаев **вручную** задана ожидаемая cell для каждой записи.
Отдельно переиспользуются `rectangle.json` + `clean_horizontal.json` существующего
synthetic generator 0.4.0, seed 1001, без новых fixture schemas. Получено 87 230
измерений; генератор и CacheWriter работают потоково с batch 1024.

Oracle сравнивает координату с интервалами **всех** ячеек, без floor, tile addressing
или будущего candidate kernel. Он ограничен 4096 ячейками и 100 000 точками для
автоматической проверки cache. Counts генератора считаются независимо от чтения
cache; ручные record labels повторно проверяются по RecordId после сортировки.
Для больших external caches явно сохраняется `oracleAvailable=false`: только
count/digest не доказывают правильность будущей растеризации.

## Предварительная координатная конвенция

Это **экспериментальное допущение**, а не закрытие Open decision algorithms §6:

- origin = `(minX,minY)`, Y растёт вверх;
- размер оси `max(1,ceil((max-min)/cell))`;
- внутренние ячейки `[origin+i*cell,origin+(i+1)*cell)`;
- последняя ячейка включает точный максимум bounding box;
- вырожденная ось имеет одну ячейку; каждая точка принадлежит ровно одной ячейке;
- нет epsilon, сдвига координат, квантования исходных double или PNG orientation.

Непредставимые dimensions/product и неоднозначные binary64 интервалы дают отказ.
Полная production coordinate policy для экстремальных координат ещё не выбрана.

## Измерения и ограничения

`results.json` содержит окружение, параметры, preflight, inputs, сырые measurements
и статистику последующих запусков. `results.csv` содержит те же measurements;
вложенные details/I/O записаны корректно quoted JSON. uint64 — decimal strings,
время — числовые **миллисекунды**. `repeat-N.json` — результаты отдельного процесса.

- `elapsedMs`: wall time операции по QElapsedTimer, включая её instrumentation.
- `verificationMs`: отдельный callback проверки.
- `totalElapsedMs`: операция, проверка и служебный сбор метрик.
- `wholeOperationElapsedMs`: подготовка, workers и сбор результатов до финальной
  сериализации/записи отчёта; запуск процесса до входа в main не учитывается.
- `processPeakWorkingSetBytes`: Windows PeakWorkingSetSize, максимум resident
  working set **за жизнь процесса**, не дельта и не отдельный phase peak.
  Workers создаются заново; previous phase/verification может определять максимум.
- `privateBytesAfter` и working set before/after — snapshots, не peaks.
- `processIo`: GetProcessIoCounters deltas, process logical I/O, включая системные
  накладные расходы. Это не physical HDD traffic и не только reads самого алгоритма.
- `fileIo`: фактические successful bytes и attempted calls только собственных
  QIODevice fixture wrappers. CacheReader statistics — отдельно в details.
  CacheWriter не раскрывает полную logical I/O statistics: она обозначена null.
- `ownedFilesAfterBytes`: сумма логических длин своих файлов на границе фаз;
  не allocated clusters. Scratch size после записи cache и после всего запуска
  фиксируется отдельно; scratch high-water mark **не измерен** (`temporaryPeakBytes=null`).
- Physical disk I/O недоступен (`null`). Нет механизма доказанного OS cache reset.

После подготовки файлы уже могли оказаться в OS cache; correctness rereads также
влияют на него. `first` означает первый worker, `subsequent` — повторные workers
над теми же файлами, без гарантии residency. Называть `first` cold нельзя.
Для subsequent сохраняются n/min/median/max/mean/sample standard deviation;
при n=1 sample stddev null. Для выводов 06.0B/C нужны больше повторов, достаточно
долгие фазы, одинаковый порядок/контролируемое чередование кандидатов и описание
cache policy. Короткий smoke не устанавливает преимущество форматов/алгоритмов.

## Ресурсы и данные пользователя

Preflight проверяет checked arithmetic, текущие available physical RAM/free disk,
настраиваемые RAM/temp/output budgets. План count inputs учитывает payload и
консервативный запас 4096 bytes на файл, cache/report reserve; это инженерная
оценка подготовки, не универсальная политика памяти. Сам процесс проверяет
working set на checkpoints; writer получает собственные production budgets.
Это cooperative limits, **не** OS hard cap/Job Object и не защита от внешнего
заполнения диска между проверками. Пиковая системная commit memory не измеряется.

Обычные tests создают только небольшие temporary datasets. Масштаб задаётся явно
`--grid-side`/`--tile-size`/budgets; огромный логический raster не создаёт огромный
массив или полный список пустых тайлов. Исходные project/cache/Recovery не меняются.
Стенд никогда не выполняет recursive cleanup. CacheWriter удаляет только свой
scratch по существующему протоколу. Незавершённые собственные input files сохраняются
рядом с диагностикой; повторное использование каталога запрещено. Ctrl+C/Break
даёт cancellation на checkpoints; parent останавливает своего worker при отмене.

Статусы различают invalid_input, io/cache_error, cancelled, resource_refused,
incorrect, verification_error, worker_error. При возможности записывается отчёт
об отказе; ошибка записи самого отчёта также возвращает ненулевой exit code.
Существующие данные и benchmark evidence не перезаписываются.

Текущий вердикт и фактические результаты: [отчёт 06.0A](../../docs/reports/task-06/density-harness.md).
