# Проверка 04.2: ProjectState, ownership и атомарная публикация

## A. Baseline

Дата: 2026-10-17. Ветка `main`, исходный HEAD
`eaaf9bec02c856fcb2ac9edc0876eb24478d97c4`. Рабочее дерево до начала чистое.
Последние изменения — исправления R1/R2 после независимого ревью 04.1;
headless parser/importer уже реализованы, их числовой conversion не менялся.
Задачи 01–03 сохраняют статус ACCEPTED.

Изучены AGENTS.md, README, задача 04 implementation-plan, относящиеся к импорту,
project lifecycle, границам слоёв, operations, persistence и ресурсам разделы
requirements/architecture, algorithms §5 и Point Cache v1. Также изучены отчёты
asc-xyz-import-check/asc-xyz-review-fixes, parser/importer, Writer/PendingCache,
PublishedFile, Reader, operation types/runner/native completion, controller,
ProjectState, ViewModel/QML и project/operation/import/cache/QML tests.

## B. Architecture Changes

- `ImportedDataset` — Qt-независимые domain metadata: CacheId, исходный путь,
  bytes/physical lines, count, X/Y bounds, Z, storage ID/directory и относительный
  cache path. Доступ: `controller.project()->dataset()`. Точки в state не хранятся.
- CacheId перенесён в `core/cache_id.h`, сохранив namespace, тип, реализацию и
  двоичное представление. Это общий value identity, без storage или Qt dependency
  в core. `CacheId::create/isNull` по-прежнему реализованы cache library.
- Application-owned `ProjectSession` объединяет state, retention reference и
  настоящий QUndoStack. Контроллер меняет один unique owner.
- `ImportRequest` содержит sourcePath, ResourceLimits и replacementConfirmed.
  `ProjectController::importAscXyz` запускает настоящий importer через единственный
  OperationRunner; перегрузка конструктора с storage root обеспечивает изоляцию tests.
- Сохраняются single-worker, bounded progress mailbox, cancellation, Ready lease,
  единственное решение, независимый Windows HANDLE и Qt join.

Новых зависимостей, registry или runtime-polymorphic products нет. Parser, Writer,
sort/index, низкоуровневый publication protocol и binary format v1 не менялись.
В cache headers добавлены только friend seams для наблюдения реального guard в tests.

## C. Typed Product Transport

`WorkerOutcome` различает конкретные `VerificationProduct` и `WorkerImportProduct`.
Успех допускает ровно один из них. Import product содержит `io::ImportProduct`,
guard каталога попытки и уже подготовленный `ImportReadyProduct`.

WorkerOutcome — локальная переменная `OperationRunner::execute`, она не кладётся
в OperationContext. Context хранит отдельный `ReadyOutcome`, где import представлен
только optional metadata/path values. Структурно PendingCache и QTemporaryDir там нет.
Копирование import metadata не выделяет память, что проверено static_assert для
установленного Qt/STL. До Ready worker сверяет metadata/path с настоящим PendingCache,
включая CacheId, count, биты bounds и source counters.

Lease даёт const ReadyOutcome и удерживает worker cleanup. Verification по-прежнему
можно принять в изолированный sink, не превращая его в domain dataset. Import нельзя
извлечь через `takeProduct()`: этот метод относится только к verification token.

## D. Ownership and File Lifecycle

| Фаза | Владелец и действие |
|---|---|
| Computing | Worker создаёт уникальный QTemporaryDir с autoRemove=true, отдельный scratch и настоящий importer. Writer возвращает PendingCache с тем же QTemporaryFile guard. Все объекты создаются в worker. |
| Ready | PendingCache остаётся в локальном WorkerOutcome. В context опубликованы metadata; наличие готового файла ещё не означает Accept проекта. Scratch импортера к этому моменту освобождён. |
| Preparation | Application создаёт новый state/storage reference/history. Worker ждёт решения и отсутствия lease. Старый проект и history действительны. AutoRemove обоих guards остаётся включённым. |
| Discard | После освобождения lease worker уничтожает import product: сначала PendingCache/QTemporaryFile, затем каталог попытки. Удаляются только данные этой попытки. |
| Accept | Application принимает подготовленный session и фиксирует необратимое решение под decision mutex. PersistentStorage принимает обязанность сохранять файл; worker guard до финализации действует от имени этого принятого решения. |
| Completion | В worker отключается autoRemove каталога, вызывается PendingCache.releaseToOwner(), уничтожающий QTemporaryFile в его потоке. Остальные worker captures очищаются. Busy снимается только после native completion и Qt join. |

Между commit и регистрацией Accept нет callbacks, событий, allocations или файловых
операций. Commit выполняется внутри `ReadyProductLease::accept` под тем же mutex,
которым защищены Discard/decision. Повторная отмена под mutex не может пропустить commit.
Shutdown меняет только Pending, не переписывает Accept.

После Accept controller/project не владеют удаляющим guard. Даже немедленное закрытие,
замена сеанса или destructor controller не могут удалить принятый файл. Worker хранит
свой product независимо от жизни нового ProjectState и обязательно финализирует уже
принятое владение. Destructor controller сначала drain/join, затем уничтожает session.
Ни один оставшийся GUI-owned OperationContext не способен уничтожить файловый guard.

Если приложение аварийно завершено, RAII не гарантирован: могут остаться attempt,
scratch и уже переименованные непринятые caches. Scavenger/recovery не добавлялись.
Прежние ограничения flush/OS deletion/power loss сохраняются; гарантия fsync не заявлена.

## E. Project Lifecycle

Первый import запускается из NoProject без фиктивного state/identity. Только success
правильного типа с применимым запросом создаёт session. Ошибка/отмена не создаёт проект.

При открытом проекте запрос без replacementConfirmed отклоняется до запуска.
Разрешение хранится в snapshot конкретного запроса вместе с OperationId и исходным
контекстом. До success прежний проект, параметры, Undo и redo-ветка сохраняются.
После success: новая монотонная runtime identity, revision=0, cell=1 мм, пустая история;
Undo не возвращает прежний проект. Прежний принятый cache остаётся на диске.

Production `newProject()` больше не создаёт пустое состояние. До FileDialog в 04.4
он отклоняется как ImportRequired, кнопка QML отключена, main не вызывает его при
запуске. Новый GUI import в 04.2 не реализован. Domain tests/dev probe используют
явный friend fixture для прежних cell/Undo/verification-инвариантов; fixture не
включён в CADContour2D.exe. Реальный lifecycle проверяется отдельным import target.

## F. Atomic Publication

1. Gate получает lease и проверяет успешный тип и применимость операции.
2. Preparation проверяет metadata, count/source/Z, storage/source/path correspondence,
   наличие cache и исчерпание identity/epoch; затем создаёт domain state, storage
   reference и пустой QUndoStack. Count/bounds копируются из CacheMetadata, облако
   не перечитывается и не пересчитывается.
3. Непосредственно перед commit повторно проверяются Open, OperationId, Pending,
   cancellation/revocation, optional исходный ProjectStamp, context epoch и разрешение
   замены. NoProject → иной контекст → NoProject не восстанавливает применимость.
4. Lease под mutex дополнительно проверяет Pending и atomic cancellation. Noexcept
   callback меняет только unique owners, counters и решение controller; затем в том
   же lock фиксируется Accept worker. Никаких fallible filesystem/history действий.
5. Старый session уничтожается после обмена. Его QUndoStack объявлен после state и
   уничтожается раньше него. Внешние уведомления идут после согласованной замены и
   Accept; повторные project/import-команды получают ReentrantCommand.

Любой отказ preparation сохраняет прежний session. bad_alloc преобразуется в
фиксированный OutOfMemory без конструирования диагностической строки, остальные
отказы candidate — CandidatePreparationFailed; некорректный product — InvalidProduct.
DecisionGuard обеспечивает Discard всех непринятых Ready. Повторные/поздние Ready
не приводят к повторному commit. requestShutdown во время preparation не ждёт lease:
он фиксирует Discard и возвращает, после выхода из gate lease освобождается.

## G. Storage and Diagnostics

Default root: `QStandardPaths::AppLocalDataLocation/projects`. Структура:

```text
project-<storage UUID>-<unique suffix>/
    datasets/<attempt UUID>.pc2dcache
    scratch-<unique suffix>/            # только до Ready
```

QTemporaryDir создаёт каталог попытки эксклюзивно; cache Writer сохраняет no-overwrite
rename. Storage UUID, attempt UUID, CacheId, OperationId и runtime identity независимы.
В core сохранены directory и относительный dataset path; source — внешняя ссылка.
Это позволяет впоследствии добавить относительные `.pc2dproj` references.

Принятые caches остаются между запусками и после замены/закрытия проекта. До 05
нет manifest, Save/Open/Recovery или GC; накопление файлов — явное ограничение.
Изолированные QTemporaryDir roots тестов не затрагивают пользовательские caches.

`OperationFailure::importError` сохраняет полный доступный cache::Error: code,
phase, operation, offset, deviceCode, bounded detail и source location/token.
Так различимы invalid input, source open/read, parsing/range, resource budgets,
cache I/O/publication, отдельно от candidate/worker execution failures. Отмена —
отдельный WorkerStatus, неприменимость — Discard без маскировки под success.
Cleanup не подменяет первопричину. Полное русское форматирование ошибок для GUI — 04.4.
Отказ выделения памяти при подготовке запроса до запуска runner возвращает
CommandResult::Rejected с ResourceUnavailable/OutOfMemory без динамической строки;
он ещё не является принятой операцией и не меняет текущий проект.

Production Z-handler отсутствует: при первом проверенном ненулевом Z parser
возвращает InvalidInput. Тестовый handler через friend access проверяет Continue
и Cancel; нет диалога, межпоточного ожидания ответа или confirmation framework.

## H. Verification

Изменены CMakeLists/tests CMake; core ProjectState/ImportedDataset/CacheId и command
details; application import adapter/types/runner/controller; main/QML/ViewModel;
существующие project/operation/ViewModel/QML fixtures, test access и dev probe.
Добавлен `import_application_tests.cpp`; обновлены README, architecture,
implementation-plan и сведения об интеграции в point-cache-format.

Новый target содержит 22 детерминированных tests; основные сценарии используют
настоящие Parser/Writer/Reader:

- первый import, metadata/CacheId, signed zero bounds, cell/history, удаление source
  после успеха, cache scan и строгая validation;
- подтверждённая/отклонённая замена, сохранение обеих caches и сброс redo-ветки;
- parsing/range/empty/nonzero-Z/open/resource/storage failures с исходными diagnostics;
- cancellation до entry, при настоящем parsing и после Ready, в обоих контекстах;
- bad_alloc/exception, invalidation/cancel/shutdown во время candidate preparation;
- неверные metadata, отсутствие cache, неверный тип, NoProject ABA, close/change/Undo;
- согласованное уведомление, отказ reentry, повторные Ready/решения;
- QObject::destroyed настоящего QTemporaryFile в worker при Accept/Discard/exception;
- заблокированная финализация после Accept, immediate Close/Shutdown, destructor
  с Ready/Accept без GUI callbacks; shutdown во время borrowed preparation;
- реальный import с блокированным C++ TLS destructor: native TIMEOUT удерживает
  Busy/Closing; shutdown из accepted stateChanged и исчерпание project identity;
- verification operation после настоящего import.
- пустой запрос, wrong-thread call и исчерпание OperationId без создания попытки.

Синхронизация использует Events/CV и существующие seams, без произвольных sleep.
Остальные варианты native failure/join/late notifications проверяет прежний operation
набор, сохранённый после расширения транспорта. Небольшие caches и budgets не являются
benchmark или representative real scan validation.

CMD-среда пользователя: vcvars64.bat, chcp 65001, Qt 6.12.0, MSVC 14.51.36231,
CMake/Ninja в локальном PATH и VCPKG_MAX_CONCURRENCY=4. В обеих конфигурациях:

```bat
cmake --build --preset windows-debug --target import_application_tests operation_tests project_tests view_model_tests qml_integration_tests CADContour2D operation_probe asc_xyz_tests cache_tests dependency_smoke all_qmllint --parallel 4
cmake --build --preset windows-release --target import_application_tests operation_tests project_tests view_model_tests qml_integration_tests CADContour2D operation_probe asc_xyz_tests cache_tests dependency_smoke all_qmllint --parallel 4
ctest --preset windows-debug -R ^AscApplication --output-on-failure
ctest --preset windows-release -R ^AscApplication --output-on-failure
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
build\windows-debug\operation_probe.exe --auto replace
build\windows-release\operation_probe.exe --auto replace
git diff --check
```

Помимо перечисленного, до последней правки startup OOM выполнен адресный Debug
набор project/operation/ViewModel/QML: 84/84 PASS. После правки повторены сборки,
focused import tests и полные production suites; итоговая таблица ниже.

| Проверка | Debug | Release |
|---|---:|---:|
| Сборка перечисленных targets | PASS | PASS |
| QML lint (`all_qmllint`) | PASS | PASS |
| Новые import integration (`-R ^AscApplication`) | 22/22 PASS, 5.41 s | 22/22 PASS, 4.34 s |
| Полная production suite (`-L production`) | 364/364 PASS, 73.14 s | 364/364 PASS, 44.01 s |
| Прежние operation tests, внутри production | 51/51 PASS | 51/51 PASS |
| Project/coordinate tests, внутри production | 32/32 PASS | 32/32 PASS |
| ASC/XYZ regression, внутри production | 42/42 PASS | 42/42 PASS |
| Cache regression, внутри production | 166/166 PASS | 166/166 PASS |
| ViewModel tests, внутри production | 36/36 PASS | 36/36 PASS |
| QML integration/registration, внутри production | 14/14 PASS | 14/14 PASS |
| Dev probe `--auto replace` | PASS, exit 0 | PASS, exit 0 |
| `git diff --check` / staged diff check | PASS | общий diff |

Локальные полные build/CTest logs лежат в игнорируемом `build/asc-application-*.log`;
в commit не включены. Старые тесты остались в полном составе; новые добавили 24
cases к прежним 340 (22 import, 1 ViewModel, 1 QML startup).

Новых compiler warnings в собственном коде не наблюдалось; остаются прежние
необязательные Qt6TaskTree/WrapVulkanHeaders и C4702 стороннего fmt/base.h.
Первичные compile errors нового кода (overload nullptr и Qt signals macro) исправлены
до успешных build/test runs; корректные тесты не отключались и не ослаблялись.

Не выполнялись: гигабайтный ASC/XYZ benchmark, HDD/UNC import, настоящее process-wide
OOM, авария питания/враждебная внешняя подмена immutable файлов, новый ручной GUI smoke.
Низкоуровневый cache algorithm/API не менялся (кроме расположения value declaration
и friend test seams); отдельный длительный cache integration не повторялся. Production
cache regressions и реальные application round-trips входят в обязательный набор.
HDD для этой задачи не понадобился. Полный пользовательский сценарий остаётся 04.3–04.4.

## I. Final Verdict

**READY FOR INDEPENDENT REVIEW.** 04.2 реализована, необходимые автоматические
проверки выполнены в Debug/Release. Подтверждение Z, полный GUI и итоговая
large-file/end-to-end приёмка остаются следующим частям 04. Вся задача 04
не объявляется принятой; статус ACCEPTED не присваивался.
