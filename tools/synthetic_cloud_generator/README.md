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

В проверенной Release-конфигурации CTest выполняет 54 generator tests (27 unit/fixture и 27 integration) и существующий dependency smoke test: 55/55 PASS. Интеграционная матрица запускает все 8 geometry с каждым из 3 базовых scenarios отдельными CLI processes.

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

ARC сохраняет signed sweep (CCW/CW), включая переход через нулевой угол. Используются double, scale-aware coordinate tolerance и отдельный angular tolerance. Общие junctions определяются последовательностью CHAIN; независимые близкие intersections не объединяются по epsilon.

## Output / воспроизводимость

ASCII, без header, `X Y 0`, шесть знаков после точки у X/Y, decimal point и LF независимо от locale. Отрицательный ноль нормализуется. Output rounding имеет погрешность около 0.0000005 мм на coordinate; spacing задаётся до сериализации. Схлопывание положительного interval или material gap из-за output precision приводит к явной ошибке.

Порядок: scan line → interval/contact → segment → point. Используется byte buffer порядка 1 MiB; полного массива cloud нет.

Manifest содержит версию generator/manifest, references и SHA-256 обоих входов, seed, direction/steps/maxLineLength, output format/reference/hash, count и bounds записанных rounded points. Timestamp и temporary paths отсутствуют. Два независимых запуска clean case с одинаковым path context дали byte-identical output и manifest. Гарантия относится к текущему поддерживаемому toolchain; отдельная numeric profile subsystem не вводится. Seed сохраняется, но в базовом этапе randomness отсутствует.

Output и manifest полностью готовятся во временных файлах на том же filesystem. Manifest публикуется первым, output последним; обработанные failures очищают temporary files и откатывают опубликованный manifest при ошибке output rename. Crash между двумя rename может оставить manifest без output: общей crash-atomic транзакции и recovery subsystem нет.

Streaming smoke на rectangle с pointStep 0.07 мм и lineStep 0.01 мм создал 8 581 430 points / 186 509 318 bytes; SHA-256 независимо сверён с manifest. Наблюдаемый peak working set при опросе процесса был около 11.0 MiB против 10.0 MiB для 87 230 points. Это smoke, не benchmark; generated files удалены и в Git не хранятся.

## Следующий этап

Не реализованы shifted/continuation shifts, pass regions, overlap/double scan, Outside Grid Cloud, Jagged Boundary, Extra Table Fragment и mixed artifacts. Любое `passes` и непустой `defects` явно отклоняются как `unsupported in current generator implementation`; пустой `defects` допустим.

Не реализованы cancellation, DXF и GUI. Используется штатный Qt JSON parser без отдельной проверки duplicate keys; входные файлы должны иметь уникальные keys. Synthetic datasets не подтверждают методы CADContour2D на representative real scans.
