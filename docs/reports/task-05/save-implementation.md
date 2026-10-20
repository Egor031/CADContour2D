# Task 05.2 — Save / Save As

Текущий статус: **05.2 — ACCEPTED** (2026-10-20).
[Финальное независимое заключение](#k-independent-verification-and-acceptance) приведено в §K;
разделы A–J сохраняют результаты и промежуточные статусы соответствующих этапов.

## A. Baseline

Дата: 2026-10-19. Branch `main`, исходный HEAD
`dd0e5f4e0e42f7dc9b2c423caf830bc2330cc5b8`; index и рабочее дерево чистые.
Снимок из задания совпал с локальным. 05.1 на `408757c` уже ACCEPTED,
не реализовывалась повторно. Прочитаны AGENTS.md, README, implementation-plan,
requirements §§31–33, architecture §§24–31/39/43–44 и относящиеся границы
core/application/persistence, algorithms (численные правила/статусы), manifest v1,
Point Cache v1 и отчёт Persistence Foundation, исходники и существующие tests.

До изменения runner выполнены ограниченные серии указанного TLS-теста:

| Условия | Результат |
|---|---|
| Исходный Debug, `--repeat until-fail:12` | 2 PASS, третий запуск `0xc0000409`, 10.24 s |
| Исходный Release, тот же repeat | 12/12 PASS |
| Debug с временной диагностикой фактического входа helper | 12/12 PASS; не доказательство устранения |
| После явного барьера входа helper, Debug | 12/12 PASS |
| После барьера, Release | 12/12 PASS |

В тесте обнаружена гонка его setup: возврат конструктора std::thread не гарантирует
прохождения Windows DLL_THREAD_ATTACH. Worker мог войти в TLS destructor под loader
lock до входа helper, который должен освободить TLS-барьер. Добавлен Event фактического
входа helper **до** Accept. Проверки ожидания native TLS, destructor, cancellation
и join сохранены; timeout/ожидания не ослаблялись. Production native completion
не изменён. Временная диагностика удалена. Это устранение конкретной гонки test
setup; точная причинная связь с каждым историческим падением 05.1 не доказана.
Конечные серии ограничены и не являются доказательством отсутствия любой нестабильности.

## B. Implementation

Application API `ProjectController`:

```cpp
CommandResult saveProject(SaveRequest request = {});
CommandResult saveProjectAs(SaveRequest request);
bool isDirty() const noexcept;
const persistence::SavedProject *savedProject() const noexcept;
const persistence::SavedProject *unconfirmedSave() const noexcept;
```

SaveRequest содержит абсолютный manifest path и overridable ManifestLimits
(application v1 profile 4 MiB / 4096 tokens / depth 32). У первого Save path обязателен;
обычный повторный использует известный path. Admission без проекта, Busy, shutdown,
reentry и неопределённый предыдущий commit дают structured CommandResult. Принятая
команда запускает worker; файловые ошибки возвращаются в OperationFailure.saveError
(code, phase, native system code, bounded diagnostic). GUI Save-команд нет.

`cadcontour_persistence/save_transaction` отвечает за файловую транзакцию и immutable
prepared metadata. Controller — за snapshot, applicability, binding/baseline и Undo.
ProjectState получил noexcept замену shared DatasetLocation без изменения logical
identity, inputRevision, cell или адреса ProjectState, на который ссылаются Undo commands.
Форматы manifest/Point Cache v1 и существующие зависимости сохранены.

Dirty сравнивает persistent content: ProjectId, cell, source path/bytes/lines,
CacheId, count, bounds (binary64 bits), Z. Storage paths/IDs, snapshotId, runtime
revision, progress и history исключены. Импорт без baseline dirty. Save(cell=1)
с последующим cell=.5 оставляет .5 и dirty; Undo к 1 снимает dirty, Redo возвращает.
Save не очищает history/redo и не использует QUndoStack::setClean.

## C. Publication Protocol

1. Owner захватывает metadata snapshot и shared binding, не выполняя файловый I/O.
2. Worker canonicalizes target, удерживает directory handles, берёт directory-wide
   `.pc2d-save.lock`, проверяет conflicts и source envelope.
3. Для нового storage bounded copy → flush/readback → no-overwrite dataset rename.
4. До Ready подготовлены новый binding, baseline, digest и полностью записанный
   manifest staging: QTemporaryFile для нового target, QSaveFile с отключённым
   direct-write fallback для замены известного manifest.
5. Controller lease сверяет OperationId, context epoch, runtime session, Open/cancel.
   Для Save cell/inputRevision не инвалидируют snapshot. Accept — только permission.
6. Worker вне context/controller mutex повторяет path, dataset и expected snapshotId
   checks под прежним lock; выполняет manifest commit и записывает fixed-size receipt.
7. После освобождения worker resources, native completion и Qt join owner обрабатывает
   receipt: только Persisted меняет binding/baseline, затем terminal notification.

До permission Cancel/Close/Shutdown запрещают commit. После permission Cancel —
NoChange, Close/замена — Busy, shutdown ждёт файловый исход и native join. Destructor
drain не зависит от event loop или доставки receipt. Старые callbacks защищены
OperationId/context/session, reentrant команды отклоняются. Все необходимые application
allocations выполнены до permission; receipt меняет только shared references.

Import Ready/Accept/Discard и его worker-owned guards сохранены: Accept по-прежнему
согласован с noexcept обменом session/history, PendingCache release выполняется в worker.
Save не добавляет второй worker или универсальную очередь задач.

## D. Dataset Ownership

Первый Save создаёт `data/<new storageId>/datasets/<CacheId>.pc2dcache` рядом с
manifest. CacheId и bytes сохраняются. Save As в новый каталог делает необходимую
копию, сохраняет ProjectId, создаёт storageId/snapshotId и после Persisted переключает
active binding. Source directory не изменяется; старые shared holders сохраняют пути.

Повторный Save не копирует cache и не читает ASC/XYZ. Проверяются file ID/volume,
size, last-write/change times, header/footer checksums, completion/layout/metadata.
scanAll, strict validate, rebuild index и сортировка IDs не вызываются.
Save As на эквивалентный path выполняет обычный Save; другое имя в том же directory
явно отклоняется Conflict, чтобы не создавать второй manifest одного storage.

Copy использует 64 KiB buffer, source read handle с запретом write/delete, размер
из проверенного v1 envelope, SHA-256 исходного потока и readback. Readback одновременно
проверяет page checksums; это не повторная алгоритмическая validation cache.
Ресурсы/размеры не зависят линейно от числа точек; metadata admission остаётся отдельным.

Temporary dataset/manifest принадлежат worker. Новый manifest также публикуется
no-overwrite rename в том же directory; QSaveFile нужен только для замены известного
manifest. После no-overwrite rename dataset
сразу удерживается без autoRemove. Поэтому отмена/ошибка последующего manifest может
оставить orphan, но потеря receipt/крах после commit не удалят достижимые данные.
DatasetLocation destructor ничего не удаляет. GC/scavenger не добавлены.

## E. Error Handling

| Проверенный отказ/событие | Диск | Application |
|---|---|---|
| Target/staging/source/read/write/verify до rename | Новый manifest отсутствует; собственный staging убран | Binding/baseline прежние |
| Collision dataset | Чужой файл не перезаписан; manifest отсутствует | Failure, прежнее состояние |
| После dataset rename / manifest prepare/write | Допустим orphan; прежний manifest цел | Binding/baseline прежние |
| Conflict непосредственно перед commit | Внешний manifest сохранён | Conflict, baseline не продвинут |
| Неизвестный target появился после финальной проверки | Не перезаписан (no-overwrite rename) | Conflict, baseline отсутствует/прежний |
| Commit возвращает false до публикации | Прежний manifest сохранён | Uncertain, baseline прежний |
| Реальный Windows sharing violation в QSaveFile commit | Прежний manifest побайтно сохранён | Uncertain, baseline прежний |
| Commit реально выполнился, но возвращает false | Новый manifest и dataset сохранены | Uncertain, прежний baseline, prepared evidence доступно |
| Исключение receipt/cleanup после успешного commit | Новый manifest и dataset сохранены | Succeeded + cleanupWarning |
| Crash во время copy | Только незавершённый собственный staging | Новый manifest отсутствует |
| Crash после dataset / перед manifest commit | Полный orphan dataset | Новый manifest отсутствует |
| Crash после commit до receipt | Manifest и полный dataset сохранены | Следующий Open использует диск |

Матрица охватывает 14 phase hooks, реальные короткие dataset/manifest writes,
инъекции ERROR_DISK_FULL/ERROR_READ_FAULT, missing/truncated/modified/corrupt cache,
foreign target/data, junction, collision, замещение directory, lock и snapshot conflict.
Проверяются реальные files/bytes/manifest/dataset validation, а не только returned status.

Replacement commit false/exception после начала commit консервативно Uncertain, даже если
вызывающий test hook знает фактический исход. Не сообщается success по предположению.
Unconfirmed metadata содержит path, snapshot/storage IDs, digest и binding; следующий
Save отклоняется UnsafeState до внешней проверки/будущего Open. Автоматической
повторной публикации или восстановления старого manifest поверх нового нет.

QLockFile — кооперативная защита, не изоляция от произвольных внешних writers.
Root/descendant directory leases и file leases с reparse rechecks уменьшают окно
подмены, но hostile mappings, восстановление timestamps, ancestor/hard-link aliases
и аппаратные повреждения полностью не исключаются. Metadata-only Save не обнаружит
произвольный bit flip в середине без изменения наблюдаемой file identity/times.
Failed optional cleanup не превращает Persisted в failure; crash leftovers допустимы.
Power-loss durability на произвольной FS не заявляется.

## F. Tests and Regression

Использована штатная MSVC/Qt среда `build/task02-env.cmd` из README, существующие
windows-debug/windows-release presets. Новые save_tests/save_crash_probe и opt-in
save_acceptance не меняют GUI. В production входят 44 новых Save tests, включая
14 параметризованных fault cases и шесть process crash checkpoints в двух тестах.
Отдельный process concurrency case блокирует worker перед commit; второй процесс
отклоняется по тому же directory lock. Два controllers дополнительно проверяют stale
snapshot после освобождения lock. Save As между C: и D: проверен, не SKIP.

| Финальная проверка | Debug | Release |
|---|---:|---:|
| Build all + all_qmllint | PASS | PASS |
| Production: passed / failed / skipped | 524 / 0 / 1 | 524 / 0 / 1 |
| Production elapsed, s | 110.82 | 51.98 |
| Save cases внутри production | 44 / 44 | 44 / 44 |
| Persistence cases: passed / failed / skipped | 25 / 0 / 1 | 25 / 0 / 1 |
| Отдельный real Windows commit failure | 1 / 1 | 1 / 1 |
| TLS test, ограниченный repeat после исправления setup | 12 / 12 | 12 / 12 |
| git diff / staged diff --check | PASS | PASS |

Единственный SKIP — прежний FileSymlinkEscape: Windows error 1314, права создания
file symlink отсутствуют. Реальные junction и cross-volume Save As прошли.
Production включает operation/import/project/cache/GUI/QML regression targets;
тесты не отключались и не ослаблялись. Новый код собран с /W4 без новых warnings;
сохранились ранее известные configure diagnostics Qt6TaskTree/Vulkan и C4702 в fmt/base.h.

Выполненные команды из корня (через существующий локальный MSVC/Qt wrapper):

```bat
build\task02-env.cmd cmake --build --preset windows-debug --target all all_qmllint --parallel 4
build\task02-env.cmd cmake --build --preset windows-release --target all all_qmllint --parallel 4
build\task02-env.cmd ctest --preset windows-debug -L production --output-on-failure --output-log D:/ZherlitsynEE/CADContour2D/build/task052-debug-production-final.log
build\task02-env.cmd ctest --preset windows-release -L production --output-on-failure --output-log D:/ZherlitsynEE/CADContour2D/build/task052-release-production-final.log
build\task02-env.cmd ctest --preset windows-debug -R ^SaveApplication.Save.WindowsSharingViolationDuringRealCommitPreservesPreviousManifest$ --output-on-failure
build\task02-env.cmd ctest --preset windows-release -R ^SaveApplication.Save.WindowsSharingViolationDuringRealCommitPreservesPreviousManifest$ --output-on-failure
build\task02-env.cmd ctest --preset windows-debug -R ^Operations.DestructorWaitsForNativeTlsEvenWhenQtWaitZeroReportsFinished$ --repeat until-fail:12 --output-on-failure
build\task02-env.cmd ctest --preset windows-release -R ^Operations.DestructorWaitsForNativeTlsEvenWhenQtWaitZeroReportsFinished$ --repeat until-fail:12 --output-on-failure
git diff --check
git diff --cached --check
```

Final build logs: `build/task052-{debug,release}-build-final.log`; production logs
указаны в командах. Отдельные Save-наборы 41/41, финальные 44/44 и real commit checks
также запускались; final Save logs: `build/task052-{debug,release}-save-final.log`.

Промежуточные результаты: первоначальный Save-набор 34/34 Debug, затем 41/41
Debug и Release; промежуточный production Debug — 521 PASS / 0 FAIL / 1 SKIP.
Один compile завершился C2397 в новом fault fixture (явное сужение Win32 к uint32),
исправлен явным cast. Две промежуточные перелинковки не прошли из-за запускаемых
одновременно test/probe executables (LNK1168/LNK1104); после завершения процессов
build/all_qmllint повторены успешно. Эти попытки не считаются успешными проверками.

При итоговом self-review найдено окно перезаписи нового неизвестного manifest между
checkTarget и QSaveFile commit. Сначала добавлен regression test на прежний код:
**ожидаемый FAIL** (foreign document заменялся manifest, baseline ошибочно продвигался),
лог `build/task052-new-target-before-fix.log`. Первопричина устранена: новый manifest
использует exclusive staging и no-overwrite rename, QSaveFile оставлен для repeat.
Regression входит в финальные 44 cases. Это не исправление принятой 05.1 — дефект
найден и исправлен в разрабатываемой 05.2 до commit.

Ручной GUI smoke и Open не выполнялись; Save GUI не создавался. Большая нагрузка
ниже не входит в CTest. Полная validation миллионных caches в benchmark не запускалась:
сам Save проверяет copy digest/pages; маленькие опубликованные artifacts и crash
outputs проверяются существующим CacheReader, включая strict validate в tests.

## G. Resource and Performance

Release `save_acceptance`, отдельные процессы на 1 000 000 и 42 000 000 synthetic
points; источник создан штатным CacheWriter до измерения с 16 MiB working budget.
Вызван реальный application Save API до native completion; repeat меняет только cell.
Ни один workload не входит в production CTest. Прогоны выполнялись на D:, параллельно
с частью сборок/регрессии; это наблюдаемые значения, не изолированный throughput benchmark.

| Показатель | 1 млн: первый | 1 млн: repeat | 42 млн: первый | 42 млн: repeat |
|---|---:|---:|---:|---:|
| Dataset bytes | 24 125 440 | те же | 1 010 900 992 | те же |
| Elapsed, ms | 1201.8051 | 753.4281 | 30532.3045 | 528.0020 |
| Dataset copies | 1 | 0 | 1 | 0 |
| Logical read bytes | 48 267 264 | 22 678 | 2 021 818 368 | 22 690 |
| Logical write bytes | 24 128 288 | 2 440 | 1 010 911 700 | 2 384 |
| Sampled peak working set | 12 009 472 | 12 091 392 | 12 038 144 | 12 197 888 |
| Process lifetime peak WS | 26 484 736 | те же | 26 533 888 | те же |
| Sampled peak new-file EOF bytes | 24 126 565 | 1 127 | 1 010 901 068 | 1 129 |
| Final project EOF bytes | 24 126 489 | 24 126 491 | 1 010 902 043 | 1 010 902 045 |

Memory не выросла пропорционально dataset (размер ×41.9 при примерно тех же 12 MiB
sampled WS). Lifetime peak включает создание fixture; все native measurement APIs
вернули успех. First read ≈2× cache — copy + readback; repeat ≈23 KiB независимо от
размера. I/O включает metadata/logging/sampler, не только cache calls. File EOF —
логическая длина, не allocated clusters/physical traffic. Сэмплы каждые 100 ms могут
пропускать короткий manifest staging; zero leftover новых файлов у repeat не означает
отсутствия временного файла во время commit. Старый source cache отдельно остаётся
на диске; итоговый project size не включает его. OS caches не сбрасывались.

Большие resource measurements выполнены до последнего усиления публикации **нового**
manifest (QSaveFile заменён на no-overwrite rename). Copy/readback и repeat Save
не менялись; таблица фиксирует именно измеренный код. Tiny manifest staging I/O/time
финальной реализации не следует считать отдельно измеренными этой таблицей.
После усиления выполнен дополнительный адресный resource smoke (см. ниже).

Финальный resource smoke на 1 млн points:
`build/task052-final-resource-1m.json`, 577.4364 ms первый Save / 76.3167 ms repeat;
copies 1/0; logical read 48 267 264 / 22 678 bytes; logical write 24 128 153 / 2 322 bytes;
sampled peak WS 12 034 048 / 12 075 008 bytes, lifetime peak 26 492 928 bytes.
Все measurement APIs успешны. Repeat короче sample interval, единственный sample
не увидел temporary EOF; это не доказательство отсутствия staging. Полный большой
прогон после metadata-only no-overwrite усиления не повторялся, heavy copy не менялся.

Локальные воспроизводимые данные: `build/task052-resource-1m.json`,
`build/task052-resource-42m.json` и соответствующие `.first.csv`/`.repeat.csv`.
В репозитории сохранён код probe, большие artifacts игнорируются Git и не коммитятся.
Workload roots были новыми изолированными каталогами. Созданные datasets и JSON/CSV
оставлены для проверки отчёта. Попытка удаления двух явно проверенных workload roots
через Remove-Item -LiteralPath -Recurse отклонена автоматической политикой выполнения
(blocked by policy); повторных обходов ограничения не выполнялось. Эти изолированные
результаты вместе с финальным smoke занимают около 2.12 GB, не входят в commit и не являются пользовательскими source datasets.
Точные resource commands и CLI — [save_acceptance README](../../../tools/save_acceptance/README.md).
UNC, HDD-specific/cold-cache и power-loss tests не выполнялись.

## H. Documentation and Git

Обновлены architecture §28.1 и Save-исключение к cancellation, manifest guarantees,
implementation-plan, README и этот отчёт. Requirements, algorithms и оба v1 формата
не менялись. Production changes: CMake, ProjectState binding, controller,
operation types/runner, новый save_transaction. Tests: save_tests, test access,
crash probe, CMake и setup barrier существующего TLS-теста. Tool: save_acceptance.

Полный code/documentation diff и новые файлы просмотрены; git diff --check и
git diff --cached --check — PASS. Index содержит только файлы 05.2 и обоснованное
исправление setup TLS-теста. Несвязанных пользовательских изменений на baseline не было.
Результат оформляется новым локальным commit `implement explicit project save and save as`;
этот отчёт входит в него. Точный hash и проверенный после commit Git status сообщаются
в итоговом ответе (self-referential hash в сам commit не записывается). Push не выполняется.

## I. Final Assessment

**READY FOR INDEPENDENT REVIEW.** Реализация и обязательные доступные проверки завершены.
05.2 не объявляется принятой; вся задача 05 остаётся незавершённой.
Open/partial availability (05.3), Autosave/Recovery (05.4) и GUI (05.5) не реализованы.
Известные границы: cooperative locking, conservative orphan retention, metadata-only
integrity checks, отказ от автоматического retry при Uncertain, отсутствие абсолютной
power-loss durability. Дальнейшая отдельная стадия — независимое ревью 05.2.

## J. Review fixes R1/R2

Дата: 2026-10-19. Исходный HEAD `4187e24`, рабочее дерево и index чистые.
По предоставленному результату независимого ревью: **REVIEW PASSED — READY FOR ACCEPTANCE**,
замечания R1 (MINOR) и R2 (OPTIONAL). Оба замечания проверены и подтверждены.

**R1:** `OperationRunner::poll()` подтверждает native completion и Qt join,
освобождает runner, затем посылает `saveFinished()` и `released()` именно в этом
порядке. Receipt обновляет binding/baseline и синхронно вызывает `stateChanged()`.
До `released()` операция controller ещё active. Прежняя проверка только
`!runner_.busy()` в `requestShutdown()` позволяла observer увидеть Ready при active.
Первопричина — смешение физического освобождения worker и terminal completion controller.

Добавлен `Save.ShutdownFromPersistedStateNotificationWaitsForTerminalCompletion`.
На исходном production-коде он дал **0 passed / 1 failed / 0 skipped**, CTest exit 1:
Ready наблюдался при active, последовательность shutdown notifications была `[Ready]`
вместо `[Closing, Ready]`. Лог: `build/r052-before-debug.log`.
После исправления отдельный Debug-прогон дал **1/1 PASS**
(`build/r052-after-debug.log`); тест также прошёл в связанных и полных наборах обеих конфигураций.

Исправление ограничено условием `!operation_.active && !runner_.busy()` перед Ready
в `requestShutdown()`. В receipt notification shutdown остаётся Closing; существующий
`finishOperation()` завершает операцию и устанавливает Ready до terminal notification.
Runner, native completion, Save permission/receipt, Import и command guard не менялись.
Тест проверяет native completion до receipt, сохранённые manifest/binding/baseline,
Closing → Ready, reentrant poll и повторный shutdown, по одному publication/release/
terminal transition, а также игнорирование позднего receipt.

**R2:** architecture §45 сохранял устаревший перечень следующей работы вопреки §28.1.
Указатель теперь отмечает реализованные в 05.2 Save/Save As и atomic publication;
Open/partial availability и Autosave/Recovery остаются следующими этапами.
Архитектурные контракты и форматы manifest/Point Cache v1 не менялись.

Использованы существующие build directories и штатная CMD-среда vcvars64/Qt 6.12.0,
без восстановления старых сборок или установки инструментов.

| Проверка | Debug | Release |
|---|---|---|
| Сборка `all` и `all_qmllint` (`--parallel 4`) | PASS | PASS |
| Связанные Save/Operations/Import, passed / failed / skipped | 252 / 0 / 1 из 253 | 252 / 0 / 1 из 253 |
| Полный production CTest, passed / failed / skipped | 524 / 0 / 2 из 526 | 524 / 0 / 2 из 526 |
| Отдельный cross-volume Save As с TEMP/TMP на F:, target на C: | 1 / 0 / 0 | 1 / 0 / 0 |

Связанный набор запущен через `-R "SaveApplication|Operations|StaleInputs|WorkerFailures|PublicationFailures|TerminalReentry|ShutdownCheckpoints|Asc"`.
Он включает все 45 Save, 51 Operations, ASC/XYZ, application Import, Z-confirmation
и GUI Import tests. Полный production дополнительно проверяет persistence,
cache, project, view model и production QML, включая native TLS/shutdown/reentrant cases.
Логи: `build/r052-{debug,release}-{build,focused-console,production,cross-volume}.log`.

Два production SKIP подтверждены по output: `SaveAsAcrossAvailableWindowsVolumes`
при стандартных TEMP/build на одном томе (отдельно проверен с TEMP/TMP
`F:\CADContour2D-benchmark`), `Persistence.StoragePath.FileSymlinkEscape` —
Windows privilege 1314. Реальный junction escape прошёл в обеих конфигурациях.
File-symlink при доступной privilege не проверен. Большие нагрузочные измерения,
ручной native GUI smoke и power-loss tests не повторялись; эти исправления
не меняют copy/readback, UI или файловый протокол.

Просмотрен итоговый diff; `git diff --check` и `git diff --cached --check` — PASS.
Изменены controller, Save regression test, architecture §45, README, статус в плане
и этот отчёт. Логи остаются в игнорируемом build, в commit не входят. Локальный commit
и status после него сообщаются отдельно; push не выполняется.

**READY FOR FINAL VERIFICATION.** R1/R2 исправлены, доступные проверки успешны.
05.2 и задача 05 целиком не объявляются принятыми; 05.3–05.5 не реализовывались.

## K. Independent verification and acceptance

**05.2 — ACCEPTED, 2026-10-20.** Финально проверен commit
`cedf844d9ff85f389b68287caa763e143983f894` поверх реализации `4187e24`:
полный diff шести файлов, исходный Git status чистый. Новых существенных замечаний нет.

**R1 — CLOSED.** Условие `!operation_.active && !runner_.busy()` устраняет смешение
физического завершения worker и terminal transition controller. При shutdown из
persisted `stateChanged` сохраняется Closing; `finishOperation()` завершает операцию
и устанавливает Ready до terminal notification. Regression test независимо проверен:
реальные manifest/binding/baseline, native completion, Closing → Ready, reentrant poll,
повторный shutdown, однократные publication/release/terminal и поздний receipt.
Журнал прежнего FAIL `build/r052-before-debug.log` прочитан и соответствует дефекту;
старый production-код в этой проверке не изменялся и не пересобирался.

**R2 — CLOSED.** Architecture §45 соответствует §28.1: Save/Save As и atomic publication
реализованы; Open/partial availability и Autosave/Recovery остаются будущей работой.
Остальные изменения документации `cedf844` проверены; исторические результаты §J
не подменяют собственные запуски ниже.

| Собственная проверка | Debug | Release |
|---|---|---|
| Incremental build `all` + `all_qmllint`, `--parallel 4` | PASS | PASS |
| R1 regression отдельно, passed / failed / skipped | 1 / 0 / 0 | 1 / 0 / 0 |
| Полный production CTest, passed / failed / skipped | 524 / 0 / 2 из 526 | 524 / 0 / 2 из 526 |
| Cross-volume Save As отдельно, TEMP/TMP на F:, target на C: | 1 / 0 / 0 | 1 / 0 / 0 |

Полный production занял 149.24 s Debug / 81.24 s Release и включает связанные Save,
Operations и Import tests; отдельный связанный набор §J повторно не запускался.
Два SKIP подтверждены: TEMP/build на одном томе в cross-volume case (отдельный запуск
на разных томах прошёл) и file-symlink privilege 1314. SKIP не считается PASS.
Локальные журналы новых запусков: `build/review052-final-20261020/`.

Границы принятия сохраняются: cooperative locking, conservative orphan retention,
metadata-only integrity checks и запрет повторного Save при Uncertain. File-symlink
с доступной privilege, UNC/SMB failures, power loss, большие нагрузки и ручной native
GUI smoke в этой финальной проверке не выполнялись. Файловый протокол, форматы,
production-код и тесты при документальном закрытии не менялись.

Статусы и ссылки согласованы в implementation-plan, README и индексе docs;
architecture §45 уже корректен после `cedf844`. Закрытие оформляется отдельным
документационным commit без изменения истории `cedf844` и без push.
Принятие ограничено 05.2: вся задача 05 не завершена и не принята,
05.3–05.5 — NOT STARTED.
