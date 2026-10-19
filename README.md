# CADContour2D

> Рабочее прототипное название проекта. Оно может быть изменено позднее без изменения назначения или архитектуры приложения.

CADContour2D — настольное приложение для восстановления двумерной CAD-геометрии плоских деталей по большим облакам измеренных точек.

Программа принимает плоские облака в текстовых форматах `.asc` и `.xyz`, позволяет получить и вручную скорректировать внешний контур, отверстия и внутренние вырезы, а затем построить компактную точную геометрию из CAD-примитивов и экспортировать её в DXF для дальнейшей работы, в том числе в Siemens NX.

Проект рассчитан на обработку файлов размером в несколько гигабайт на обычных офисных компьютерах с ограниченной оперативной памятью, HDD и возможным доступом к исходным данным через медленную локальную сеть.

## Input and Output

### Input

Поддерживаются:

```text
.asc
.xyz
```

Оба расширения рассматриваются как один формат без заголовка:

```text
X Y Z
X Y Z
X Y Z
...
```

Основной рабочий случай — плоское облако с `Z = 0`.

Основные геометрические единицы проекта — миллиметры.

### Output

Конечный результат — редактируемая двумерная CAD-геометрия детали:

- внешний контур;
- отверстия;
- внутренние вырезы.

Итоговая CAD-геометрия первой версии содержит только LINE, ARC и CIRCLE. Контур — замкнутая последовательность LINE/ARC либо самостоятельный CIRCLE; внешний и все внутренние контуры замкнуты. Это единый контракт автоматического fitting, ручного редактирования, precise-stage, Geometry Optimization и validation.

Финальная геометрия экспортируется в компактный 2D DXF для Siemens NX с сохранением типов и координатами в миллиметрах:

```text
LineSegment → LINE
CircularArc → ARC
Circle      → CIRCLE
```

SPLINE/BSpline и ELLIPSE не поддерживаются в первой версии. DXF передаёт восстановленную геометрию; сохранение NX constraints, 3D bodies и истории построения не требуется. Версия DXF и совместимость будущего exporter проверяются отдельно.

Приложение также использует сохраняемый рабочий проект `.pc2dproj` и локальный постоянный кэш исходных точек.

## Workflow

Целевая пользовательская модель: при запуске без проекта программа предлагает выбрать ASC/XYZ. Новый проект создаётся и принимается только после успешного потокового импорта и создания постоянного Point Cache; самостоятельного пустого проекта нет. «Новый проект» начинает выбор другого облака с подтверждением замены открытого проекта. До успеха текущий проект и история сохраняются; ошибка, отмена или отказ от подтверждения их не уничтожают. После успеха прежняя история Undo/Redo не переносится, Undo не возвращает предыдущий проект, ранее принятые постоянные файлы cache не удаляются из-за замены.

В новом проекте выбранный `cell` начинается с 1 мм без наследования прежнего значения. Это изменяемый пользователем параметр последующего явного построения Density Map; импорт не требует `cell` и карту не строит. Изменение выбранного `cell` не меняет существующие карты; каждая версия хранит собственные параметры построения. Автоматическая рекомендация `cell` — потенциальная тема исследования без выбранного алгоритма и утверждённой необходимости, не зависимость задач 04 или 06.

GUI выбора ASC/XYZ, подтверждения замены/Z и карточка dataset реализованы в 04.4A.
Save/Open `.pc2dproj`, восстановление параметров и частично доступного
состояния при отсутствующем или повреждённом cache — отдельная задача 05; обычная
работа после импорта не требует исходного ASC/XYZ. Ниже описан целевой поток
обработки, фактическая готовность — в [Project Status](#project-status).

Основной поток данных:

```text
ASC / XYZ
    ↓
Streaming Import
    ↓
Persistent Point Cache
    ↓
Density Map
    ↓
Rough Stage
    ↓
Rough Geometry
    ↓
Reduced Point Cloud
    ↓
Precise Stage
    ↓
Precise CAD Geometry
    ↓
DXF
```

### Rough Stage

Работает по фиксированной density PNG с физическим размером ячейки `cell`. Пользователь выбирает неразрушающую rectangular/lasso ROI, получает и редактирует rough-внешний контур, отверстия, HoleGroup и вырезы.

Rough geometry является приближением для последующей выборки реальных точек, а не финальной CAD-геометрией.

### Precise Stage

Явный Build Reduced Point Cloud принимает текущий rough-state и выбирает реальные точки около нужных границ. Пользователь удаляет посторонние точки, явно запускает refinement и редактирует полученную geometry.

Refinement строит компактные CAD-примитивы с continuity, topology и one-sided tolerance validation. Дополнительная Geometry Optimization запускается отдельно.

## Key Properties

- Основные геометрические единицы — миллиметры; исходное большое облако не загружается целиком в RAM.
- Immutable persistent point cache заменяет повторный разбор ASC/XYZ и позволяет работать без исходного файла.
- Пиксель density map соответствует `cell × cell` мм; zoom/pan не меняют разрешение. Новая карта строится явно; требования предусматривают сохранение всех версий с собственным Rough-состоянием и переключение без пересчёта — [requirements.md, §8](docs/requirements.md#8-фиксированный-размер-cell).
- Rough использует PNG, precise — реальные точки. Тяжёлые операции запускаются явно.
- ROI не разрушает исходную карту; неоднозначный выбор детали не заменяется выбором самого большого объекта.
- Повторный rough-search заменяет результат без сопоставления со старым и предупреждает о потере ручных правок. Полный повторный поиск holes может найти удалённое отверстие заново.
- Обычные последующие операции не возвращают удалённые/отфильтрованные holes как cutouts или precise holes и не используют их области для reduced-cloud extraction.
- Изменение rough-входов после начала precise-stage требует подтверждённого полного сброса второго этапа без Undo.
- HoleGroup сохраняется на обоих этапах; rough group size не требует одинаковых итоговых precise-диаметров.
- Precise-элемент можно фиксировать и снова разрешать автоматическое изменение. Fixed не меняется автоматически.
- Финальная topology: один outer contour, holes/cutouts без запрещённых пересечений, касаний независимых границ и material islands; C0 и Corner/Tangent semantics сохраняются.
- Автоматическая precise geometry сохраняет материал. Выступы measured scan сами по себе не считаются noise; экспериментальные методы подтверждаются реальными scans.
- Ручные удаления reduced points сохраняются с проектом и не меняют исходный cache. Явное полное перестроение создаёт новый рабочий набор.
- Undo/Redo действует внутри этапов в текущем сеансе; история после открытия не восстанавливается. Project save и autosave/recovery сохраняют согласованное состояние, включая версии карт и ручные изменения.
- При неприменённых precise-изменениях DXF export предупреждает и позволяет отменить экспорт либо явно экспортировать текущую построенную geometry.
- Основной UI первой версии — русский; внутренние identifiers и технические артефакты не обязаны переводиться.

## Technology Stack

```text
Language:       C++20
GUI:            Qt 6.12 Quick / QML, Qt Quick Controls
Raster / Math:  OpenCV 4.x / Eigen
Build:          CMake / MSVC
Target OS:      Windows 10/11 x64
Testing:        GoogleTest
Logging:        spdlog
```

Канонический baseline — [architecture.md, §3](docs/architecture.md#3-технологический-стек). Visual Studio допустима как IDE; источником сборки является CMake. Дискретная GPU не требуется.

## Architecture

QML — presentation layer. Project/domain state, geometry, I/O, обработка данных, Undo/Redo и export реализуются на C++; domain geometry отделена от visual items.

Ответственность подсистем, зависимости и владение состоянием определены в [architecture.md](docs/architecture.md). Логическое разделение не фиксирует физическую структуру каталогов.

## Documentation

| Документ | Назначение |
|---|---|
| [Индекс документации](docs/README.md) | Навигация по основным документам, инструментам, исследованиям, отчётам и benchmark evidence |
| [AGENTS.md](AGENTS.md) | Правила работы Codex |
| [requirements.md](docs/requirements.md) | Требования, ограничения и пользовательское поведение |
| [architecture.md](docs/architecture.md) | Подсистемы, зависимости, состояние и persistence |
| [algorithms.md](docs/algorithms.md) | Вычислительные контракты, Current approach и Open decisions |
| [point-cache-format.md](docs/point-cache-format.md) | Binary layout v1, sort, Writer/Reader, spatial queries и validation задачи 03 (5/5) |
| [implementation-plan.md](docs/implementation-plan.md) | План реализации, границы задач и актуальные статусы |
| [synthetic-cloud-generator.md](docs/tools/synthetic-cloud-generator.md) | Каноническая спецификация вспомогательного генератора: Geometry JSON + Scan Scenario JSON → XYZ/ASC + manifest; DXF/NX не являются его входом |

## Build / Run / Tests

### Prerequisites

- Windows x64 и MSVC с поддержкой C++20; команды выполняются из x64 Developer Command Prompt (окружение штатного `vcvars64.bat`).
- Официальная установка Qt 6.12 с Core, Gui, Qml, Quick и Quick Controls; Qt через vcpkg не устанавливается.
- CMake и Ninja доступны в `PATH` текущего процесса.
- `VCPKG_ROOT` указывает на установленный vcpkg, `QT_ROOT` — на каталог Qt kit, содержащий `bin`, `lib` и `include`.
- При первой настройке нужен сетевой доступ к registry и исходникам зависимостей; последующие настройки используют кэш vcpkg.

Проверенное окружение: Visual Studio 2026, MSVC 19.51.36260.0 / v145 (14.51.36231), Windows SDK 10.0.26100.0, Qt 6.12.0 `msvc2022_64`, CMake 3.30.5, Ninja 1.12.1. Проверки выполнены вне sandbox Codex; глобальный `PATH` не изменяется.

### Dependencies

`vcpkg.json` использует manifest mode и фиксированный `builtin-baseline` из установленного vcpkg registry snapshot. Это закрепляет версии port recipes без отдельных произвольных overrides. Presets подключают toolchain через `VCPKG_ROOT`, используют triplet `x64-windows` и размещают установленные пакеты внутри соответствующего build directory.

Проверены OpenCV 4.12.0#7, Eigen 5.0.1, GoogleTest 1.17.0#3 и spdlog 1.17.0#1. У OpenCV отключены default features, включая ненужные сейчас GUI/video integrations. OpenCV/Eigen пока проверяются отдельным headless dependency smoke; production application использует Qt и spdlog для ограниченного rotating operation log. Новые сторонние зависимости не добавлены.

### Configure / Build / Tests

Из корня репозитория в указанной CMD-среде:

```bat
chcp 65001 >nul
set "PATH=%QT_ROOT%\bin;%PATH%"
cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-release --target CADContour2D import_application_tests asc_xyz_tests cache_tests operation_tests project_tests view_model_tests qml_integration_tests dependency_smoke operation_probe all_qmllint --parallel 4
ctest --preset windows-release -L production --output-on-failure
```

UTF-8 code page нужна в проверенной русскоязычной среде MSVC для корректного распознавания `/showIncludes` в CMake/Ninja. Она меняется только в текущей консоли. Qt `bin` в локальном `PATH` нужен также production tests, использующим Qt runtime.

Debug также фактически проверен:

```bat
cmake --preset windows-debug -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-debug --target CADContour2D import_application_tests asc_xyz_tests cache_tests operation_tests project_tests view_model_tests qml_integration_tests dependency_smoke operation_probe all_qmllint --parallel 4
ctest --preset windows-debug -L production --output-on-failure
```

Задача 02 после исправления R1: **132/132 PASS в Debug и Release** — 32 domain/application/coordinate, 51 operation, 35 view model, 13 production QML integration и один dependency smoke. Production tests и отдельный `operation_probe` включаются при `BUILD_TESTING` независимо от генератора. Фильтр `-L production` исключает его полную регрессию. `all_qmllint` прошёл для production и verification QML в обеих конфигурациях.

Исправление R1 покрыто четырьмя новыми случаями в составе 51 operation test. В Debug и Release шесть целевых регрессий прошли по 50 повторов — **300/300 PASS в каждой конфигурации**; это повторные запуски существующих тестов. Независимый воспроизводитель R1 дал **3/3 PASS**, проверка pending completion — **PASS** в обеих конфигурациях.

QML integration tests загружают тот же статический модуль `CADContour2D` и `Main.qml`, что приложение. Проверяются bindings, Apply/Enter/Undo/Redo/New/Close, draft/error/cursor/selection во время progress и публикации, смена проекта до/после Accept, cancel/failure, shutdown и resize 400×380 / 1000×700. По умолчанию используется offscreen + software + Basic; проверка QML warnings остаётся строгой. Дополнительно выполнен запуск с настоящей Windows-платформой и штатным стилем:

```bat
set "QT_QPA_PLATFORM=windows"
build\windows-debug\qml_integration_tests.exe
build\windows-release\qml_integration_tests.exe
set "QT_QPA_PLATFORM="
```

Результат: **13/13 PASS в каждой конфигурации**. Проверка создаёт и закрывает настоящие тестовые окна. Выполненная визуальная приёмка описана ниже.

Synthetic Point Cloud Generator включается через `-DCADCONTOUR2D_BUILD_TOOLS=ON`. Его ранее проверенная регрессия: 95 generator tests и dependency smoke, 96/96 PASS в каждой конфигурации. В задаче 02 полная generator regression не повторялась, тяжёлые datasets не создавались. Команды и ограничения — в [README инструмента](tools/synthetic_cloud_generator/README.md). Production application не зависит от generator.

### Point Cache foundation — задача 03, реализация 1/5

В той же CMD-среде фактически выполнены для обоих presets:

```bat
cmake --preset windows-debug -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-debug --target cadcontour_cache cache_tests project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
ctest --preset windows-debug -R ^Cache --output-on-failure
ctest --preset windows-debug -L production --output-on-failure

cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-release --target cadcontour_cache cache_tests project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
ctest --preset windows-release -R ^Cache --output-on-failure
ctest --preset windows-release -L production --output-on-failure
```

**43/43 cache tests и 175/175 production tests PASS в каждой конфигурации**
(37 исходных cache + 6 регрессий B1/B2 + 132 прежних). Новый production-код и cache tests собраны с `/W4`
без предупреждений. Проверены independent golden bytes/digests, binary64 edge cases,
overflow, повреждения, short I/O, EOF/zero progress, ошибки seek/flush, rename и
ownership/cleanup, существующий target, Unicode и пробелы. Flush failure вводится
через небольшой test seam; фактическое заполнение диска не выполнялось. Необходимые
для следующих этапов большие cache/spatial performance проверки ещё не проводились.
GUI не изменён; новые тесты используют только Qt Core без GUI application.

### Point Cache streaming ingest и external sort — задача 03, реализация 2/5

`PointStreamSort` принимает batches X/Y/Z, назначает стабильные RecordId, вычисляет
metadata и выдаёт исходные binary64 X/Y в порядке `(Morton128, RecordId)` через
одноразовый callback. Реализованы bounded runs, multi-generation K-way merge,
memory/disk/file budgets, cancellation, structured failures и cleanup. Финальный
полный scratch-файл не требуется. Контракты и ресурсные формулы — в
[point-cache-format.md](docs/point-cache-format.md#streaming-ingest-и-external-sort-25).
Нормативный binary layout v1 не изменён.

В указанной выше CMD-среде выполнены:

```bat
cmake --build --preset windows-debug --target cadcontour_cache cache_tests project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
ctest --preset windows-debug -R ^Cache --output-on-failure
ctest --preset windows-debug -L production --output-on-failure

cmake --build --preset windows-release --target cadcontour_cache cache_tests project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
ctest --preset windows-release -R ^Cache --output-on-failure
ctest --preset windows-release -L production --output-on-failure
```

**78/78 cache tests и 210/210 production tests PASS в каждой конфигурации**
(43 прежних cache + 35 новых). Изменённый C++ и тесты собраны с `/W4` без
предупреждений. Проверены независимые Morton golden words и числовой oracle порядка,
точные ID/биты координат, разные batches и буферы, несколько поколений при fan-in=2,
ограниченные массивы и отсутствие роста directory в RAM. Windows handle counts и
фактические размеры/число scratch-файлов проверены отдельно от плановых счётчиков.
Есть инъекции I/O и `bad_alloc`, реальный open failure, усечение при чтении,
повреждение runs, cancellation по фазам/между поколениями, отказ sink, reentry и
терминальные вызовы. Native handle без share-delete проверяет отказ cleanup,
сохранение первичной ошибки и повторное удаление деструктором.

Реальное исчерпание RAM/заполнение диска не выполнялись. Гигабайтные прогоны,
HDD/seeks performance и сравнение индексов относятся к следующим заданиям.
Полная Synthetic Point Cloud Generator regression повторно не запускалась;
подсистема не затронута. Это результаты реализации 2/5; writer/index добавлены ниже.

### CacheWriter, BBox index и PendingCache — задача 03, реализация 3/5

`CacheWriter` использует существующий `PointStreamSort`, потоково пишет point pages
и строит packed BBox hierarchy. Ограниченный descriptor stream и повторное чтение
предыдущего index level позволяют обойтись одним page buffer и одним массивом
entries. Общий ресурсный бюджет учитывает одновременно живущие sorter и writer.
Проверка собственного результата внутри `StagedFile::publish()` предшествует
единственному rename. `PendingCache` владеет готовым файлом до Discard или явного
`releaseToOwner()`. Формат v1 не изменён; API, ресурсные формулы и границы validation —
в [point-cache-format.md](docs/point-cache-format.md#cachewriter-bbox-hierarchy-и-pendingcache-35).

В указанной выше CMD-среде выполнены configure Debug/Release и:

```bat
cmake --build --preset windows-debug --target cadcontour_cache cache_tests project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
ctest --preset windows-debug -L production --output-on-failure

cmake --build --preset windows-release --target cadcontour_cache cache_tests project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
ctest --preset windows-release -L production --output-on-failure
```

**116/116 cache tests и 248/248 production tests PASS в каждой конфигурации**
(78 прежних cache + 38 новых writer/index). Новый C++ и tests собраны с `/W4` без
предупреждений. Configure сохраняет известные необязательные Qt6TaskTree/Vulkan
diagnostics. Независимый тестовый DFS проверяет бинарное дерево, фактические
records/counts/bounds, все IDs и биты X/Y, включая signed zero/subnormal/adjacent
binary64; проверены 2726/2727 records и 1022/1023 descriptors. Искусственные
descriptors проверяют уровни без миллионов реальных records; такие scratch-файлы
не публикуются как datasets.

Проверены rehashed semantic corruption, multi-generation sorter+writer, writer и
sorter failures после записанного префикса/всего sink, cancellation по фазам,
реальные flush/rename пути с инъекцией flush failure, запрет overwrite, move и
Accept/Discard. Native handles, фактические file lengths/counts и capacities рабочих
буферов проверены отдельно от плановых budgets. Native locks проверяют cleanup
failure, сохранение первичной ошибки и повторное удаление деструктором.

Реальное исчерпание RAM/заполнение диска, peak RSS, большие scans, HDD benchmarks и
power-loss durability не проверялись. Production CacheReader и spatial queries
пока отсутствуют; полного production round-trip нет. GUI не изменён; полная
Synthetic Point Cloud Generator regression повторно не запускалась. Это результаты
этапа 3/5, когда задача 03 оставалась IN PROGRESS и не была принята целиком.

### CacheReader и spatial queries — задача 03, реализация 4/5

Headless `CacheReader` открывает готовые файлы без передачи владения PendingCache,
проверяет envelope/root и выдаёт bounded batches с исходными ID и битами binary64.
`scanAll` читает physical order; `scanBox` и `scanBoxes` используют один packed DFS
с inclusive boundaries и union без повторов. Страницы проверяются перед выдачей.
Отдельная полная validation проверяет весь dataset, реальные bounds/counts и строгое
покрытие ID 0..N-1 через существующий bounded external sort. Формат/Writer не изменены.
Контракты API/ownership/budgets — в [point-cache-format.md](docs/point-cache-format.md#cachereader-и-полная-validation-45).

В приведённой CMD-среде выполнены configure обоих presets и те же build targets,
что для этапа 3/5, затем:

```bat
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
ctest --preset windows-debug -L cache_integration -V
ctest --preset windows-release -L cache_integration -V
```

**292/292 production tests (160 cache, включая 44 новых Reader tests) и отдельный
integration test PASS в каждой конфигурации.** Индекс высоты 2 создан настоящим
Writer: 2 786 009 records, 1023 point pages, 3 index pages, файл 67 248 128 байт.
Локальная область/полоса/union прочитали 2/32/7 point pages вместо 1023;
эталонные ID и биты координат совпали. Рабочие массивы Reader — 245 440 байт,
один handle и два traversal frames. Детальные результаты и ограничения измерений —
в [cache-reader-check.md](docs/reports/task-03/cache-reader-check.md). Новый C++ собран с `/W4`
без предупреждений; configure сохраняет известные Qt diagnostics.

На этапе 4/5 HDD, peak RSS и гигабайтная матрица ещё не проверялись;
результаты последующего этапа — ниже. Application integration относится к задаче 04.

### Reliability и Performance — задача 03, реализация 5/5

Формат **v1 зафиксирован без изменения byte layout**. Полная validation проверяет
глобальный `(Morton128, RecordId)`; fast open сохраняет прежнюю стоимость.
Добавлены две регрессии rehashed перестановок, четыре изолированных crash cases
и проверки предельной высоты packed index без гигантских файлов.

Release HDD матрица: **7/7 PASS**, включая два Writer → Reader → validation цикла
по 42 млн records (1.008 GB point records), budgets 8/128 MiB и пять распределений
по 1.2 млн records. Build — 106.0/62.8 s, validation — 107.5/64.4 s, peak process
Working Set к концу validation — 19.5/139.5 MiB. Local query читает 3 вместо
2672 point pages source-order baseline. OS cache не сбрасывался; это first-pass/
warm measurements, без обещания cold-cache latency или power-loss durability.
Полный отчёт, resource/I/O tables, baseline tradeoffs и raw JSON —
[cache-performance-report.md](docs/reports/task-03/cache-performance-report.md).

В обеих конфигурациях выполнены configure/build изменённых targets, затем
`ctest --preset windows-debug -L production --output-on-failure` и аналогичная
команда для `windows-release`: **298/298 PASS**, в том числе **166 cache tests**.
Отдельный `ctest --preset windows-debug -L cache_integration -V` и аналогичный
Release запуск — **1/1 PASS** в каждой конфигурации; integration не входит в
production label. Новый C++ собран с `/W4` без предупреждений.

Benchmark — отдельный opt-in target, гигабайтные данные не входят в production CTest:

```bat
cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-release --target cache_tests cache_benchmark --parallel 4
build\windows-release\cache_benchmark.exe --help
build\windows-release\cache_benchmark.exe --parent F:\CADContour2D-benchmark --report build\cache-benchmark-smoke.json --points 120000 --memory-mib 8 --distribution multi --repeats 3
```

Используется CMD-среда выше; `--report` требует новый путь, `--parent` — существующий
каталог с достаточным резервом. Tool создаёт уникальный child, освобождает собственные
данные при успехе и сохраняет диагностику при failure. Большие команды — в отчёте.

### Run

Добавление Qt runtime в `PATH` относится только к текущей консоли и её дочерним процессам:

```bat
set "PATH=%QT_ROOT%\bin;%PATH%"
start /wait "" build\windows-release\CADContour2D.exe
```

Проверен и Debug запуск:

```bat
start /wait "" build\windows-debug\CADContour2D.exe
```

Production запускается без проекта и в 04.4A предлагает ASC/XYZ через native FileDialog.
«Новый проект» повторяет выбор; прямой legacy controller-вызов возвращает `ImportRequired`.
Создание пустого проекта из production недоступно. Программный C++ API
`ProjectController::importAscXyz(ImportRequest)` работает в фоне и принимает новый
проект только после успеха. Подробности API и проверок —
[отчёт GUI 04.4A](docs/reports/task-04/asc-xyz-gui-check.md) и
[отчёт 04.2](docs/reports/task-04/asc-xyz-application-check.md). Это запуск из установленного Qt kit,
не deployment package. Приведённые ниже сценарии приёмки задач 01–02 описывают
исторически проверенный каркас; их cell/Undo/verification-инварианты сохранены
в тестах с отдельным friend fixture, отсутствующим в production executable.

В задаче 01 запуск настоящих Debug/Release окон и начальное состояние подтверждены через Windows accessibility; оба executable закрыты с кодом 0, runtime logs пусты. Автоматические QML tests проверили сценарий, resize и закрытие, включая native Windows run. Пользователь подтвердил успешное выполнение всех семи пунктов согласованной ручной приёмки, включая изменение размера окна и закрытие приложения.

### Manual Acceptance — задача 01 (выполнена)

1. Запустить Debug или Release командой выше: `cell = 1`, карта отсутствует, Undo/Redo недоступны.
2. Ввести `0,5`, нажать «Применить»: состояние и поле становятся `0.5`; «Отменить» возвращает `1`, «Повторить» — `0.5`.
3. После Undo проверить `0`, `-1`, `NaN`, `1,2.3`: ошибка, draft сохраняется, cell и redo-ветка не меняются. Ввод `1,0` — no-op; Redo остаётся доступным. Проверить также Enter в поле.
4. «Новый проект»: `1`, новая identity, ревизия `0`, пустая история. Карта всё время отсутствует.
5. Уменьшить и увеличить окно, проверить доступность элементов и читаемость ошибок; закрыть окно. Сообщить о несовпадении значений, кнопок, сохранности ввода, clipping или проблемах закрытия.

### Проверочный запуск и ручная приёмка — задача 02

В той же CMD-среде с Qt bin в PATH:

```bat
build\windows-debug\operation_probe.exe --auto known
build\windows-release\operation_probe.exe --auto known
```

Для обеих конфигураций фактически выполнены все семь `--auto` сценариев: `known`, `unknown`, `failure`, `cancel`, `replace`, `close`, `quit` — **7/7 PASS в Debug и Release**. Все завершились с кодом 0 и Ready shutdown; success получил Accept, остальные — Discard. `operation_probe` — test/dev executable, использующий production Main.qml/controller/runner/shutdown; проверочные кнопки отсутствуют в обычном `CADContour2D.exe`. Он не импортирует облако и не создаёт domain datasets.

Ручной запуск без `--auto`:

```bat
build\windows-release\operation_probe.exe
```

1. «Проверить фоновую операцию»: окно остаётся отзывчивым, после начальной подготовки появляются количество и процент, затем завершение. Во время progress ввести незавершённый draft без Apply, выделить часть текста: уведомления и завершение сохраняют текст/выделение.
2. До запуска ввести `1,2.3`, нажать «Применить», затем запустить обычный сценарий и «Проверить ошибку»: ошибка параметра и draft сохраняются; ошибка операции выводится отдельно.
3. «Неизвестный объём»: счётчик без фиктивного процента и неопределённый progress; во время работы изменить размер окна до 400×380 и обратно, проверить чтение/доступность элементов через scroll.
4. Запустить и нажать «Остановить операцию»: cooperative остановка, результат не применён; после завершения доступен новый запуск.
5. Во время операции применить `0,5`, проверить Undo/Redo: параметр меняется, прежняя операция не публикует результат по старым входам.
6. Во время операции нажать «Новый проект»: новая identity, `cell = 1`, пустая история; прежний worker завершается без записи в новый проект. После завершённой операции New также скрывает её старый статус.
7. В двух отдельных запусках закрыть активную операцию кнопкой «Закрыть» и системным крестиком: окно обрабатывает события до безопасного завершения и затем закрывается. Повторное закрытие не создаёт ошибку.

**Визуальная приёмка: VERIFIED.** Первоначальный независимый ревьюер выполнил полный осмотр основного приложения и `operation_probe`: сценарии A–H, размеры 400×380, 680×600 и 1000×700. После исправления R1 выполнены ограниченный GUI smoke Release `operation_probe` (обычное завершение, повторный запуск, выход с кодом 0) и заключительная независимая проверка затронутых контрактов; полный визуальный осмотр A–H не повторялся. В smoke worker завершился до ручного закрытия; закрытие активной операции после R1 подтверждено native QML tests и `--auto close` / `--auto quit` в Debug/Release.

### Known Diagnostics

- Qt сообщает об отсутствующем `Qt6TaskTree` для необязательного `Qt6QmlAssetDownloaderPrivate`, а также `WrapVulkanHeaders`. Текущий QML-модуль их не использует; сборка, lint и runtime прошли.
- При сборке OpenCV его CMake detection не распознаёт `MSVC_VERSION=1951` и сообщает `Cannot set OpenCV_RUNTIME`. vcpkg собрал x64 библиотеки с `/MD` и `/MDd`; link и dependency smoke test прошли в Release и Debug. Также есть предупреждения стороннего OpenCV о старых CMake policies и metadata `OPENCV_BUILD_INFO_STR`.
- Встроенный загрузчик vcpkg получил SSL error 35 для Eigen и fmt. Официальные архивы загружены системным curl в download cache и проверены по SHA-512 из port recipes; проверка TLS и хэшей не отключалась. Это ограничение загрузки в текущем сетевом окружении, не ошибка компиляции.
- В задаче 02 MSVC сообщает C4702 внутри стороннего `fmt/base.h` при сборке header-only spdlog; compile/link, smoke и runtime прошли в Debug/Release.
- Расхождение Qt 6.12.0 `wait(0)` с полным завершением C++ TLS воспроизведено и покрыто независимым Windows-guard. Подробности и повторяемая проверка — [thread-completion-check.md](docs/reports/task-02/thread-completion-check.md).

## Project Status

**Задачи 01–04: ACCEPTED. Synthetic Point Cloud Generator сохраняет ранее проверенную готовность.**

Задача 04 принята 2026-10-19 после финального независимого ревью:
**REVIEW PASSED — TASK 04 ACCEPTABLE** на HEAD
`a227f3865f5bbd3068eb7e8eb95febc3e2ce241d`. Подтверждённых дефектов P0–P3 и
существенных неподтверждённых рисков — 0; обязательных исправлений нет.
Ключевые проверки, ограничения и решение — в [записи о принятии](docs/reports/task-04/acceptance.md).
Следующий этап — задача 05 (Save/Open и восстановление); реализация не начата.

Задача 02 принята пользователем после реализации, проверок Debug/Release, визуальной приёмки и независимого ревью. Обнаруженный при ревью R1/P1 исправлен; заключительное независимое ревью — **PASS, новых замечаний P0–P3 нет**.

На текущем этапе:

- сформированы функциональные требования;
- реализованы `cadcontour_core` (GUI-независимый `ProjectState`, structured command results, operation stamp и model/view mapping), `cadcontour_application` (единственный владелец состояния, команды и Qt Undo) и переиспользуемый `cadcontour_ui` (QObject view model и production QML);
- реализована основа фоновых операций: `QThread::create()`, один worker, immutable snapshot, OperationId/Stamp, bounded progress mailbox, cooperative cancel/invalidation, structured outcome, production publication gate и Ready → Accept/Discard handshake;
- освобождение worker и shutdown защищены independent native HANDLE + Qt join; Closing сохраняет GUI event loop до Ready, destructor fallback не требует GUI callbacks;
- operation notifications сохраняют draft/error/cursor/selection; отдельный `operation_probe` использует production controller/runner/QML и изолированный verification sink без фиктивного ProjectState dataset;
- `cell` — конечный положительный `double` в мм, по умолчанию `1`; identity и ревизии защищены от переполнения, уведомления публикуются после согласованного изменения состояния и истории, повторный вход отклоняется;
- наличие/совместимость результатов отделены от ревизий операций. Фиктивных datasets или глобального единственного Density Map slot нет; хранилище версий и raster mapping в задаче 01 не реализованы;
- определена архитектура приложения;
- определены алгоритмические контракты и открытые исследовательские решения;
- согласован набор итоговых примитивов LINE/ARC/CIRCLE; созданы synthetic JSON fixtures и базовый generator: validation, horizontal/vertical analytic scan, sampling/maxLineLength, isolated contacts и streaming XYZ/ASC с manifest;
- проверены все 8 geometry × 4 чистых scenarios, включая Horizontal+Vertical без deduplication; все 32 outputs сохранили SHA-256 baseline 0.3.0; реализованы regions/offsets/continuation shifts, overlap/double scan, MissingPoints, OutsideGridCloud, JaggedBoundary, ExtraTableFragment и mixed artifacts; повторные artifact runs детерминированы;
- финальный аудит Debug/Release сохранил все clean hashes и artifact determinism; пять representative cases дали одинаковые bytes между конфигурациями; чистый streaming case повторно создал 41.98 млн points / 1.030 GB, 25.30 с и peak working set 11.38 MiB; artifacted multi-pass smoke — 7.54 млн points / 167 MB, 4.73 с и peak working set 11.40 MiB;
- созданы CMake presets для Debug и Release, vcpkg manifest и минимальное Qt Quick/QML приложение;
- для исходного каркаса ранее проверены configure и clean build; в задаче 01 повторно выполнены configure/build, production tests, regression, dependency smoke, QML lint и запуск обеих конфигураций.

Устойчивые контракты — [architecture.md, §§24–26](docs/architecture.md#24-длительные-операции), состояние задачи — [план реализации](docs/implementation-plan.md#02-безопасное-выполнение-фоновых-операций). При интеграции будущих операций предстоит проверить cancellation checkpoints реальных длительных I/O, атомарность cache/PNG/history publication adapters, version/Rough applicability, lifetime больших datasets и транзакционность сохранения реальных результатов. Это ответственность следующих этапов, а не блокирующие дефекты принятой задачи 02.

Для задачи 03 (5/5) реализован самостоятельный headless `cadcontour_cache`: binary64 codec, зафиксированный v1, checksums/checked layout/file I/O, bounded Morton128 external sort, CacheWriter, packed BBox hierarchy и CacheReader со spatial queries и строгой полной validation. Sealing/readback, no-overwrite rename и PendingCache сохраняют cleanup owner до явного Accept; Reader владеет только read-only handle. Большой round-trip, ресурсы, HDD/source-order comparison и crash lifecycle проверены — [отчёт](docs/reports/task-03/cache-performance-report.md). Интеграция с application state реализована в 04.2. **Статус задачи 03: ACCEPTED**, все пять частей реализации завершены.

Задача 03 принята пользователем после независимого комплексного ревью: **PASS WITH COMMENTS**, без подтверждённых дефектов P0–P2. Единственное замечание R2/P3 относилось к устаревшему перечню Open decisions в [algorithms.md, §45](docs/algorithms.md#45-указатель-основных-open-decisions) и устранено. В ходе завершённого ревью ранее повторены Debug/Release production (**298/298**), cache integration (**1/1**) и benchmark regression (**7/7**) в каждой конфигурации; исходные результаты и команды — в разделах выше и [историческом отчёте](docs/reports/task-03/cache-performance-report.md#исправление-r1p2-после-независимого-ревью). При оформлении принятия сборки, тесты и нагрузочные измерения не повторялись; ранее описанные ограничения производительности, ресурсов и сохранности данных сохраняются.

В 04.1 реализованы headless `cadcontour_io`: потоковый ASC/XYZ parser и
`io::importAscXyz` → существующий CacheWriter → RAII `PendingCache`.
Пустые строки разрешены, непустые требуют ровно три token; malformed/range/I/O
failures и cancellation возвращают structured error. Прямое binary64 conversion
проверяется независимыми рациональными golden pairs; единственный Z Continue/Cancel
пока синхронный. Формат Point Cache v1 и application/GUI не менялись.
Интерфейсы, проверки и ограничения — [отчёт 04.1](docs/reports/task-04/asc-xyz-import-check.md).

Первоначальные проверки 04.1 в Debug и Release: сборка и `all_qmllint` — PASS, production CTest —
**337/337 PASS** (включая **39 новых** parser/importer и **166 cache** cases),
отдельный `-L cache_integration` — **1/1 PASS**. Только новый набор запускается
`ctest --preset windows-debug -R ^AscXyz --output-on-failure` или с
`windows-release`. Это исторические проверки только 04.1; итоговая приёмка всей задачи 04 приведена ниже.

После независимого ревью подтверждены и исправлены R1/R2: fallback при bad_alloc
использует static diagnostic без conversion/truncation; parser принимает только
non-sequential binary QIODevice (обычный QFile/QBuffer) с начальной позицией 0.
Sequential sources отклоняются до чтения. Доказательства, адресные tests и границы
проверки OOM — [отчёт исправлений](docs/reports/task-04/asc-xyz-review-fixes.md).
Повторные Debug/Release проверки: сборка и QML lint — PASS, ASC/XYZ — **42/42**
(включая 3 regression cases), production — **340/340**, cache integration — **1/1**
в каждой конфигурации.

Density map, rough stage, reduced point cloud, precise stage и DXF export пока не реализованы.

В 04.2 реальный import подключён к OperationRunner. PendingCache и файловые guards
остаются в worker; Ready содержит metadata, а необратимый Accept согласован с
noexcept обменом подготовленных ProjectState/QUndoStack. Первый импорт не требует
проекта, повторный требует явного разрешения замены. Новый проект получает новую
identity, `cell = 1 мм` и пустую историю. Source после успеха не требуется.
Принятые caches сохраняются в AppLocalDataLocation даже после Close/Shutdown;
до 05 допускается их накопление без manifest/GC. Ненулевой Z в production пока
безопасно отклоняется, без молчаливого Continue. Подробный технический отчёт —
[asc-xyz-application-check.md](docs/reports/task-04/asc-xyz-application-check.md).

Проверки 04.2 в Debug и Release: сборка и `all_qmllint` — PASS, новые application
import tests — **22/22**, полная production CTest — **364/364** в каждой конфигурации
(включая ASC/XYZ **42**, cache **166**, operation **51**, QML **14** cases).
Адресный запуск: `ctest --preset windows-debug -R ^AscApplication --output-on-failure`
либо с `windows-release`. Dev probe `--auto replace` — PASS в обеих конфигурациях.
Исторический статус реализации 04.2 — **READY FOR INDEPENDENT REVIEW**; часть принята в составе задачи 04.

В 04.3 реализовано межпоточное подтверждение ненулевого Z: worker публикует
единственный типизированный запрос и ждёт Continue/Cancel через condition variable.
Controller предоставляет регистрацию consumer, pending request, уведомление и
защищённый ответ с OperationId/UUID. Cancel, invalidation, Close и shutdown снимают
ожидание; Continue сохраняет отдельный Ready/Accept gate. Без зарегистрированного
consumer ненулевой Z по-прежнему отклоняется с диагностикой, без зависания.
Механизм и полный отчёт — [nonzero-z-confirmation-check.md](docs/reports/task-04/nonzero-z-confirmation-check.md).

Проверки 04.3: Debug/Release build и `all_qmllint` — PASS; confirmation — **29/29**,
полный production CTest — **393/393** в каждой конфигурации. Адресные команды:
`ctest --preset windows-debug -R ^AscConfirmation --output-on-failure` и
`ctest --preset windows-release -R ^AscConfirmation --output-on-failure`.
Исторический статус реализации 04.3 — **READY FOR REVIEW**; GUI подключён в 04.4A.

В 04.4A приложение предлагает ASC/XYZ после готовности окна; отмена оставляет
«Нет открытого проекта», «Новый проект» повторяет выбор. Нативный Windows FileDialog
передаёт QUrl в C++ adapter, который проверяет Windows/UNC paths и URL semantics.
Замена подтверждается отдельно для выбранного файла/контекста; прежний dataset/history
сохраняется до успешного Accept. Настоящий QML Z-dialog использует протокол 04.3;
закрытие означает Cancel, поздние callbacks не применяются. Карточка показывает
принятые count/bounds/Z, Point Cache и `cell = 1 мм`; карта плотности не строится.

GUI profile: 128 MiB рабочего бюджета, 32 handles; worker определяет доступное место
на томе cache и оставляет резерв 64 MiB + 5% остатка. Это не предел RSS или размера
source. Недостаточные ресурсы и I/O failures возвращают structured failure.
Progress показывает реальные укрупнённые фазы с неизвестным total, без общего процента.

Проверки 04.4A в Debug/Release: сборка приложения/test targets и `all_qmllint` — PASS;
полный production CTest — **431/431** в каждой конфигурации, включая **37 новых GUI/UI**
cases и **30 Z-confirmation** cases с новой регрессией пробуждения без polling.
Windows GUI smoke проверен отдельно: native selection/cancel, Unicode/spaces,
import/metadata, replacement, Z и закрытие. Полный отчёт, команды и ограничения —
[asc-xyz-gui-check.md](docs/reports/task-04/asc-xyz-gui-check.md).

Замечания независимого ревью 04.4A R1–R3 воспроизведены и исправлены: безопасные
продолжения ViewModel, повторная проверка выбора перед FileDialog и startup,
привязанный ко времени жизни окна. Добавлен 21 regression case; Debug/Release build,
`all_qmllint` и полный production CTest — **452/452** в каждой конфигурации.
Доказательства, команды и ограничения native smoke —
[asc-xyz-gui-review-fixes.md](docs/reports/task-04/asc-xyz-gui-review-fixes.md).

04.4B проверила native GUI E2E, cancellation/shutdown, безопасные resource failures
и один большой Release import: **41 984 566 точек**, **69.18 с** до native completion/join,
Peak Working Set **222.9 MiB**, итоговый cache **1 010 507 776 байт**.
Полная Reader validation/ID coverage и независимые X/Y bit controls — PASS.
Целевой Writer/Sorter-прогон на HDD также прошёл; OS cache не сбрасывался,
logical I/O не считается physical traffic. Debug/Release build, `all_qmllint`
и production CTest — **452/452** в каждой конфигурации.
Production-код не менялся. Отчёт A–J, методика, точные результаты и ограничения —
[asc-xyz-e2e-performance-check.md](docs/reports/task-04/asc-xyz-e2e-performance-check.md).
Opt-in инструмент доступен при `CADCONTOUR2D_BUILD_TOOLS=ON`:
[import_acceptance](tools/import_acceptance/README.md); большие workloads не входят в CTest.

Проверенные команды после стандартной CMD-инициализации из Build:

```bat
cmake --build --preset windows-debug --target CADContour2D gui_import_tests view_model_tests qml_integration_tests nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests dependency_smoke operation_probe all_qmllint --parallel 4
cmake --build --preset windows-release --target CADContour2D gui_import_tests view_model_tests qml_integration_tests nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests dependency_smoke operation_probe all_qmllint --parallel 4
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
```

Части 04.1–04.4B завершены и приняты; **задача 04 — ПРИНЯТА (ACCEPTED)**.
Финальное независимое ревью — **REVIEW PASSED — TASK 04 ACCEPTABLE**.
По предоставленному результату ревью: Debug/Release production CTest — **452/452 PASS**,
cache integration — **1/1 PASS** в обеих конфигурациях, GUI/UI — **58/58 PASS**,
QML integration — **14/14 PASS**; сборки, `all_qmllint` и Git diff checks — PASS.
При документационном закрытии сборки, тесты и нагрузки не повторялись.
[Запись о принятии](docs/reports/task-04/acceptance.md) фиксирует проверенный commit
и ограничения. В задаче 05 реализована только 05.1 (Persistence Foundation),
ожидающая повторной проверки исправлений H1/H2; пользовательские Save/Open и восстановление ещё не реализованы;
явное построение и версии Density Map относятся к 06. Статусы ACCEPTED и результаты
приёмки задач 01–03 сохраняются.

### Persistence Foundation — 05.1

Добавлен отдельный `cadcontour_persistence`: metadata snapshot, строгий UTF-8 JSON
manifest `.pc2dproj` v1 с точным round-trip чисел, проверка переносимых dataset paths
и Windows reparse points. Новый импорт получает постоянный ProjectId; cell и Undo/Redo
его сохраняют. Shared immutable DatasetLocation удерживает физическое расположение
без удаления принятых cache при Close/замене. Binary Point Cache v1, worker gate и GUI
сохранены. Save/Save As, Open, копирование cache, autosave/recovery ещё не реализованы.

Точная схема и ограничения — [Project Manifest v1](docs/project-manifest-format.md),
результаты проверок — [отчёт 05.1](docs/reports/task-05/persistence-foundation.md).
После независимого ревью `80eff06` исправлены H1/H2 в lexical JSON validation:
неизвестные escapes/raw control bytes и malformed version numbers отклоняются до Qt.
Воспроизведение до исправления и регрессия — в [дополнении к отчёту](docs/reports/task-05/persistence-foundation.md#h1-h2-review-fixes).
Статус исправлений: **H1/H2 FIXED — READY FOR VERIFICATION**; 05.1 ещё не принята,
задача 05 целиком не завершена.

Проверены полные Debug/Release сборки и `all_qmllint`, затем production CTest:
в каждой конфигурации **480 passed, 0 failed, 1 skipped** из 481. Пропущен только
file-symlink test: Windows error 1314, права создания symlink недоступны;
реальный junction escape проверен. Точные команды в стандартной CMD-среде выше:

```bat
cmake --build --preset windows-debug --target all all_qmllint --parallel 4
cmake --build --preset windows-release --target all all_qmllint --parallel 4
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
```

