# 06.2 — каноническое хранилище Density Map D v1

2026-10-29. **READY FOR INDEPENDENT REVIEW**. Реализован headless storage API;
собственные проверки не являются независимым ревью или пользовательской приёмкой.
06.1 остаётся ACCEPTED WITH LIMITATIONS; её исторический отчёт не изменён.
Point Cache→C1→D и приложение не подключались (06.3–06.8).

## A. Baseline

- Checkout `D:/ZherlitsynEE/CADContour2D`, `main`, HEAD
  `1f5ce9028d46ed781ce616a21fa4fbc3d3509998`, исходный Git status чистый.
  Контрольный HEAD совпал, родительских AGENTS.md на D:/ и D:/ZherlitsynEE нет.
- Прочитаны AGENTS/README/docs index, plan 06.1–06.8, density/mapping/storage/
  resources/lifecycle contracts и отчёты 06.1, 06.0B, P1. Изучены cache binary/file/
  writer/reader/sort patterns, tests/CMake. Экспериментальный D06BCT01 использован
  как исследовательский материал, его layout/ordinal/RAM-index не перенесены.
- C++20/MSVC x64: MSVC 19.51.36260, toolset 14.51.36231, SDK 10.0.26100.0;
  Qt 6.12.0, zlib 1.3.2#1 из pinned vcpkg, GoogleTest 1.17.0#3,
  CMake 3.30.5, Ninja 1.12.1. Использован существующий локальный
  `build/task02-env.cmd` (vcvars64, UTF-8, process-local PATH), глобальная среда не менялась.
- Известный baseline 06.1: Debug 579 PASS / 0 FAIL / 1 SKIP;
  Release 578 PASS / 1 FAIL / 1 SKIP. Recovery `/6` с NoChange/Applied ранее FAIL;
  symlink SKIP связан с Windows privilege 1314. Recovery не исправлялся.

## B. Реализация и API

Единственный нормативный layout — [density-map-format.md](../../density-map-format.md).
Новые `src/density/density_storage.h/.cpp`, target `cadcontour_density`:

- `Writer::create`, `addTile`, `finish → PendingDataset`;
- `Reader::openReader`, проверенные `metadata()/mapping()`, `readTile`,
  borrowed-callback `forEachTile`, `fullValidate`, `statistics()`;
- отдельные DatasetId, Metadata/BuildContract, TileKey/Shape/Tile,
  ResourcePolicy и structured Result/Error. Units mm, stable mapping wire-ID=1.

Header/sections/index/completion имеют фиксированные LE widths и CRC-32/ISO-HDLC;
metadata doubles сохраняются побитово. UUID RFC 4122, собственный dataset identity,
source CacheId и versioned SuppliedTiles/CompletePointCache contract.
Mapping один раз восстанавливается factory при open с явной ValidationPolicy;
origin/minimum bits, dimensions и convention сверяются. Factory/FP-контракт 06.1
и `/fp:strict` не изменены. CompletePointCache дополнительно требует
total==declared source measurements; распределение измерений проверит 06.3.

Side 256 (начальный) и 512, X-fast/Y-up, partial edges без padding. uint32 iff
tile maximum представим, иначе uint64. UINT64_MAX допустим; все суммы checked.
Нулевые physical tiles запрещены, нулевые submissions учитываются сортировкой.
Произвольные ingest keys сортируются через spool + две фиксированные descriptor
tapes: bounded in-memory runs, затем two-way merge. Duplicate zero/nonzero keys
терминально отклоняются при finish. Не нужны linear ordinals, полная logical area
или RAM-массив всех descriptors/keys. Index — fixed records, O(T) validation при open,
O(log T) selective search с одной stack descriptor; physical streaming O(T).

Reuse: CacheId UUID convention, checked arithmetic/LE codec, exact file I/O,
StagedFile/PublishedFile. В StagedFile адресно добавлен явный terminal `discard()`
для диагностики cleanup; прежний cache lifecycle и его callers не переписаны.
PointStreamSort специфичен для point records/IDs, поэтому descriptor pair-key sort
локализован в D. CMake/vcpkg объявляют zlib прямой production dependency;
dumpbin benchmark подтверждает Qt6Core/zlib/CRT без QML/Quick/Gui/OpenCV/Python/TIFF.

## C. Корректность и независимый oracle

`tests/density_storage_tests.cpp` содержит независимый генератор: собственные
shift-based LE packing/decoder, bitwise CRC и отдельный compress2 path. Production
binary/layout helpers не используются. Готовые writer bytes сверяются с generator
целиком; reader дополнительно открывает independently generated файлы.

Проверены 0/1/65535/65536/UINT32_MAX/UINT32_MAX+1/UINT64_MAX, mixed encoding,
tile/aggregate overflow, zero-only, все partial edge shapes и Y/X ordering,
стороны 256/512, degenerate axes, signed zero, 0.1/исходный maximum, source contract.
Reverse ingest и 512 construction-group ordering при storage side 256, omitted
edge/interior tiles, sparse maps до 1000003×1000001 без full-area allocation.
Keys >UINT32_MAX и произведения logical dimensions, не представимые в uint64,
проверены на checked addressing; гигантская wire mapping отклоняется factory budget
до линейного обхода. Production bypass для таких тестов не добавлялся.

Structural mutations с пересчитанными CRC: header flags/version/units/convention/
reserved/origin/dimensions/sections/overflow; index ranges/raw lengths/sample kinds/
summaries/reserved/duplicate/order; footer/completion/file size/trailing bytes.
Отдельный итоговый test использует одинаковые shapes обеих записей и проверяет
именно error о duplicate/order, исключая более ранний shape failure.
Payload tests: bit flips/short trailer/short или oversized output, dictionary,
trailing/concatenated streams (в том числе внутри допустимого compressBound), CRC,
zero omission и max/total при корректном decoded CRC. Open не читает payload;
selective/full integrity обнаруживают скрытое повреждение без partial success.

В первоначальном independent mutation generator Debug обнаружил iterator assertion
при поддельном index offset: helper использовал его для вычисления собственного CRC.
Helper исправлен на реальные physical positions fixture. Это ошибка тестового oracle,
не reader; первоначальные Release 101 PASS не использованы как окончательное evidence.
Исправленные тесты повторены в обеих конфигурациях без ослабления assertions.

## D. Ресурсы, отмена, failure и ownership

ResourcePolicy отдельно ограничивает owned working buffers, custom zlib allocations,
sort array, scratch/output и одновременный disk footprint, handles, index records,
work units и mapping checks. Zero semantics заданы format/API document. Начальные
значения — caller policy, не новые geometry/storage limits. Index cache не выделяется.
Codec zalloc/zfree проверяет размеры/лимит до calloc и учитывает allocation headers.
Qt/STL allocation/length failures преобразуются в ResourceRefused. I/O ошибки
сохраняют bounded detail, offset/device code; при tile failure передаётся key.

Disk accounting проверяется до extension собственных файлов. Он учитывает spool,
обе tapes и candidate одновременно, но измеряет logical bytes, без обещания cap для
NTFS allocation/OS caches. Writer требует пять собственных dataset handles с readback,
reader один. Неизвестное T контролируется во время ingest; zero submissions тоже
расходуют descriptor/index-work budget. Sort runs максимум 4096 records, budget
может ограничить их одной записью; merge не требует списка runs в RAM.

Cancellation: entry, sample blocks до 4096, codec chunks до 16384 bytes, I/O chunks
до 65536 bytes, runs/merge/index records, seal/flush/readback и последний success
checkpoint. Wall-clock latency не заявлена. Writer single-use; failure/cancel/refusal
терминальны, reentry возвращает InvalidState без разрушения внешней операции.
Reader guard запрещает overlapping/reentrant operations; независимые handles
проверены одновременно открытыми readers с interleaved operations.

Проверены нулевые/малые бюджеты, точный buffer budget и один недостающий byte,
runtime scratch/output/codec/work refusal, forced spill/merge, отказ consumer,
отмена callback по фазам и deterministic IO/Cancelled fault seam на phase boundaries,
включая flush/readback/publication. Реально повреждённый completed candidate
не проходит full readback и не публикуется. Реальная collision сохраняет прежний файл;
discard pending удаляет собственный dataset, releaseToOwner передаёт ответственность.
Настоящие Windows sharing handles вызывают cleanup failure scratch/pending;
diagnostic callback получает его отдельно, primary IO failure сохраняется.

Flush/readback предшествуют non-overwriting rename через существующий staged guard.
Finish не делает application Accept, выбор версии или deletion прежних datasets.
Callbacks noexcept, borrowed context живёт до окончания соответствующей операции;
diagnostic context — до уничтожения writer/pending. Streaming Tile заимствован только
на время callback; возвращённый readTile владеет vector, caller отвечает за удерживаемые
копии/результаты. Consumer refusal возвращает failure после возможного emitted prefix.

## E. Фактически выполненные проверки

В штатной MSVC CMD-среде (локально через `cmd /d /c build\task02-env.cmd ...`):

```bat
cmake --build --preset windows-debug --target density_storage_tests raster_mapping_tests density_storage_benchmark --parallel 4
cmake --build --preset windows-release --target density_storage_tests raster_mapping_tests density_storage_benchmark --parallel 4
ctest --preset windows-debug -R DensityStorage --output-on-failure
ctest --preset windows-release -R DensityStorage --output-on-failure
ctest --preset windows-debug -R RasterMapping --output-on-failure
ctest --preset windows-release -R RasterMapping --output-on-failure
```

Headless targets также собраны в Debug/Release:
`project_tests cache_tests persistence_tests asc_xyz_tests operation_tests
import_application_tests nonzero_z_confirmation_tests save_tests open_tests recovery_tests dependency_smoke`.
Inventory получен `ctest --preset windows-release --show-only=json-v1`; из него
выбраны test names по executable basename этих targets, `raster_mapping_tests`
и `density_storage_tests`, затем пересечены с label production. Большой cache
integration с другим label не включён. Выполнены:

```bat
ctest --preset windows-debug -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/density062-headless-tests.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density062-debug-regression.xml
ctest --preset windows-release -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/density062-headless-tests.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density062-release-regression.xml
```

| Проверка | Debug | Release |
|---|---|---|
| D/core и перечисленные headless targets build | PASS | PASS |
| D storage, финальный набор | 115 PASS / 0 FAIL / 0 SKIP | 115 PASS / 0 FAIL / 0 SKIP |
| RasterMapping | 14 PASS / 0 FAIL | 14 PASS / 0 FAIL |
| Широкая headless regression, 692 cases | 691 PASS / 0 FAIL / 1 SKIP | 691 PASS / 0 FAIL / 1 SKIP |
| Release benchmark / independent summary checks | N/A | PASS, 6 trials |

Широкий прогон включал предыдущие 112 D cases и 580 прежних cases. После него
узкий D набор повторён после финальных checked diagnostic counters/size handling;
при дополнительной проверке oracle добавлены shape-valid duplicate/order case
и два cases отмены после partial codec/sort/merge/copy/readback/index/streaming work.
Финальные 115 cases повторены в обеих конфигурациях. Широкий набор повторно не запускался:
за его пределами production code не менялся. XML/log hashes и точные результаты —
[verification.json](../../benchmarks/density062/verification.json).

SKIP: `Persistence.StoragePath.FileSymlinkEscape`, Windows error 1314.
Ранее известный Release FAIL
`RecoveryApplication.FileFailures/RecoveryFault.FailedUpdatePreservesPreviousRecordAndAllDatasets/6`
в этих широких прогонах PASS; его baseline FAIL и причина NoChange/Applied не отменяются,
fixture не исправлена. Optional Qt TaskTree/QmlAssetDownloader/Vulkan configure diagnostics
сохранились; storage targets успешно собраны, compiler warnings в финальных build logs нет.

NOT RUN: GUI/lint (storage их не меняет), физический disk-full/flush device failure,
power-loss/process crash durability, hostile filesystem mutation/fuzz/security audit,
UNC/cold-HDD/real scans. Phase-level IO injection не выдаётся за physical device fault.

## F. Адресные Release-измерения

[Raw evidence и воспроизводимая методика](../../benchmarks/density062/README.md).
i5-7500, 17.14 GB RAM, D: NTFS SATA HDD; synthetic, warm cache, один trial/profile/area.
T=96, side=256; 24576×256 против 1000003×1000001 logical cells. Index всегда
7680 bytes; open всегда читает 8064 bytes, zero decoded tiles; пять requests
декодируют пять tiles. Sort reserve 144 bytes, шесть merge passes.

Compressible/entropy/sparse final bytes: 34848 / 25182144 / 35520, одинаковые
для обеих площадей; scratch peaks 42144 / 25189440 / 42816. Нет работы/descriptor
на пустую logical area. Mapping open cost отдельно: small ~0.112 ms,
large 8.79–9.02 ms. Остальная structural open — около 0.17–0.21 ms.
Ingest 178–727 ms; sort/merge 4.4–5.1 ms; payload/index copying 0.8–35.7 ms;
finish 106–160 ms, включая readback 99–116 ms; пять reads ~4.8–5.7 ms;
full physical iteration ~93–105 ms. Это наблюдения, не performance thresholds.

Owned buffer reserve 1 638 573 bytes, custom codec peak 268 104 bytes,
sort array 144 bytes; borrowed input buffer 524 288 bytes отдельно. Process working
set измерен до/после, Windows process-lifetime peak до 10 964 992 bytes;
это не OS peak отдельного trial и не полный allocation trace. I/O counters логические.
Полный C1 pipeline и real-scan quality/эксплуатационные профили остаются 06.3/06.8.

## G. Статус и границы

**READY FOR INDEPENDENT REVIEW**: обязательные storage критерии реализованы и
проверены; известных открытых дефектов D нет. API готов к использованию builder 06.3
в пределах явных budgets. Широкая headless проверка сохранила baseline ограничения;
независимое ревью и пользовательская приёмка ещё требуются.

Dataset immutable, owner сохраняет файл на всё время reader. Произвольные внешние
изменения после open не гарантированно обнаруживаются. CRC не аутентификация,
flush не power-loss durability. Logical disk/owned allocation budgets не OS limits.
06.3–06.8, versions/Undo/Recovery/Canvas/ROI/Rough/morphology/новые backends не реализованы.
Requirements, manifest v1, принятый workflow задачи 05 и исторические отчёты сохранены.
