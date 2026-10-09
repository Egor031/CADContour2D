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
| [AGENTS.md](AGENTS.md) | Правила работы Codex |
| [requirements.md](docs/requirements.md) | Требования, ограничения и пользовательское поведение |
| [architecture.md](docs/architecture.md) | Подсистемы, зависимости, состояние и persistence |
| [algorithms.md](docs/algorithms.md) | Вычислительные контракты, Current approach и Open decisions |
| [synthetic-cloud-generator.md](docs/tools/synthetic-cloud-generator.md) | Каноническая спецификация вспомогательного генератора: Geometry JSON + Scan Scenario JSON → XYZ/ASC + manifest; DXF/NX не являются его входом |
| [nx-dxf-fixture-analysis.md](docs/nx-dxf-fixture-analysis.md) | Исторический фактический анализ экспорта NX; не контракт генератора |

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

Проверены OpenCV 4.12.0#7, Eigen 5.0.1, GoogleTest 1.17.0#3 и spdlog 1.17.0#1. У OpenCV отключены default features, включая ненужные сейчас GUI/video integrations. Эти библиотеки используются только в отдельном headless dependency smoke test; production executable зависит от Qt.

### Configure / Build / Tests

Из корня репозитория в указанной CMD-среде:

```bat
chcp 65001 >nul
set "PATH=%QT_ROOT%\bin;%PATH%"
cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=OFF
cmake --build --preset windows-release
ctest --preset windows-release
cmake --build --preset windows-release --target all_qmllint
```

UTF-8 code page нужна в проверенной русскоязычной среде MSVC для корректного распознавания `/showIncludes` в CMake/Ninja. Она меняется только в текущей консоли. Qt `bin` в локальном `PATH` нужен также production tests, использующим Qt runtime.

Debug также фактически проверен:

```bat
cmake --preset windows-debug -DCADCONTOUR2D_BUILD_TOOLS=OFF
cmake --build --preset windows-debug
ctest --preset windows-debug
cmake --build --preset windows-debug --target all_qmllint
```

Без development tools CTest запускает 72 теста: 32 domain/application/coordinate, 34 view model, 5 production QML integration и один dependency smoke. Release: 72/72 PASS; Debug: 67 headless/smoke и затем 5/5 QML PASS. Итоговая полная регрессия с tools ниже также проверяет все production tests в обеих конфигурациях. Они включаются при `BUILD_TESTING` независимо от генератора. `all_qmllint` прошёл в Debug и Release.

QML integration tests загружают тот же статический модуль `CADContour2D` и `Main.qml`, что приложение. Проверяются bindings, Apply/Enter/Undo/Redo/New/Close handlers, ошибочный draft, required injection и resize 400×380 / 1000×700. По умолчанию они используют offscreen + software + Basic: Windows NativeStyle в offscreen выдаёт диагностику размеров TextField. Проверка QML warnings остаётся строгой. Дополнительно проверено выполнение с настоящей Windows-платформой и её штатным стилем:

```bat
set "QT_QPA_PLATFORM=windows"
build\windows-debug\qml_integration_tests.exe
build\windows-release\qml_integration_tests.exe
set "QT_QPA_PLATFORM="
```

Результат: 5/5 PASS в каждой конфигурации. Такая проверка создаёт и закрывает настоящие тестовые окна, но не заменяет визуальную пользовательскую приёмку.

Synthetic Point Cloud Generator включается через `-DCADCONTOUR2D_BUILD_TOOLS=ON`. Его Debug/Release configure/build, 95 generator tests и существующий dependency smoke проверены: 96/96 PASS в каждой конфигурации; QML lint также прошёл. Команды и ограничения — в [README инструмента](tools/synthetic_cloud_generator/README.md). Production application не зависит от generator.

После задачи 01 повторно выполнены `cmake --preset windows-debug -DCADCONTOUR2D_BUILD_TOOLS=ON` и аналогичный Release configure, build, CTest и `all_qmllint`: **167/167 PASS в каждой конфигурации** (71 production + 95 generator + smoke). Гигабайтные облака при этой регрессии не создавались.

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

Приложение создаёт новый C++-проект с `cell = 1 мм`. Окно показывает параметры, runtime identity, ревизию входов, отсутствие Density Map; доступны изменение `cell`, Undo/Redo и новый проект. Точка и запятая разрешены, ошибочный draft остаётся в поле. Изменения параметра не запускают вычислений. Это запуск из установленного Qt kit, не deployment package.

В задаче 01 запуск настоящих Debug/Release окон и начальное состояние подтверждены через Windows accessibility; оба executable закрыты с кодом 0, runtime logs пусты. Автоматические QML tests проверили сценарий, resize и закрытие, включая native Windows run. Полная визуальная приёмка ожидается: Computer Use capture завершился `FrameArrived timed out` / `window capture timed out`; click не выполнился из-за отсутствия geometry capture. Эти ограничения не выдаются за успешный ручной сценарий.

### Manual Acceptance — задача 01

1. Запустить Debug или Release командой выше: `cell = 1`, карта отсутствует, Undo/Redo недоступны.
2. Ввести `0,5`, нажать «Применить»: состояние и поле становятся `0.5`; «Отменить» возвращает `1`, «Повторить» — `0.5`.
3. После Undo проверить `0`, `-1`, `NaN`, `1,2.3`: ошибка, draft сохраняется, cell и redo-ветка не меняются. Ввод `1,0` — no-op; Redo остаётся доступным. Проверить также Enter в поле.
4. «Новый проект»: `1`, новая identity, ревизия `0`, пустая история. Карта всё время отсутствует.
5. Уменьшить и увеличить окно, проверить доступность элементов и читаемость ошибок; закрыть окно. Сообщить о несовпадении значений, кнопок, сохранности ввода, clipping или проблемах закрытия.

### Known Diagnostics

- Qt сообщает об отсутствующем `Qt6TaskTree` для необязательного `Qt6QmlAssetDownloaderPrivate`, а также `WrapVulkanHeaders`. Текущий QML-модуль их не использует; сборка, lint и runtime прошли.
- При сборке OpenCV его CMake detection не распознаёт `MSVC_VERSION=1951` и сообщает `Cannot set OpenCV_RUNTIME`. vcpkg собрал x64 библиотеки с `/MD` и `/MDd`; link и dependency smoke test прошли в Release и Debug. Также есть предупреждения стороннего OpenCV о старых CMake policies и metadata `OPENCV_BUILD_INFO_STR`.
- Встроенный загрузчик vcpkg получил SSL error 35 для Eigen и fmt. Официальные архивы загружены системным curl в download cache и проверены по SHA-512 из port recipes; проверка TLS и хэшей не отключалась. Это ограничение загрузки в текущем сетевом окружении, не ошибка компиляции.

## Project Status

**Задача 01 технически реализована; статус AWAITING MANUAL ACCEPTANCE. Synthetic Point Cloud Generator сохраняет готовность по текущему контракту clean/artifact scenarios.**

На текущем этапе:

- сформированы функциональные требования;
- реализованы `cadcontour_core` (GUI-независимый `ProjectState`, structured command results, operation stamp и model/view mapping), `cadcontour_application` (единственный владелец состояния, команды и Qt Undo) и переиспользуемый `cadcontour_ui` (QObject view model и production QML);
- `cell` — конечный положительный `double` в мм, по умолчанию `1`; identity и ревизии защищены от переполнения, уведомления публикуются после согласованного изменения состояния и истории, повторный вход отклоняется;
- наличие/совместимость результатов отделены от ревизий операций. Фиктивных datasets или глобального единственного Density Map slot нет; хранилище версий и raster mapping в задаче 01 не реализованы;
- определена архитектура приложения;
- определены алгоритмические контракты и открытые исследовательские решения;
- согласован набор итоговых примитивов LINE/ARC/CIRCLE; созданы synthetic JSON fixtures и базовый generator: validation, horizontal/vertical analytic scan, sampling/maxLineLength, isolated contacts и streaming XYZ/ASC с manifest;
- проверены все 8 geometry × 4 чистых scenarios, включая Horizontal+Vertical без deduplication; все 32 outputs сохранили SHA-256 baseline 0.3.0; реализованы regions/offsets/continuation shifts, overlap/double scan, MissingPoints, OutsideGridCloud, JaggedBoundary, ExtraTableFragment и mixed artifacts; повторные artifact runs детерминированы;
- финальный аудит Debug/Release сохранил все clean hashes и artifact determinism; пять representative cases дали одинаковые bytes между конфигурациями; чистый streaming case повторно создал 41.98 млн points / 1.030 GB, 25.30 с и peak working set 11.38 MiB; artifacted multi-pass smoke — 7.54 млн points / 167 MB, 4.73 с и peak working set 11.40 MiB;
- созданы CMake presets для Debug и Release, vcpkg manifest и минимальное Qt Quick/QML приложение;
- для исходного каркаса ранее проверены configure и clean build; в задаче 01 повторно выполнены configure/build, production tests, regression, dependency smoke, QML lint и запуск обеих конфигураций.

Автоматические проверки задачи 01 и регрессия успешны; визуальная пользовательская приёмка остаётся обязательной. Задача 02 не начата.

Import, cache, density map, rough stage, reduced point cloud, precise stage и DXF export пока не реализованы.

