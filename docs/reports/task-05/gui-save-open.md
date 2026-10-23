# Task 05.5A — GUI Save/Open и безопасный жизненный цикл

Независимое ревью исходного commit `d45358a` завершилось **NEEDS FIXES** (R1/O1).
Разделы исходной реализации ниже сохраняют исторические результаты, а не объявляют
замечания закрытыми. Исправления и текущие проверки —
[Independent recheck fixes](#independent-recheck-fixes).
После native GUI-приёмки на `ecc0d42` получен переданный пользователем вердикт
**PARTIAL VERIFICATION / MANUAL ACTION REQUIRED** и замечание L1.
Адресное исправление и новые результаты — [L1 после native GUI-приёмки](#l1-после-native-gui-приёмки).

## Baseline

2026-10-22, ветка `main`, HEAD
`381191c107671e6849b3a4b67b891020b55226d1`; index и рабочее дерево перед работой чистые.
05.1–05.4 — ACCEPTED. Задание ограничено 05.5A; GUI Recovery/startup decision,
независимое финальное ревью и сквозная приёмка 05.5 в него не входят.

Прочитаны AGENTS, README, план 05.5, UI §§3/6/9–11, requirements §§31–33,
architecture §§24–31 и связанные контракты, algorithms (статусы/численные границы),
отчёты 05.1–05.4 и текущие controller/view model/shutdown/QML/tests.

## Implementation

- Меню «Файл»: новый проект/импорт ASC/XYZ, отдельный Open `.pc2dproj`, Save,
  Save As, закрытие проекта и выход. Toolbar использует те же Actions; «Правка»
  сохраняет Undo/Redo, «Вид» открывает диагностику. Будущих Recovery/DXF/Canvas
  команд нет. Основные параметры и сообщения прокручиваются при малом окне.
- Имя, dirty, путь, draft, состояние worker и cache availability видны в окне.
  Файловые ошибки показывают причину и техническую диагностику; Uncertain
  не объявляется успехом и сохраняет запрет следующего Save до Open manifest.
- Первый Save запрашивает путь, повторный использует baseline, Save As — новый
  путь. При Open входящий manifest и Save прежнего проекта имеют разные пути.
  Отказ от FileDialog не запускает операцию. Перезапись неизвестного manifest,
  второй manifest в одном storage и другие конфликты решает прежний persistence API.
- Draft `cell` принадлежит C++ UI adapter. Перед Save/закрытием/заменой пользователь
  выбирает Apply/Discard/Cancel; недопустимое число оставляется для исправления.
  Dirty читается из controller и разрешается отдельным Save/Discard/Cancel.
  Закрытие диалога/Escape — Cancel, не Discard. Явно отброшенный draft не
  восстанавливается после последующей ошибки операции.
- Save receipt, progress и посторонние notifications не меняют текст, ошибку,
  курсор и выделение. Open синхронизирует `cell` и историю после принятия сеанса.
  Новый ввод `cell` во время принятого Open/Import недоступен; Save позволяет
  последующие правки и сохраняет свой snapshot без их потери.
- Missing/corrupt cache показывает metadata и конкретное ограничение; cell,
  Undo/Redo, Save/Save As недоступны. Есть явный bounded recheck существующим API.
  Исходный ASC/XYZ для Open не нужен. Полная validation, point scan, PNG/reduced
  rebuild, сортировка и геометрические операции из просмотра не запускаются.
- Close проекта оставляет приложение открытым. Системная кнопка, Exit и Quit
  входят в тот же workflow. Активный Save дожидается terminal receipt/native join;
  более поздние draft/dirty требуют нового решения. Cancel оставляет окно/сеанс.

Изменены `ProjectViewModel`, отдельный implementation-файл файлового workflow,
`ApplicationShutdown`, production `Main.qml`, CMake и GUI tests.
`operation_probe --auto` явно отвечает Discard через тот же token API;
интерактивный probe не получает автоматического согласия.

## Architecture

Runner, persistence transactions, manifest v1 и Point Cache v1 не изменены.
В `ProjectController` добавлен только read-only `fileCommandContext()`:
он предоставляет прежние stamp/epoch/operation identity также во время Busy,
не разрешая запуск и не меняя guards. Это необходимо для Open, запрошенного
при активном worker: прежний selection getter тогда возвращает пустое значение,
по одному optional stamp нельзя отличить NoProject → NoProject после Close.
Два regressions проверяют сохранение намерения после cancellation worker
и отзыв при смене epoch без появления проекта. Исходные import getters
делегируют чтение этому getter с прежними admission условиями.
Единственный project session, saved baseline, dirty и Undo/Redo
остаются в controller. QML содержит presentation, не сохранение или domain state.
Новый C++ workflow координирует только пользовательские решения и существующие API.

Подтверждения одноразовые и проверяют session/input stamp и draft. FileDialog
создаётся отдельно для каждого token: старый native callback сохраняет прежний
token даже при уже открытом следующем диалоге. Закрывающийся decision popup
сохраняет свой token отдельно от следующего запроса.

Повторное admission после Busy/reentrant command запускается notification о
завершённом command transition. Самоповторяющихся таймеров нет. Подтверждённый Import
передаётся исходному deferred protocol 05.4; его cancellation, native completion,
coalesced notification и nested event guards сохранены. Смена inputs/session
отзывает старый выбор; Autosave сам по себе этого не делает.

Результат ожидаемого Save фиксируется по OperationId в terminal notification.
Это необходимо, поскольку очередной Autosave может начаться до доставки queued
продолжения. После успеха повторно проверяются draft/dirty; после failure,
cancellation или Uncertain исходное Close/Open/Import не продолжается.

Quit event больше не вызывает низкоуровневый shutdown в обход UI, а инициирует
закрытие окна. После решений пользователя действуют прежние Open → Closing → Ready,
publication permission, cooperative cancellation, native completion/Qt join и
recovery retention. Подтверждённый Discard передаётся существующему Close/Shutdown.
Open/Import сохраняют прежнюю conservative retirement policy.

## Tests

Проверки выполняются в штатном x64 MSVC окружении README, Qt 6.12.0,
с локальными `QT_ROOT`, `VCPKG_ROOT` и PATH. Глобальное окружение не менялось.
Новые тесты используют production QML module и реальные Import/Save/Open contracts;
барьеры SaveTransaction удерживают worker до/после permission без случайных delays.

Итоговые команды:

```bat
cmake --build --preset windows-debug --target all all_qmllint --parallel 4
cmake --build --preset windows-release --target all all_qmllint --parallel 4
ctest --preset windows-debug -R FileGui --output-on-failure
ctest --preset windows-release -R FileGui --output-on-failure
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
```

На окончательных исходниках:

| Проверка | Debug | Release |
|---|---|---|
| Build `all` + `all_qmllint` | PASS, без warnings | PASS, без warnings |
| Целевой CTest `FileGui` | 33/33 PASS | 33/33 PASS |
| Полный production CTest | 728 PASS / 0 FAIL / 1 SKIP (729) | 728 PASS / 0 FAIL / 1 SKIP (729) |
| Recovery в production | 64/64 PASS | 64/64 PASS |
| Open в production | 84/84 PASS | 84/84 PASS |
| Save в production | 45/45 PASS | 45/45 PASS |
| Import/Z/lifecycle `AscGui` в production | 80/80 PASS | 80/80 PASS |
| `operation_probe --auto close` / `--auto quit` | exit 0 / 0 | exit 0 / 0 |

Все поднаборы входят в production и не суммируются с ним. Из 34 новых tests
33 находятся в `gui_file_tests`, один — в `gui_lifecycle_tests`.
Единственный SKIP в обеих конфигурациях — прежний
`Persistence.StoragePath.FileSymlinkEscape`: Windows file symlink privilege
unavailable, **1314**. Новых FAIL/SKIP нет. Известный `Applied`/`NoChange`
Recovery case в текущих полных прогонах не упал; его нестабильность не объявляется
исправленной. Время production — 240.80 s / 154.96 s, это не benchmark.

Smoke probe выполнен после сборки с локальными `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`, `QT_QUICK_CONTROLS_STYLE=Basic`:

```bat
build\windows-debug\operation_probe.exe --auto close
build\windows-debug\operation_probe.exe --auto quit
build\windows-release\operation_probe.exe --auto close
build\windows-release\operation_probe.exe --auto quit
```

Логи окончательной серии: `build/055a-complete-build-{debug,release}.log`,
`055a-complete-target-{debug,release}.log`,
`055a-complete-production-{debug,release}.log`,
`055a-complete-probe-{debug,release}-{close,quit}.log` в `build/`;
полные CTest diagnostics — `build/windows-{debug,release}/Testing/Temporary/LastTest.log`.
Более ранние прогоны (726/727 tests) не подменяют эту проверку 729 tests.

Покрыты первый/повторный Save и Save As, path cancellation/conflict, I/O Open
failure, Uncertain Save, cancellation до permission, snapshot с более поздними
правками, Open без source, partial missing/corrupt и bounded recheck, отдельные
Draft/Dirty/Undo к baseline, все четыре пути закрытия и каждый из них при уже
разрешённом Save с/без новых правок, Busy/Autosave, одноразовые и retired-dialog
callbacks, nested notifications и уничтожение adapter из workflow notification.

Прежние Import/Z/deferred/nested/lifetime tests сохранены. В сценариях замены
добавлен явный ответ на новый dirty prompt. Проверка failure с draft вводит его
после запуска заведомо ошибочного Import и по-прежнему проверяет реальный Failed,
историю, текст, ошибку, курсор и выделение. Проверка диагностики теперь явно
раскрывает её кнопкой; layout test прокручивает доступные параметры в viewport
и сохраняет assertions их положения и размеров. Assertions worker/lifecycle
и production filters не ослаблялись.

При разработке выявлены и исправлены потеря queued продолжения при раннем
Autosave после Save, reentrant ответ во время создания prompt и недостаточная
изоляция token при повторном использовании native FileDialog; добавлены regressions.
Отдельно закрыта NoProject ABA-граница при первоначальном Busy. Промежуточные
ошибки/логи не выдаются за окончательный PASS.

Известное наблюдение Recovery 05.4 (`Applied`/`NoChange` после автоматического
Autosave) не исправлялось и assertions не менялись. Итог текущего запуска
фиксируется отдельно; успешный запуск не доказывает устранение нестабильности.

## GUI и ручная приёмка

Автоматические тесты работают с offscreen/software/Basic. Новые FileDialog tests
передают `selectedFile/accepted/rejected` реальному production QML Dialog object,
подавляя только открытие OS-окна. Прежние tests включают Quick FileDialog fallback.
Это не ручная проверка native Windows FileDialog, фокуса или DPI.
В этой сессии нет доступного инструмента управления Windows desktop GUI;
ручная приёмка ожидается. Воспроизводимый сценарий:

1. Запустить `build\windows-release\CADContour2D.exe` из окружения README.
   Отменить startup Import: окно остаётся без проекта, Open доступен.
2. Импортировать небольшой ASC/XYZ, включая имя с кириллицей, пробелами и `%`.
   При ненулевом Z отказаться, затем повторить и согласиться: отказ сохраняет
   прежний сеанс, согласие принимает новый cache. `cell` начинается с 1 мм.
3. Ввести `0,5`, выбрать Save. В draft prompt нажать Cancel/Escape: ввод остаётся.
   Повторить с Apply и выбрать новый отдельный каталог `.pc2dproj`: звёздочка
   исчезает, путь виден, файл существует. Изменить cell, выполнить повторный Save:
   FileDialog не открывается. Undo к сохранённому значению снимает dirty.
4. Ввести `1e-`, вызвать Save/Exit. Apply не принимает число; поле и ошибка
   доступны для исправления. Повторить с Discard, затем Cancel в dirty prompt:
   окно остаётся открытым. Проверить курсор/выделение во время progress/Save.
5. Save As в отдельный каталог сохраняет копию и меняет текущий путь. Другое имя
   manifest в прежнем каталоге даёт Conflict, не ложный успех. Отказаться от
   native FileDialog и overwrite question: операция не начинается.
6. Удалить/переименовать исходный ASC/XYZ, открыть сохранённый проект. Cell
   восстановлен, история пуста, cache доступен. Изменить cell и открыть другой
   manifest: проверить Save/Discard/Cancel, включая отказ от Save path и I/O error.
7. В копии проекта временно переименовать cache. Open принимает metadata с
   диагностикой missing; cell/Undo/Save недоступны. Вернуть cache и вызвать
   ограниченную повторную проверку: доступность восстанавливается без карты
   и full validation. Повторить с повреждённым header cache в отдельной копии.
8. На dirty проекте проверить Close Project, системную кнопку, File → Exit
   и системный Quit/завершение сеанса приложения в обычном пользовательском пути.
   Cancel оставляет сеанс; Discard закрывает по выбранному пути. Close Project
   оставляет главное окно. При Save на более крупном cache закрытие ожидает
   фактического завершения; новая правка после snapshot требует нового решения.
9. Проверить native focus, Tab/Enter/Escape, повторное открытие диалогов, размеры
   400×380 и 1000×700, DPI 100/150/200% и перенос между мониторами. Все решения
   доступны, параметры прокручиваются, закрытие popup не означает Discard.

Принудительное завершение процесса не является штатным Quit и не обязано
показывать dialogs. GUI Recovery здесь не проверяется и не предлагается.

## Limitations / Status / Git

05.5A ожидает ручной приёмки native GUI. Recovery GUI/startup decision,
независимое финальное ревью и сквозная приёмка остаются в 05.5B; задача 05 и
05.5 — **IN PROGRESS / NOT ACCEPTED**. Большие benchmarks/HDD/UNC и power-loss
этой работой не подтверждены; прежние storage/retention ограничения сохраняются.

**Status: AWAITING MANUAL ACCEPTANCE.** Автоматические проверки — **TESTS PASSED**.
Полный diff просмотрен; `git diff --check` и staged diff check — PASS.
Локальные Markdown paths в затронутых документах проверены, отсутствующих нет;
anchors отдельно не проверялись. Временный environment wrapper удалён,
логи проверок остаются в ignored `build/`. Несвязанных пользовательских изменений
на baseline не было. Локальный commit следует стилю истории; hash — в итоговом ответе.
Push, PR, deployment и переход к 05.5B не выполняются.

## Independent recheck fixes

### A. Baseline

Ветка `main`, исходный HEAD `d45358a4e095c956ea51018ca2b4375ddbc73a2c`,
рабочее дерево и index чистые. Область — подтверждённые R1/O1 и отрицательная
регрессия обычного Failed Save. Повторная реализация 05.5A и 05.5B не выполнялись.

### B. R1 Verification

На исходном production-коде новые тесты детерминированно воспроизвели R1 в Debug
и Release: **0 PASS / 2 FAIL** в каждой конфигурации, до и после permission Autosave.
После старого Discard вводился `1e-`, затем разрешалось завершение worker.
Import запускался ровно один раз без нового решения, менял session/history,
сбрасывал поле в `1` и терял selection token. Assertions на сохранность draft,
прежнего проекта и отсутствие Import падали. Исходная диагностика сохранена:
`build/055a-r1-red-{debug,release}.log`.

Причина: `advanceWorkflow()` завершал Import workflow до actual admission,
а retry `pendingImport_` вызывал `startSelectedImport()` напрямую. Тем самым
действовавшее согласие на старое состояние переживало появление нового draft.

Workflow теперь удерживается до definitive admission/failure/cancellation.
Retry проходит тот же `advanceWorkflow()` и проверяет новый draft. После
pre-dispatch notification есть финальная проверка поля: вложенный observer
может изменить его, поэтому управление возвращается в тот же workflow.
Прежний deferred protocol, его guards и command-transition wake сохранены.
Очереди, polling, новые context/token механизмы и блокировка ввода при Autosave
не добавлены. `clearSelection()` согласованно завершает удерживаемый workflow;
Close/Exit могут явно отозвать отложенный Import без обхода нового draft/dirty.

При Apply новой persistent-правки прежний Discard отзывается. Stamp/context
обновляются до проверки следующего draft, в том числе если он появился внутри
notification самого Apply. Ошибка Apply сохраняет выбранный файл и ввод,
показывает ошибку и новый token решения; Cancel отменяет намерение и оставляет
поле для исправления. Устаревший ответ не применяет Discard к новому тексту.

Регрессии через production QML/controller: оба состояния permission; валидный
и невалидный поздний ввод; Apply/Discard/Cancel; новое dirty после Apply;
повторные старые ответы и новый token; вложенная доставка до dispatch,
в terminal Autosave, на stale reply и в notification Apply. Проверяются
выбранный файл, проект/history, текст/курсор/выделение и число actual Import.
Существующие N1/revocation/destruction/Close/Shutdown tests не ослаблены.

### C. O1 Verification

Причина подтверждена управляемой доставкой штатного post-Import Autosave:
старый оракул детерминированно упал **1/1** в Debug и Release на ожидании
«Импорт завершён» у уже другой текущей операции. Это воспроизведение допустимого
порядка событий, а не заявление о частоте естественного падения.
Логи — `build/055a-o1-red-{debug,release}.log`.

Тест сохраняет terminal notification именно Import: OperationId, Succeeded,
Accept, worker status, identity принятого проекта и видимое сообщение QML.
После этого намеренно дожидается запуска Autosave. Исходный содержательный
assertion «Импорт завершён» проверяет сохранённый terminal feedback; текущая
операция также обязана иметь непустую отличающуюся обратную связь. Metadata,
cache availability и независимость от source проверяются по принятому проекту.
Production-контракт показывает текущую операцию, включая Autosave; менять GUI
ради удержания текста прошлой операции не потребовалось. Assertion не удалён
и не заменён проверкой произвольного success.

### D. Failed Save Regression

Три сценария: Close, Open и Import с выбором Save. Существующий Save fault seam
удерживает ManifestWrite и возвращает write failure до publication. Во время
ожидания вводится невалидный draft; сохраняются ошибка, курсор и выделение.
Проверяются именно Failed / FileIO, отсутствие `unconfirmedSave`, прежние
baseline/session/history/dirty, сохранность ввода и отсутствие continuation
или ложного success. Старые callbacks ничего не запускают; новый явный Save
после устранения fault успешно сохраняет тот же проект. Прежний Uncertain test
сохранён отдельно.

### E. Tests

Проверки окончательных исходников выполнялись с Qt 6.12.0/MSVC, CMake presets
`windows-debug` и `windows-release`, после загрузки `vcvars64` и путей Qt/CMake/Ninja.
Ниже `<cfg>` обозначает каждую из этих двух конфигураций (`debug`, `release`):

```text
cmake --build --preset windows-<cfg> --target all all_qmllint
ctest --preset windows-<cfg> -R FileGui
ctest --preset windows-<cfg> -R AscGui
ctest --preset windows-<cfg> -L production
ctest --preset windows-<cfg> -R FirstImportUsesAcceptedMetadataAndRetainsTerminalSuccess --repeat until-fail:20
ctest --preset windows-<cfg> -R R1NewDraftBeforeAdmissionRequiresAnotherDecision --repeat until-fail:20
ctest --preset windows-<cfg> -R R1NestedNotificationAndStaleAnswersCannotAdmitNewDraft --repeat until-fail:10
build/windows-<cfg>/operation_probe.exe --auto close
build/windows-<cfg>/operation_probe.exe --auto quit
git diff --check
```

| Проверка | Debug | Release |
|---|---|---|
| `all` + `all_qmllint` | PASS | PASS |
| FileGui, включая 25 новых случаев | 58 PASS / 0 FAIL | 58 PASS / 0 FAIL |
| AscGui, Import/lifecycle | 80 PASS / 0 FAIL | 80 PASS / 0 FAIL |
| Production, 754 теста | 753 PASS / 0 FAIL / 1 SKIP | 753 PASS / 0 FAIL / 1 SKIP |
| O1, один тест × 20 | 20 PASS / 0 FAIL | 20 PASS / 0 FAIL |
| R1, два состояния permission × 20 | 40 PASS / 0 FAIL | 40 PASS / 0 FAIL |
| R1, восемь reentry случаев × 10 | 80 PASS / 0 FAIL | 80 PASS / 0 FAIL |
| Probe Close / Quit | exit 0 / exit 0 | exit 0 / exit 0 |

Режим GUI-проверок — offscreen/software/Basic. Каждый probe вернул
`status=6` (Cancelled), `decision=2` (Discard), `shutdown=2` (Ready).
Повторы используют `until-fail`, а не повторение до успеха; намеренные RED-прогоны
R1/O1 сохранены отдельно и не включены в результаты окончательного кода.

`TEMP`/`TMP` — `C:\Users\User\AppData\Local\Temp`, build расположен на `D:`.
`OpenApplication.Open.CrossVolumeDirectoryTransfer` и
`SaveApplication.Save.SaveAsAcrossAvailableWindowsVolumes` прошли в обоих
production-прогонах, межтомных SKIP нет. Единственный SKIP в обеих конфигурациях —
`Persistence.StoragePath.FileSymlinkEscape`: Windows privilege unavailable,
system code **1314** (Debug — подробный production log, Release — также адресный
`ctest -R FileSymlinkEscape -V`). Время полного production: Debug 274.79 с,
Release 164.10 с.

В итоговых incremental build/qmllint логах предупреждений и ошибок нет.
Чистая пересборка здесь не выполнялась; прежний сторонний C4702 в `fmt/base.h`
не объявляется исправленным. Warnings и критерии тестов не подавлялись.

Логи окончательных прогонов: `build/055a-recheck-build-<cfg>.log`,
`055a-recheck-file-<cfg>.log`, `055a-recheck-import-<cfg>.log`,
`055a-recheck-production-<cfg>.log`, `055a-recheck-{o1,r1,nested}-repeat-<cfg>.log`,
`055a-recheck-probe-<cfg>-{close,quit}.log` (все в ignored `build/`).

### F. Scope and Limitations

Production изменён только в `project_file_workflow.cpp` и `project_view_model.cpp`.
Тесты — `gui_file_tests.cpp` и `gui_import_tests.cpp`; документация — этот отчёт.
Controller/runner, persistence formats, cache v1, snapshot publication,
cancellation, native completion/join и Recovery retention не менялись.
Новых зависимостей нет; GUI Recovery/startup decision не затронуты.
Прежние ограничения ручной приёмки native FileDialog/фокуса/DPI сохраняются.
Известная нестабильность Recovery `Applied`/`NoChange` не исправлялась и не
объявляется устранённой при успешных повторах других сценариев.

### G. Status

**READY FOR INDEPENDENT RECHECK.** R1 воспроизведён до исправления и покрыт
регрессиями; O1 имеет оракул конкретного принятого Import; обычный Failed Save
проверен для трёх продолжений. Все запрошенные автоматические проверки прошли
с указанным symlink SKIP. 05.5A, 05.5 и задача 05 не объявляются принятыми;
исправления предназначены тому же независимому ревьюеру для целевой повторной
проверки R1/O1.

### H. Git

Полный diff просмотрен; `git diff --check` и staged diff check — PASS.
Временный environment wrapper удалён; диагностика остаётся в ignored `build/`.
В локальный commit включены только пять перечисленных файлов. Его hash и итоговый
Git status приводятся в ответе. Push/PR и переход к 05.5B не выполнялись.

## L1 после native GUI-приёмки

### Baseline

2026-10-23, ветка `main`, HEAD `ecc0d4210443e682b46abc7da6f3409235f89583`.
Index и рабочее дерево перед работой чистые. Область — только L1 в 05.5A.
Сообщённый результат независимой native GUI-проверки не подменяется offscreen-тестами.

### L1: причина и исправление

`fileMessage_` принадлежал UI adapter, но его сброс зависел от начала файлового
workflow, а не от смены runtime identity проекта. Замена/Close через production
controller могли оставить сообщение Save предыдущего сеанса. Поздний terminal
notification старой операции также мог сформировать сообщение уже после Close.

При фактической смене runtime identity, включая переход к отсутствующему проекту,
adapter очищает сообщение и отмечает OperationId прежнего контекста обработанным.
Исключение — активный принятый Open нового сеанса: его собственное завершающее
сообщение должно быть доставлено. Существующие token/lifetime guards не меняются.
Правки cell, dirty, Save receipt и повторные notifications без смены identity
не очищают актуальные Failed/Cancelled/Uncertain сообщения.

На исходной production-реализации с новыми тестами Debug: **2 PASS / 2 FAIL**.
RED — Import B непосредственно через controller при открытом production QML и
Close через controller: GUI продолжал показывать сообщение Save A. Обычный путь
через QML replacement workflow и успешный Open прошли уже на baseline; точный
native-сценарий из замечания в offscreen-режиме этим путём не воспроизведён.
Это различие сохранено явно, без утверждения о воспроизведении native-порядка событий.
Лог: `build/055a-l1-red-cases-debug.log`; production-код в том executable ещё исходный.

### Tests

Добавлено 8 случаев через production QML/controller:

- Import A (production fixture) → GUI Save A → Import B через QML либо controller.
  Проверяются новая runtime identity, dataset/число точек, «Без имени», dirty,
  отсутствие пути, cell = 1, cache availability, пустая история, заголовок,
  отсутствие/невидимость сообщения A и бездействие устаревших Save callbacks.
- Close после Save и повторный queued notification; успешный Open с сохранением
  его актуального сообщения; Close во время Open с поздним terminal cancellation
  закрытого сеанса (worker удерживается существующим барьером).
- Failed Save, Cancelled Save и Uncertain Save: соответствующий фактический статус,
  сохранённый сеанс/dirty, GUI-диагностика и её сохранность после изменения cell
  и queued notification. Существующие Save/Open/Import/lifecycle assertions сохранены.

Проверки выполняются в окружении MSVC/Qt из README, без изменения глобальных путей.
GUI tests — offscreen/software/Basic; native Windows GUI здесь не проверялся.
Фактические команды для каждой конфигурации:

```text
cmake --build --preset windows-<cfg> --target all all_qmllint --parallel 4
ctest --preset windows-<cfg> -R FileGui --output-on-failure
ctest --preset windows-<cfg> -R AscGui --output-on-failure
ctest --preset windows-<cfg> -L production --output-on-failure
git diff --check
```

| Проверка | Debug | Release |
|---|---|---|
| `all` + `all_qmllint` | PASS, без warnings в incremental build | PASS, без warnings в incremental build |
| FileGui (включая 8 новых L1) | 66 PASS / 0 FAIL / 0 SKIP | 66 PASS / 0 FAIL / 0 SKIP |
| AscGui | 80 PASS / 0 FAIL / 0 SKIP | 80 PASS / 0 FAIL / 0 SKIP |
| Production, 762 теста | 761 PASS / 0 FAIL / 1 SKIP | 761 PASS / 0 FAIL / 1 SKIP |
| `git diff --check` | PASS (общий diff) | PASS (общий diff) |

Поднаборы входят в production и не суммируются с ним. Единственный SKIP в обеих
конфигурациях — `Persistence.StoragePath.FileSymlinkEscape`, Windows file symlink
privilege unavailable, **1314** (подтверждено в обоих `LastTest.log`).
Время полных прогонов — Debug 311.84 с / Release 168.84 с; это не benchmark.
Существующий нестабильный Recovery `Applied`/`NoChange` case в этих прогонах
не упал и не объявляется исправленным.

Логи текущих проверок — ignored `build/055a-l1-build-{debug,release}.log`,
`055a-l1-file-{debug,release}.log`, `055a-l1-asc-{debug,release}.log`,
`055a-l1-production-{debug,release}.log`; отдельный GREEN новых 8 случаев Debug —
`055a-l1-green-debug.log`. Полная диагностика production —
`build/windows-{debug,release}/Testing/Temporary/LastTest.log`.

Первый адресный CTest был вызван до завершения линковки: discovery завершился
ошибкой `resource busy or locked`, тесты не выполнялись
(`build/055a-l1-red-debug.log`). После завершения build отдельный baseline-прогон
и последующие проверки выполнялись последовательно относительно сборки.
Исторические результаты предыдущих разделов не используются как новые PASS.

### Scope

Изменены только `src/ui/project_view_model.cpp`, `tests/gui_file_tests.cpp`
и этот отчёт. Production-изменение — 9 строк в существующем state notification
handler; новых абстракций и зависимостей нет. Persistence/controller/runner,
форматы, публикация snapshot, cancellation/native completion/join и lifecycle
guards сохранены. 05.5B и GUI Recovery/startup decision не затронуты.

### Limitations

Для независимой адресной перепроверки остаётся native GUI-сценарий L1.
Ручная приёмка системного DPI 150/200% и длительного Save с поздними правками
остаётся незавершённой; автоматические snapshot/later edits tests её не заменяют.
Перенос между мониторами — **N/A**, второго монитора нет (по данным приёмки).
Функциональных изменений ради этих сценариев не внесено.

### Status and Git

**READY FOR INDEPENDENT RECHECK.** Изменение предназначено для адресной проверки L1.
05.5A, 05.5 и задача 05 не объявляются принятыми; полный аудит не перезапускается.
Полный diff просмотрен; `git diff --check` — PASS. В локальный commit включаются
только три перечисленных файла; его hash и итоговый Git status приводятся в ответе.
Логи остаются в ignored `build/`; временных исходников/инструментов не добавлено.
Push не выполняется. После commit работа над исправлением останавливается.
