# Recovery P3 — воспроизводимое evidence

Baseline: `main`, `8d622e0129ccdfa6de6f0ad75eba0845da9de1e3`, clean checkout.
Дата проверки: 2026-10-29. Исправляются только test fixture/helpers; production
Autosave, Save/Recovery protocol, Density Storage и Raster Mapping не меняются.

## Воспроизведение

Из корня checkout на Windows x64:

```powershell
python -u docs/reports/task-05/recovery-p3/verify.py --env-cmd build/task02-env.cmd --out build/recovery-p3/verification-final
```

Выходной каталог должен отсутствовать: runner не перезаписывает прежние результаты.
`--env-cmd` — CMD wrapper для штатного `vcvars64.bat`, `QT_ROOT`, `VCPKG_ROOT`,
process-local PATH с Qt/CMake/Ninja; последняя команда wrapper — `%*` с передачей
exit code. Использован существующий локальный `build/task02-env.cmd`:
Visual Studio 2026/MSVC 19.51.36260, Qt 6.12.0 `msvc2022_64`, CMake 3.30.5,
Ninja 1.12.1. Глобальное окружение и credentials не меняются.

Runner последовательно собирает Debug/Release headless targets и GUI targets,
включающие изменённый header (`gui_import_tests` включает `gui_lifecycle_tests.cpp`).
Потом **последовательно** получает оба `ctest --show-only=json-v1`, проверяет
уникальность всех 1086 имён в каждом inventory, формирует точные списки:

- Recovery: 118 cases (68 application/process-crash + 50 GUI);
- repeats: 20 cases, каждый запускается 20 раз в каждой конфигурации;
- headless production: 699 cases по executable basename, включая Recovery application,
  Save/Open/Import/Operations, persistence/cache, RasterMapping и DensityStorage.

Все CTest-запуски используют `--tests-from-file`, `-L production`,
`--output-on-failure`, `--output-junit` и `--parallel 1`.
Нет until-pass, выборочного исключения FAIL или параллельного discovery.
Состав повторов: четыре `RecoveryScheduling` cases, семь `RecoveryFault`,
Close/Discard, FirstImport, NoRewrite/Undo, PeriodicTimer, MatchingExplicit,
ExplicitSaveReconciles, ConflictSurvives, CancelledAcceptance и исторически падавший
InvalidRecords `/7`.

## Проверяемое содержание

Матрица `RecoveryScheduling` различает Import/Save и два управляемых порядка:

1. При блокировке сигналов timers завершён только foreground worker; Recovery ещё
   отсутствует. Явный Autosave обязан вернуть Applied и опубликовать запись.
2. Без блокировки ожидаются наблюдаемые `Autosave`, native completion и recovery
   record через существующий `spinUntil` (прежний timeout 10 s). Идентичный snapshot
   обязан дать NoChange без смены bytes, OperationId и вызова transaction hooks.

В обоих случаях следующая правка cell обязана дать Applied, новый snapshotId,
достичь BeforeCommit/Published ровно один раз и сохранить точное persisted content,
physical dataset binding и explicit baseline. Проверяются bytes explicit manifest,
SHA-256 всех малых fixture datasets, dirty и Undo/Redo.

Fault hooks ставятся после завершённой подготовки предыдущей записи и новой правки.
Все семь failures обязаны реально вызвать injection, достичь своей фазы и не
достичь Published. Предыдущие bytes/digest/snapshotId, dataset paths/cache ID,
baseline и оба datasets остаются прежними. Эти фазовые отказы не имитируют
фактическое заполнение физического диска.

`QSignalBlocker` восстанавливает прежнюю блокировку сигналов при выходе из scope.
Timers останавливаются перед снятием блокировки; TearDown также останавливает их,
сбрасывает fixture hooks и уничтожает controller с drain до удаления storage.
Новых глобальных settings и барьеров с заимствованным временем жизни нет.

## Артефакты

`results.json` — компактная сводка результатов, source/log/XML SHA-256 и всех
промежуточных FAIL. Полные логи, JUnit XML, точные test lists, inventory и команды
остаются в ignored `build/recovery-p3/`; `verification-final/summary.json` содержит
полные argv каждой команды. Скрипт воспроизводит проверки без этих локальных logs.

Исторические независимые FAIL сохраняются отдельно:

- post-Import: `build/review062-independent/regression-debug-discovery-duplicates.xml`,
  `Recovery.ClosePreservesRecoveryAndConfirmedCloseDiscards`; первоначальный inventory
  того ревью содержал дубликаты, сам failure ожидания Applied зафиксирован в XML/log;
- post-Save: `build/review061/release-headless.xml`,
  `RecoveryFault.FailedUpdatePreservesPreviousRecordAndAllDatasets/6`;
- [история 05.4](../autosave-recovery.md#неблокирующее-наблюдение-о-надёжности-тестов),
  [06.1](../../task-06/raster-foundation.md#h-independent-review-and-acceptance),
  [06.2](../../task-06/density-storage.md#e-фактически-выполненные-проверки).

Успешные текущие повторы не отменяют эти FAIL. Результаты P3 и ограничения
приведены в [отдельном разделе Recovery-отчёта](../autosave-recovery.md#n-recovery-p3--синхронизация-fixture).
