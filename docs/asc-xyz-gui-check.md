# Проверка 04.4A: GUI импорта ASC/XYZ

## A. Baseline

Дата: 2026-10-18. Ветка `main`, исходный HEAD
`7ac8329efc7110232040a0c56da730947b8c5bdb`. Working tree и index были чистыми.
Baseline совпал с заданием. Реализация ограничена 04.4A; 04.4B не выполнялась.

Изучены AGENTS.md, README, задача 04 в implementation-plan, относящиеся разделы
requirements (§§2.1, 4–7, 24, 31, 35), architecture (стек, C++/QML, state/import,
operations/threading, rendering, errors/resources/testability), algorithms §§2, 5,
Point Cache v1 и отчёты asc-xyz-import-check, asc-xyz-application-check,
nonzero-z-confirmation-check. Проверены фактические main/QML/ViewModel/shutdown,
controller/import adapter/runner/context, ImportedDataset, parser/importer,
Writer/Sorter resource checks, tests/test hooks и CMake presets.

Использованы MSVC 14.51.36231, Qt 6.12.0, C++20 и предоставленная CMD-инициализация.
Новых сторонних зависимостей, глобальных настроек или системных инструментов нет.
HDD не потребовался. Задачи 01–03 сохраняют принятый статус.

## B. Scope and Implementation

Изменённые группы файлов:

- `qml/Main.qml`: native FileDialog, replacement/Z dialogs, startup, dataset card,
  диагностика, progress и фиксированная панель управления с доступным Cancel.
- `src/ui/project_view_model.h/.cpp`: UI commands/tokens, регистрация consumer,
  проверка QUrl, metadata accessors и отдельные ошибки cell/import.
- `src/application/import_operation.h/.cpp`, `project_controller.h/.cpp`:
  optional selection context и interactive resource policy перед существующим import.
- `src/application/operation_types.h`: фазы Parsing и BuildingCache.
- `src/io/asc_xyz_importer.h/.cpp`: optional observer двух границ фаз.
- `tests/gui_import_tests.cpp`, CMake, test access, ViewModel/QML/confirmation tests
  и QML test main: 37 новых GUI/UI cases, Z-wake regression и обновлённые ожидания New.
- README, architecture, implementation-plan и этот отчёт.

Отдельного consumer QObject нет: существующая ViewModel уже имеет правильные
Qt thread affinity и lifetime. Отдельного project state в QML нет. Metadata читает
только `ProjectState::dataset()`, Ready и WorkerOutcome UI не использует.
`src/main.cpp` и shutdown coordinator не потребовали изменения.

## C. GUI Workflow

`Component.onCompleted` планирует `offerStartupImport()` через `Qt.callLater`.
Предложение однократно для адаптера; Cancel не закрывает окно и не создаёт ProjectState.
«Новый проект» начинает новый одноразовый выбор. При Busy, выборе файла,
replacement confirmation и Closing второй импорт недоступен.

Используется `QtQuick.Dialogs.FileDialog` с OpenFile и русскими фильтрами ASC/XYZ.
В production действует native Windows dialog, без DontUseNativeDialog.
`selectedFile` передаётся как QUrl в C++: `isValid/isLocalFile`, absolute path,
расширение и отсутствие query/fragment/credentials/port/NUL; путь получается
только через `toLocalFile()`. Нет ручного удаления `file://` или повторного decode.
Unicode, пробелы, literal `%20`, `%`, `#`, `+` проверены реальным import.
Чтение/проверка доступности source выполняется в worker, включая disappearance
между выбором и стартом. UNC semantics проверены без подключения к сети;
реальный network share не проверялся.

До выбора controller выдаёт `ImportContext`: optional исходный stamp, epoch,
last operation id. ViewModel хранит выбранный путь и UUID текущего UI действия.
Подтверждение разрешает только этот файл и этот контекст. Controller повторно
проверяет expectedContext непосредственно перед launch; изменения проекта,
NoProject ABA, начало другой операции и Closing делают старое разрешение бесполезным.
После одного ответа UI token потребляется, глобального разрешения замены нет.

Предупреждение явно объясняет сохранение старого проекта до success, поведение
failure/cancel, новую пустую history и невозможность возврата прежнего проекта Undo.
После Accept новая identity, cell=1 мм и пустые Undo/Redo принадлежат controller.
Прежние принятые caches сохраняются. Density Map автоматически не создаётся.

## D. Z Confirmation and Lifecycle

ViewModel регистрируется через `setNonZeroZConsumer(this)` до запуска импорта.
Worker по-прежнему пользуется протоколом 04.3 и condition variable; QObject и
QML callbacks в worker не передаются. GUI event loop не блокируется.

Диалог показывает физический номер строки и shortest binary64 Z, в том числе
`5e-324` и `1e-100`. В нём явные «Продолжить импорт» и «Отменить импорт».
QML хранит UUID показанного запроса; adapter сравнивает его с актуальным pending
request и передаёт исходный typed NonZeroZRequest в `answerNonZeroZ`.
Повторное notification не открывает второй диалог. Continue не повторяет запрос
в этой операции и не заменяет Ready/Accept gate.

Пользовательское закрытие (включая Escape) эквивалентно Cancel. Перед программным
закрытием после общего Cancel/shutdown QML очищает UI token. Поздний onClosed или
старый Continue не отвечает на новый запрос. Shutdown не выполняет auto-Continue.

Новый destruction regression выявил Debug assertion: QObject::destroyed consumer
синхронно отменял import, а controller notification возвращалось в уже уничтоженный
derived ProjectViewModel. Destructor адаптера теперь отключает входящие controller
connections до QObject destruction. Отзыв consumer и wake остаются обязанностью
существующего controller. Тест проверяет worker Ready после удаления ViewModel
без GUI events до подтверждения выхода из Z-wait.

## E. Data, Progress and Diagnostics

Карточка содержит имя source, count, X/Y min/max, миллиметры, Point Cache готов и
признак ненулевого Z. При failure/cancel замены она продолжает показывать прежний
принятый dataset. Count — QString из uint64_t, без промежуточного QML number.
Проверен `UINT64_MAX = 18446744073709551615` в явно metadata-only fixture.
Coordinates/Z форматируются `QLocale::c()` с FloatingPointShortest: значимые digits,
subnormal, отрицательные/большие значения и exponent сохраняются. Десятичная точка
используется как технический формат; все пользовательские тексты русские.

Interactive profile разрешается в worker после создания storage root и проверки
его тома через QStorageInfo. Explicit limits headless callers не изменены.

| Ресурс | Политика GUI |
|---|---|
| Рабочая память | 128 MiB для parser + Writer + Sorter, с прежними reserves |
| Открытые файлы | 32 суммарно; importer вычитает source handle, Writer — свои |
| Диск | `usable = availableBytes - 64 MiB`, budget = `usable - usable/20` |
| Неизвестное место или available <=64 MiB | ResourceExhausted, без publication |

Budget не зависит от RAM компьютера разработчика или размера малого fixture.
128 MiB — ограничение контролируемых рабочих allocations операции, не RSS процесса,
Qt runtime или OS page cache. Нет предела N по размеру source: Sorter использует
external runs и bounded merge. Его source/destination и Writer cache/descriptors
по-прежнему делят disk budget консервативно пополам. Реальная вместимость облака
зависит от этой политики, существующих постоянных caches и свободного места.
Reserve покрывает часть filesystem overhead/изменения места, но не гарантирует
write/flush/rename. Их проверки и ошибки сохранены. Ноль не трактуется как unlimited.

Preparing → Parsing (включая запись сортируемых блоков) → BuildingCache
(sort/merge/index/sealing) → Finalizing → terminal. Два optional observer calls
сообщают границы фаз, не точки. Общего процента или искусственных весов нет.
Unknown total отображается indeterminate; существующий determinate path также
сохранён и проверяется operation/QML regression tests. Z-wait отображается отдельно.
Terminal import success/failure виден и при изменении project identity / NoProject.

Import errors переводятся в русские смысловые сообщения; технический раздел и
QVariantMap сохраняют code/phase/operation/detail, line/column/coordinate/token,
offset и QFileDevice code. Все 64-bit source counters в DTO — строки. Технические
enum names не переводятся. Cancel не становится системной ошибкой.
Ошибка импорта/phase progress не сбрасывают cell draft/error/cursor/selection.

## F. Verification

Применена CMD-инициализация из задания: vcvars64.bat, UTF-8, VCPKG_ROOT, QT_ROOT,
CMake/Ninja/Qt в PATH, VCPKG_MAX_CONCURRENCY=4. Далее фактически выполнены:

```bat
cmake --build --preset windows-debug --target CADContour2D gui_import_tests view_model_tests qml_integration_tests nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests dependency_smoke operation_probe all_qmllint --parallel 4
cmake --build --preset windows-release --target CADContour2D gui_import_tests view_model_tests qml_integration_tests nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests dependency_smoke operation_probe all_qmllint --parallel 4
ctest --preset windows-debug -R ^AscGui --output-on-failure
ctest --preset windows-debug -R DatasetAndDialogsResize --output-on-failure
ctest --preset windows-release -R DatasetAndDialogsResize --output-on-failure
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
git diff --check
```

| Итоговая проверка | Debug | Release |
|---|---:|---:|
| Сборка приложения и затронутых targets | PASS | PASS |
| all_qmllint | PASS | PASS |
| Полный production CTest | 431/431 PASS | 431/431 PASS |
| В том числе новые GUI/UI | 37/37 PASS | 37/37 PASS |
| В том числе Z confirmation | 30/30 PASS | 30/30 PASS |
| В том числе import application | 22/22 PASS | 22/22 PASS |
| В том числе ASC/XYZ | 42/42 PASS | 42/42 PASS |
| В том числе operation regressions | 51/51 PASS | 51/51 PASS |

Итоговый полный CTest занял 107.90 s / 46.11 s соответственно. В нём также прошли
project/coordinates, ViewModel, production QML/registration, dependency smoke и
166 cache cases. Отдельные cache_integration/benchmark tests не запускались.

Новые 37 cases используют production Main.qml, настоящие диалоги и UI handlers.
Тестовый `fileDialogEnabled=false` подавляет только OS window; valid URLs подаются
через selectedFile/accepted настоящего FileDialog. Заведомо недопустимые URLs
проверяются прямо на C++ UI boundary, поскольку FileDialog сам отвергает их.
Системное окно не автоматизируется в CTest. Worker/parser/Writer/Accept настоящие;
ограниченные friend fixtures используются только для проверки uint64 metadata
и искусственного контекстного ABA, не для основного пользовательского сценария.

`CancelWakesZWaitWithoutPollShutdownOrDrain`: после настоящего импорта и подтверждённого
waitingForZ под mutex вызывается Cancel, затем condition variable подтверждает Ready
и Cancelled worker outcome до любого poll/shutdown/drain или GUI events. Только
после этого даётся Discard и проверяются отсутствие publication и cleanup.
Sleep не используется; timeout защищает только test harness.

Первый Debug GUI набор выявил описанный lifetime assertion, цикл implicitHeight
для длинного имени в replacement dialog и неверную подачу отсутствующих/invalid
files через selectedFile (сам Qt их отвергает с warning). Lifetime исправлен;
dialog получил ограниченную высоту и прокручиваемое содержимое без циклического
измерения (ограниченный Flickable viewport не зависит по высоте от размеров текста);
disappearance моделируется после валидного выбора, invalid URLs —
на UI boundary. QML warnings по-прежнему завершают tests ошибкой; проверки не
ослаблены. CRT asserts в QML test main направлены в stderr, без modal runtime box;
сами assertions сохранены. Финальные production runs прошли с 0 failures.

Windows GUI smoke выполнен через computer-use отдельно от offscreen tests:
native startup/Cancel/повторный выбор, Unicode+spaces, successful import, metadata,
replacement confirmation, Z warning с физической строкой/малым значением,
Continue с принятием Z metadata, Cancel с сохранением старого dataset и нормальное
закрытие. Debug и Release завершились с кодом 0.

Известные configure diagnostics Qt6TaskTree/WrapVulkanHeaders и сторонний C4702
в fmt сохранились; новых compiler/QML lint warnings не обнаружено.

## G. Regression Assessment

01–02: project/cell/Undo, notification consistency, draft preservation,
single-worker Busy, cancellation/invalidation, Ready lease, irreversible Accept,
native completion/join и shutdown regressions прошли в обеих конфигурациях.
04.1: parser/binary64/golden/range/source/sink/cancel cases прошли; parser и числовой
conversion не менялись. 04.2: все 22 existing import application cases прошли,
включая worker-owned PendingCache, metadata-only Ready, atomic session/history и
source independence/cache retention. 04.3: все 29 прежних cases и новый wake test
прошли; GUI использует тот же typed protocol, дополнительные tests подтверждают
Continue/Cancel/close/general cancel/shutdown/stale callbacks и consumer destruction.
Это оценка затронутых контрактов, не новое независимое ревью задач 01–04.

## H. Limitations

Реальные UNC/network reads, большие файлы, HDD/performance/resource матрица,
faults длительного OS I/O и окончательная сквозная приёмка остаются в 04.4B.
Зависимость cancellation latency от блокирующего системного I/O сохраняется.
Нет полного визуального аудита Windows 10/11, нескольких мониторов/масштабов DPI,
разных тем и screen readers. Resize/long filename проверены автоматически.
Progress внутренних sort/index/seal объединён; достоверный byte/record total не
добавлен. Политика resource budget консервативна и не равна измеренному peak RSS.
Накопление принятых caches без manifest/GC и отсутствие Save/Open — прежние
ограничения до задачи 05. Density Map/Canvas/geometry/export не входят в этап.

Автоматическая проверка разрешений отклонила очистку собственных внешних
verification artifacts (`blocked by policy`): `%TEMP%/cadcontour-044a-smoke`,
`%TEMP%/gui_import_tests-dfUnMS` и два маленьких принятых synthetic cache-каталога
в `%LOCALAPPDATA%/CADContour2D/projects` с UUID `e4fea3fd-0c9b-4161-b3d7-fe9ff7d09168`
и `f9caca6a-5d26-4f37-9075-98f4303332b4`. Они оставлены вне репозитория;
тестовые процессы завершены. Временный CMD adapter также остаётся в
`%TEMP%/cadcontour-044a.cmd`. В Git эти данные не включены.

## I. Final Verdict

**READY FOR REVIEW.** 04.4A реализована, обязательные Debug/Release проверки прошли,
блокирующих дефектов по выполненной проверке не обнаружено. Подготовлена к целевому
независимому GUI-ревью. 04.4B может выполняться отдельным заданием после такого ревью;
вся задача 04 принятой не объявляется.

Изменения и документация фиксируются одним commit после просмотра diff/checks.
Конечный HEAD и состояние дерева после commit приводятся в итоговом сообщении:
самоссылочный hash в документ не записывается. Push не выполняется.
