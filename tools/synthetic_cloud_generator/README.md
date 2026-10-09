# Synthetic Point Cloud Generator

Консольный development/test tool: отдельные Geometry JSON и Scan Scenario JSON преобразуются в XYZ/ASC и manifest. Он не входит в `CADContour2D.exe` и не передаёт ground truth основной программе.

Входной контракт и дальнейший объём — [каноническая спецификация](../../docs/tools/synthetic-cloud-generator.md). Согласованные примеры — [synthetic fixtures](../../tests/fixtures/synthetic/README.md).

## Build / Tests

Используется общий CMake-проект, C++20, MSVC x64, установленный Qt 6.12 и уже существующий GoogleTest. Новые vcpkg dependencies не добавлены. Вычисления и runtime инструмента требуют только standard library и Qt Core; общий configure проекта по-прежнему требует принятый Qt Quick kit.

Из корня repository в x64 Developer Command Prompt, с `QT_ROOT` и `VCPKG_ROOT` по [основному README](../../README.md):

```bat
chcp 65001 >nul
cmake --preset windows-release -DCADCONTOUR2D_BUILD_TOOLS=ON
cmake --build --preset windows-release
set "PATH=%QT_ROOT%\bin;%PATH%"
ctest --preset windows-release --output-on-failure
```

Эти configure/build/test операции проверены на toolchain проекта: MSVC 19.51, Qt 6.12.0, CMake 3.30.5 и Ninja 1.12.1. При выключенном `CADCONTOUR2D_BUILD_TOOLS` generator targets отсутствуют. Debug инструмента пока отдельно не проверен.

Targets:

- `synthetic_generator` — внутренняя static library;
- `synthetic-cloud-generator` — console executable;
- `synthetic_generator_tests` — GoogleTest executable при `BUILD_TESTING=ON`.

В проверенной Release-конфигурации CTest выполняет 67 generator tests (30 unit/fixture и 37 integration) и существующий dependency smoke test: 68/68 PASS. Интеграционная матрица запускает все 8 geometry с каждым из 4 чистых scenarios отдельными CLI processes. SHA-256 всех 24 прежних geometry × scan outputs сравнивались с generator 0.2.0 и сохранились; hashes трёх rectangle cases закреплены regression test.

## CLI

```text
synthetic-cloud-generator --geometry <file> --scan <file> --output <file.xyz|file.asc>
synthetic-cloud-generator --help
synthetic-cloud-generator --version
```

Формат выбирается расширением `.xyz` или `.asc`; manifest всегда записывается рядом как `<output>.manifest.json`. Отдельные `--format`, `--manifest` и overrides seed/steps не поддерживаются.

Проверенные базовые runs, из корня repository:

```bat
build\windows-release\synthetic-cloud-generator.exe --geometry tests\fixtures\synthetic\geometry\rectangle.json --scan tests\fixtures\synthetic\scans\clean_horizontal.json --output build\rectangle-horizontal.xyz
build\windows-release\synthetic-cloud-generator.exe --geometry tests\fixtures\synthetic\geometry\rectangle.json --scan tests\fixtures\synthetic\scans\clean_vertical.json --output build\rectangle-vertical.asc
build\windows-release\synthetic-cloud-generator.exe --geometry tests\fixtures\synthetic\geometry\rectangle.json --scan tests\fixtures\synthetic\scans\short_segments_horizontal.json --output build\rectangle-short.xyz
```

Фактические runs использовали отдельный temporary subdirectory в `build`; количества points соответственно 87 230, 86 759 и 87 413. Для повторного запуска нужен новый target path: существующий output или manifest не перезаписываются.

Exit codes: `0` — success, `2` — CLI error, `3` — invalid input/unsupported feature, `4` — I/O error, `5` — numerical/resource failure. Success summary пишется в stdout, diagnostics — в stderr с файлом и field path, когда он известен.

## Поддерживаемый этап

- `formatVersion=1`, mm; CHAIN из LINE/ARC или самостоятельный CIRCLE.
- Closure, self-intersection/overlap, independent boundary intersection/touch, containment и отсутствие nested inner contours.
- Horizontal/Vertical analytic scan, subtraction strict interiors, boundary runs и isolated contacts.
- Положительный segment: `ceil(length/pointStep)+1` points, включая endpoints; любой положительный короткий хвост сохраняется. Isolated contact даёт одну point.
- `maxLineLength` и один общий seam соседних segments; без global deduplication.
- `clean_horizontal`, `clean_vertical`, `short_segments_horizontal`.
- `clean_horizontal_vertical`: два полных независимых прохода, сначала Horizontal, затем Vertical; общие pointStep 0.07 мм и lineStep 1 мм, без shifts/regions/defects/randomness.

Необязательный непустой `passes` задаёт порядок проходов. Объект pass допускает только `direction: "Horizontal"|"Vertical"` или `{}` с наследованием корневого direction; положительные шаги всегда наследуются. Отсутствие passes сохраняет прежний один проход. Повторно измеренные совпадающие точки сохраняются без deduplication.

Проверенный запуск, после создания `build\clean-clouds`:

```bat
build\windows-release\synthetic-cloud-generator.exe --geometry tests\fixtures\synthetic\geometry\complex_part.json --scan tests\fixtures\synthetic\scans\clean_horizontal_vertical.json --output build\clean-clouds\10_complex_horizontal_vertical.xyz
```

Для complex part получено 335 856 points = 168 374 Horizontal + 167 482 Vertical. Multi-pass output побайтово равен конкатенации независимых outputs; два запуска с одинаковым path context дали одинаковые output и manifest. Для ручной проверки в `build\clean-clouds` также сгенерированы все восемь geometry в Horizontal и complex part в Vertical. Outputs и manifests в Git не хранятся.

ARC сохраняет signed sweep (CCW/CW), включая переход через нулевой угол. Используются double, scale-aware coordinate tolerance и отдельный angular tolerance. Общие junctions определяются последовательностью CHAIN; независимые близкие intersections не объединяются по epsilon.

## Output / воспроизводимость

ASCII, без header, `X Y 0`, шесть знаков после точки у X/Y, decimal point и LF независимо от locale. Отрицательный ноль нормализуется. Output rounding имеет погрешность около 0.0000005 мм на coordinate; spacing задаётся до сериализации. Схлопывание положительного interval или material gap из-за output precision приводит к явной ошибке.

Порядок: pass → scan line → interval/contact → segment → point. Используется byte buffer порядка 1 MiB; полного массива cloud нет.

Manifest содержит версию generator/manifest, references и SHA-256 обоих входов, seed, default direction/steps/maxLineLength, `passes` с фактическими directions/counts, output format/reference/hash, общий count и bounds записанных rounded points. Timestamp и temporary paths отсутствуют. Два независимых запуска clean case с одинаковым path context дали byte-identical output и manifest. Гарантия относится к текущему поддерживаемому toolchain; отдельная numeric profile subsystem не вводится. Seed сохраняется, но в чистом этапе randomness отсутствует.

Output и manifest полностью готовятся во временных файлах на том же filesystem. Manifest публикуется первым, output последним; обработанные failures очищают temporary files и откатывают опубликованный manifest при ошибке output rename. Crash между двумя rename может оставить manifest без output: общей crash-atomic транзакции и recovery subsystem нет.

## Большой чистый performance case

Компактные inputs в `tests/fixtures/synthetic/performance/`: пластина 3000 × 1000 мм, углы R50, два отверстия Ø200 с центрами (750, 500) и (2250, 500). Horizontal, pointStep 0.07 мм, lineStep 1 мм, maxLineLength 200 мм; никаких artifacts, shifts или randomness.

Проверенный запуск, после создания `build\clean-performance`:

```bat
build\windows-release\synthetic-cloud-generator.exe --geometry tests\fixtures\synthetic\performance\large_plate.geometry.json --scan tests\fixtures\synthetic\performance\clean_horizontal.scan.json --output build\clean-performance\large_plate.xyz
```

Фактическая проверка generator 0.3.0 на Release toolchain проекта, 2026-10-09:

- 41 984 566 points; 1 029 576 096 bytes (1.030 GB, 0.959 GiB).
- Генерация с запуском процесса и публикацией manifest: 23.37 с; последующая независимая проверка в это время не включена.
- Наблюдаемый peak working set: 11 948 032 bytes (11.4 MiB), через `Process.PeakWorkingSet64` при опросе каждые 25 мс. Sampled peak private memory: 6 549 504 bytes (6.25 MiB).
- Для малого rectangle (87 230 points, 1 896 098 bytes) тем же методом наблюдалось 6 242 304 bytes working set: увеличение output примерно в 543 раза не вызвало пропорционального роста RAM. Это smoke measurement, не универсальная гарантия времени или benchmark hardware.
- Независимый потоковый подсчёт подтвердил 41 984 566 ASCII records; hashes обоих inputs и output совпали с manifest.
- Output SHA-256: `75810e6e316d4ef4a0dafc4c8220b3749abe3d97f34d923a029c82ac3e4fdd8c`.

Большой XYZ, manifest и measurements оставлены в `build\clean-performance` для локальных проверок; они не коммитятся. Обычные unit/integration tests проверяют geometry/config и характерные intervals, но не создают гигабайтный dataset.

## Следующий этап

Не реализованы shifted/continuation shifts, lineOffset, pass regions, overlap/double scan, MissingPoints, Outside Grid Cloud, Jagged Boundary, Extra Table Fragment и mixed artifacts. Наличие любого такого поля pass и непустой `defects` явно отклоняются как `unsupported in current generator implementation`; пустой `defects` допустим. Чистый полный multi-pass не реализует отдельную механику overlap или локального repeat/double scan.

Не реализованы cancellation, DXF и GUI. Используется штатный Qt JSON parser без отдельной проверки duplicate keys; входные файлы должны иметь уникальные keys. Synthetic datasets не подтверждают методы CADContour2D на representative real scans.
