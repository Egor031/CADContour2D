# Task 05.4 — Autosave / Recovery

**Статус: READY FOR INDEPENDENT REVIEW.** 05.1–05.3 остаются ACCEPTED;
05.5 — NOT STARTED. Вся задача 05 не завершена и не принята.

## A. Baseline

2026-10-21, Windows x64. Branch `main`, исходный HEAD
`4d0f45147cbe9a0675b5fa3ad92efb5301c629c1`. Рабочее дерево и index чистые.
Сведения приложенного задания совпали с репозиторием. Изучены AGENTS.md,
README, requirements §§31–33, architecture §§24–31/39/43–44, применимые правила
algorithms, implementation-plan 05, manifest v1, принятые отчёты 05.1–05.3,
controller/state/runner, Save/Open/codec/resolver и соответствующие tests.

Использован имеющийся C++20/MSVC 19.51.36260 (toolset 14.51.36231), Qt 6.12.0,
CMake 3.30.5/Ninja 1.12.1. Vcpkg подтвердил установленные Eigen 5.0.1,
GoogleTest 1.17.0#3, OpenCV 4.12.0#7, spdlog 1.17.0#1; новых зависимостей нет.
Сборки/процессные tests выполнялись вне sandbox: штатная автоконфигурация vcpkg
в sandbox получила access denied к download cache AppData. Qt bin добавлялся
только в PATH запускаемого процесса, глобальное окружение не менялось.

## B. Design

Устойчивый application/lifecycle контракт — [architecture §31.1](../../architecture.md#311-c-autosave--recovery-054).
Одна текущая recovery-запись на recovery lineage, без backup history/GC.
Новый runtime session при restore использует тот же lineage UUID, чтобы исходный
record и datasets оставались доступны до следующей публикации.

Recovery v1 — 10 ASCII lines с завершающим LF:

| Line | Значение |
|---|---|
| 1 | `PC2D-RECOVERY 1` |
| 2 | Ненулевой canonical lowercase session UUID |
| 3 | `open`, `closed` или `rejected` |
| 4 | Canonical base64 UTF-8 абсолютного dataset root |
| 5 | Canonical base64 UTF-8 физического относительного dataset path |
| 6 | Canonical base64 существующего manifest v1 codec для текущего snapshot |
| 7 | Base64 UTF-8 absolute explicit manifest path либо пустая строка |
| 8 | Base64 manifest codec explicit baseline либо пустая строка |
| 9 | Lowercase hex SHA-256 точных explicit manifest bytes либо пустая строка |
| 10 | Lowercase hex SHA-256 lines 1–9, включая их LF |

SnapshotId внутри manifest различает публикации. Outer digest используется как CAS
при обновлении/reject; одинаковая runtime revision ничего не доказывает о content.
Base64 wrapper нужен только для framing существующего codec; универсальный второй
serializer не введён. Checksums обнаруживают повреждение, не аутентифицируют файлы.
Проверяются signature/version/число lines/checksum/UTF-8/base64/UUID, manifest schema
и ссылки, согласованность ProjectId baseline/state, absolute и portable relative
paths. Root должен быть absolute/clean; traversal, ADS, device components и NUL
отклоняются. Descendant reparse points проверяются существующим resolver при I/O.
Metadata admission profiles: 12 MiB/record, 1024 records и 64 MiB прочитанных metadata
на discovery, включая explicit baseline; manifest limits — 4 MiB/4096 tokens/depth 32.
Discovery имеет cancellation checkpoints между records. Это caller-overridable
бюджеты metadata, не общий предел RAM/Point Cache.

Snapshot хранит logical metadata, отдельный physical binding — уже принятый cache.
Это позволяет использовать импорт до первого Save, не изменяя canonical dataset
layout `.pc2dproj` v1 и не копируя cache. Retention сохраняет принятые файлы на диске
независимо от shared_ptr/process lifetime. Новые большие datasets не создаются.

## C. Implementation

`src/persistence/recovery_store.*`: bounded framing/codec, discovery, lifetime marker,
restore/reject/close и QSaveFile transaction с private fault seams. Win32 enumeration
отличает отсутствие root/записей от отказа доступа; discovery не пишет lock/marker
или recovery. Плохая запись возвращается как Invalid с structured Error.

`project_open.*`: вынесен metadata-only `bindProjectSnapshot`, общий bounded probe
читает физическую ссылку DatasetLocation. Обычный Open по-прежнему получает её
из проверенного manifest. Save permission/receipt и форматы v1 сохранены.

Controller API:

```cpp
CommandResult autosave();
CommandResult discoverRecovery(persistence::RecoveryLimits limits = {});
const std::vector<persistence::RecoveryCandidate> &recoveryCandidates() const noexcept;
CommandResult restoreRecovery(RecoveryRequest request);
CommandResult rejectRecovery(persistence::RecoveryCandidate candidate,
                             persistence::RecoveryLimits limits = {});
const persistence::RecoveryRecord *recoveryRecord() const noexcept;
bool recoveryRequiresSaveAs() const noexcept;
CommandResult closeProject(bool discardConfirmed = false);
CommandResult requestShutdown(bool discardConfirmed = false);
```

`RecoveryRequest` содержит candidate, metadata/reader budgets, replacementConfirmed
и optional expectedContext. `recoveryChanged` отделён от domain notifications.
Ошибки сохраняются в `OperationFailure::recoveryError`; низкоуровневые модули
не показывают dialogs. Новые Worker/Ready products проходят typed transport check.

Autosave использует существующие native completion, Ready lease и noexcept owner
permission. Transaction готовится/пишется/flush-ится на worker; atomic QSaveFile commit
выполняется там же после Accept, а receipt обрабатывается только после native join.
Cancel до permission отбирает публикацию; cell во время записи не откатывается.
Background Busy не создаёт очередь. Явный start отменяет pending Autosave и требует
повторного вызова после join; принятая permission дожидается результата.

## D. Recovery Scenarios

| Сценарий | Поведение |
|---|---|
| До первого Save | Точный cell/ProjectId/source metadata/cache reference; baseline отсутствует, dirty=true |
| Explicit save не изменён | Восстановленный baseline, dirty по persistent content; последующий обычный Save работает |
| State совпадает с текущим explicit | Clean, даже если snapshotId последнего эквивалентного Save отличается |
| Независимо изменённый/недоступный explicit | Conflict, detached unsaved state, обычный Save запрещён, нужен Save As |
| Missing/corrupt cache | Metadata и параметры сохраняются, зависимые команды закрыты, явный bounded recheck возможен |
| Active owner | Discovery показывает Active; другой restore/reject не получает эксклюзивный marker |
| Reject | Атомарный Rejected; failure сохраняет исходную пригодную запись; datasets не удаляются |
| Close/Shutdown без Discard | Closed с последним опубликованным snapshot; отсутствует подразумеваемый отказ от правок |

Restore готовит candidate без изменения действующего проекта/истории. После final
applicability check обменивается единственный ProjectSession: новый runtime identity,
прежний ProjectId и точный cell, revision=0, пустые Undo/Redo. При conflict независимый
explicit save не перезаписывается. Перед Ready для Closed записи публикуется только
Open lifecycle с прежним state; отменённая попытка может остаться Interrupted, но
исходные данные не теряются. Kernel marker удерживается и после Accept до следующего
snapshot/закрытия, без разрыва ownership при crash сразу после восстановления.

## E. Safety

QLockFile сериализует writers, expected digest запрещает stale read/modify/write,
directory HANDLE удерживает root от rename/delete. Atomic QSaveFile replacement
с direct-write fallback=false сохраняет опубликованное состояние при short/failed
write, flush/publication failures и отмене до permission. Поздний cleanup failure
возвращает warning, не удаляя новый record. Explicit files и datasets не записываются.

UUID kernel event в Global namespace определяет живого владельца без timestamp/PID
эвристик; он не оставляет persistent lock после crash. Ограничение — один Windows
host: общий recovery root между компьютерами не поддержан. Неудача наблюдения marker
является диагностикой, а не разрешением занять чужое состояние.

Lifecycle Closed/Rejected выполняется последовательно тем же managed runner после
завершения предыдущего worker. Shutdown Ready ждёт native completion обоих этапов.
Destructor fallback drain работает без event delivery; ошибки оставляют целый record,
который может консервативно остаться Interrupted. Неподтверждённый shutdown не
удаляет recovery. Только явный discardConfirmed маркирует пользовательский Discard.
Accepted datasets, старые bindings и неидентифицированные файлы не удаляются.

## F. Tests

Финальная контрольная точка на окончательных исходниках:

| Проверка | Debug | Release |
|---|---|---|
| Build `all` и `all_qmllint` | PASS | PASS |
| Полный production CTest, 668 tests | 667 PASS / 0 FAIL / 1 SKIP | 667 PASS / 0 FAIL / 1 SKIP |
| Recovery application/integration (в составе production) | 58/58 PASS | 58/58 PASS |
| Process crash tests (в составе Recovery) | 7/7 PASS | 7/7 PASS |
| Остальные production regressions, включая persistence/Save/Open/operations/shutdown/UI | 609 PASS / 0 FAIL / 1 SKIP | 609 PASS / 0 FAIL / 1 SKIP |
| Отдельный resource smoke с XML properties | 2/2 PASS | 2/2 PASS |

SKIP — прежний `StoragePath.FileSymlinkEscape`: Windows privilege недоступен,
error 1314. Реальный junction test выполнен в production. Финальный production
wall time: Debug 173.21 s, Release 106.62 s; это время tests, не performance benchmark.
Финальные локальные логи — `build/production-{debug,release}-054-final.log`,
подробности — `build/windows-{debug,release}/Testing/Temporary/LastTest.log`.

Проверенные команды после штатной x64 MSVC/Qt инициализации из README:

```bat
cmake --build --preset windows-debug --target all all_qmllint --parallel 4
cmake --build --preset windows-release --target all all_qmllint --parallel 4
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
ctest --preset windows-debug -R ^RecoveryApplication --output-on-failure
```

Набор Recovery включает cell round-trip, timer/first import, no-change/Undo,
изменение во время write, Busy без очереди, baseline/dirty, restore/reject/restart,
conflict/Save As, partial availability/recheck, active instance, stale callbacks,
reentry, shutdown/cancel до и после permission, destructor без receipt, schema/paths
и resource budgets. Private seams инъецируют open/write/short write/flush/commit,
disk-full return и cleanup failure; реальный Windows read-sharing lease блокирует
замену record. Физически заполнение всего тома не выполнялось.

`recovery_crash_probe` изолирован и завершается parent `QProcess::kill()` без
destructors. Проверены: перед первой публикацией; до commit обновления; частично
записанный новый record; после commit до controller receipt; active второй процесс;
после Accept до следующего snapshot, включая исходный Closed record. Проверяется
целый old/new snapshot, точный cell, доступный completed cache и ноль point pages.
До самой первой публикации recovery отсутствует, а завершённый cache удерживается;
незавершённый record никогда не предлагается для восстановления.

Промежуточные неуспехи не скрыты: первый Recovery прогон выявил пропущенные новые
product kinds в runner validation; следующий 50/51 — пропуск lifecycle work при
немедленном destructor cancellation. Исправлены typed validation и специальный
metadata-only CloseRecovery drain. Расширенный набор 56/56 затем прошёл.
Первый полный production в каждой конфигурации имел 3 failures: ранее синхронные
test setups доставляли Autosave tick во время ожидания другого процесса/GUI events.
Добавлено ожидание native завершения именно фонового Autosave перед проверяемой
явной командой; Save-lock, identity и dialog assertions сохранены, admission Import
дополнительно проверен. Узкая повторная проверка этих трёх и двух новых crash tests —
5/5 PASS Debug. Тесты не отключались, timeout/диагностика не ослаблялись.

Одна промежуточная Debug link получила LNK1163 в nonzero_z_confirmation_tests;
после удаления только сгенерированного project_controller.cpp.obj и пересборки
ошибка не повторилась, linker flags не менялись. Причина самого COMDAT failure
отдельно не доказана. Configure сохранил известные необязательные Qt6TaskTree/Vulkan
diagnostics, compiler — warning C4702 в стороннем fmt/base.h.

## G. Resources

Autosave/discovery проходят при source, удалённом после Import, и cache, закрытом
реальным эксклюзивным Win32 handle: cache/source не открываются и не копируются.
Число cache files остаётся одним. Restore ReadStatistics подтверждают 73 728 read bytes,
ноль point pages; source не нужен. CacheReader::validate/scanAll, ASC/XYZ importer,
cache writer/sort, PNG/reduced build и geometry не входят в recovery call graph.
Fixtures — две точки, без гигабайтных тестовых данных.
Отдельный resource smoke — 2/2 PASS в каждой конфигурации, XML properties:
73 728 bytes bounded cache read и 1789 bytes unsaved recovery record на этом fixture
(размер record зависит от paths/metadata). Эти числа не являются benchmark большого
облака. Peak Working Set, большие
performance benchmarks, HDD/UNC и физический power-loss не измерялись.

## H. Documentation and Git

Обновлены requirements §33 (уточнение уже заданного поведения), architecture §31.1
и актуальные указатели, manifest retention note без изменения схемы, implementation
plan, README/docs index и этот отчёт. Algorithms не изменялся: вычислительных методов
не добавлено. Scope ограничен 05.4; GUI workflow и задачи 06–12 не реализовывались.
`git diff --check` и `git diff --cached --check` — PASS, полный diff просмотрен.
Несвязанных пользовательских изменений не было; временный environment wrapper удалён.
Локальный implementation commit содержит этот отчёт; hash сообщается в итоговом
ответе после commit и проверки Git status. Push не выполняется.

## I. Limitations

- Гарантии относятся к завершению процесса и обычным filesystem failures. Физическая
  потеря питания, hostile writers/TOCTOU/hard links, UNC и несколько Windows hosts
  с общим recovery root не подтверждены/не поддержаны.
- Сохраняется последний опубликованный snapshot; между изменением и очередным
  успешным Autosave есть окно потери последних правок. До первой публикации snapshot
  отсутствует. Низкоуровневый Close не принуждает final Save; GUI 05.5 должен
  реализовать Save/Discard/Cancel и осмысленное предложение восстановления.
- Failure закрывающего marker либо отменённый restore после lifecycle Open может
  оставить консервативный Interrupted. Данные остаются пригодными; это не доказательство
  точной причины неожиданного завершения.
- Conservative retention допускает накопление accepted caches и crash scratch.
  Отсутствие runtime references и Reject не доказывают право удалить dataset; GC нет.
- AvailableLimited не означает полную integrity validation всех страниц/RecordId.
  Disk-full проверен fault injection, а не заполнением реального диска.

## J. Final Assessment

**05.4 — READY FOR INDEPENDENT REVIEW.** Независимое ревью и принятие 05.4 ещё не выполнялись.
05.5 и вся задача 05 остаются незавершёнными.
