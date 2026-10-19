# 04.4A — исправления независимого ревью R1–R3

## A. Baseline

Проверен `d13793a61bf2c4f220d611119563c443d5cb0f45`, ветка `main`.
До работы working tree и index были чистыми. Новые regression tests сначала
собраны против неизменённых production sources этого commit.

Изучены актуальные AGENTS.md, README, относящиеся к импорту и lifecycle положения
requirements/architecture/algorithms, ProjectViewModel, Main.qml, consumer destruction,
cancel/shutdown controller, runner test support и существующие GUI/ViewModel/QML/Z tests.
Область ограничена R1–R3 и непосредственно связанными продолжениями adapter.

## B. Verification of Findings

| Finding | Независимое воспроизведение до исправления | Причина и необходимость исправления |
|---|---|---|
| R1/P1 | `GuiLifecycle.R1DeletionInsideZNotificationCancelsAndWakesWorker`: реальный importer достиг Z-wait, обычный runner timer доставил уведомление, синхронный observer уничтожил ViewModel. GoogleTest зарегистрировал SEH `0xC0000005`; отдельный процесс завершился с кодом 1. | После `nonZeroZChanged` lambda испускала `operationChanged` на уничтоженном `this`. Disconnect в деструкторе защищает от новых входящих вызовов, но не останавливает уже выполняемое продолжение. Исправление требуется. |
| R2/P2 | `Contexts/ReentrantShutdown.R2RevokedSelectionDoesNotRequestFileDialog` с NoProject и существующим проектом: token создан, shutdown из `importUiChanged` его отзывает, затем приходит лишний `fileSelectionRequested`. В настоящем Main.qml тест `R2ShutdownDuringSelectionDoesNotOpenProductionFileDialog` получил `closing=true`, `selecting=false`, пустой token и `visible=true`. | `newProject()` не перепроверял выбор после синхронного внешнего callback, QML безусловно открывал FileDialog. Исправление требуется на обеих границах. |
| R3/P3 | `GuiLifecycle.R3DeletedWindowBeforeDeferredStartupDoesNotAccessQmlContext`: Main.qml загружен и root уничтожен до обработки events. Получен `Main.qml:20: TypeError: Cannot read property 'project' of null (exception occurred during delayed function evaluation)`. | Отложенная anonymous function переживала root и обращалась к обнулённому `window`. Исправление требуется; crash/публикация проекта в этом сценарии не обнаружены. |

R1 проверялся отдельным запуском executable до общего CTest. SEH был перехвачен
GoogleTest: это не следует описывать как необработанный exit code процесса
`0xC0000005`. После исключения исходный тест также получил timeout в teardown;
controller destructor завершил drain. После исправления штатное завершение
проверяется без исключения и без timeout.

Отдельный `R3DeletedWindowBeforeDeferredCloseDoesNotAccessQmlContext` сначала
обрабатывает startup при живом окне, затем инициирует shutdown и удаляет root
до deferred close. Он прошёл уже на baseline. Аналогичный дефект `window.close()`
не подтверждён, этот участок не изменён; отрицательный тест оставлен.

Для выбора, отменённого и созданного заново внутри callback, baseline отправлял
два запроса вместо одного (`R2SupersededSelectionOnlyRequestsCurrentToken`).
Отдельная подача stale presentation signal также открывала FileDialog без
выбора и после shutdown. Это проверяет необходимость самостоятельной QML-защиты.

## C. Fixes

- `src/ui/project_view_model.cpp`: локальный QPointer останавливает продолжения
  после внешних синхронных уведомлений/команд, если adapter уничтожен. Проверены
  Z, последовательность state/error/input/operation notifications, Apply/Undo/Redo,
  cancel, создание выбора и запуск импорта. После clearSelection в refreshImportUi
  выполняется return: одно уведомление уже отправлено, повторное не требуется.
- `requestClose()` сохраняет ссылку на session controller до requestShutdown
  и читает результат через неё. Controller по прежнему контракту переживает adapter;
  возвращаемое значение Ready не меняется при уничтожении adapter в notification.
- `newProject()` сохраняет локальный UUID именно этого выбора и после уведомления
  проверяет lifetime, тот же token, Open, selectingFile и текущий ImportContext.
  Повторный вход с созданием другого выбора не получает запрос от старого вызова.
- `qml/Main.qml`: FileDialog требует Open, существующий выбор, непустой token
  и отсутствие активной операции. Startup ставит в Qt.callLater метод самого окна.
  Qt delayed queue охраняет receiver QObject method; уничтоженное окно не вызывается.
- `tests/gui_lifecycle_tests.cpp` подключён к существующему gui_import_tests.
  Нового test framework, manager, зависимости или threading mechanism нет.
- `docs/architecture.md` уточняет эти lifecycle-гарантии; README содержит результат
  проверок и ссылку на этот отчёт. Исторический отчёт реализации 04.4A не переписан.

Выбор deferred method проверен по [исходникам Qt 6.12.0 delayed queue](https://raw.githubusercontent.com/qt/qtdeclarative/v6.12.0/src/qml/qml/qqmldelayedcallqueue.cpp):
QObject method получает guard receiver, execute проверяет его перед вызовом.
Это объясняет выбранное исправление startup; отдельный тест доказывает его
поведение на установленной версии. Безопасность соседнего close установлена
экспериментом, без предположения, что любой JavaScript closure имеет такой guard.

## D. Lifecycle Safety

Деструктор ViewModel по-прежнему отключает controller signals до разрушения
derived object. Регистрация consumer и destroyed connection controller сохранены.
Удаление consumer отменяет импорт и будит worker; worker не получает QObject.

Новый R1-тест ждёт Ready worker непосредственно внутри удаляющего observer,
после model.reset(), без последующих GUI events, poll, shutdown или drain.
Затем проверяет Cancelled worker/operation, отсутствие failure, pending Z,
проекта, занятого runner и временного cache-каталога. Timeout — только ограничение
test harness; application timeout не добавлен.

Ограниченная проверка соседних продолжений обнаружила на baseline SEH также
при отзыве selection, перед запуском импорта, на четырёх границах state notification,
при no-op canonicalization и Apply/Undo/Redo/requestClose. Все эти paths теперь
покрыты тестами. Cancel path прошёл и до исправления; guard исключает вызов
handleResult на уничтоженном adapter после уведомления controller.

clearSelection, validation error, answerNonZeroZ и остальные tail notifications
не продолжают доступ к adapter после внешнего вызова. QML программно снимает token
перед закрытием replacement/Z dialogs; onClosed получает пустой token и не отвечает
за новый запрос. Accepted/stale callbacks сохраняют существующую C++ проверку
token/context. Существующие dialog-close/cancel/shutdown tests прошли.

Дополнительный диагностический probe удалял QML root синхронно из выполняющегося
QML onClicked. Qt остановил его с сообщением, что уничтожение объекта во время
его QML signal handler не поддерживается и следует использовать deleteLater.
Это неподдерживаемая последовательность test harness, не доказательство дефекта
приложения. Изолированный процесс остановлен, probe исключён до постоянного набора.
Показанное пользователю CRT-окно относится к нему. Assertions/warnings не подавлены;
tests/qml_test_main.cpp в этой работе не менялся.

## E. Regression Tests

Добавлен **21 case**, production CTest вырос с 431 до 452:

| Группа | Cases | Проверка |
|---|---:|---|
| R1 Z notification | 1 | Уничтожение adapter, немедленное пробуждение, cancellation, отсутствие publication и cleanup |
| R1 state notification boundaries | 4 | Уничтожение на errorMessageChanged/stateChanged/cellInputSyncRequested/operationChanged |
| R1 commands | 5 | Apply, Undo, Redo, cancel waiting import, requestClose |
| R1 selection/no-op continuations | 4 | Удаление при создании/отзыве selection, перед launch и при canonicalization |
| R2 | 5 | Shutdown в NoProject/Project, настоящий FileDialog, superseded token, самостоятельная QML-защита |
| R3 | 2 | Удаление до startup и отдельно до close; отсутствие runtime warnings/worker/project |

QML tests загружают production Main.qml. Для проверки фактического FileDialog.open()
используется его штатный DontUseNativeDialog option: OS dialog не нужен в CTest,
но production handler и visible/requestToken проверяются. QQmlEngine warnings
собираются и приводят к failure. Старые 431 case не изменены и не ослаблены.

Baseline evidence в ignored `build/windows-debug/`:
`044a-review-baseline-r1.log`, `044a-review-baseline-r2-r3-isolated.log`,
`044a-review-baseline-continuations.log`, `044a-review-baseline-state-signals.log`,
`044a-review-baseline-commands.log`. No-op SEH также зафиксирован до остановленного
неподдерживаемого probe в `044a-review-baseline-neighbours.log`.
Эти локальные logs не являются tracked artifacts; постоянное воспроизведение — tests.

## F. Debug/Release Verification

Окружение: Windows x64, VS 2026/MSVC v145, Qt 6.12.0 msvc2022_64, CMake/Ninja,
стандартная CMD-инициализация README. Использован уже существующий
`%TEMP%/cadcontour-044a.cmd`, задающий только process environment и рабочий каталог.
Команды запускались через CMD helper из PowerShell, stdout сохранялся Out-File
в соответствующий ignored build directory. Глобальное окружение не менялось.

Перед production исправлениями собран Debug gui_import_tests. Отдельные запуски:

```bat
build\windows-debug\gui_import_tests.exe --gtest_filter=GuiLifecycle.R1DeletionInsideZNotificationCancelsAndWakesWorker
build\windows-debug\gui_import_tests.exe --gtest_filter=*R2*:*R3*
build\windows-debug\gui_import_tests.exe --gtest_filter=GuiLifecycle.R1DeletionBeforeLaunchDoesNotStartImport:GuiLifecycle.R1DeletionWhileRevokingSelectionStopsNotifications
build\windows-debug\gui_import_tests.exe --gtest_filter=Signals/StateNotificationDeletion.*
build\windows-debug\gui_import_tests.exe --gtest_filter=Commands/CommandNotificationDeletion.*
```

Результаты baseline: R1 — SEH; R2/R3 — 6 failed, отдельный close case passed;
два selection continuations — SEH; четыре state boundaries — SEH;
commands — 4 SEH, cancel case passed. Исправления затем проверены сначала адресно:

```bat
cmake --build --preset windows-debug --target gui_import_tests --parallel 4
build\windows-debug\gui_import_tests.exe --gtest_filter=GuiLifecycle.*:Contexts/ReentrantShutdown.*:Signals/StateNotificationDeletion.*:Commands/CommandNotificationDeletion.*
cmake --build --preset windows-debug --target CADContour2D gui_import_tests view_model_tests qml_integration_tests nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests dependency_smoke operation_probe all_qmllint --parallel 4
cmake --build --preset windows-release --target CADContour2D gui_import_tests view_model_tests qml_integration_tests nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests dependency_smoke operation_probe all_qmllint --parallel 4
build\windows-release\gui_import_tests.exe --gtest_filter=GuiLifecycle.*:Contexts/ReentrantShutdown.*:Signals/StateNotificationDeletion.*:Commands/CommandNotificationDeletion.*
ctest --preset windows-debug -L production
ctest --preset windows-release -L production
git diff --check
```

| Проверка | Debug | Release |
|---|---|---|
| CADContour2D и перечисленные test/probe targets | PASS | PASS |
| all_qmllint | PASS | PASS |
| Новые адресные tests | 21/21 | 21/21 |
| Полный production CTest | **452/452**, 121.74 s | **452/452**, 54.27 s |

CTest включает GUI/UI, ViewModel, QML integration, 30 Z confirmation, import
application, parser, cache, operation и project regressions. Все команды завершились
с кодом 0. QML lint/runtime warnings в успешных тестах не обнаружены. При Release
regeneration осталось известное configure warning об отсутствующем Qt6TaskTree
для optional Qt6QmlAssetDownloaderPrivate; сборке/lint/tests оно не препятствует.
Logs: `044a-review-targeted.log`, `044a-review-build.log`, `044a-review-ctest.log`
в обоих build directories. Diff checks выполнены также перед commit.

Короткий native Windows smoke Debug через computer-use: startup/Cancel,
повторный выбор, импорт двух точек из `точки flat.xyz`, count/bounds/cell/cache metadata,
Cancel повторного выбора с сохранением проекта, нормальное закрытие — PASS, exit 0.
UI helper при следующем FileDialog дважды сообщил `element ... is not available in
cached app state`; продолжать native Z-сценарий не стали. Выбор отменён, приложение
закрыто. Z Continue/Cancel и shutdown при диалогах подтверждены автоматическими
production QML/application tests в обеих конфигурациях; повторный native Z/shutdown
smoke в этой работе не завершён. Native Release smoke повторно не запускался.

## G. Regression Assessment

Изменения не затрагивают parser, CacheWriter, формат Point Cache v1, worker algorithms,
ResourceLimits и structured diagnostics. Consumer protocol 04.3, Continue/Cancel,
context invalidation, native completion/drain, Ready/Accept и worker-owned PendingCache
не менялись. Полные suites подтверждают прежнее поведение первого импорта, replacement,
NoProject/Project/history, atomic publication, metadata/progress и Undo/Redo в пределах
существующих и добавленных regression scenarios.

Это целевая проверка R1–R3, не окончательная приёмка задачи 04. Большие scans,
HDD/UNC/performance/resource benchmarks и 04.4B не выполнялись. Не проводился полный
аудит всех QObject и всех произвольных reentrant observers приложения.

Внешние smoke/test artifacts предыдущих запусков, включая ранее заблокированную
очистку, не удалялись и не перезаписывались. Два прежних маленьких smoke inputs
только прочитаны. Новый принятый synthetic cache
`%LOCALAPPDATA%/CADContour2D/projects/project-3f8f0749-2862-4213-b51f-e4c2ad335c0e-umHesX`
оставлен вне Git по существующему cache retention contract. Временные данные
постоянных тестов принадлежат их QTemporaryDir; crash-baseline leftovers не удалялись.

## H. Final Verdict

**FIXED — READY FOR INDEPENDENT VERIFICATION.** R1–R3 независимо подтверждены и
исправлены; непосредственно связанные regressions и обязательные Debug/Release
проверки прошли. Изменения фиксируются одним commit после diff review.
Конечный HEAD и состояние дерева сообщаются после commit в итоговом ответе,
без самоссылочного hash в этом файле. Push не выполняется. 04.4B не начата,
задача 04 принятой не объявляется.
