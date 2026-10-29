# 06.1 — Raster Foundation

**06.1 — ACCEPTED WITH LIMITATIONS** по решению пользователя от 2026-10-29.
Независимое ревью контрольного commit `1db741b7fcce215080e1eb58cc139f6bdba154e0`
завершено с вердиктом **READY WITH LIMITATIONS**; основания и остаточные ограничения —
в [разделе H](#h-independent-review-and-acceptance). 06.2–06.8 не начаты.
Разделы A–G сохраняют исторический отчёт исполнителя от 2026-10-29,
его первоначальный статус и исходные результаты проверок.

## A. Baseline

- Ветка `main`, HEAD `3b1e036db468109a31475cbbb88bf268ecccc4c5`.
- Исходный `git status --short` пуст; пользовательских изменений не было.
- Прочитаны AGENTS, README, 06.1 implementation plan, относящиеся requirements,
  architecture и algorithms, координаты, cache types, tests/CMake и evidence 06.0.
- Windows 10 Pro 10.0.19045 x64, Intel Core i5-7500 @ 3.40 GHz;
  MSVC 19.51.36260 / toolset 14.51.36231, Windows SDK 10.0.26100.0,
  CMake 3.30.5, Ninja 1.12.1, Qt 6.12.0, существующий GoogleTest 1.17.0.
- В начальном PowerShell PATH отсутствовали CMake/Ninja/MSVC. Использован
  существующий локальный `build/task02-env.cmd`: штатный vcvars64, UTF-8 console,
  process-local Qt/tool paths. Глобальное окружение не менялось, установки не нужны.

## B. Реализация

`src/core/raster_mapping.h/.cpp` добавлены в `cadcontour_core`. Это минимальный
самостоятельный value API без Qt/GUI/OpenCV, storage, cache reader и project state.
Существующий `ModelPoint` используется для minimum/maximum, как в `ImportedDataset`;
переносить Qt-зависимый `cache::BoundingBox` в core или вводить несовместимую
систему координат не потребовалось. Point Cache и parser не изменены.

Публичный `raster::Mapping`:

- `create(minimum, maximum, cell, ValidationPolicy, CancelCheck, context)`;
- исходные minimum/maximum, min-origin, неизменный nominal cell, width/height,
  идентификатор `Binary64SeparateRoundNearestEven`;
- `locate(ModelPoint)` → `CellIndex`;
- `cellBounds(CellIndex)` → nominal/coverage intervals по X/Y, с включённостью концов;
- `Result<T>` содержит значение **либо** structured `Error` (code, axis,
  boundary index, required work для preflight refusal). Категории:
  InvalidInput / Numerical / ResourceRefused / Cancelled.

Factory — единственный путь создания; default/частично валидного состояния нет,
изменяющих методов нет. Mapping занимает 56 bytes в проверенном x64 toolchain.

Для каждой оси `B(0)=origin`,
`B(i)=RN(origin+RN(double(i)*cell))`, `i>0`. Каждый integer index до `2^53`
представим точно. При невырожденной оси минимальный N ищется двоично на `[1,2^53]`,
с максимум 53 итерациями на ось; overflow probes — только верхние sentinels.
Конечность terminal boundary проверяется, все соседние пары затем проверяются
полным проходом. При вырождении N=1 с обязательной проверкой B(1)>B(0).
Span subtraction, division, ceil и full-area product не нужны.

Принадлежность определяется сравнением с B(i), без floor/clamp/epsilon:
internal boundary → right/up; exact original maximum → final cell;
outside original bounds → error. Nominal final interval закрыт справа,
coverage заканчивается в исходном maximum, degenerate coverage — closed singleton.
Центр ячейки и точный round-trip через него не обещаются.

## C. Numerical verification

Во всех строках PASS в Debug и Release. Всего 14 GoogleTest cases; один case
перебирает 14 независимых golden fixtures и все соседние representable точки
до/на/после их границ, включая выход за исходные bounds.

| Сценарий | Проверенное свойство | Результат |
|---|---|---|
| Positive/negative, cross-zero, cell 1 / 0.1 / 0.01 / 0.001 / 0.125 / 0.000001 | Dimensions, точные bits, indices | PASS |
| 1234.567890, ±9999.999999 | Реальные binary64 значения шестизначных координат | PASS |
| Exact minimum/maximum, nextafter до/на/после | Минимальный N, однозначный владелец, outside refusal | PASS |
| Partial final edges, разные X/Y indices | Nominal bounds отличаются от coverage | PASS |
| Degenerate X, Y, both; ±0 | Одна cell, closed singleton, одинаковое геометрическое поведение signed zero | PASS |
| Subnormal и около 1e300 | Представимые границы и безопасные результаты | PASS |
| Max double, overflow probes, required product overflow | Finite final boundary либо structured Numerical | PASS |
| Начальный и более поздний collapse | Отказ с axis/boundary index | PASS |
| N=2^53, N>2^53, area>uint64, invalid indices | Exact index conversion; без area product; безопасный отказ | PASS |
| Reversed/nonfinite bounds, NaN/∞, zero/negative/nonfinite cell, invalid points | Structured InvalidInput | PASS |
| Budget 0, exact budget, insufficient budget | Refusal до линейного обхода; required checks | PASS |
| Cancel before/after preflight, during X/Y, before success | Ни одного частичного mapping | PASS |
| RN modes, FTZ, DAZ, все 6 exception masks, callback меняет environment | Structured UnsupportedEnvironment | PASS |
| terminal `origin=0, cell=binary64(0.1), M=0x3fd3333333333334` | N=3; M отличается от decimal 0.3 (`0x3fd3333333333333`) | PASS |
| FMA-sensitive `origin=-1, cell=0.1, i=10` | B(10)=+0; fused результат = 2^-54 | PASS |

Oracle: `tests/fixtures/raster/generate_boundaries.py` использует точные `Fraction`
и собственное **целочисленное** RN/ties-to-even, включая subnormal, carry и ties;
host float arithmetic и production helpers не вызываются. Committed golden bit
patterns находятся в `raster_boundaries.inc`; Python не нужен при запуске CTest.
Ожидаемая принадлежность проверяется отдельным линейным поиском по golden intervals,
затем перебором всех физических cell coverage проверяется ровно один владелец.
Это дополняет, а не заменяет независимые bits/minimality проверки.

### Floating-point checks

Только mapping TU получает `/fp:strict`; существующие `project_state.cpp` и
`coordinates.cpp` сохранили `/fp:precise`. Нет конфликтующих `/fp` options и D9025.
Дополнительно в mapping включены `fenv_access(on)` и `fp_contract(off)`.
Использована [официальная семантика MSVC /fp](https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior?view=msvc-170),
подтверждённая проверками **фактического** компилятора 19.51:

- Проверены Ninja compile commands: Debug `/Od /RTC1 /fp:strict`,
  Release `/O2 /Ob2 /fp:strict`.
- `dumpbin /disasm` production Release object: раздельные `mulsd`, `addsd`.
- Дополнительная компиляция mapping `/O2 /arch:AVX2 /fp:strict /FAs`:
  раздельные `vmulsd`/`vaddsd`, **0 FMA instructions**; все 14 tests PASS
  при линковке именно с этим объектом.
- На каждом arithmetic entry проверяется MXCSR: все exceptions masked,
  RN-even, FTZ/DAZ off, и `fegetround()==FE_TONEAREST`. Режим вызывающего потока
  не подменяется; sticky status bits могут меняться от арифметики.
  После callback environment перепроверяется. Неподдерживаемый режим отклоняется
  до арифметики с входными числами.

## D. Build/tests

Ниже команды запускались из корня через существующий env wrapper. Приведены
основные завершающие команды; stdout/logs и JUnit локально в `build/raster061-*`.

```bat
cmd /d /c build\task02-env.cmd cmake --preset windows-debug
cmd /d /c build\task02-env.cmd cmake --build --preset windows-debug --target raster_mapping_tests --parallel 4
cmd /d /c build\task02-env.cmd cmake --build --preset windows-release --target raster_mapping_tests raster_validation_benchmark --parallel 4
cmd /d /c build\task02-env.cmd ctest --preset windows-debug -R RasterMapping --output-on-failure
cmd /d /c build\task02-env.cmd ctest --preset windows-release -R RasterMapping --output-on-failure
```

Release configure выполнен автоматически при первом build после изменения CMake.
Debug benchmark и связанные targets также собраны в обеих конфигурациях:

```bat
cmd /d /c build\task02-env.cmd cmake --build --preset windows-debug --target raster_validation_benchmark project_tests cache_tests persistence_tests asc_xyz_tests operation_tests import_application_tests nonzero_z_confirmation_tests save_tests open_tests recovery_tests --parallel 4
cmd /d /c build\task02-env.cmd cmake --build --preset windows-release --target project_tests cache_tests persistence_tests asc_xyz_tests operation_tests import_application_tests nonzero_z_confirmation_tests save_tests open_tests recovery_tests --parallel 4
```

CTest inventory получен `ctest --preset windows-release --show-only=json-v1`;
в локальный `build/raster061-headless-tests.txt` выбраны имена тестов этих
executables, `dependency_smoke` и `raster_mapping_tests`. Пересечение с label
production даёт 580 tests, включая 32 project_tests (из них 8 Coordinates) и 14 RasterMapping;
большой `cache_reader_integration` имеет другой label и не включён.

```bat
cmd /d /c build\task02-env.cmd ctest --preset windows-debug -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/raster061-headless-tests.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/raster061-debug-regression.xml
cmd /d /c build\task02-env.cmd ctest --preset windows-release -L production --tests-from-file D:/ZherlitsynEE/CADContour2D/build/raster061-headless-tests.txt --output-on-failure --output-junit D:/ZherlitsynEE/CADContour2D/build/raster061-release-regression.xml
```

| Проверка | Debug | Release |
|---|---|---|
| Mapping/core + связанные headless targets build | PASS | PASS |
| Отдельный RasterMapping CTest | 14 PASS | 14 PASS |
| Coordinates + ProjectState внутри production selection | 32 PASS | 32 PASS |
| Headless production selection, 580 cases | 578 PASS / 1 FAIL / 1 SKIP | 578 PASS / 1 FAIL / 1 SKIP |
| Изолированные повторы упавшего Recovery case | 10/10 PASS | 10/10 PASS |
| FMA-capable `/O2 /arch:AVX2` mapping object + tests | — | 14 PASS |

Оба FAIL произошли в неизменённом helper `tests/recovery_tests.cpp:46`:
`autosave().outcome == NoChange`, тогда как assertion ожидает `Applied`.
Debug: `RecoveryApplication.Recovery.PeriodicTimerSavesChangedAppliedState`.
Release: `RecoveryApplication.SchemaAndPaths/RecoveryInvalid.InvalidRecordsAreReportedWithoutMutation/7`.
Этот тип нестабильности уже воспроизведён независимым probe на старых commits
и [зафиксирован в 05.4](../task-05/autosave-recovery.md#неблокирующее-наблюдение-о-надёжности-тестов).
Raster Mapping не используется этими сценариями, Autosave/test sources не менялись.
Изолированные `ctest -R <case> --repeat until-fail:10` прошли в обеих конфигурациях;
исходные FAIL сохранены, устранение нестабильности не заявлено.

SKIP: `Persistence.StoragePath.FileSymlinkEscape`, Windows privilege 1314.
Compiler warnings нового кода и изменённых targets: **0**. При configure есть
предупреждения установленного Qt об отсутствии optional `Qt6TaskTree` /
`Qt6QmlAssetDownloaderPrivate`, и сообщение об отсутствии WrapVulkanHeaders;
configure/build успешны, для headless mapping эти пакеты не нужны.
Первый Debug build benchmark встретил stale Ninja configuration (CMake изменялся
во время первой конфигурации); явный configure устранил unknown target.
Промежуточный ручной AVX2 link потребовал `/SUBSYSTEM:CONSOLE`; завершающий link/tests PASS.

GUI acceptance, полный production suite с GUI, HDD/06.0 experiments, реальное
исчерпание RAM и representative real scans — **NOT RUN**, вне 06.1.

## E. Resources

`ValidationPolicy.maxBoundaryChecks` обязателен; ноль не unlimited.
Начальная политика **10 000 000** выбрана по стоимости ниже, не заимствована
из experimental guard и не задаёт предел RAM/размеров storage. До linear scan
рассчитывается exact width+height <= 2^54 (static proof отсутствия uint64 overflow).
Превышение возвращает ResourceRefused с required checks. Полная area не вычисляется.

Измерялся существующий production API, пять повторов / median, после warm-up:

```powershell
& ./build/windows-debug/raster_validation_benchmark.exe
& ./build/windows-release/raster_validation_benchmark.exe
```

Preflight — тот же create с нулевым бюджетом: обе dimensions + refusal без обхода.
Validation time включает и preflight, его отдельная стоимость приведена рядом.
Оси одинаковые; полный raster не создаётся.

| Проверяемых пар (width+height) | Debug ms | Release ms | Preflight Debug/Release ms |
|---:|---:|---:|---:|
| 2 000 | 0.0300 | 0.0090 | 0.0020 / 0.0006 |
| 200 000 | 2.8239 | 0.8118 | 0.0020 / 0.0008 |
| 2 000 000 | 30.2254 | 8.9410 | 0.0027 / 0.0011 |
| 10 000 000 | 152.7913 | 44.9264 | 0.0029 / 0.0011 |
| 50 000 000, явно повышенный budget | 723.7109 | 209.0016 | 0.0028 / 0.0011 |

Raw evidence с WorkingSet/PeakWorkingSet/PrivateUsage bytes:
[Debug](../../benchmarks/raster061/debug.txt), [Release](../../benchmarks/raster061/release.txt).
На 50 млн checks Release peak WS 3 645 440 bytes, PrivateUsage всё время 643 072;
Debug peak WS 4 247 552 bytes, после warm-up PrivateUsage 712 704.
O(1) memory следует из scalar-only реализации и отсутствия allocation/containers;
WS measurements согласуются с этим, но не являются heap-allocation profiler.

Refusal для `width=height=2^53`, required checks=18 014 398 509 481 984:
0.0025 ms Debug / 0.0008 ms Release, без linear work и area overflow.
Отмена на checkpoint после 4096 проверенных X-пар: total 0.0580 / 0.0181 ms,
после возврата callback до failure result 0.0003 / 0.0001 ms (Debug/Release).
Это deterministic cooperative probe; OS scheduling/external-thread request latency
не измерялись. Между checkpoints не больше 4096 пар; callback `noexcept`, его
время — ответственность caller. Универсальный wall-clock deadline не обещается.

## F. Documentation

- `algorithms.md` §6: точная формула, dimensions/minimality, membership,
  inverse coverage, representability, work refusal, environment/cancellation.
- `architecture.md` §§10–11: общий core mapping API и границы его ответственности;
  §45: актуальный указатель оставшихся Density decisions.
- `implementation-plan.md` 06.1: достигнутый результат и READY WITH LIMITATIONS;
  06.2–06.8 не начаты.
- README и индекс docs: production foundation, фактические команды/проверки,
  ограничения. Исторические отчёты/evidence 06.0 и experimental harness сохранены.

## G. Итог

**READY WITH LIMITATIONS** для независимого ревью. Численный контракт и API 06.1
реализованы, новые tests Debug/Release/AVX2 PASS. Ограничения — один существующий
нестабильный Recovery failure в каждом полном headless selection, symlink SKIP,
и стоимость полной проверки осей, ограниченная явным бюджетом.
Блокирующих вопросов к реализации 06.1 не выявлено; независимое ревью и
пользовательское принятие ещё не выполнены. Storage D, C1 integration, versions,
persistence, Canvas/ROI и rough algorithms в эту реализацию не входят.

## H. Independent review and acceptance

2026-10-29. Проверен контрольный commit реализации
`1db741b7fcce215080e1eb58cc139f6bdba154e0` относительно baseline
`3b1e036db468109a31475cbbb88bf268ecccc4c5`, ветка `main`, исходный Git status пуст.
Независимое ревью — **READY WITH LIMITATIONS**, новых замечаний P0–P3 к Raster
Foundation нет. Пользователь принял **06.1 — ACCEPTED WITH LIMITATIONS**.

Ревьюер подтвердил численный контракт, минимальность dimensions и terminal-cell
regression для `cell=binary64(0.1)`, независимость golden oracle, согласованность
`locate`/nominal/coverage bounds и безопасные численные/ресурсные отказы.
Проверены свежие Debug/Release objects и отдельная AVX2-компиляция:
фактический `/fp:strict`, раздельные FP instructions, отсутствие FMA contraction.
Архитектурных препятствий к 06.2–06.7 не обнаружено.

### Результаты независимых проверок

Это отдельные результаты ревьюера, а не замена исторических результатов разделов C–E.

| Проверка | Debug | Release |
|---|---|---|
| Сборка core и связанных headless targets | PASS | PASS |
| RasterMapping | 14 PASS | 14 PASS |
| Coordinates/ProjectState (в составе headless selection) | 32 PASS | 32 PASS |
| Headless production selection, 580 tests | 579 PASS / 0 FAIL / 1 SKIP | 578 PASS / 1 FAIL / 1 SKIP |
| Независимый differential probe | PASS | PASS |
| Дополнительная AVX2-сборка и RasterMapping | — | 14 PASS |

Новых compiler warnings — 0. Differential probe в каждой конфигурации сопоставил
8000 осей, 55 172 результата `cellBounds`, 277 478 запросов принадлежности и
216 сценариев изменения FP-окружения callback без расхождений. Oracle использует
точные `Fraction`, отдельное округление произведения/суммы через CPython и линейный
поиск размера/принадлежности, без production helper или генератора golden fixtures.

Локальное evidence в `build/review061/` доступно при документальном закрытии
и сверено с этим итогом: `commands.txt`, `debug-headless.log/.xml`,
`release-headless.log/.xml`, `avx2-run.log`, `numerical/evidence.txt`
и `docs-integration/recovery-probe.log`. Это build-артефакты, не versioned evidence.
Временный FP harness первоначально дал 12 FAIL в каждой конфигурации из-за неверного
ожидания числа callback; после исправления только ожидания harness расхождений нет.
Исходные логи сохранены, production tests/assertions не изменялись.

### Остаточные ограничения

- **Legacy Recovery P3 — не исправлен.** В независимом Release-прогоне упал
  `RecoveryApplication.FileFailures/RecoveryFault.FailedUpdatePreservesPreviousRecordAndAllDatasets/6`:
  helper `tests/recovery_tests.cpp:46` ожидал `Applied`, получил `NoChange`.
  Это вызов после `save()` на строке 320, до установки fault hooks: автоматический
  post-Save Autosave успел сохранить тот же snapshot. Такой `NoChange` корректен;
  Raster Mapping не участвует, production/test sources сценария не менялись.
  Причина соответствует ранее подтверждённой [нестабильности 05.4](../task-05/autosave-recovery.md#неблокирующее-наблюдение-о-надёжности-тестов).
  Отдельные 10/10 повторов PASS не отменяют исходный FAIL. Независимый управляемый
  probe подтвердил допустимый `NoChange`, сохранность snapshot и успешное сохранение
  следующей правки (50/50). Требуется отдельное адресное исправление синхронизации
  Recovery fixture без ослабления проверяемых инвариантов; для 06.1 неблокирующее.
- **Windows symlink SKIP.** `Persistence.StoragePath.FileSymlinkEscape` пропущен
  в обеих конфигурациях из-за отсутствия privilege 1314. Это унаследованное
  ограничение среды; текущий SKIP не считается PASS, прежние сведения о проверках
  сохраняются в отчётах задачи 05.
- **Стоимость больших осей.** Полная проверка — O(width+height) времени и O(1)
  дополнительной памяти. Начальный явный budget 10 млн — инженерная политика,
  не универсальный предел размеров карты; превышение даёт `ResourceRefused`.
  Независимые Release medians: 10 млн — 41,96 мс; 50 млн с повышенным бюджетом —
  207,38 мс. Универсальная гарантия времени не установлена.
- GUI, HDD, representative real scans и полный Point Cache → C1 → D pipeline
  в 06.1 не проверялись; это последующие этапы, не подтверждение Rough на real scans.

### Граница принятия и следующий этап

06.1 завершён и принят с перечисленными ограничениями; обязательных исправлений
Raster Foundation перед переходом нет. Зависимость 06.2 от 06.1 выполнена,
можно начинать проектирование канонического хранилища D. 06.2–06.8 не начаты.
Численные контракты, исходные критерии этапа и историческое evidence 06.0 сохранены.
При документальном закрытии изменена только документация; сборки, тесты и probes
не повторялись, результаты независимого ревью не являются новыми прогонами Close.
