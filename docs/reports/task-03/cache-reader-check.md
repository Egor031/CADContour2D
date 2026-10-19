# CacheReader — реализация 4/5 задачи 03

Исторический отчёт этапа 4/5. Последующая нагрузочная приёмка и усиленный контракт
каноничности полной validation — в [cache-performance-report.md](cache-performance-report.md).

## A. Baseline

Проверка 17 октября 2026 г., Windows x64. Ветка `main`, исходный HEAD
`6b5493612421c83207ebc3461242083d24e2a002` (`implement cache writer and packed spatial index`).
Tracked/staged/untracked изменений перед работой не было. Изучены README,
AGENTS, задача 03 implementation plan, point-cache-format, architecture §§12–14,
численные правила algorithms, требования к исходным measurements, cache codecs,
file primitives, sorter, Writer и связанные tests/CMake.

## B. Implementation

- `src/cache/cache_reader.h/.cpp`: open, metadata/index/layout, scanAll, scanBox,
  scanBoxes и полная validate; Result/Status/Error и прежние callback conventions.
- `src/cache/cache_types.h`: только недостающие phases CacheRead/Validation.
- CMake подключает Reader к `cadcontour_cache`; tests/cache_reader_tests.cpp
  подключён к существующему cache_tests. Отдельный CTest label cache_integration
  отделяет большой end-to-end тест от обычного `-L production`.

Формат v1, Writer и Morton sorting не изменялись. Алгоритм packed DFS, доказательство
coverage/уникальности ID и точные budgets описаны в
[point-cache-format.md](../../point-cache-format.md#cachereader-и-полная-validation-45).
Ни GUI, ни project/application state не затронуты.

## C. Correctness

Round-trip настоящего Writer после его уничтожения: N=0, 1, 271, 2726, 2727, 12000.
Проверены metadata, каждый исходный ID и биты X/Y: ±0, subnormal, adjacent binary64,
отрицательные/предельные конечные значения, совпадающие measurements, Z flag.
Тесты проверяют, что open читает только envelope/root, без point pages.

Независимый oracle использует исходные measurements и прямые inclusive comparisons.
Проверены scanAll, scanBox, union, repeated/overlapping/disjoint rectangles,
нулевые ширина/высота, точечные области, пустой union/result, границы страниц,
разные масштабы; пространственная выдача — подпоследовательность physical scanAll.

Реальный Writer создал N=2 786 009, 1023 point pages, 3 index pages, height=2;
последняя point page имеет 37 records. Bounds/counts children различаются.
Вход чередует горизонтальные/вертикальные проходы:
`j=id/2`, чётные `(j%2048, j/2048)`, нечётные `(j/2048, j%2048)`.
Проверены исходные ID/биты, spatial и последовательный отбор, полный scan и полная
validation. Для heights 3/4 есть ограниченные проверки packed shape и hostile
height; end-to-end файлов такой высоты не создавалось.

## D. Corruption & Faults

28 отдельных cases проверяют checksum обоих subblocks/footer/root/point page,
ordinals, kind/level/count, references/root, subtree counts, bounds и identity.
Семантические мутации пересчитывают SHA-256. Дополнительно 8 повреждений non-root
index page проверены на настоящем height=2 файле. Ошибка не скрывается full-scan
fallback. Невостребованная повреждённая point page не мешает unrelated query;
полная validation её отклоняет. Ложные bounds, согласованные между header/root,
отклоняются при сравнении с фактическими point pages.

Проверены дубликаты и пропуски ID, в том числе через границы point pages/runs.
Мутация `0,1,2,3,4,5 -> 0,0,3,3,4,5` сохраняет sum/XOR/min/max и всё равно
отклоняется строгой validation. ID >= N также отклоняется.

Реально выполнены missing-file/open failure, усечения header/page/footer, лишние
байты, truncation другим handle после выданного префикса. Детерминированные hooks
стоят непосредственно перед production read/seek; проверены их ошибки и bad_alloc.
Проверены Status/exception потребителя, отмена до выдачи/после prefix/в validation
и ID sort, reentry и concurrent rejection, повторные запросы после отказов.
Исходные байты после callback/cancellation failures сверены; dataset не приобретается
во владение Reader. Реальные device seek/read errors и физические disk-full/OOM
не создавались; fault injection не объявляется проверкой таких условий ОС.

## E. Resource Model

Аналитически Reader хранит одну encoded page, 2726 PointRecord и один Frame на
проверенный уровень. Нет global ID set, полного index или результата в RAM.
Validation использует отдельный batch искусственных measurements и прежний sorter;
память/временный диск/handles ограничены caller ResourceLimits. После любой ошибки
следующий запрос допустим с повторной проверкой читаемых страниц.

Фактически проверены наличие/размеры выделенных рабочих массивов и постоянство
их размера во время batches. Height=2: 245 440 байт массивов, 2 frames; сравнение
с малыми height=1 datasets подтверждает зависимость от высоты, а не полный массив N.
Это payload allocations, без allocator overhead/Qt/runtime/OS file cache;
peak process RSS на этом этапе не измерялся.

Windows GetProcessHandleCount показывает +1 handle на Reader и возврат к baseline
после destruction. Validation после завершения не оставляет дополнительных handles
и scratch directories. Проверены слишком малые RAM/disk/handle budgets, disk budget
failure после начала операции, cancellation/ошибка с очисткой и повторный успех.
ID sort наследует production regression проверок generations, disk limits,
I/O failure и cleanup протокола PointStreamSort.

В integration caller budgets: Writer 4 MiB RAM / 512 MiB temporary disk / 4 handles,
Reader 1 MiB / 0 scratch / 1 handle, validation **всего** 2 MiB / 512 MiB / 3 handles.
Это лимиты рабочих ресурсов, а не измеренный peak RSS.

## F. Early Performance

Один локальный запуск каждой конфигурации через QTemporaryDir, HDD `F:` не
использовался. Размер dataset — **67 248 128 байт**; point section — 67 043 328 байт.
OS cache не сбрасывался: после записи/readback и предыдущих queries условия
warm/partially warm, гарантированного cold-cache нет. Результаты synthetic,
без performance нормативов и без изоляции от обычной нагрузки Windows.

| Запрос | Index pages | Point pages | Read bytes | readExact/seek calls | Returned | Release ms | Debug ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| `[10,15] × [10,15]` | 2 | 2 | 262 144 | 4 / 4 | 72 | 2.833 | 31.541 |
| `x=100, y∈[0,2047]` | 2 | 32 | 2 228 224 | 34 / 34 | 2729 | 26.274 | 172.517 |
| `[10,30]² ∪ [20,40]² ∪ [10,30]²` | 2 | 7 | 589 824 | 9 / 9 | 1522 | 6.874 | 44.129 |
| scanAll | 0 | 1023 | 67 043 328 | 1023 / 1023 | 2 786 009 | 920.817 | 6594.164 |

Последовательный scanAll с независимым отбором для первых трёх запросов каждый раз
читает 1023 point pages / 67 043 328 байт / 1023 readExact и seek calls. Выбранные
72/2729/1522 records совпали с spatial query и исходным oracle. Время по запросам:
Release 801.210/804.173/805.837 ms; Debug 5043.644/4969.582/5162.777 ms.
Таймеры включают callback/test oracle bookkeeping; scanAll без отбора дополнительно
проверяет биты всех records. Это ранние измерения приложения, не чистая storage
microbenchmark и не сопоставление физического HDD seek time.

Главный подтверждённый результат — сокращение point-page I/O с 1023 до 2/32/7.
Header/footer и предшествующий open не включены в query counters. Во всех сценариях
массивы Reader занимают 245 440 байт; handle один, frames два.

## G. Tests and Regression

Использована указанная пользователем CMD-инициализация: vcvars64 Visual Studio 18
Community, Qt 6.12.0 msvc2022_64, bundled CMake/Ninja, VCPKG_MAX_CONCURRENCY=4.
Фактически выполнены `cmake --preset windows-debug` и `windows-release`, а затем
для каждой конфигурации:

```bat
cmake --build --preset windows-debug --target cadcontour_cache cache_tests project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
cmake --build --preset windows-release --target cadcontour_cache cache_tests project_tests operation_tests view_model_tests qml_integration_tests dependency_smoke --parallel 4
ctest --preset windows-debug -L production --output-on-failure
ctest --preset windows-release -L production --output-on-failure
ctest --preset windows-debug -L cache_integration -V
ctest --preset windows-release -L cache_integration -V
```

Production: **292/292 PASS в Debug и Release**, включая 160 cache tests (116 прежних
и 44 новых). Отдельный integration: **1/1 PASS** в обеих конфигурациях, Debug
82.51 s, Release 13.12 s. Новый C++/tests собраны с `/W4` без предупреждений;
промежуточное предупреждение о затенении устранено. Configure сохранил известные
необязательные Qt6TaskTree/WrapVulkanHeaders diagnostics. `git diff --check` чистый.
После усиления assertions open/error codes и сохранности исходных байтов повторно
выполнен `ctest --preset windows-debug -L production -R Reader --output-on-failure`
и та же команда для windows-release: **44/44 PASS** в обеих конфигурациях.
GUI smoke и Synthetic Point Cloud Generator regression повторно не запускались:
их код не затронут; production QML/application tests выполнены.

## H. Documentation and Git

Обновлены README, point-cache-format, implementation-plan и краткий статус/ownership
в architecture. Algorithms/requirements не менялись. Новый публичный API относится
только к cache module; layout и прежние внешние контракты сохранены. Работа ведётся
в исходной main; итоговый commit и чистота working tree сообщаются в отчёте чата.
Push не выполняется.

## I. Verdict

**READY FOR REVIEW / NEXT STAGE** для реализации 4/5. Вся задача 03 остаётся
IN PROGRESS; формат v1 не заморожен. На 5/5 передаются гигабайтные datasets,
peak RSS/долгие I/O, HDD/cold-warm matrix, сравнение index вариантов и общая
нагрузочная приёмка. End-to-end heights >=3 пока не измерены. Power-loss durability,
физический OOM/disk-full, real scans и application lifecycle не подтверждались.
