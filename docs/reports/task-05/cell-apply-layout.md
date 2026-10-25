# Исправление обрезания кнопки «Применить» — 2026-10-25

## A. Baseline и область

- Ветка `main`, HEAD `13286ecbe1ca5c4d12b6e4e073e111f5e8e383da`; исходный Git status чистый.
- Источник нового Low layout finding: `F:/CADContour2D-benchmark/native055-final-20261024/acceptance-report.txt`, §F/L1.
  Это отдельный дефект, не прежний закрытый L1 сообщения Save.
- Проверены README, AGENTS.md, GUI-контракты requirements/architecture/ui-design,
  production Main.qml и существующие GUI-тесты. Контракты и статусы принятия
  05.5A, 05.5B и задачи 05 не меняются; исторические результаты остаются в прежних отчётах.

## B. Root cause

В основном [ScrollView](../../../qml/Main.qml) `contentWidth: availableWidth`
и `ColumnLayout.width: parent.width` оставляли всю ширину содержимому, включая
область вертикального scrollbar. В фактически используемом Windows style
`rightPadding` равен 0: одного `availableWidth` недостаточно для исключения scrollbar.
`clip: true` и полоса прокрутки скрывали правую часть строки.

Измерения настоящего Main.qml через Windows backend, client 400×380,
в scene coordinates (логические пиксели), одинаковы после Open и отмены Import:

| Геометрия | Baseline | После исправления |
|---|---:|---:|
| Видимая область без scrollbar, X | 16…367 | 16…367 |
| Колонка / строка cell, ширина | 368 / 368 | 351 / 351 |
| TextField, ширина | 293 | 276 |
| Кнопка, X | 314…384 | 297…367 |
| Вертикальный scrollbar, ширина | 17 | 17 |

`TextField.implicitWidth` равен 112, `Layout.fillWidth` уже позволял полю
сжиматься. Причиной не был обязательный минимум поля или кнопки.
Ранний вариант теста, учитывающий только `availableWidth`, ошибочно проходил:
он не исключал перекрывающий scrollbar. После исправления геометрического
оракула baseline дал **0 PASS / 2 FAIL** (`red3-windows.log`).
Прежний `ProductionQml.ResizedLayoutKeepsControlsInsideWindow` проверяет
границу всего окна, поэтому не обнаруживает выход за более узкий viewport.

## C. Исправление

Основной ScrollView вычитает из `availableWidth` только ширину scrollbar,
ещё не учтённую в `rightPadding`:
`availableWidth - Math.max(0, effectiveScrollBarWidth - rightPadding)`.
Колонка явно получает этот же `contentWidth`. Это также исключает двойной
учёт места в стилях, которые уже резервируют scrollbar в padding.

Минимальный размер, clipping, подпись и сама строка TextField/Button сохранены.
`Layout.fillWidth` поля распределяет оставшееся место. Обработчики Apply/Enter,
enabled bindings, C++ controller, persistence, workers и остальные GUI-компоненты
не изменялись. requirements.md и ui-design.md описывают прежний контракт и не переписываются.

## D. Автоматическая проверка

В существующий [gui_file_tests.cpp](../../../tests/gui_file_tests.cpp) добавлены
два production QML case `ProjectOpenAndImportCancelled/CellLayout.ApplyFitsViewportAndFieldUsesRemainingWidth`:
реальный Save/Open временного проекта и отсутствие проекта после отмены Import.
Каждый проверяет 400×380, 960×720 и 1200×900, вертикальную прокрутку при минимуме,
границы кнопки с исключением scrollbar, отсутствие перекрытия с полем, полную
ширину подписи, enabled/disabled и расширение поля при увеличении окна.
Кнопка прокручивается в viewport по вычисленной геометрии. Pending layout
обрабатывается через `ensurePolished`, без timed sleep.

Для открытого проекта отправляются настоящие QMouseEvent в центр кнопки и
QKeyEvent Return/Tab/Backtab в окно: проверены Apply, Enter, draft/dirty и
`hasActiveFocus` поля/кнопки. Это автоматическая доставка Qt events, отдельно
от native SendInput-проверки ниже. QML warnings остаются failures.

В проверенной CMD-среде пользователя выполнены для `windows-debug` и `windows-release`:

```bat
cmake --build --preset <preset> --target all all_qmllint --parallel 4
ctest --preset <preset> -R "CellLayout|FileGui|AscGui|RecoveryGui|ProductionQml|QmlIntegration|QmlRegistration" --output-on-failure
ctest --preset <preset> -L production --output-on-failure
```

| Проверка окончательного кода | Debug | Release |
|---|---|---|
| all + all_qmllint | PASS | PASS |
| GUI integration/regression subset | 206 PASS / 0 FAIL | 206 PASS / 0 FAIL |
| Полный production CTest, 808 cases | 806 PASS / 0 FAIL / 2 SKIP | 806 PASS / 0 FAIL / 2 SKIP |
| Адресная регрессия, offscreen + Basic (в subset/production) | 2/2 PASS | 2/2 PASS |
| Адресная регрессия, Windows backend, системные 100% | 2/2 PASS | 2/2 PASS |
| Адресная регрессия, Windows backend, системные 150% | 2/2 PASS | 2/2 PASS |

Два SKIP: `OpenApplication.Open.CrossVolumeDirectoryTransfer` и
`SaveApplication.Save.SaveAsAcrossAvailableWindowsVolumes`; TEMP и тестовый
workspace находятся на одном томе. Дополнительные cross-volume сценарии
в этой локальной layout-задаче не запускались. Тестовые оракулы прежних тестов не менялись.

Адресные Windows-прогоны после той же CMD-инициализации:

```bat
set "QT_QPA_PLATFORM=windows"
build\windows-debug\gui_file_tests.exe --gtest_filter=*CellLayout*
build\windows-release\gui_file_tests.exe --gtest_filter=*CellLayout*
```

Переменная задавалась только в дочерних CMD-процессах. `QT_SCALE_FACTOR`
не использовалась. Полные production-прогоны завершены до изменения системного DPI.

## E. Native Windows GUI

Использован skill [computer-use](C:/Users/Egor31/.codex/plugins/cache/openai-bundled/computer-use/26.1021.31345/skills/computer-use/SKILL.md),
настоящие `build/windows-debug/CADContour2D.exe` и `build/windows-release/CADContour2D.exe`,
штатный Windows backend/style и native FileDialog. Probe не использовался.
Минимальная начальная геометрия задавалась штатным аргументом Qt
`-qwindowgeometry 400x380` ([Qt 6.12 QGuiApplication](https://doc.qt.io/qt-6.12/qguiapplication.html)).
Maximize/restore возвращали это же минимальное окно после Open.

| Условие | 100% | 150% |
|---|---|---|
| Минимум, Import отменён, Apply целиком видна, disabled | PASS, Debug | PASS, Release |
| Минимум, открытый проект, вертикальная прокрутка, поле и вся подпись Apply | PASS, Debug | PASS, Release |
| Увеличенное окно, строка адаптируется | PASS | PASS |
| Обычное окно 960×720 после отмены Import | PASS, Release | Отдельно не повторялось; проверены минимум и maximized |
| Мышь: ввод и Apply | 0.125 → 0.5, revision 1 | 0.75 → 1.25, revision 1 |
| Enter из поля | 0.75, revision 2 | 1.5, revision 2 |
| Shift+Tab возвращает ввод в поле | PASS: следующий Ctrl+A/ввод попадает в cell | PASS: следующий Ctrl+A/ввод попадает в cell |
| Tab переводит к Apply, Space активирует её | Tab доставлен; активация отдельно не повторялась | PASS: draft 2 → current 2, revision 3 |

На 100% наблюдалась синяя рамка кнопки после Tab. Однако отдельное
однозначное подтверждение native **видимого фокуса без hover на обоих DPI — NOT VERIFIED**:
по уменьшенным captures на 150% индикатор недостаточно различим. Это ограничение
визуальной проверки, не установленный новый focus defect. Функциональный переход
Tab → Space на 150% и автоматические `hasActiveFocus` assertions подтверждены.
Для ручной проверки: открыть тестовый проект, уменьшить до 400×380, прокрутить к
cell, оставить указатель над полем, нажать Tab и оценить индикатор Apply;
Space должна применить draft, Shift+Tab вернуть редактирование поля.

Масштаб действительно изменён в Windows Settings → System → Display с 100% на
150%; приложение запускалось новым процессом. **Исходные 100% восстановлены** и
подтверждены Settings. Разрешение не менялось. 200% — **NOT VERIFIED**:
безопасные presets данного дисплея содержат 100/125/150/175%; custom scaling,
выход из сеанса и изменение других приложений для этого не выполнялись.

Open/правки/Save выполнены только над отдельной копией маленького fixture:
`build/layout-apply-20261025/native-project`, источник —
`F:/CADContour2D-benchmark/native055-final-20261024/A/saved` (2 точки, cache 139264 bytes).
Пользовательские Restore/Reject не выполнялись. SHA-256 всех 16 исходных
production recovery records совпадают до/после. Собственные Open/Save создали
тестовые runtime recovery metadata. Все запущенные production процессы завершены.

## F. Итог

**FIXED WITH LIMITATIONS**: Low layout-дефект устранён и проверен геометрической
регрессией и standalone native GUI при системных 100%/150%. Failures в итоговых
сборках/тестах нет. Ограничения — native visual focus, отдельное обычное окно на
150% и недоступный безопасный preset 200%. Это не закрывает другие условия
приёмки 05.5A/05.5B и не меняет статусы этапов. Работа над задачей 06 не начиналась.

## G. Evidence и Git

Локальные доказательства в ignored `build/layout-apply-20261025/`:
`red3-windows.log` (baseline RED), `content-windows.log` (исправление GREEN с геометрией),
`build-{debug,release}.log`, `gui-{debug,release}.log`,
`production-{debug,release}.log`, `target-windows{100,150}-{debug,release}.log`,
`baseline-min100.jpg`, `fixed-*.jpg`, `keyboard-*.jpg`,
`scale100-before.jpg`, `scale150.jpg`, `scale100-restored.jpg`,
`recovery-before.json`, `recovery-unchanged.json`.
Промежуточные диагностические попытки сохранены только как локальные evidence logs;
диагностических выводов в коде нет. Временный CMD wrapper удалён перед commit.

Изменённые файлы: Main.qml, gui_file_tests.cpp, этот отчёт и ссылка на него из
gui-recovery.md. Полный diff просмотрен, `git diff --check` — PASS.
Hash commit и итоговый status приводятся в ответе; push не выполняется.
