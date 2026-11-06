# 06.3B — verification evidence

2026-11-05. Baseline `main` / `bc28195971ddbe43ef36a12f38b5d49453bd85d6`,
исходный status чистый. Это проверки реализации, не независимое принятие.
[Контракты, результаты и ограничения](../../reports/task-06/density-build-operation.md).

## Команды и окружение

Из корня репозитория, штатная CMD-среда `build\task02-env.cmd` (vcvars64,
UTF-8 console, process-local Qt/CMake/Ninja paths):

```bat
build\task02-env.cmd cmake --build --preset windows-debug --target density_operation_tests operation_tests --parallel 4
build\task02-env.cmd build\windows-debug\density_operation_tests.exe --gtest_output=xml:build/density063b/adapter-debug-initial.xml
build\task02-env.cmd cmake --build --preset windows-debug --target all all_qmllint --parallel 4
build\task02-env.cmd cmake --build --preset windows-release --target all all_qmllint --parallel 4
build\task02-env.cmd cmake --build --preset windows-debug --target density_operation_tests operation_tests --parallel 4
build\task02-env.cmd build\windows-debug\density_operation_tests.exe --gtest_output=xml:build/density063b/adapter-debug.xml
build\task02-env.cmd cmake --build --preset windows-debug --target all all_qmllint --parallel 4
build\task02-env.cmd cmake --build --preset windows-release --target all all_qmllint --parallel 4
```

Первые 35 cases: 33 PASS / 2 FAIL (ошибки expectations/order fixture), затем
промежуточные 46/46. `adapter-debug-intermediate.*` — копии фактического второго
запуска, сохранённые до финального расширения до 47 cases. Первые failures не скрыты.

Для final test/discovery использован PowerShell с process-local
`PATH=D:\Qt\6.12.0\msvc2022_64\bin;...`; CTest вызван по абсолютному пути
`D:\Qt\Tools\CMake_64\bin\ctest.exe`. MSVC environment тестам не требуется.
В этой среде фактически выполнены команды:

```powershell
& .\build\windows-debug\density_operation_tests.exe --gtest_output=xml:build/density063b/adapter-debug-final.xml
& 'D:\Qt\Tools\CMake_64\bin\ctest.exe' --preset windows-debug --show-only=json-v1
& 'D:\Qt\Tools\CMake_64\bin\ctest.exe' --preset windows-release --show-only=json-v1
& .\build\windows-release\density_operation_tests.exe --gtest_output=xml:build/density063b/adapter-release-final.xml
& 'D:\Qt\Tools\CMake_64\bin\ctest.exe' --preset windows-debug -L production --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063b/production-debug.xml
& 'D:\Qt\Tools\CMake_64\bin\ctest.exe' --preset windows-release -L production --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/density063b/production-release.xml
```

Первый широкий Debug на предыдущем diff: 1123 PASS / 0 FAIL / 1 SKIP из 1124.
После финального просмотра уточнена классификация std::invalid_argument из receiver
prepare (CandidatePreparationFailed, в отличие от внутреннего InvalidProduct) и
добавлен subcase в существующий test. Повторены `all all_qmllint` обеих конфигураций
(`build-*-verified.log`), последовательно discovery Debug/Release, adapter 47/47
в каждой конфигурации, затем широкие production. Final production Debug/Release
запущены параллельно в отдельных build directories; GUI использует offscreen/software
и собственные temporary fixtures. Предыдущие outputs/provenance/inventories сохранены
с prefix `before-taxonomy-`, результаты не подменены последующими PASS.

Исходные outputs находятся в ignored `build/density063b`; gzip-копии
XML/log/JSON сохраняются здесь без изменения содержания. `summary.json` содержит
результаты, `provenance.json` — SHA-256 исходников/исполняемых файлов;
inventories и сравнение — отдельно.

Финал: Debug/Release `all all_qmllint` — PASS; в каждой конфигурации adapter
**47 PASS**, полный production **1123 PASS / 0 FAIL / 1 SKIP** из 1124.
Входящий headless subset — **875 PASS / 0 FAIL / 1 SKIP** из 876;
248 automated UI cases прошли. SKIP — file-symlink privilege 1314.
Production wall time: Debug 478.80 s, Release 223.50 s; это продолжительность
проверок, не benchmark приложения. Raw files проверены побайтово после gzip roundtrip,
размеры и SHA-256 исходного/сжатого содержания — в `artifacts.json`.

## Inventories

Discovery Debug, затем Release выполнены последовательно. В каждом inventory
1263 case-sensitive unique names; production — 1124, headless production — 876.
Exact headless inventories совпадают. Полные и production inventories различаются
четырьмя существующими `AscGui.Signals/StateNotificationDeletion` labels: GTest
печатает bytes member-function pointers, отличающиеся между binaries/ASLR.
Это видно в неизменённом `tests/gui_lifecycle_tests.cpp`: параметр `ModelSignal`.
Все команды/GTest filters после замены только пути конфигурации совпадают точно.
Имена не нормализованы и tests не исключены из запуска. Первый inline Python assert
равенства полного inventory завершился `AssertionError: all`; фактические различия
и результат последующего сравнения сохранены в `inventories.json`.

Широкий production включает headless, adapter и automated GUI/file/lifecycle tests;
пересекающиеся subsets не суммируются. Исторический Open Closing/Ready observation
не объявляется исправленным. Новый failure, если появится, сохраняется отдельно.

## Измерения и пределы

`SmallProductionPipelineRecordsLatencyEventsAndOwnership` записывает XML properties
для реального маленького Point Cache → C1 → D: elapsed от admission до prepare,
commit и terminal после native completion/join, число progress notifications,
source/events/D bytes, owned allocation peak и retained descriptors. Test process
и import не входят в этот интервал. Это synthetic latency smoke с работающим owner
event loop, без performance thresholds или вывода о больших сканах/process RAM.

Builder B1/B2, storage/Mapping и existing native/TLS/lifecycle regressions запускаются
в production вместе с adapter. Large scans, physical disk faults, UNC, cold cache,
power loss и ручной Build UI здесь не проверяются.
