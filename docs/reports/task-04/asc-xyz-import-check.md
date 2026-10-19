# Проверка 04.1: headless ASC/XYZ → PendingCache

Первоначальные проверки относятся к HEAD `9af1a0c`. После независимого ревью
R1/R2 исправлены fallback при bad_alloc и контракт source: parser принимает
только non-sequential binary QIODevice с позицией 0. Результаты повторных
проверок и границы OOM-проверки — [asc-xyz-review-fixes.md](asc-xyz-review-fixes.md).

## A. Baseline

Дата: 2026-10-17. Ветка `main`, исходный HEAD
`8ff297300fe9ba69210d0ae82ba6e908f0f3e09f`; рабочее дерево до работы чистое.
HEAD совпал с указанным в задании. Задачи 01–03 сохраняют статус ACCEPTED.

Изучены AGENTS.md, README, implementation-plan, requirements §§4–6,
architecture (стек/границы/импорт/resources/operations/errors), algorithms §§2–5
и point-cache-format. Проверены фактические `cache_types`, `cache_file`,
`PointStreamSort`, `CacheWriter`, `PendingCache`, `CacheReader`, result/cancel API,
существующие tests/test hooks и CMake presets.

Исходные ограничения: parser и importer отсутствовали; cache уже реализовывал
bounded ingest/sort/index/publication, а PendingCache требовал сохранения cleanup
owner до Accept. GUI задач 01–02 создаёт пустой ProjectState — этот принятый каркас
в 04.1 не изменяется. Новая пользовательская модель интегрируется позднее.

## B. Implementation

Добавлены:

- `src/io/asc_xyz_parser.h/.cpp` — синхронный parser borrowed non-sequential binary `QIODevice` → bounded sink;
- `src/io/asc_xyz_importer.h/.cpp` — файловый headless orchestrator;
- `tests/asc_xyz_parser_tests.cpp`, `tests/asc_xyz_importer_tests.cpp`;
- `tests/fixtures/import/binary64_golden.h`, `generate_binary64_golden.py`;
- этот отчёт.

Изменены CMakeLists.txt и tests/CMakeLists.txt (`cadcontour_io`, `asc_xyz_tests`),
cache_types.h (общий Measurement и source diagnostics), point_stream_sort.h
(Measurement перенесён без смены namespace/типа), README, requirements,
architecture, algorithms и implementation-plan.

Parser не зависит от CacheWriter: он использует общие Measurement/Result/Error,
QIODevice и callback sink. Importer соединяет его с существующим Writer:

```cpp
cache::Result<ImportProduct> io::importAscXyz(
    const QString &sourcePath, const QString &finalCachePath,
    const QString &temporaryParent, cache::ResourceLimits limits,
    const io::ZHandler &zHandler,
    io::CancelCheck cancel = nullptr, void *context = nullptr);
```

`ImportProduct` содержит move-only PendingCache и SourceSummary: прочитанные bytes,
физические строки, measurements и optional `{line, value}` первого ненулевого Z.
На ошибке product отсутствует. Source path известен caller и не дублируется в cache.

QFile выбран как достаточный штатный Unicode/UNC file API: binary/unbuffered,
фиксированный read buffer, short reads, отдельные EOF/read failure, RAII handle.
Win32 adapter и новая файловая подсистема не понадобились. Size/mtime до/после
разбора и число реально прочитанных bytes выявляют доступные признаки изменения
source; абсолютная неизменность не обещается.

Z-handler синхронно возвращает `Continue` или `Cancel`. Он вызывается один раз
после проверки всей записи и до следующего read/sink call, включая случай,
когда текущая запись заполняет batch. Continue не перечитывает файл. Cancel даёт
`ErrorCode::Cancelled`, а не parse failure. При ненулевом Z без handler — InvalidInput;
неверное enum-решение — InvalidState, исключение callback — ConsumerFailed,
bad_alloc — ResourceExhausted. `+0`/`-0` не вызывают запрос, subnormal вызывает.

## C. Numerical Correctness

ASCII grammar:

```text
[+-]?(digits[.digits?]?|.digits)([eE][+-]?digits)?
```

Только точка, decimal e/E, обязательные digits в mantissa/exponent.
NaN/Inf, hex, D exponent, суффиксы, запятая и посторонние символы запрещены.
Грамматика проверяется до `std::from_chars(..., double&, chars_format::general)`.
Ведущий `+` удаляется из view; отрицательный знак не меняется. Проверяются ec,
полное потребление token, конечность и запрет nonzero mantissa → zero.
Выход from_chars при ошибке не используется. Нет float, fixed decimal rounding
или арифметического масштабирования координат.

Набор **37** finite golden pairs вычислен отдельным checked-in Python script
через exact Fraction/integer quotient и nearest/ties-to-even. Script не использует
float, strtod, from_chars или production parser. Он определяет binary exponent
целочисленным сравнением, округляет точный quotient и кодирует IEEE поля.
Дополнительно сохранены **2** независимых range-error input: точная половина
минимального subnormal и overflow midpoint. Python нужен только для регенерации.

Эталоны охватывают 6/9/12/15 знаков после точки, разные масштабы/знаки, signed zero,
точный ноль с огромным ±exponent, соседние binary64, оба направления midpoint tie,
minimum/maximum normal, subnormal и normal/subnormal midpoint. Отдельно проверены
overflow и nonzero underflow-to-zero. Все X/Y/Z сравниваются через uint64 bits;
X/Y после cache проверяются по RecordId для обоих расширений, включая повторные
измерения и signed zero. Golden bitwise comparisons прошли в Debug и Release.
В тесте задана русская QLocale; locale-sensitive conversion/classification в коде нет.

## D. Streaming and Resources

| Объект | Ограничение |
|---|---:|
| Read buffer | 65 536 bytes |
| Line buffer | 65 536 bytes |
| Batch | 4096 × 24 = 98 304 bytes |
| Все массивы parser | 229 376 bytes (224 KiB) |
| Parser/source reserve | 294 912 bytes (288 KiB), включая 64 KiB control/stack/Qt allowance |
| Source path allowance | checked `16 × sourcePath.size()` bytes дополнительно |
| Source handles | 1 |

Это budget рабочей памяти, не обещание точного process Working Set: allocator,
DLL/runtime и OS cache не относятся к массивам parser. Borrowed source/sink/handler
state generic parser принадлежит caller. Production importer использует unbuffered
QFile и bounded Writer. Массивы освобождаются после parsing, до finish.

Importer вычитает reserve и source handle из caller-selected ResourceLimits,
остальные memory/disk/files передаёт существующему Writer. Disk budget не меняется:
source не принадлежит операции. Checked sizes/counts/offsets и bad_alloc дают
structured failure. Тестовый профиль 8 MiB / 64 MiB temporary disk / 6 handles
не является универсальной настройкой.

Пустые строки пропускаются с сохранением физической нумерации. Непустая строка
требует ровно три token. Maximum line исключает LF/CRLF и начальный BOM; 65 537-й
byte немедленно отклоняется без увеличения buffer. Поддержаны SP/TAB/VT/FF,
LF/CRLF/смешанные окончания, финальная строка без LF и initial UTF-8 BOM.
Одиночный CR, NUL, UTF-16 и unsupported bytes дают failure. Форматная ошибка
содержит line/byte column и coordinate, когда она относится к конкретному token.
Fragment ограничен 64 source bytes (после escaping до 256 characters), detail —
256 UTF-16 code units. ErrorCode различает ParseError, NumericRange, ReadFailed,
WriteFailed, SourceChanged, InsufficientData, ResourceExhausted, Cancelled,
InvalidState и существующие cache/publication failures; phase/op/deviceCode/offset
и исходная причина cache failure сохраняются.

Checkpoints: до/после reads, каждые 1024 bytes сканирования, после записей,
до/после передачи batch, вокруг Z-handler, перед/после finish и все существующие
Writer/Sorter checkpoints. Обработка одного token ограничена 64 KiB. В тесте
импорт сначала подсчитывает полный набор checkpoint calls успешного сценария,
затем отменяется на каждом из них, включая before-finish, Writer и after-finish.

Блокирующий OS I/O нельзя прервать callback: задержка отмены включает время read.
Same-size in-place rewrite с сохранённым/необновлённым mtime может не обнаружиться.
Реальный UNC share и зависание сетевого I/O в этой работе не проверялись.

## E. Cache Integration

Использованы без изменения API `CacheWriter::create()`, `append()` и `finish()`;
также прежние Result/Status, ResourceLimits, cancellation, Writer/Sorter hooks,
PendingCache и `CacheReader::open/scanAll/validate`.

Writer назначает RecordId по порядку непустых measurements, сохраняет X/Y bits,
делает Morton128 sort, packed point pages и BBox hierarchy, вычисляет count/bounds/Z
и CacheId. Persistent metadata остаётся прежней: источник текста/строки/первый Z
в binary v1 не добавлены. Parser metadata возвращается отдельно в SourceSummary.

После finish файлом владеет PendingCache; importer не вызывает releaseToOwner.
Reader работает при живом guard и закрывается до его уничтожения. Тесты
проверяют удаление завершённого cache при отказе от product, cleanup после
ошибки чтения уже переданного префикса, append/finish faults, cancellation и
source changes. Существующий target не заменяется и не удаляется. Cleanup failure
не заменяет первую причину; прежнее ограничение RAII при запрете удаления ОС
сохраняется (отдельного scavenger нет).

## F. Tests

Команды выполнялись в CMD после vcvars64.bat, с Qt 6.12.0/MSVC 14.51.36231,
UTF-8 code page, локальным PATH CMake/Ninja/Qt и VCPKG_MAX_CONCURRENCY=4,
как указано пользователем. Глобальное окружение не менялось. Обе сборки автоматически
повторно выполнили CMake configure существующих presets (tools=ON сохранено).
Зависимости уже установлены; новых нет. Прежние warnings о необязательных
Qt6TaskTree/WrapVulkanHeaders сохранились; новый C++ собран с /W4 без warnings.

Фактически выполненные build-команды:

```bat
cmake --build --preset windows-debug --target CADContour2D asc_xyz_tests cache_tests operation_tests project_tests view_model_tests qml_integration_tests dependency_smoke operation_probe all_qmllint --parallel 4
cmake --build --preset windows-release --target CADContour2D asc_xyz_tests cache_tests operation_tests project_tests view_model_tests qml_integration_tests dependency_smoke operation_probe all_qmllint --parallel 4
```

| Проверка | Фактическая команда | Debug | Release |
|---|---|---:|---:|
| Сборка и QML lint | build-команды выше, включая `all_qmllint` | PASS | PASS |
| Новые parser + importer tests | `ctest --preset windows-debug -R ^AscXyz --output-on-failure`; `ctest --preset windows-release -R ^AscXyz --output-on-failure` | 39/39 PASS | 39/39 PASS |
| Полная production suite | `ctest --preset windows-debug -L production --output-on-failure`; `ctest --preset windows-release -L production --output-on-failure` | 337/337 PASS, 67.50 s | 337/337 PASS, 43.70 s |
| Cache regression | Те же production-команды, все cases executable `cache_tests` | 166/166 PASS | 166/166 PASS |
| Production QML integration | Те же production-команды, QmlIntegration/QmlRegistration | 13/13 PASS | 13/13 PASS |
| Отдельная существующая cache integration | `ctest --preset windows-debug -L cache_integration --output-on-failure`; `ctest --preset windows-release -L cache_integration --output-on-failure` | 1/1 PASS, 86.24 s | 1/1 PASS, 13.95 s |
| Golden regeneration | `python tests/fixtures/import/generate_binary64_golden.py` | PASS; одинаковый checked-in результат | общий fixture |
| Diff | `git diff --check`; после staging `git diff --cached --check` | PASS | общий diff |

Новые tests входят в production label. В их составе 32 parser cases (включая 20 parameterized malformed
inputs и parser → real Writer fault path), 7 importer/integration cases.
Fault injection использует локальный QIODevice для short/failed reads и существующие
Writer/Sorter hooks для write failure; system disk/credentials не меняются.

## G. Review of Invariants

| Инвариант | Результат и основание |
|---|---|
| 1. Память parser независима от числа точек | Да: три fixed arrays, одна незавершённая строка, постоянные counters; тест многих blocks/lines |
| 2. Неверные записи не пропускаются | Да: 20 malformed cases, unsupported bytes, line limit и empty-source failures |
| 3. X/Y напрямую в binary64 | Да: from_chars(double), grammar/full-consumption/range checks и independent golden bits |
| 4. Writer сохраняет ID и X/Y | Да: реальные ASC/XYZ round-trips по RecordId; повторные measurements остаются отдельными |
| 5. Единственный Z request | Да: fully validated record, first/middle/last/subnormal/Continue/Cancel tests |
| 6. Failure/Cancel без success | Да: Result alternative, fault injection, cancellation sweep и cleanup assertions |
| 7. RAII PendingCache | Да: releaseToOwner не используется в importer/tests; Reader живёт внутри guard lifetime |
| 8. Point Cache v1 неизменён | Да: codec/layout/sort/index/file-publication реализации не менялись; cache regression/validation |
| 9. Application/GUI 01–02 сохранены | Да: их исходники не менялись; production application/operation/QML tests и lint прошли |

## H. Remaining Work

04.2: typed WorkerOutcome/application adapter для ImportProduct, snapshot и
применимость запроса создания/замены без фиктивного ProjectIdentity, подготовка
нового ProjectState и атомарное принятие через gate, проверка межпоточного lifecycle
PendingCache в Ready/Accept/Discard. Существующий headless API предоставляет
product, diagnostics, Z-handler и общий cancellation callback; перенос между
потоками и releaseToOwner пока не реализованы и не проверены.

В дальнейших частях 04 нужны настоящее межпоточное подтверждение Z, FileDialog/GUI,
создание/замена проекта после успеха с cell=1 мм, сохранение прежнего состояния
до успеха, точный progress внутренних cache phases и large-file/end-to-end проверки.
Progress observer не добавлялся, Writer/Sorter ради него не перестраивались.
Save/Open/Recovery относятся к 05; Density Map к 06. Representative real scans
и подтверждение rough/precise методов к 04.1 не относятся.

## I. Final Verdict

**READY FOR 04.2.** Реализация 04.1 завершена, обязательные Debug/Release проверки
прошли. Ограничения blocking I/O/source-change detection и будущей application
integration явно перечислены выше. Нового визуального GUI smoke, реального UNC,
42-миллионного ASC import, HDD/SSD comparison и benchmark regression tools в этой
части не выполнялось; cache integration — существующий correctness test, не новое
исследование производительности. Вся задача 04 не объявляется завершённой или принятой.
