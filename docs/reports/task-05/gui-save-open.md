# Task 05.5A — GUI Save/Open и безопасный жизненный цикл

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
