# Проверка 04.3: межпоточное подтверждение ненулевого Z

## A. Baseline

Дата: 2026-10-18. Ветка `main`, исходный HEAD
`a197e611d03577f6e7f75f28ac4bc9334b8676be`. Рабочее дерево и index до изменений
чистые; фактический baseline совпал с заданием.

Изучены AGENTS.md, README, implementation-plan (04), requirements §§4–6,
architecture §§3–4, 13, 24–26, 39, algorithms §5, отчёты 04.1/04.2;
parser/importer, import adapter, operation types/runner/controller, ViewModel,
test access/support, CMake/presets и существующие import/operation tests.
Parser уже вызывал синхронный ZHandler один раз и проверял cancellation после
Continue. В 04.2 production передавал пустой handler и безопасно отклонял ненулевой Z.

Непосредственно связанное отклонение: Close из NoProject во время первого импорта
возвращал NoChange без invalidation. Теперь такой Close отзывает операцию и меняет
context epoch. Regression `CloseDuringFirstImportAlsoRevokesNoProjectContext`
проверяет это на настоящем ожидающем импорте. Границы архитектуры 04.2 сохранены.

## B. Implementation

Изменены:

- `src/application/operation_types.h` — fixed-size `NonZeroZRequest` и availability
  в immutable snapshot;
- `src/application/operation_runner.h/.cpp` — единственный request/response mailbox,
  worker wait, owner response, poll notification и синхронизированная отмена;
- `src/application/import_operation.cpp` — подключение синхронного handler к control;
- `src/application/project_controller.h/.cpp` — consumer registration/lifetime,
  запрос/ответ, уведомление, command/applicability checks, Close из NoProject;
- `tests/operation_test_access.h`, `tests/CMakeLists.txt` — два private worker hooks
  и новый test target;
- `tests/nonzero_z_confirmation_tests.cpp` — 29 новых cases;
- README, architecture, implementation-plan и этот отчёт.

`NonZeroZRequest` содержит OperationId, QUuid конкретного запроса, физическую
строку и binary64 Z. `optional<NonZeroZRequest>` присутствует ровно пока ожидается
ответ. Нет ссылок на parser/cache/worker, очереди, произвольного payload или
иерархии confirmations. UUID создаётся на запуске операции, до worker; повторный
вызов request в одной операции не создаёт нового запроса. UUID исключает применение
старого ответа при повторении локального OperationId в новом controller.

Application API:

```cpp
CommandResult setNonZeroZConsumer(QObject *consumer);
std::optional<NonZeroZRequest> pendingNonZeroZ() const;
CommandResult answerNonZeroZ(const NonZeroZRequest &, io::ZDecision);
// signal: nonZeroZConfirmationChanged()
```

Consumer регистрируется до запуска, живёт в потоке controller и обязуется
обслуживать запросы. Подключение другого consumer при Busy отклоняется; отключение
или уничтожение текущего отменяет активный import. QPointer не владеет consumer;
controller отключает destroyed connection перед собственным drain.

Путь: parser → synchronous ZHandler → WorkerControl → OperationContext mailbox →
OperationRunner::poll → controller signal/accessor → answerNonZeroZ → context
decision + notify → возврат ZDecision → прежний parser checkpoint.
Wrong/duplicate/stale identity и invalid enum дают Rejected/InvalidValue.
Wrong-thread, shutdown и reentrant calls дают обычные соответствующие command
errors. Getter вне owner thread возвращает пустой optional.

## C. Synchronization

Использован существующий `OperationContext::mutex/decisionChanged`.
Request, optional Z-decision, publication decision и изменение atomic cancellation
защищены тем же mutex. Чтение cancellation обычными checkpoints остаётся atomic.

| Состояние | Событие | Переход |
|---|---|---|
| Не запрошено | Первый Z при доступном consumer и применимой операции | Pending request, dirty mailbox |
| Не запрошено | Отмена/Discard до запроса | Cancel без ожидания |
| Pending | Валидный Continue | Identity потреблена, решение Continue, notify |
| Pending | Валидный Cancel | Identity потреблена, cancellation=true, notify |
| Pending | General cancel/invalidation/shutdown | Request снят, cancellation=true, notify |
| Resolved/revoked | Повторный/поздний ответ | Отклонён, состояние не меняется |
| Continue | Parser закончил вычисление | Отдельный Ready → Accept/Discard |

Predicate Z-wait: cancellation **или** publication decision != Pending **или**
недоступность consumer **или** отсутствие pending request **или** наличие Z-decision.
После wake Continue возвращается только при отсутствии cancellation/Discard и
наличии именно Continue. Остальные случаи дают Cancel.

Lost wakeup исключается протоколом: worker проверяет predicate под mutex; wait
атомарно освобождает mutex и засыпает. Cancel/response меняют predicate под тем же
mutex и затем уведомляют. Если отмена произошла до wait, predicate уже истинный;
если worker начал ждать, notifier получает mutex после освобождения и будит его.
Один только atomic store + notify вне протокола не используется.

Ни Z-handler, ни test hooks, ни Qt observers не вызываются под mutex. Worker
публикует dirty bit; GUI notifications переносит существующий poll без per-record
events. Dirty publication/clearing могут объединиться в одно уведомление; оно
означает перечитать актуальный optional. Controller может немедленно уведомить о
снятии запроса, а следующий poll — ещё раз; это ограниченные уведомления изменения.
Доставка, обнаруженная под controller command guard, откладывается singleShot до
возврата команды. Это сохраняет завершение startImport и установку source metadata
до возможного синхронного ответа/обработки Ready. Обычный consumer может ответить
прямо из notification; ответ из guarded state observer отклоняется.

Context удерживается shared_ptr runner/worker/control. Production handler
синхронно заимствует control на время вызова importer; не переживает его. Ни QObject,
ни controller, ни QML pointer не попадают в worker handler. Work/captures очищаются
по прежнему worker protocol. Z-wait заканчивается до публикации Ready, поэтому
worker не держит одновременно два ожидания. Busy/context освобождаются только
после прежних native completion и Qt join.

## D. Cancellation and Shutdown

Cancel до публикации не создаёт request. Cancel между публикацией и фактическим
wait делает predicate истинным; отмена во время wait будит worker. Контролируемые
hooks с Event barriers воспроизводят все три последовательности без sleep.

Ответы сериализуются в потоке controller и дополнительно mutex context. Первый
валидный response потребляет request; второй отклоняется. Continue перед общей
отменой не отменяет её необратимость: cancellation/store и отзыв application
publication права сохраняются, даже если вычисление уже закончено. Общая отмена
перед Continue отклоняет ответ. После Continue остаётся прежний parser checkpoint.
Пользовательский Z Cancel даёт WorkerStatus/OperationStatus::Cancelled без failure.

Relevant input change, Undo/Redo, смена/Close проекта используют прежний invalidation
и синхронизированный cancel. ABA ревизий/контекста не восстанавливает публикацию.
Shutdown сначала снимает Z-wait отменой, затем даёт Discard прежнему Ready-wait;
никакого ожидания между этими действиями нет. Irrevocable Accept не переписывается.

Destructor controller отключает lifetime connection, делает drain до уничтожения
session/history. Worker не требует events/callbacks GUI. Проверены как уничтожение
при Z-wait без единого poll, так и Continue/commit/completion без processEvents.
Late response после shutdown отклоняется как ShuttingDown; после completion,
отмены или в новой операции — как InvalidValue. После destruction вызывать методы
уничтоженного QObject нельзя; token предыдущего controller проверен на новом объекте.

## E. Integration

Контракты `io::NonZeroZ/ZDecision/ZHandler`, parser/importer API и исходники
`src/io` не менялись. При доступном consumer import adapter передаёт handler,
синхронно вызывающий `control.confirmNonZeroZ`. Поэтому во время ожидания
не исполняются дальнейшие source reads или Writer appends, остаются только прежние
bounded buffers. Continue продолжает тот же parse call, без rewind/повторного файла.

Без зарегистрированного consumer handler остаётся пустым: плоское облако
импортируется, первый ненулевой Z возвращает InvalidInput с диагностикой
`Nonzero Z requires a decision handler`. Request не создаётся, таймер ожидания
не нужен. Production GUI пока не регистрирует consumer; 04.4 подключит его к
готовому application API. Регистрация — явный контракт доступности ответа,
не автоматическое обнаружение произвольных signal connections.

Continue не является PublicationDecision::Accept. Metadata-only Ready,
borrowed lease, noexcept owner exchange/Accept, PendingCache/QTemporaryFile в worker,
порядок cleanup, persistent storage и формат Point Cache v1 сохранены.

## F. Tests and Verification

Windows x64, MSVC 14.51.36231, Qt 6.12.0. Использована предоставленная CMD
инициализация vcvars64/chcp/VCPKG_ROOT/QT_ROOT/PATH/VCPKG_MAX_CONCURRENCY=4.
Глобальные настройки/зависимости не менялись. Инкрементальная сборка автоматически
повторила configure обоих presets. Сохранились известные diagnostics
Qt6TaskTree/WrapVulkanHeaders и C4702 внутри стороннего fmt; новые исходники
собрались без новых warnings.

Фактически выполненные итоговые build-команды (Debug test target дополнительно
пересобран после последнего test case):

```bat
cmake --build --preset windows-debug --target CADContour2D nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests view_model_tests qml_integration_tests dependency_smoke operation_probe all_qmllint --parallel 4
cmake --build --preset windows-release --target CADContour2D nonzero_z_confirmation_tests import_application_tests asc_xyz_tests operation_tests project_tests view_model_tests qml_integration_tests dependency_smoke operation_probe all_qmllint --parallel 4
cmake --build --preset windows-debug --target nonzero_z_confirmation_tests --parallel 4
ctest --preset windows-debug -R ^AscConfirmation --output-on-failure
ctest --preset windows-release -R ^AscConfirmation --output-on-failure
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
```

| Проверка | Debug | Release |
|---|---:|---:|
| Production/test build | PASS | PASS |
| `all_qmllint` | PASS | PASS |
| Новые confirmation tests | 29/29 PASS, 0 FAIL | 29/29 PASS, 0 FAIL |
| Полный production CTest | 393/393 PASS, 0 FAIL, 69.70 s | 393/393 PASS, 0 FAIL, 40.27 s |
| Существующие import application в production | 22/22 PASS | 22/22 PASS |
| Существующие ASC/XYZ в production | 42/42 PASS | 42/42 PASS |
| Существующие operation в production | 51/51 PASS | 51/51 PASS |
| Существующие cache в production | 166/166 PASS | 166/166 PASS |
| QML integration/registration в production | 14/14 PASS | 14/14 PASS |
| `git diff --check`, затем `git diff --cached --check` | PASS | общий diff |

Первый Release CTest остановился при discovery старого view_model_tests:
`--gtest_list_tests` превысил стандартный CMake discovery timeout, до запуска
confirmation cases. Отдельный запуск executable с тем же флагом завершился с 0;
повторный discovery, адресный набор и затем полный production прошли. Причина
единственного startup timeout не установлена; лимиты/тесты не ослаблялись.

Новые tests используют настоящие QThread worker и production request/response
API. Большинство проходят реальный файловый importAscXyz → parser → CacheWriter →
PendingCache → controller publication. Cases первого Z в начале/середине/конце
обрабатывают 10 000 measurements, несколько read blocks и batches; CacheReader
проверяет metadata, полноту/уникальность RecordId, биты X/Y и validate.
Проверены signed zero, subnormal, один request при последующих ненулевых Z,
отсутствующий/отключённый/уничтоженный consumer, отмена замены с обеими history
ветками, invalid/late/duplicate tokens, Busy и отдельный publication gate.

Concurrency seams — только два private hooks до publication и между publication/wait,
Event/condition-variable barriers и наблюдаемый `waitingForZ` под mutex. Timeouts
защищают harness, не задают production поведение. Artificial sleeps не добавлены.
Контекст wait наблюдается после атомарного освобождения mutex worker, а не по времени.
Сценарии state/context replacement используют существующий friend fixture.
Paused-I/O case отдельно соединяет реальный parser и CacheWriter с подклассом QFile,
считающим reads/seeks; это verification operation, а не второй importer. При pause
передано ровно 4096 measurements, счётчики не меняются при poll; после Continue
переданы все 12 000, seek не было, завершённый cache проверен Reader. Это не mock
машины состояний. Test seams не доступны production QML.

## G. Regression Assessment

Задача 02: production operation cases сохранили Ready/Accept/Discard, borrowed
lease при shutdown, irreversible Accept, late completions, TLS/native completion,
удержание Busy до полного worker exit и destructor fallback. ProjectState/Undo,
ViewModel и QML cases полного production также прошли.

04.1: parser/importer не изменены; golden binary64, signed zero/subnormal,
malformed/range/source/sink failures и cancellation checkpoints прошли.

04.2: все 22 существующих import cases прошли, включая atomic session/history
publication, worker-thread destruction настоящего QTemporaryFile при
Accept/Discard/exception, сохранение cache при Accept → Close/Shutdown до worker
finalization, cleanup, сохранение старого проекта/history, lease и native TLS.
Новые confirmation cases проверяют продолжение полного cache pipeline и запрет
публикации после Continue + cancellation. Частичные результаты текущими не становятся.

## H. Limitations

QML consumer/dialog, FileDialog, визуальная приёмка импорта и финальные большие
HDD/UNC/end-to-end прогоны относятся к 04.4 и здесь не выполнялись. Save/Open и
recovery остаются задачей 05. Отдельный cache_integration test вне production
не запускался; новые inputs малы. HDD для 04.3 не понадобился.

Явно зарегистрированный consumer должен действительно отвечать или отключаться;
намеренно не отвечающий живой consumer может ждать пользователя без timeout.
Он обязан сохранять Qt thread affinity controller. Во время shutdown/destruction
ожидание не зависит от consumer/event loop. Задержка отмены блокирующего системного
I/O вне Z-wait остаётся прежней; во время Z-wait новых чтений нет.

UUID использует прежний Qt UUID механизм проекта, без математической гарантии
отсутствия случайных коллизий. Проверки и анализ протокола подтверждают конкретные
инварианты/последовательности, а не абсолютное отсутствие любых конкурентных ошибок.
Thread/data race sanitizer и независимое ревью в этом этапе не выполнялись.

## I. Final Verdict

**READY FOR REVIEW.** 04.3 реализована; обязательные Debug/Release проверки прошли,
подтверждённых блокирующих defects не обнаружено. 04 в целом не объявляется принятой.

Изменения и отчёт фиксируются одним commit в `main` после просмотра diff и проверок.
Конечный HEAD и фактическое состояние дерева после commit приведены в итоговом
сообщении. Push не выполняется. Переход к 04.4 и независимое ревью — отдельные задания.
