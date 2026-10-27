# Density Map storage experiment — 06.0B

Два ограниченных адаптера к [стенду 06.0A](../README.md). Здесь нет production
Density Map, PNG, ProjectState, Save/Open/Recovery, GUI или растеризации 06.0C.
Рекомендация эксперимента не изменяет обязательный PNG-контракт Rough Stage.
[Результаты и ограничения](../../../docs/reports/task-06/density-formats.md).

## Сборка

Отдельный CMake project, MSVC x64/C++20, Qt Core 6.12.0, zlib **1.3.2#1**,
libtiff **4.7.2** (`core,zip`, без libdeflate), GoogleTest **1.17.0#3**.
Production manifest и targets этих зависимостей не получают. `vcpkg.json`
закрепляет тот же registry baseline, что основной проект.

В этом окружении bundled Visual Studio vcpkg пытался обновлять registry HEAD
при установке нового manifest. Два долгих fetch остановлены. Установка выполнена
из **неизменённых port recipes того же закреплённого commit**, уже доступного
в локальном registry. Проверенная подготовка из корня в PowerShell:

```powershell
$snapshotDir = Join-Path (Get-Location) 'build/density-06b-port-snapshot'
New-Item -ItemType Directory -Path $snapshotDir -ErrorAction Stop | Out-Null
git --git-dir="$env:LOCALAPPDATA/vcpkg/registries/git/.git" archive --format=tar --output="$snapshotDir/ports.tar" 1460b31b08c42cc2e9ac2c79f45ec8707e2675e2 ports/tiff ports/zlib ports/gtest ports/vcpkg-cmake ports/vcpkg-cmake-config
tar -xf "$snapshotDir/ports.tar" -C $snapshotDir
$manifest = Get-Content tools/density_benchmark/formats/vcpkg.json -Raw | ConvertFrom-Json
$manifest.PSObject.Properties.Remove('builtin-baseline')
$manifest | ConvertTo-Json -Depth 10 | Set-Content "$snapshotDir/vcpkg.json" -Encoding utf8
'{"default-registry":null,"overlay-ports":["ports"]}' | Set-Content "$snapshotDir/vcpkg-configuration.json" -Encoding utf8
& 'D:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg\vcpkg.exe' install --triplet x64-windows "--x-manifest-root=$snapshotDir" --x-install-root=build/density-06b-deps --x-buildtrees-root=build/density-06b-buildtrees --x-packages-root=build/density-06b-packages
```

Каталог snapshot должен быть новым. На другом компьютере нужны соответствующие
пути и доступный registry commit; это локальная процедура, не установка системных
инструментов. TIFF source archive проверяется штатным SHA-512 port recipe.

Configure/build/test проверены для **Debug и Release**; ниже Release, для Debug
заменить `density-06b-release` на `density-06b-debug` и `Release` на `Debug`.
Команды CMD после штатной инициализации из корневого Build README:

```bat
cmake -S tools/density_benchmark/formats -B build/density-06b-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR="%CD%/build/density-06b-deps" -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_PREFIX_PATH="%QT_ROOT%"
cmake --build build/density-06b-release --parallel 4
ctest --test-dir build/density-06b-release -L density_formats --output-on-failure
```

Также проверяются обычные `density_benchmark_tests`, label `density_experiment`,
в обеих основных конфигурациях: общие `Spec`/interchange принимают side до 1024,
CSV содержит candidate, статистика разделяет кандидатов и добавляет nearest-rank
p95. Defaults и прежние сценарии 06.0A сохранены.

## D: экспериментальный count-tile v1

`count_container.cpp`: little-endian файл с 96-byte header, независимыми zlib
streams (Deflate level 6, default strategy/window), затем индексом по Y/X.
Header: magic `D06BCT01`, uint32 version/countBits, uint64 width/height,
uint32 side/reserved, uint64 entryCount/indexOffset/fileBytes, uint32 headerCRC/
indexCRC и 24 нулевых reserved bytes. Header CRC считается со своим полем = 0.

Запись индекса — 40 bytes: uint64 rasterOrdinal/offset/compressedLength/rawLength,
uint32 decodedCRC/reserved. Counts — row-major LE uint32/uint64, без padding за
краями карты. CRC32 — обнаружение случайных ошибок, не аутентификация.

В индексе только ненулевые тайлы; отсутствующая запись однозначно означает zero.
Offsets обязаны описывать непрерывные непересекающиеся payloads между header и
index; ordinals строго возрастают. Проверяются file length, checksums, размеры,
reserved fields, overflow, resource budget, точные потреблённая и распакованная
длины. Opening читает header/index, а не raster. Тайл ищется binary search.
Индекс находится в RAM, O(число сохранённых тайлов); paged index не реализован.
Immutable означает отсутствие API редактирования после завершения записи.

## E: узкий TIFF/BigTIFF profile

`tiff_adapter.cpp`: libtiff `TIFFClientOpenExt` над тем же unbuffered QFile,
с ограничениями single/cumulated allocations и per-handle diagnostics.
Writer: `TIFFWriteEncodedTile`, `TIFFWriteDirectory`, `TIFFFlush`, `TIFFClose`,
затем проверенный QFile flush/close. BigTIFF `w8l` в основных замерах; классический
TIFF `wl` проверяется отдельно. Deflate level 6, zlib subcodec, predictor **1**,
один unsigned sample на pixel, 32 или 64 bits; padding краевых тайлов — zero.
Не используется RGBA/image conversion API. Row 0 соответствует логической строке
0 стенда; это не утверждение production raster orientation.

**Строгий reader** использует `TIFFReadRawTile` и `uncompress2`: проверяет полный
zlib stream, checksum, точные input/output lengths, затем byte order и counts.
Причина: native `TIFFReadEncodedTile` в libtiff 4.7.2 может вернуть полный буфер,
не достигнув Z_STREAM_END, если Deflate trailer укорочен. Это воспроизведено
регрессионным тестом до исправления адаптера; `tif_zip.c::ZIPDecode` подтверждает
причину. Native API отдельно проверен на точном UINT64_MAX. Таким образом,
основное сравнение включает строгую проверку обоих кандидатов; E — не замер
штатного TIFF decoder без дополнительного контроля.

Все логические тайлы записываются, включая нулевые. Отсутствующий offset/length
считается повреждением, zero substitution не используется. Open валидирует
metadata, все offsets/lengths, порядок и границы; built-in integrity checksum
для всего IFD/index у TIFF нет. Reader поддерживает только этот profile, а не
произвольные TIFF. Исправление осмысленной metadata другим осмысленным значением
не всегда обнаруживается без внешней identity/checksum.

Image width/height и номера тайлов API — uint32 даже в BigTIFF; offsets — uint64
(этот QFile adapter ограничен signed qint64 file positions).
Tile side кратен 16. Массивы offsets/counts пропорциональны **логическому** числу
тайлов. Huge-sparse стенда превышает uint32 tile count и безопасно отклоняется
до создания TIFF. Поддержка 64-bit samples подтверждена отдельно и не выводится
из BigTIFF. Никакого lossy преобразования counts нет.

## Запуски и метрики

`density_formats_benchmark --output-dir NEW` создаёт только новый каталог,
поддерживает `--help` в console без GUI message box. `--scenario`, `--seed` и
генератор — прежние; `--width/--height` задают одинаковые ordinary dimensions
независимо от side. Huge-sparse всегда сохраняет исходные размеры 06.0A.

`run-matrix.ps1` воспроизводит выбранную матрицу (3/12/48 млн cells; 300 млн
пропущены после оценки стоимости повторной генерации). В Qt/MSVC CMD-среде:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools/density_benchmark/formats/run-matrix.ps1 -Executable build/density-06b-release/density_formats_benchmark.exe -OutputDirectory build/density-06b-matrix -StorageLabel "D: Hitachi HDS721050CLA362 SATA HDD"
```

Повторение требует другого output directory. Large runs не входят в CTest.
`audit-results.ps1` независимо сверяет сохранённые JSON/CSV, paired read coverage/
requests/sums и fingerprints, копирует новые raw files без overwrite и создаёт
manifest SHA-256. Проверенная команда из корня:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/density_benchmark/formats/audit-results.ps1 -InputDirectory build/density-06b-matrix -EvidenceDirectory docs/benchmarks/density06b
```

Она требует нового `audit.json` и существующего выбранного matrix.json в evidence
directory. Выполненный audit: 12 серий, 2363 ok + 7 ожидаемых unsupported rows.

Каждый dataset/candidate/repetition выполняется в новом процессе; порядок D/E
чередуется по повторениям. Каждый trial пишет **новый файл**, затем читает его.
First — первый такой trial, subsequent — следующие, а не чтение того же filename.
Cold cache не заявляется. Verification и предыдущие фазы могут прогревать cache.

Измеряются source generation, write/finalize, open/index, full source/logical
read, отдельные тайлы, small ROI, central large ROI, 128 повторных tile requests
с deterministic permutation, physical-only traversal. Генерация и packing входят
в write; отдельная generation baseline позволяет оценить их вклад. Read sink
считает cells/tiles/sum, без full-raster materialization. Verification вне timing
повторно читает каждый результат, сравнивает все значения/форму и порядок с
общим `generate`; write verification также сравнивает канонический fingerprint.
Independent literal/formula/golden controls и crop oracle проверяются в tests.

Full обычных сценариев включает implicit zero, physical — только реальные
payloads. Для huge-sparse Full/ROI по контракту ReadPlan перечисляют только четыре
source tiles: complement zero пропускается. **Полный логический обход 10^18 ячеек
не выполнен** и не выдаётся за быстрый full scan; explicit zero tile измеряется.

Используется только прежний `measure()/preflight()/summary()`. FileIo теперь
покрывает QFile calls обоих адаптеров, включая client callbacks TIFF; определения
в строках старого measurement layer всё ещё описывают fixture wrappers.
QIODevice::Unbuffered не отключает OS cache. GetProcessIoCounters — process
logical I/O, physical disk I/O неизвестен.
PeakWorkingSetSize — максимум за жизнь worker; RAM snapshots не phase peaks.
Allocation trace отсутствует (`null`), известны оценка плана и размеры buffers/
index; точное число/пик дополнительных allocations не измерены. p50 = median,
p95 = nearest rank; при четырёх subsequent trials p95 равен max.

Limits cooperative: RAM/disk preflight, per-tile cancellation/working-set checks,
library allocation limits и последующие проверки retained output. Это не Job
Object/OS hard cap. План включает несжимаемые payloads, индексы, buffers и запас;
не является обещанием пика системной commit memory. Partial output после failure
сохраняется для диагностики в собственном каталоге и не становится успешным
dataset. Предыдущие evidence/source/project/cache/Recovery не изменяются.
