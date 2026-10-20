# Task 05.3 — Open и частичная доступность

## A. Baseline

Дата: 2026-10-20. Branch `main`, исходный HEAD
`3d717e176e039693120dc76a0ddf7f30f2aa71ea`; рабочее дерево и index чистые.
Локальный baseline совпал со снимком задания. 05.1 (`408757c`) и 05.2 (`4187e24`,
исправления `cedf844`, принятие `3d717e1`) присутствуют и уже ACCEPTED.

Изучены AGENTS.md, README, задача 05 плана и последующие зависимости, requirements
§§6/8/31–33, architecture (state/cache/threading/persistence/errors/testability),
применимые численные и импортные контракты algorithms, manifest/cache v1,
отчёты 05.1–05.2 и финальные заключения ревью. Особое внимание: 05.2 R1 — shutdown
Ready только после terminal completion, и R2 — актуальность architecture §45.
Прочитаны domain/controller/runner/types, codec/resolver, SaveTransaction,
CacheReader и связанные tests. Существенных расхождений с заданием нет.

## B. Implementation

```cpp
CommandResult ProjectController::openProject(OpenRequest request);
CommandResult ProjectController::recheckPointCache(cache::ResourceLimits limits = {1ULL << 20, 0, 2});
const persistence::CacheAvailability *ProjectController::pointCacheAvailability() const noexcept;
```

OpenRequest: абсолютный manifestPath, ManifestLimits (4 MiB / 4096 tokens / depth 32),
readerLimits (1 MiB / два handles), replacementConfirmed и optional expectedContext.
Это caller-overridable metadata/Reader budgets, не размер облака и не предел RSS.
Application admission проверяет owner thread, reentry, shutdown, Busy, подтверждение
замены и captured ImportContext. Необязательный context доступен через существующий
`importContext()`; headless вызов без него подтверждает состояние на момент вызова.

Новые файлы `src/persistence/project_open.h/.cpp`: bounded read существующим codec,
точный SHA-256 прочитанных bytes, relative path resolution, limited probe и подготовка
immutable LoadedProject. ReadManifest получил additive overload с digest output:
хэшируется тот же byte buffer, который прошёл decode, без второго чтения файла.

В `ProjectState` добавлен validating constructor с saved cell, без пользовательских
команд и роста revision. Qt/I/O/JSON в core не добавлены. Восстанавливаются ProjectId,
source metadata, CacheId/count/bounds/Z и shared project-owned DatasetLocation от
фактического каталога документа. Новый runtime identity и revision = 0; история пуста.

SavedProject остаётся единственным saved baseline: snapshot, exact manifest digest,
path, content и physical binding. DatasetFileIdentity стала optional — отсутствующему
или непроверенному файлу не приписывается нулевая фиктивная identity. Save по-прежнему
проверяет observed identity; существующий cross-volume test адаптирован к optional
с сохранением проверок. Форматы manifest/Point Cache v1 не изменены.

Controller/operation types/runner расширены только Open и RecheckCache transport и
gate. CacheReader предоставляет read statistics, включая initial open buffers/reads.
CMake регистрирует persistence source и отдельный `open_tests` с production label;
Windows ACL test использует системную Advapi32. Новых сторонних зависимостей нет.

## C. Availability Model

| Access | Смысл |
|---|---|
| AvailableLimited | Header/footer/layout/root и descriptor проверены; все point pages/RecordId не проверялись |
| Missing | Ожидаемый файл или родитель отсутствует |
| Corrupt | Фактически обнаружены truncation/checksum/layout/root/metadata mismatch |
| Unsupported | Неподдерживаемый cache format при корректной checksum проверяемого блока |
| FileIO | Ошибка доступа, sharing, чтения либо наблюдения file identity |
| UnsafePath | Resolver отклонил reparse/type/path structure |

Runtime availability не сериализуется и не участвует в dirty. Diagnostic cache/path
errors сохраняются отдельно от manifest errors. Некорректный manifest блокирует Open;
валидный manifest с недоступным dataset создаёт partial session с прежними metadata.
Resource admission failure остаётся failure, а не partial success.

Probe повторяет resolver перед I/O, удерживает read-only Windows handle с запретом
обычных write/delete, наблюдает identity и вызывает только `CacheReader::open`.
Сверяются CacheId, count, binary64 bits bounds и Z. Manifest закрыт до probe;
lease и Reader требуют два handles. Reader проверяет physical size, header/footer,
packed shape и корень индекса. Point pages не читаются, временные файлы не создаются.

Partial session допускает metadata/Close/подтверждённые Import/Open/recheck.
Cell/Undo/Redo, Save/Save As и нынешняя verification операция запрещены в C++.
Это guards конкретных текущих действий, не глобальная блокировка будущих PNG/Rough.

Явный recheck повторяет только ограниченный probe по прежнему descriptor. При успехе
атомарно обновляет availability и optional file identity; published snapshot/content/
digest, runtime session, revision, пользовательские параметры, history и dirty
сохраняются. Wrong CacheId не принимается. Обнаруженная недоступность публикует новую
availability; resource failure/cancel не уничтожает прежнее состояние.
Recheck ещё не сохранённого импорта или Uncertain возвращает UnsafeState: для первого
есть Save, для второго требуется Open фактического документа.

Availability — наблюдение, не гарантия неизменности filesystem. Исчезновение после
проверки обнаруживается при следующем I/O/recheck; cell сам не делает файлового I/O.
Save повторно проверяет путь/identity/envelope и безопасно отказывает без изменения
manifest. После явного recheck зависимые команды блокируются. История сохраняется.

Полная validation не запускается автоматически. Отдельный явный C++ контракт остаётся
существующим `CacheReader::open` → `validate(limits, temporaryParent, cancel, context)`
на worker с собственным бюджетом и cancellation. Новая controller/GUI-команда полной
validation для 05.3 не понадобилась; состояния FullyValidated нет.

## D. Application Lifecycle

Worker подготавливает только immutable metadata. Owner до final applicability check
выделяет новый ProjectSession/state/history/baseline. Ready lease сохраняет candidate
при reentrant cancellation/shutdown. Noexcept commit под существующим gate меняет
единственного владельца session и Accept, увеличивает epoch/runtime identity;
verification products инвалидируются. Никакого файлового I/O под gate mutex нет.

Failure, OOM, cancellation, context/revision change и stale result не публикуются.
Прежние state, binding, dirty, Undo/Redo и baseline сохраняются. Close отзывает
публикацию и не позволяет worker восстановить закрытый проект. Destructor выполняет
drain без queued delivery. Terminal и shutdown Ready требуют native completion/join.
Reentrant команды из синхронных уведомлений отклоняются; late callbacks игнорируются.

Dirty использует прежнее сравнение persistent content: Open clean, cell dirty,
Undo к baseline clean. Обычный Save после Open переиспользует cache без copy;
история не очищается при Save. Save permission не равен receipt: commit после
permission необратим, Open/Import остаются Busy, shutdown ждёт безопасного исхода.
Регрессия 05.2 R1 включена в полный production. При PublicationUncertain Open читает
диск: проверены как состоявшийся, так и несостоявшийся commit, без угадывания snapshot.

## E. Data Safety

Open/recheck не изменяют manifest/dataset. Несуществующий source не требуется.
Новый project root получается из выбранного manifest; перенос каталога сохраняет
relative references. Лексические traversal/absolute/UNC/drive-relative paths
отклоняет codec, filesystem reparse/junction — resolver. Проверены реальный junction,
sharing denial и Windows ACL read denial с восстановлением прав на test fixture.

DatasetLocation ничего не удаляет: Close, замена и уничтожение controller сохраняют
принятые datasets. Ошибка Open не меняет старый сеанс. Выход за storage boundary
не разрешается как рабочий cache. Существующие ограничения против hostile TOCTOU,
hard links, writable mappings и аппаратных отказов не усиливаются до абсолютной
гарантии. Последующий I/O обязан повторять проверки; глобальных watchers/GC нет.

## F. Tests and Regression

Использованы существующие Debug/Release build directories и штатная CMD-среда
`build/task02-env.cmd` (vcvars64, Qt 6.12.0, CMake/Ninja). Системные инструменты и
глобальное окружение не менялись. Счётчики поднаборов ниже извлечены из полного
production; отдельные Save/Import/Operations прогоны дополнительно не запускались.

| Проверка | Debug | Release |
|---|---|---|
| Сборка `all` | PASS | PASS |
| Новые Open tests, passed / failed / skipped | 58 / 0 / 0 | 58 / 0 / 0 |
| Связанные integration: production QML | 14 / 0 / 0 | 14 / 0 / 0 |
| Save/Persistence regression | 70 / 0 / 1 из 71 | 70 / 0 / 1 из 71 |
| Operation/Import/ASC/confirmation/GUI regression | 206 / 0 / 0 | 206 / 0 / 0 |
| Полный production CTest | 583 / 0 / 1 из 584 | 583 / 0 / 1 из 584 |
| `all_qmllint` | PASS | PASS |
| `git diff --check` / `git diff --cached --check` | PASS (общий diff) | PASS (общий diff) |

Полный production: 143.30 s Debug, 79.51 s Release. Из regression counts: Save 45,
Persistence 25 passed + 1 skipped, Operations с параметрическими fixtures 51,
ASC/XYZ 42, application Import 25, Z-confirmation 30, GUI Import/lifecycle 58.
Существующая R1 shutdown receipt регрессия прошла в составе Save.

Единственный SKIP в обеих конфигурациях — `Persistence.StoragePath.FileSymlinkEscape`,
Win32 1314 (нет privilege создания file symlink). Это не PASS. Реальные junction
cases прошли. Save As cross-volume и новый Open cross-volume прошли без SKIP:
TEMP на C:, workspace destination на D:. Новый ACL denial test реально запретил
чтение test cache, проверил partial state, восстановил DACL и успешный recheck.

Open suite включает round-trip с точным cell и metadata/Z, новый runtime, пустую
history, clean/dirty/Undo и последующий Save без copy; перенос root/тома и удалённый
source; malformed/version/fields/types/reference/path errors и bytes/tokens/depth/
Reader admission; missing/truncated/header/footer/root damage, CacheId/count/bounds/Z
mismatch и cache version; sharing/ACL; page corruption и явную full validation;
partial recovery/неверный restored ID/исчезновение доступного файла; сохранение старого
сеанса при error/cancel/OOM/preparation exception; Busy при Import/Save до и после
permission/Open; Close/shutdown/stale context/callback/reentry/destructor; Uncertain
с обоими фактическими исходами commit; bounded reads на многостраничном cache.
OOM воспроизведён fault seam при preparation, а не исчерпанием всей памяти процесса.

Фактические финальные команды из корня репозитория:

```bat
build\task02-env.cmd cmake --build --preset windows-debug --target all all_qmllint --parallel 4
build\task02-env.cmd cmake --build --preset windows-release --target all all_qmllint --parallel 4
build\task02-env.cmd ctest --preset windows-debug -R ^OpenApplication --output-on-failure
build\task02-env.cmd ctest --preset windows-release -R ^OpenApplication --output-on-failure
build\task02-env.cmd ctest --preset windows-debug -L production --output-on-failure
build\task02-env.cmd ctest --preset windows-release -L production --output-on-failure
git diff --check
git diff --cached --check
```

Build output перенаправлен в `build/task053-{debug,release}-build-final.log`;
CTest использовал `--output-log D:/ZherlitsynEE/CADContour2D/build/task053-<config>-<suite>.log`
с suite `open-final`/`production`; также сохранены соответствующие `-console.log`.
Итоговые build logs без новых compiler/lint diagnostics.

Промежуточная сборка выявила отсутствие `noexcept` у cancel callback для существующего
CancelCheck; сигнатура исправлена. Первый Debug Open-прогон: 51 PASS / 1 FAIL из 52 —
test expectation ошибочно требовал исходный count = 2 после намеренного изменения
manifest count на 1. Partial state корректно сохранял descriptor count = 1; исправлено
ожидание, проверка corruption сохранена. Эти попытки не считаются успешными финальными
проверками. После добавления оставшихся случаев выполнены финальные 58/58 в обеих
конфигурациях и полный production выше. Существующие проверки не отключались.

## G. Resource and Performance

Адресные tests на caches с 2 и 10 000 points подтверждают initial open/recheck:
3 Reader reads, 73 728 bytes, одна index page, ноль point pages/returned records.
Point-page corruption не мешает limited Open, но обнаруживается при отдельно
вызванном полном Reader validate. File bytes и список project files сохраняются;
Save после Open проверен без CopyRead/CopyWrite/CopyVerify и без смены dataset identity.

По анализу production пути нет validate/scanAll, copy, sort, index rebuild, PNG или
reduced cloud. Память состоит из bounded manifest DOM/metadata и Reader buffers/frames,
не массива всех points. Обычный operation log не отключён; приведённые counters —
логические Reader reads, не весь process I/O и не physical disk traffic.

Новый многогигабайтный dataset не создавался, отдельный performance benchmark
не запускался. Elapsed Open/Peak Working Set/process read/write/temporary disk high
water marks не измерялись; выводы о производительности на больших real scans,
cold cache, HDD/network I/O и физических traffic/durability не делаются.

## H. Documentation and Git

Обновлены README, docs index, requirements §32, architecture §28.2/§45,
implementation-plan и уточнение manifest documentation. Required пользовательское
решение о лёгком Open явно записано в requirements; formats/алгоритмические методы
не менялись, algorithms.md не требовал правок. 05.4/05.5 не реализовывались.

Перед commit просмотрен итоговый code/documentation diff, выполнены git diff --check
и cached --check. Новые файлы также включены в просмотр.
Все изменения относятся к 05.3; начальных пользовательских изменений не было.
Локальный commit и post-commit status сообщаются в итоговом ответе. Push не выполняется.
Логи находятся в игнорируемом build и не включаются в commit.

## I. Limitations

Нет GUI Open/Save/Discard/Cancel (05.5), Autosave/Recovery (05.4), controller-команды
полной integrity validation, PNG/Rough/Reduced datasets и GC. Limited access не
гарантирует integrity всех страниц/RecordId. Реальные UNC/SMB failures, power loss,
произвольные hostile filesystem races и большой resource benchmark не проверялись.
Cancellation кооперативна: уже выполняющийся OS read не прерывается принудительно;
публикация отзывается немедленно, metadata read ограничен ManifestLimits.
Отдельный file-symlink test при наличии Windows privilege определяется средой;
результат полного production указан в §F.

## J. Final Assessment

**READY FOR INDEPENDENT REVIEW.** Реализация 05.3 и необходимые доступные проверки
завершены. 05.3 ещё не принята; вся задача 05 не завершена. 05.4 Autosave/Recovery
и 05.5 GUI не начаты. Следующий этап — независимое ревью 05.3 в отдельном контексте.
