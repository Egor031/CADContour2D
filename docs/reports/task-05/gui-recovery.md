# Task 05.5B — GUI Recovery, startup и сквозные проверки

## Baseline и статус

2026-10-24, `main`, исходный HEAD `5028c288af789823caa75c95fa8232c2d900b2c6`.
Рабочее дерево и index перед работой чистые. 05.1–05.4 остаются **ACCEPTED**,
05.5A — **ACCEPTED WITH LIMITATIONS**. Её R1/O1/L1 не переоткрывались.
Результаты ниже относятся к 05.5B, а не к повторному принятию C++ persistence.

**05.5B — READY FOR INDEPENDENT REVIEW. 05.5 и задача 05 — IN PROGRESS / NOT ACCEPTED.**
Нужны независимое ревью и оставшаяся ручная приёмка; задача 06 не начата.

## Реализация и файлы

- `src/ui/project_view_model.{h,cpp}`, `project_file_workflow.cpp`: startup discovery,
  metadata rows, tokenized Restore/Reject, продолжение Import после решения,
  сохранение намерения при Autosave и повторная проверка applicability.
- `qml/Main.qml`: меню «Восстановление», прокручиваемый список, отдельные подтверждения
  Reject/замены, «Позже», диагностика и постоянное предупреждение Save As при конфликте.
- `tests/gui_recovery_tests.cpp`, `tests/CMakeLists.txt`: 44 новых production QML cases.
  `recovery_crash_probe.cpp` расширен сценарием explicit Save → изменённый Autosave → crash;
  `operation_probe_main.cpp` — изолированным native режимом `--recovery-storage`.
- `tests/{qml_integration_tests,gui_import_tests,gui_lifecycle_tests}.cpp`: существующие
  startup assertions согласованы с обязательным асинхронным discovery, без ослабления
  проверок отсутствия лишнего worker и безопасного удаления окна.
- `README.md`, `docs/{README,architecture,implementation-plan,ui-design}.md` и этот отчёт:
  актуальный GUI-контракт, проверенные команды и статус. Requirements/algorithms не менялись.

После готовности окна discovery выполняется управляемым worker. Пустой успешный
результат продолжает startup Import; при записях или ошибке сначала открывается Recovery.
Список различает Active/Interrupted/Closed/Rejected/Invalid и Unsaved/MatchesExplicit/
Modified/Conflict/Unknown. Показываются source, cell в мм, число точек, physical cache,
snapshot и explicit path, если доступны, и технические причины ограничений.
Active/Invalid/Rejected недоступны для Restore/Reject. Discovery не открывает cache;
причина partial availability становится известна после Restore.

Restore, подтверждённый Reject и отсрочка — разные действия. Escape/закрытие/«Позже»
не вызывают Reject и сохраняют запись. Повторный доступ — меню «Файл». Восстановление
обновляет карточку, cell/draft, dirty и availability через controller; history пуста.
Conflict виден до и после Restore; обычный Save направляет в Save As. Cancel/failure
Save As и последующий Autosave не снимают требование. Partial recovery оставляет
metadata, причину и прежний явный bounded recheck, сохраняя C++ запреты на Save/cell/Undo.

Использован один существующий файловый workflow. При замене действуют подтверждение
потери истории, draft и dirty с прежними Apply/Discard/Cancel и Save/Discard/Cancel.
Adapter удерживает immutable metadata candidate, не domain state. Ответы проверяют
одноразовый token, project/input stamp и file context; отдельная identity workflow
защищает возврат из reentrant dispatch. Admission после Autosave ждёт native join
и command transition, без циклических таймеров. Успех определяется своим OperationId.
Close/Exit отзывают recovery-намерение через прежний безопасный lifecycle.

Controller/OperationRunner, publication gate, native completion/join, storage protocols
и persistent formats не изменены. Новых зависимостей нет. Startup/Open/Recovery не
вызывают copy, scanAll, full validation, PNG/reduced rebuild или geometry processing.

## Автоматическая проверка

Окружение: Windows x64, Visual Studio 18 Community, MSVC 14.51.36231,
Qt 6.12.0 `msvc2022_64`, CMake/Ninja из Qt Tools. CMD инициализирован `vcvars64.bat`,
`chcp 65001`, `VCPKG_ROOT`, `QT_ROOT`, process-local PATH и `VCPKG_MAX_CONCURRENCY=4`
по инструкции пользователя. Build dirs — `C:/dev/CADContour2D/build/windows-{debug,release}`.
GUI CTest использует offscreen/software/Basic, не выдаётся за native GUI.

Команды для каждой конфигурации `<cfg>` = `debug`, `release`:

```bat
cmake --build --preset windows-<cfg> --target all all_qmllint --parallel 4
ctest --preset windows-<cfg> -R RecoveryGui --output-on-failure
ctest --preset windows-<cfg> -R Recovery --output-on-failure
ctest --preset windows-<cfg> -R Crash --output-on-failure
ctest --preset windows-<cfg> -L production --output-on-failure
```

| Проверка | Debug | Release |
|---|---|---|
| `all` + `all_qmllint` | PASS | PASS |
| Новый RecoveryGui | 44 PASS / 0 FAIL | 44 PASS / 0 FAIL |
| Существующие FileGui / AscGui / QML integration | 66 / 80 / 14 PASS | 66 / 80 / 14 PASS |
| Recovery C++ + GUI (`-R Recovery`) | 108 PASS / 0 FAIL | 108 PASS / 0 FAIL |
| Process crash (`-R Crash`) | 19 PASS / 0 FAIL | 19 PASS / 0 FAIL |
| Финальный production, 806 cases | 803 PASS / 0 FAIL / 3 SKIP | 803 PASS / 0 FAIL / 3 SKIP |
| Cross-volume отдельно с TEMP/TMP на F: | 2 PASS / 0 FAIL | 2 PASS / 0 FAIL |

Поднаборы входят в production и не суммируются с ним. Полные прогоны заняли
461.98 с / 200.11 с (Debug / Release); это время тестов, не benchmark.
Три production SKIP: Open cross-volume
и Save As cross-volume при TEMP/build на C:, а также file symlink без privilege.
Межтомные проверки прошли отдельно с TEMP/TMP на F: и build на C: в обеих
конфигурациях. File-symlink SKIP подтверждён verbose-прогоном в обеих: Windows **1314**.

Отдельные фактические команды для каждой конфигурации в отдельном CMD-процессе:

```bat
set "TEMP=F:\CADContour2D-benchmark\055b-cross-volume\<cfg>"
set "TMP=%TEMP%"
ctest --preset windows-<cfg> -R Volume --output-on-failure
```

Каталоги TEMP созданы перед запуском; переменные менялись только для тестового
процесса. Symlink отдельно проверен без переопределения TEMP:

```bat
ctest --preset windows-<cfg> -R FileSymlinkEscape -V
```

Успех межтомных cases не снимает отдельный symlink SKIP.

Первоначальная Release auto-reconfigure сообщала об отсутствующих optional
Qt TaskTree/QmlAssetDownloaderPrivate и Vulkan headers; configure/build завершились
успешно. Финальные incremental build/lint не выдали compiler/QML warnings.
Прежний сторонний C4702 в `fmt/base.h` не объявляется исправленным.

Новые cases проверяют пустой startup, несколько записей, discovery failure/retry,
отсрочку и повторный доступ, явный Reject и его failure, inactive/invalid/unsupported
records, baseline/dirty, conflict/Save As failure/cancel/Autosave, partial missing/
corrupt/unsupported cache и recheck, draft/dirty replacement с Save/Discard/Cancel,
stale tokens/digest/context, nested notifications, deferred admission и девять
Close/Window/Quit случаев во время Discover/Restore/Reject. Шесть GUI crash cases
реально убивают дочерний процесс и затем восстанавливают либо отклоняют Interrupted
record. Новый GUI запуск после Restore/Reject/отсрочки проверен отдельно.
C++ guards вызываются напрямую, проверки не ограничены `enabled` в QML.
Restore/recheck на обычном непустом cache: **73 728 bytes, 0 point pages**.

### Обнаруженные проблемы тестовых сценариев

Первый новый direct-executable прогон: 22 PASS / 9 FAIL из 31. Исправлены поиск
динамических QML controls по visual tree, явно ненулевые ManifestLimits и барьер
фактического поступления Quit до разрешения worker. Это ошибки harness; после
исправлений 31/31, затем расширенный набор 41/41. Итоговый набор расширен до 44.

Первый полный production выявил старые предположения о синхронном startup и
абсолютных OperationId 0/1: Startup QML test, Invalid URL ×8, UNC boundary и deletion
перед deferred close: в каждой конфигурации **792 PASS / 11 FAIL / 3 SKIP** из 806.
Теперь tests ждут discovery/native join, явно проверяют его
тип/успех и сравнивают ID до/после проверяемой команды. UNC case детерминированно
останавливает независимый Autosave. Содержательные assertions и guards сохранены;
production ради этих старых оракулов не менялся. Первые FAIL не скрыты повтором до успеха.

Известное наблюдение 05.4 `Applied`/`NoChange` не исправлялось в production.
В новом crash `explicit` ненужная повторная публикация совпадающего состояния
не требуется; проверяется реально изменённый snapshot. Новые helpers при NoChange
проверяют наличие recovery и совпадение его persistent content с текущим state.
Это не доказательство устранения прежней нестабильности старого Recovery-теста.
В итоговых целевых и полных прогонах этот прежний тест не упал. Согласно существующему
`autosave()` NoChange допустим, когда совпадают content, conflict flag и explicit
baseline/path/digest уже опубликованного recovery, в том числе после автоматического
сохранения нового Save As binding. Новое поведение этому наблюдению не приписывается.

## Сквозные сценарии задачи 05

| Сценарий | Проверка |
|---|---|
| Import → cell → Save → Close → Open без source | PASS — новый production QML round-trip |
| Save As → новый owned storage → Open | PASS — тот же round-trip, физический binding и новый path |
| Ошибка/отмена Save/Open сохраняют текущий проект | PASS — существующий FileGui и новые FailedRecoveryReplacement |
| Поздняя правка после Save snapshot | PASS — существующий `SaveOldSnapshotPreservesLaterEditsAndDraftCursorSelection` |
| Первый Autosave → crash → GUI Recovery | PASS — новые `active`/`published` process cases |
| Explicit Save → правка → crash → Restore/Reject | PASS — новые `explicit` process cases |
| Conflict → обязательный Save As → обычный Save | PASS — новый conflict GUI case, original explicit bytes неизменны |
| Partial Open/Recovery и bounded recheck | PASS — существующий FileGui + три новых PartialRecovery |
| Draft/dirty, Busy, pending Import, закрытие/замена | PASS — существующие R1/N1/Import и новые Admission/Lifecycle cases |

Повторный Save после round-trip дополнительно запрещает CopyRead/CopyWrite/CopyVerify
через существующий fault seam. Чтение source не требуется; cache binding сохраняется.

## Native Windows GUI и HDD

Использован навык `computer-use`, обычный Windows backend/нативные FileDialog.
Production `CADContour2D.Main` загружался существующим `operation_probe` с отдельным
storage: пользовательский AppLocalData и его recovery не менялись. Основные native
Restore/Save/HDD сценарии проверены в probe; standalone EXE проверен отдельно ниже.

Проверенные запуски после стандартной CMD-инициализации:

```bat
build\windows-debug\operation_probe.exe --recovery-storage C:/dev/CADContour2D/build/055b-native/imports
build\windows-release\operation_probe.exe --recovery-storage F:/CADContour2D-benchmark/055b-recovery-resource-correct/imports
```

Debug: process-crash record найден перед Import; Escape → native Import → Escape;
меню «Файл → Восстановление» снова показывает запись; Restore даёт `cell=0.125`,
dirty «Без имени *», доступный cache, revision=0 и пустые Undo/Redo. Ctrl+S открыл
native SaveFile, сохранение в `build/055b-native/project.pc2dproj` завершилось
«Проект сохранён», имя/path обновились, dirty исчез. Manifest фактически прочитан
с `cell=0.125`; системное закрытие завершило test process.

На окончательном Release повторён запуск того же storage: Closed/MatchesExplicit
виден до Import; Restore возвращает чистый проект с прежним path, `cell=0.125`,
доступным cache и пустой историей. Системное закрытие завершило процесс без dirty
диалога. Progress не пересоздаёт metadata rows списка во время ожидания операции.

Также запущен настоящий итоговый `build/windows-release/CADContour2D.exe` со штатным
AppLocalData. Startup показал 16 ранее существовавших records, прокрутку, Rejected
с недоступными действиями и Closed/Conflict с предупреждением Save As до Import.
Восстановление/Reject существовавших записей не выполнялись; системное закрытие завершило
EXE. SHA-256 всех 16 records до/после совпали. Это проверка standalone startup и
сохранности записей; полный Restore/Save через standalone EXE отдельно не заявляется.

Release/HDD: использован существующий cache **101 261 312 bytes / 4 202 892 points**
из `F:/CADContour2D-benchmark/ascxyz-044b-20261018-f1f2b934/complete-storage`.
Новых больших datasets не создавали. Тестовый recovery содержит metadata из
4096-byte header; source counters синтетические и source отсутствует. Physical
reference указывает на существующий cache, исходный файл не менялся.
GUI Restore показал 4 202 892 points, правильные bounds, `cell=0.125` и доступный
cache без новой копии dataset. В новом resource root остался только recovery metadata.

Observed Release process working set до/после: 102 666 240 / 103 219 200 bytes,
private bytes: 104 869 888 / 105 000 960; lifetime peak после — 116 551 680 bytes.
Это разовые samples всего GUI, не benchmark и не измерение physical disk I/O.
Точное ограниченное чтение подтверждено отдельными application/GUI assertions выше.
Gigabyte/UNC/power-loss workloads не выполнялись.

Первый resource fixture ошибочно приравнял имя cache-файла к CacheId. Реальный ID
в header отличается; production корректно показал partial state с диагностикой
`Dataset metadata differs from manifest` и заблокировал зависимые действия.
После исправления test metadata по header повторный Release native Restore успешен.
Оба test process закрыты через dirty decision, физический cache не удалялся.

## Оставшаяся приёмка

- Независимое ревью 05.5B обязательно до ACCEPTED; результаты 05.4/05.5A выше не
  заменяют это ревью. Дополнительно повторить Restore/Save в standalone production EXE
  с собственным тестовым recovery, native Reject/Conflict/partial recheck и keyboard/focus
  при малом окне.
- Системный Windows DPI 150% и 200%: изменить масштаб в Windows, перезапустить
  приложение, проверить читаемость списка/диалогов, прокрутку, доступность действий
  и native Save/Open. В этой работе системный DPI не менялся; Qt scaling не объявляется
  его проверкой. Перенос между мониторами — N/A, второго монитора нет.
- Длительный native Save: использовать большой cache в отдельном test storage,
  после старта Save изменить и применить cell; дождаться успеха. Ожидаются сохранённый
  старый snapshot, новый cell в GUI и dirty; Close должен запросить новое решение.
  Автоматический сценарий есть; отдельная native приёмка остаётся незавершённой.

## Evidence и Git

Финальные логи — ignored `build/055b-last-{build,recovery,crash,production}-<cfg>.log`,
`055b-cross-<cfg>.log`, `055b-symlink-<cfg>.log`. Исходные полные 11 FAIL сохранены
в `build/055b-production-<cfg>.log`, новые первые 9 FAIL — в `055b-recovery-first-debug.log`.
Resource metadata/memory — `build/055b-resource*.json`,
native synthetic artifacts — `build/055b-native` и отдельные каталоги F: выше.
Временный CMD environment wrapper удалён перед commit; в репозиторий не входит.
Список изменённых файлов приведён выше. Diff/staged checks — PASS;
hash и итоговый Git status приводятся в ответе. Push/PR/deployment не выполняются.

## Последующее адресное исправление layout — 2026-10-25

Новое Low finding из native-приёмки 2026-10-24 — обрезание кнопки «Применить»
при минимальном окне — исправлено отдельно. Причина, геометрическая регрессия,
Debug/Release CTest и standalone проверки системных 100%/150% приведены в
[отчёте исправления](cell-apply-layout.md). Результаты выше относятся к прежней
версии и сохранены; статусы принятия 05.5A/05.5B/05 этим исправлением не меняются.

## IR-05-T1 — детерминированный Recovery lifecycle, 2026-10-26

### Baseline и независимое ревью

`main`, HEAD `c3f1ed749b19fbe10f298a0ed34c91c7dee0ad30`; tracked worktree и index
до исправления чистые. Прочитаны AGENTS.md, README, относящиеся lifecycle,
publication и Recovery-контракты requirements/architecture/algorithms, тесты,
OperationRunner, ProjectController, файловый workflow и ApplicationShutdown.

Исторический вердикт заключительного независимого ревью — **NEEDS FIXES** только
из-за **IR-05-T1 (Medium)**. Подтверждённых production lifecycle/data-loss дефектов
ревью не обнаружило. Сохранённые результаты на этом baseline:

| Проверка независимого ревью | Debug | Release |
|---|---|---|
| `all` + `all_qmllint` | PASS | PASS |
| Адресные 78 cases | 77 PASS / 1 FAIL | 77 PASS / 1 FAIL |
| Production CTest, 808 cases | 806 PASS / 1 FAIL / 1 SKIP | 805 PASS / 2 FAIL / 1 SKIP |
| Process crash | 19 PASS | 19 PASS |

Источники — сохранённые локальные ignored
`build/final-review-05-20261026-{debug,release}-{targeted,production}.{log,xml}`,
`build/final-review-05-20261026-commands.txt` и диагностические
`build/final-review-05-20261026-lifecycle-{trace,probe,probe-repeat}.log`.
Полные production logs подтверждают FAIL `/2` в обеих конфигурациях и `/5`
в Release. Диагностический повтор `/2` дал 2 PASS / 3 FAIL; при FAIL видны
`hasProject=true`, `dirty=true`, `workflowPrompt=dirty`, активный worker и открытое
окно. Исходный дефект подтверждён этими сохранёнными доказательствами, отдельный
повтор исходной реализации в рамках исправления не выполнялся. Прежние FAIL
и прежние результаты выше не заменяются новыми PASS.

### Root cause и исправление

`OperationRunner::execute()` устанавливает `ready=true` **до** `beforeDecisionWait`.
Барьер этого hook удерживает worker, но GUI polling продолжает выдавать Accept.
Quit проходит event filter и отложенный вызов `window.close()`: отправка Quit
не доказывает обработку Close. Прежний тест ошибочно требовал результат Close
до Accept при обоих допустимых порядках. Restore после Accept корректно создаёт
dirty-проект и требует решения; подтверждённый Reject после Accept корректно
публикует Rejected.

Тестовый код изменён только в `tests/gui_recovery_tests.cpp`; production-код, форматы, контракты
и остальные тестовые оракулы сохранены. Новая синхронизационная инфраструктура
не добавлялась:

- Исходные девять `LifecycleRoutes/RecoveryClosing` сохранены. После Ready тест
  заимствует существующий `ReadyProductLease`, исключая второй lease и GUI Accept
  во время обработки асинхронного Quit. До освобождения lease/worker проверяются
  `publicationRevoked`, controller Pending, context cancellation и конкретный
  shutdown state. Для Window/Quit context уже Discard; для Close Project Pending
  окончательно превращается в Discard через gate после возврата lease.
- Новые шесть `PermittedLifecycleRoutes/PermittedRecoveryClosing` сначала явно
  выполняют owner poll и проверяют Accept в controller и context, затем Close.
  Discover отдельной фазой не дублируется: он не восстанавливает проект и не
  публикует Reject. Все три его исходных маршрута остаются в первой группе.
- Restore после Accept требует dirty-решения. Cancel сохраняет identity, cell,
  dirty, окно, Open и Active recovery; разрешённый worker успешно завершается.
  Повторное закрытие снова требует dirty-решения; только явный Discard разрешает
  завершение и persisted Rejected восстановленного сеанса.
- Reject до permission оставляет Closed record побайтово неизменным. После
  permission до освобождения worker запись ещё прежняя; затем проверяется именно
  persisted Rejected. Для Restore до Accept допустим предусмотренный архитектурой
  Open marker незавершённой попытки: после освобождения lease это Interrupted,
  с сохранённым snapshot и dataset, без принятого проекта и неявного Reject.
- Проверяются точные terminal status/decision исходной операции, native completion,
  освобождение runner context после Qt join, shutdown/window/project/dirty,
  persisted session и ссылки. Содержимое малого fixture dataset сравнивается
  побайтово, а не только проверяется его наличие.
- Барьер принадлежит fixture и освобождается в TearDown до drain, поэтому fatal
  assertion не оставляет worker с захватом уничтоженного локального объекта.
  Hook одноразовый и принадлежит отдельному controller; ApplicationShutdown
  уничтожается, `quitOnLastWindowClosed` восстанавливается при любом исходе.
  Ожидания используют состояния и существующий `spinUntil`; его watchdog служит
  только границей отказа. Sleep и циклы processEvents для выбора порядка не добавлены.

Ожидаемые результаты до Accept (каждая строка покрывает отдельный исходный variant):

| Variant | Операция / маршрут | Terminal / decision | Shutdown / окно | Проект / dirty | Persisted recovery |
|---|---|---|---|---|---|
| `/0` | Restore / Close Project | Discarded / Discard | Open / видно | нет / false | Interrupted |
| `/1` | Restore / Window Close | Cancelled / Discard | Ready / скрыто | нет / false | Interrupted |
| `/2` | Restore / Quit | Cancelled / Discard | Ready / скрыто | нет / false | Interrupted |
| `/3` | Reject / Close Project | Discarded / Discard | Open / видно | нет / false | Closed, прежние bytes |
| `/4` | Reject / Window Close | Cancelled / Discard | Ready / скрыто | нет / false | Closed, прежние bytes |
| `/5` | Reject / Quit | Cancelled / Discard | Ready / скрыто | нет / false | Closed, прежние bytes |
| `/6` | Discover / Close Project | Discarded / Discard | Open / видно | нет / false | Closed, прежние bytes |
| `/7` | Discover / Window Close | Cancelled / Discard | Ready / скрыто | нет / false | Closed, прежние bytes |
| `/8` | Discover / Quit | Cancelled / Discard | Ready / скрыто | нет / false | Closed, прежние bytes |

После Accept исходные Restore/Reject завершаются Succeeded / Accept. Для Restore
Cancel оставляет Open/видимое окно/dirty-проект и Active record во всех трёх routes.
После явного Discard Close Project удаляет текущий проект и оставляет Open/видимое
окно; Window/Quit достигают Ready/скрытого окна, сохраняя объект dirty-проекта
в завершающемся controller. В обоих случаях record Rejected лишь после явного решения.
Reject не создаёт проект; Close Project оставляет Open/видимое окно, Window/Quit
достигают Ready/скрытого окна. Persisted Rejected — ожидаемый результат явного Reject.
Dataset остаётся неизменным во всех 15 сценариях.

### Адресная проверка исправления

Окончательный C++ тест собран в Debug и Release; incremental `all` + `all_qmllint`
в обеих конфигурациях завершились с exit code 0, без compiler/linker/QML warnings.
Окружение — существующие MSVC/Qt 6.12.0/CMake/Ninja из baseline, build directories
`D:/ZherlitsynEE/CADContour2D/build/windows-{debug,release}`.
Для сборки использован предварительно прочитанный локальный `build/task02-env.cmd`
(vcvars64, UTF-8 code page, process-local Qt/CMake/Ninja PATH и VCPKG_ROOT/QT_ROOT).
Для CTest/executable PATH дополнен `D:/Qt/6.12.0/msvc2022_64/bin` и
`D:/Qt/Tools/CMake_64/bin` только в соответствующем процессе PowerShell.

Фактически выполненные команды из корня репозитория для `<cfg>` = debug, release:

```bat
cmd /d /c build\task02-env.cmd cmake --build --preset windows-<cfg> --target gui_recovery_tests --parallel 4
ctest --preset windows-<cfg> -R "RecoveryClosing\." --output-on-failure -O build/ir05-t1-<cfg>-lifecycle.log --output-junit build/ir05-t1-<cfg>-lifecycle.xml
cmd /d /c build\task02-env.cmd cmake --build --preset windows-<cfg> --target all all_qmllint --parallel 4
ctest --preset windows-<cfg> -R "RecoveryClosing\.ProjectClose.*/[25]$" --repeat until-fail:40 --output-on-failure -O build/ir05-t1-<cfg>-repeat.log --output-junit build/ir05-t1-<cfg>-repeat.xml
build\windows-<cfg>\gui_recovery_tests.exe --gtest_filter=*RecoveryClosing* --gtest_repeat=3
ctest --preset windows-<cfg> -L production --output-on-failure -O build/ir05-t1-<cfg>-production.log --output-junit build/ir05-t1-<cfg>-production.xml
```

Первый адресный CTest дал 15/15 PASS в каждой конфигурации. Затем добавлены
дополнительные строгие проверки dataset bytes, OperationId/kind и context cancellation;
окончательная версия проверена сборкой all/lint, повторами и production ниже.
Повторы `until-fail:40` — отдельные процессы CTest; `gtest_repeat=3` — три прохода
всех fixtures в одном процессе для проверки независимости и восстановления Qt settings.
Это не until-pass и не повторение упавшего теста до случайного успеха.

| Окончательная адресная проверка | Debug | Release |
|---|---|---|
| Исходные 9 combinations, один процесс, 3 прохода | 27 PASS / 0 FAIL | 27 PASS / 0 FAIL |
| Новые 6 after-Accept combinations, тот же прогон | 18 PASS / 0 FAIL | 18 PASS / 0 FAIL |
| Прежний нестабильный `/2`, отдельные процессы | 40 PASS / 0 FAIL | 40 PASS / 0 FAIL |
| Прежний нестабильный `/5`, отдельные процессы | 40 PASS / 0 FAIL | 40 PASS / 0 FAIL |

Все 9 исходных variants и все 6 новых variants также исполняются отдельно внутри
production CTest. Подробная таблица ожидаемых результатов выше является их оракулом;
вариантов `Open || Closing` или произвольного persisted outcome нет.

### Полная регрессия, ограничения и Git

| Проверка окончательной версии | Debug | Release |
|---|---|---|
| `all` + `all_qmllint` | PASS | PASS |
| Production CTest, 814 cases | **813 PASS / 0 FAIL / 1 SKIP** | **813 PASS / 0 FAIL / 1 SKIP** |
| Исходные 9 lifecycle variants в production | 9 PASS / 0 FAIL | 9 PASS / 0 FAIL |
| Новые 6 after-Accept variants в production | 6 PASS / 0 FAIL | 6 PASS / 0 FAIL |
| Recovery (`Recovery` в имени) | 114 PASS / 0 FAIL | 114 PASS / 0 FAIL |
| RecoveryGui | 50 PASS / 0 FAIL | 50 PASS / 0 FAIL |
| AscGui / FileGui | 80 / 68 PASS | 80 / 68 PASS |
| AscGui.GuiLifecycle | 11 PASS / 0 FAIL | 11 PASS / 0 FAIL |
| Shutdown (`Shutdown` в имени) | 49 PASS / 0 FAIL | 49 PASS / 0 FAIL |
| Process crash (`Crash` в имени) | 19 PASS / 0 FAIL | 19 PASS / 0 FAIL |

Все поднаборы этой таблицы извлечены из JUnit XML одного полного production-прогона
для соответствующей конфигурации; они пересекаются и не суммируются. Дополнительный
отдельный crash-прогон не требовался: все 19 cases входят в production. Обе полные
команды завершились с exit code 0; время Debug / Release — 462.01 / 269.88 с,
это не benchmark. После окончательного изменения кода ошибок в выполненных
проверках не было.

Единственный SKIP — неизменённый `Persistence.StoragePath.FileSymlinkEscape`:
обе новые XML подтверждают Windows privilege **1314**. Его ранее выполненная
отдельная приёмка не переоткрывалась. Других SKIP нет, включая cross-volume cases.
GUI проверки выполнялись offscreen/software/Basic. Native GUI, DPI, гигабайтные,
HDD и standalone Recovery-сценарии повторно не запускались согласно области задачи;
результаты прежней ручной приёмки этим исправлением не расширяются.

Свидетельства новых прогонов сохранены в ignored `build/ir05-t1-<cfg>-*.log`:
`build`, `lifecycle`, `repeat`, `lifecycle-inprocess`, `production` и console captures.
CTest разрешил относительный `--output-junit` внутри binary directory, поэтому XML
находятся в `build/windows-<cfg>/build/ir05-t1-<cfg>-{lifecycle,repeat,production}.xml`.
Полные новые production logs и старые review logs сохранены раздельно.

Полный diff просмотрен, `git diff --check` — PASS. В commit входят только
`tests/gui_recovery_tests.cpp` и этот отчёт; hash и итоговый status приводятся
в ответе. Push не выполняется. Production-код, публичные API, persistent formats,
статусы **05.5** и **задачи 05** не меняются; документальное принятие и задача 06
не выполнялись.

Вердикт по **IR-05-T1: FIXED AND VERIFIED**. Исторический вердикт независимого
ревью **NEEDS FIXES** и его исходные FAIL остаются в предыдущем подразделе.
