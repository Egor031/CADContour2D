# Исправления R1/R2 после независимого ревью 04.1

## A. Baseline

Дата проверки: 2026-10-17. Ветка `main`, исходный HEAD
`9af1a0cbd4380322eb7f5520a9a8b9b899da85b4`. Рабочее дерево до начала чистое.

Изучены AGENTS.md, README, относящиеся к импорту разделы architecture/algorithms,
предыдущий [отчёт 04.1](asc-xyz-import-check.md), parser/importer, cache_types.h/.cpp,
test sources/CMake. Для Qt 6.12.0 проверены установленные qstring.h,
qarraydatapointer.h, документация QIODevice/QString; для MSVC 14.51.36231 — variant.
Другие подсистемы повторно не проектировались.

## B. Verification of Findings

### R1: CONFIRMED — FIX REQUIRED

Первоначальная цепочка обоих публичных handlers:

```text
catch bad_alloc → error(const char*) → QString::fromLatin1
    → makeError → detail.left(256) → Error → Result<T> → caller
```

Непустой fromLatin1 создаёт динамический UTF-16 payload. При нехватке памяти
повторное выделение в самом catch могло сорвать возврат ResourceExhausted.
Предварительный regression test после контролируемого bad_alloc в sink/Z-handler
получил `detail.capacity() == 25` и `isDetached() == true`: возвращённый Error
владел динамически выделенным diagnostic buffer. Это свидетельство механизма
повторного выделения, а не эксперимент с глобальным исчерпанием памяти.

Остальной путь проверен отдельно:

- makeError принимает QString по значению. Для коротких прежних сообщений
  left(256) возвращает всю строку, но фабрика не имеет специального OOM-контракта;
- QString copy/move и QArrayDataPointer в установленном Qt передают/разделяют
  указатели без allocation; QStringLiteral использует static UTF-16 payload;
- Error содержит значения и inline optional, source в fallback disengaged;
- Result(Error) перемещает Error в std::variant. У установленного MSVC альтернативы
  размещены внутри union, Error alternative конструируется напрямую без heap;
- Result<SourceSummary> и Result<ImportProduct> не конструируют успешную альтернативу
  при возврате ошибки. В тестах static_assert проверяют noexcept copy/move Error
  и construction обеих соответствующих variant из Error&&.

Нарушенный контракт — преобразование исключения выделения памяти в structured
ResourceExhausted. Локальное исправление возможно без изменения Result/Error API.

### R2: CONFIRMED — FIX REQUIRED

Документация установленного Qt 6.12 описывает atEnd через отсутствие доступных
сейчас данных; sequential devices не имеют общего понятия конечного размера/позиции.
QProcess относится к асинхронным устройствам. `read() == 0 && atEnd()` не является
универсальным признаком окончательного завершения такого потока.

Детерминированная модель выдала `1 2 0\n`, затем вернула zero read/atEnd=true
во время паузы; позднее к ней добавлена вторая запись. Исходный parser возвратил
success префикса, сделал два reads и один sink call. Regression ожидал отказ
неподдерживаемого источника до чтения и падал на исходном коде.

Нарушенный контракт — success только после достоверного завершения поддерживаемого
source. Обычный файловый importer использует QFile; он не был воспроизведением
этого дефекта. Выбран минимальный новый контракт parser: non-sequential binary
QIODevice с начальной позицией 0, например обычный QFile или QBuffer.
Sequential devices дают InvalidInput / SourceRead / Read с пояснением до чтения.
Нет event loop, ожидания данных, потоков или поддержки QProcess.

Первоначальный тестовый Source был конечным устройством с искусственно достоверным
atEnd, хотя объявлял себя sequential. Теперь это finite random-access file model
с size/seek и теми же controlled short reads/failures/zero-progress. Неподдерживаемая
sequential модель выделена в отдельный regression case. Проверки границ блоков,
EOF после последней записи и I/O failure сохранены.

Источники контрактов: локальные `C:/Qt/Docs/Qt-6.12.0/qtcore/qiodevice.html`
(atEnd/isSequential/read), `qstring.html` (QStringLiteral/left),
`C:/Qt/6.12.0/msvc2022_64/include/QtCore/qstring.h`, `qarraydatapointer.h`,
`VC/Tools/MSVC/14.51.36231/include/variant`. Веб-документация другой версии
в качестве доказательства не использовалась.

## C. Fixes

- `src/io/asc_xyz_parser.cpp`: catch напрямую возвращает aggregate Error с
  QStringLiteral и пустыми optional; sequential source отклоняется до read/buffers.
- `src/io/asc_xyz_importer.cpp`: такой же прямой fallback для собственного catch;
  прежние code/phase/operation/detail сохранены.
- `src/io/asc_xyz_parser.h`: уточнён публичный контракт поддерживаемых source.
- `tests/asc_xyz_parser_tests.cpp`: адаптирован finite source и добавлены два
  regression cases; `tests/asc_xyz_importer_tests.cpp`: ещё один case и type checks.
- architecture/algorithms и предыдущий отчёт уточняют ограничение QIODevice;
  README содержит ссылку на эту проверку.

В catch больше нет вызовов error(), fromLatin1() и makeError()/left(). Возврат
ограничен построением literal QString, aggregate Error и Error alternative Result.
Общая error/result infrastructure не менялась. Числовой parser, buffers, budgets,
Writer/Reader, сортировка/индекс/layout v1, application и GUI оставлены прежними.

## D. Regression Tests

| Test | Что проверяет |
|---|---|
| AscParserReviewRegression.BadAllocReturnsStaticDiagnosticThroughResult | bad_alloc отдельно из sink и Z-handler; ResourceExhausted с исходными phase/op и static diagnostic storage через Result<SourceSummary> |
| AscParserReviewRegression.RejectsSequentialSourceBeforeTemporaryEof | неподдерживаемый paused sequential source отклоняется; zero reads/deliveries, InvalidInput/SourceRead |
| AscImportReviewRegression.BadAllocPropagatesThroughImportResultAndCleansWriter | bad_alloc во время реального QFile → Parser → Writer сценария; static Error переживает копирование в Result<ImportProduct>, исходная phase сохранена, Writer scratch удалён |

До production-правок собраны и выполнены первые два теста:

```bat
cmake --build --preset windows-debug --target asc_xyz_tests --parallel 4
ctest --preset windows-debug -R ReviewRegression --output-on-failure
```

Результат до исправления: **0/2 PASS**, оба ожидаемо выявили первопричину
(динамический diagnostic buffer и успешный неполный поток). После исправлений
выполнен весь ASC/XYZ набор, включая все три regression cases (результаты ниже).

Граница R1-проверки: настоящий process-wide OOM, отказы Qt malloc/realloc и
невозможность выделения памяти при cleanup не имитировались. Importer test
проходит через parser catch и перенос Error в ImportProduct result; отдельный
outer catch импортера не принуждался к исполнению Qt allocation failure.
Его прямое построение Error/Result проверено анализом установленной инфраструктуры
и compile-time type checks общего пути. capacity/isDetached проверяют итоговое
storage, а не являются счётчиком всех временных allocations. Устранение
промежуточных выделений в двух fallback выражениях установлено разбором кода.

## E. Debug/Release

CMD-окружение пользователя: vcvars64.bat, UTF-8, Qt 6.12.0, MSVC 14.51.36231,
CMake/Ninja в локальном PATH, VCPKG_MAX_CONCURRENCY=4. Новых зависимостей нет.

Фактически выполнены:

```bat
cmake --build --preset windows-debug --target CADContour2D asc_xyz_tests cache_tests operation_tests project_tests view_model_tests qml_integration_tests dependency_smoke operation_probe all_qmllint --parallel 4
cmake --build --preset windows-release --target CADContour2D asc_xyz_tests cache_tests operation_tests project_tests view_model_tests qml_integration_tests dependency_smoke operation_probe all_qmllint --parallel 4
ctest --preset windows-debug -R ^AscXyz --output-on-failure
ctest --preset windows-release -R ^AscXyz --output-on-failure
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
ctest --preset windows-debug -L cache_integration --output-on-failure
ctest --preset windows-release -L cache_integration --output-on-failure
git diff --check
```

| Проверка | Debug | Release |
|---|---:|---:|
| Сборка затронутых компонентов и production targets | PASS | PASS |
| Новые regression tests (в составе `-R ^AscXyz`) | 3/3 PASS | 3/3 PASS |
| Все ASC/XYZ tests (`-R ^AscXyz`) | 42/42 PASS, 6.85 s | 42/42 PASS, 3.85 s |
| Cache regressions (в составе `-L production`) | 166/166 PASS | 166/166 PASS |
| Полная production CTest (`-L production`) | 340/340 PASS, 68.49 s | 340/340 PASS, 43.81 s |
| Отдельная cache integration (`-L cache_integration`) | 1/1 PASS, 84.93 s | 1/1 PASS, 14.15 s |
| QML lint (`all_qmllint` в build-командах) | PASS | PASS |
| QML integration (в составе `-L production`) | 13/13 PASS | 13/13 PASS |
| `git diff --check` | PASS | общий diff |

Новый C++ собран с /W4 без warnings. Test discovery существующего `cache_tests`
содержит 166 cases в обеих конфигурациях; все они выполнены в production suite.
Все counts указаны по фактическим завершённым запускам. Отдельный native GUI smoke,
большие benchmarks и повторный реальный QProcess для R2 не выполнялись.

## F. Invariants

Числовой conversion не изменён: std::from_chars(double), signed zero/subnormal,
37 finite golden pairs и 2 независимых range errors проверяются прежними tests.
Сохранены fixed read/line/batch buffers, строгая диагностика malformed records,
отдельные failure/cancellation outcomes, Z Continue/Cancel и ResourceLimits.
Сопоставление cache records делается по RecordId с bitwise X/Y; PendingCache
остаётся RAII owner, в том числе при controlled bad_alloc/Discard. Binary v1,
Morton128 и BBox hierarchy не менялись. EOF для non-sequential source по-прежнему
требует zero read и atEnd; temporary pause неподдерживаемого sequential source
больше не может стать успешным импортом.

## G. Remaining Limitations

Локальные fallback выражения не выделяют память в проверенной Qt/STL конфигурации.
Это не абсолютная гарантия восстановления процесса после произвольного OOM:
Qt/runtime могут завершиться assertion/abort вместо исключения, а cleanup и
остальная обработка ошибок Qt не имеют общей гарантии отсутствия allocations.
Публичные Result-конструкторы не объявлены noexcept; они не изменялись.

Parser сознательно не поддерживает asynchronous sequential source. Обычное чтение
файла остаётся блокирующим; cancellation не прерывает зависший OS read.
Прежние ограничения size/mtime detection, отсутствие real UNC/large benchmark
и будущая application/GUI integration сохраняются. К 04.2 работа не переходила.

## H. Final Verdict

**R1/R2 FIXED — READY FOR 04.2.** Оба замечания независимо подтверждены как
требующие исправления, необходимые локальные изменения внесены, regression
и полные production проверки прошли в Debug/Release. Ограничения проверки OOM
явно отделены от устранённых дефектов. Ни одно замечание не отклонялось.
04.2 не реализовывалась; вся задача 04 не объявляется завершённой или принятой.
