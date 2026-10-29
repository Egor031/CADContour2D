# Документация CADContour2D

Основные документы задают действующие контракты системы. Технические отчёты
сохраняют историю реализации и проверок; исследования и benchmark evidence
размещены отдельно.

## Основная документация

| Документ | Назначение |
|---|---|
| [Requirements](requirements.md) | Функциональные требования, ограничения и пользовательское поведение |
| [Architecture](architecture.md) | Подсистемы, зависимости, владение состоянием, threading и persistence |
| [Algorithms](algorithms.md) | Математические контракты, Required, Current approach и Open decisions |
| [UX/UI Design](ui-design.md) | Единая подробная целевая UX/UI-спецификация: компоновка, команды, этапы, workflow и открытые UX-вопросы |
| [Implementation Plan](implementation-plan.md) | Последовательность реализации, границы задач и актуальные статусы |
| [Persistent Point Cache Format](point-cache-format.md) | Binary layout v1, codec, Writer/Reader, spatial queries и validation |
| [Density Map D v1](density-map-format.md) | Нормативный immutable count-tile format, integrity, storage API и resource semantics 06.2 |
| [Project Manifest v1](project-manifest-format.md) | Строгая JSON schema, точные числа, metadata budgets, пути и retention 05.1 |

План реализации не имеет приоритета над требованиями, архитектурой и алгоритмами.
UX/UI Design действует в пределах их контрактов; согласование интерфейса не означает
реализацию функций и не меняет статусы задач.
Обзор проекта и проверенные команды сборки — в [корневом README](../README.md).

06.0 закрыта с ограничениями; пользователь согласовал **D + C1**: immutable exact
count tiles / zlib и один scanAll с bounded event grouping только occupied groups.
PNG-export необязателен; аналитические данные исходного разрешения отделены от
Canvas/display LOD. Raster Foundation 06.1 — `ACCEPTED WITH LIMITATIONS`
по решению пользователя от 2026-10-29 после независимого ревью:
[API, проверки, ограничения и принятие](reports/task-06/raster-foundation.md#h-independent-review-and-acceptance).
Headless D v1 **06.2 — ACCEPTED WITH LIMITATIONS** пользователем 2026-10-29;
независимый вердикт **READY WITH LIMITATIONS**, Recovery P3 отдельно **FIXED AND VERIFIED**.
[Принятие, результаты по источникам и tracked evidence](reports/task-06/density-storage.md#независимое-ревью-и-принятие).
Зависимость для проектирования/реализации 06.3 выполнена; следующий шаг — спецификация 06.3A.
Полный builder/интеграция Density Map, Canvas и ROI не реализованы, 06.3–06.8 не начаты.
Задача 06 целиком остаётся **IN PROGRESS**.
[Архитектура](architecture.md#15-density-map),
[численные условия и ограничения](algorithms.md#6-построение-density-map),
[план 06.1–06.8](implementation-plan.md#06-density-map-engineering-canvas-и-roi).
Исторический 06.0D `INCONCLUSIVE` и последующее `P1 CLOSED WITH LIMITATIONS`
сохраняются раздельно. Manifest v1 и принятая задача 05 не изменяются.

## Инструменты разработки

- [Synthetic Point Cloud Generator: спецификация](tools/synthetic-cloud-generator.md) — Geometry JSON + Scan Scenario JSON → XYZ/ASC + manifest.
- [README генератора](../tools/synthetic_cloud_generator/README.md) — реализация, проверенные команды и воспроизводимость.
- [Import acceptance tool](../tools/import_acceptance/README.md) — opt-in проверки импорта, ресурсов и независимого oracle для 04.4B.

## Исследования

- [NX DXF fixture analysis](research/nx-dxf-fixture-analysis.md) — исторический анализ экспорта Siemens NX. Не является действующей спецификацией генератора; принятое решение — Geometry JSON + Scan Scenario JSON.

## Технические отчёты

### Task 02

- [Подтверждение завершения native worker thread](reports/task-02/thread-completion-check.md).

### Task 03

- [CacheReader, spatial queries и ранние измерения](reports/task-03/cache-reader-check.md) — этап 4/5.
- [Cache reliability и performance](reports/task-03/cache-performance-report.md) — этап 5/5 и исправление после ревью.

### Task 04

1. [04.1: headless ASC/XYZ import](reports/task-04/asc-xyz-import-check.md).
2. [04.1: исправления независимого ревью](reports/task-04/asc-xyz-review-fixes.md).
3. [04.2: application integration и атомарная публикация](reports/task-04/asc-xyz-application-check.md).
4. [04.3: межпоточное подтверждение ненулевого Z](reports/task-04/nonzero-z-confirmation-check.md).
5. [04.4A: GUI импорта](reports/task-04/asc-xyz-gui-check.md).
6. [04.4A: исправления независимого ревью GUI](reports/task-04/asc-xyz-gui-review-fixes.md).
7. [04.4B: сквозные и нагрузочные проверки](reports/task-04/asc-xyz-e2e-performance-check.md).

**Итог: [задача 04 принята (ACCEPTED)](reports/task-04/acceptance.md).**
Промежуточные статусы и результаты внутри отчётов относятся ко времени их создания.

### Task 05

- [05.1 Persistence Foundation](reports/task-05/persistence-foundation.md#independent-verification-and-acceptance) — **ACCEPTED** после независимой проверки исправлений H1/H2.
- [05.2 Save / Save As](reports/task-05/save-implementation.md#k-independent-verification-and-acceptance) — **ACCEPTED** после финальной независимой проверки `cedf844`; R1/R2 закрыты.
- [05.3 Open и частичная доступность](reports/task-05/open-implementation.md#l-independent-verification-and-acceptance) — **ACCEPTED** 2026-10-21 после повторной независимой проверки `1415a6a`; R1.1/R1.2 — FIXED и закрыты.
- [05.4 Autosave / Recovery](reports/task-05/autosave-recovery.md#m-acceptance) — **ACCEPTED** 2026-10-22 после независимой проверки `b086cad`, вердикт **READY FOR ACCEPTANCE**; H1–H3/N1 — FIXED и закрыты. Debug/Release: build/lint — PASS, production 694 PASS / 0 FAIL / 1 SKIP, Recovery 64/64, process crash 7/7, целевые N1 + H1/H2 25/25, независимый probe 29/29. В отчёте сохранено неблокирующее наблюдение о нестабильности теста; при документальном закрытии сборки/CTest не повторялись.
- [05.5A GUI Save/Open и lifecycle](reports/task-05/gui-save-open.md) — **ACCEPTED WITH LIMITATIONS**; R1/O1/L1 FIXED. Последующие проверки подтвердили Late Save и системный DPI 150%; layout исправлен, C4702 устранено воспроизводимым vcpkg overlay. История и прежние результаты сохранены в отчёте, актуальные ограничения — в итоговом принятии ниже.
- [05.5B GUI Recovery/startup и итог задачи 05](reports/task-05/gui-recovery.md#независимое-закрытие-ir-05-t1-и-принятие-задачи-05) — **ACCEPTED WITH LIMITATIONS**, IR-05-T1 CLOSED на `fb1f706`. Независимые Debug/Release: production **813 PASS / 0 FAIL / 1 SKIP**, lifecycle 15/15, `/2` и `/5` по 30/30, связанные tests 170/170. Межтомные cases прошли; FileSymlinkEscape здесь SKIP 1314, ранее отдельно PASS в обеих конфигурациях.
- [Наблюдение по Operations-тесту](reports/task-05/persistence-foundation.md#operations-test-observation) — UNRESOLVED OBSERVATION / NOT CONFIRMED DEFECT.

05.1–05.4 — **ACCEPTED**; 05.5A, 05.5B, 05.5 и задача 05 — **ACCEPTED WITH LIMITATIONS**.
Принята C++ реализация Autosave/Recovery с чтением v1 и публикацией v2;
GUI Save/Open, Save/Discard/Cancel и partial availability добавлены в 05.5A;
GUI Recovery/startup decision и сквозные проверки реализованы в 05.5B;
заключительное независимое ревью и перепроверка IR-05-T1 завершены.
Standalone Recovery — NOT VERIFIED; keyboard/visible focus — PARTIAL VERIFICATION;
системный DPI 200% — OUT OF SCOPE; перенос между мониторами — N/A.
На момент принятия 05 наблюдение Recovery `Applied`/`NoChange` оставалось открытым;
последующее отдельное исправление fixture **P3 FIXED AND VERIFIED** (`085a5f1`)
и сохранённые первоначальные failures — в [Recovery-отчёте](reports/task-05/autosave-recovery.md#n-recovery-p3--синхронизация-fixture).
На момент принятия 05 задача 06 не начата; [полные границы принятия](reports/task-05/gui-recovery.md#независимое-закрытие-ir-05-t1-и-принятие-задачи-05).

### Task 06

- [06.0A: стенд и oracle](reports/task-06/density-harness.md).
- [06.0B: D/E storage](reports/task-06/density-formats.md).
- [06.0C: C1/C2 construction](reports/task-06/density-construction.md) — исходная рекомендация пересмотрена.
- [06.0D/P1: sparse scaling C1](reports/task-06/density-construction-p1.md) — P1 закрыт с ограничениями.
- [06.1: Raster Foundation](reports/task-06/raster-foundation.md) — production mapping, numerical verification и work policy; ACCEPTED WITH LIMITATIONS после независимого ревью и решения пользователя.
- [06.2: Density storage](reports/task-06/density-storage.md#независимое-ревью-и-принятие) — **ACCEPTED WITH LIMITATIONS**, независимое ревью D `8d622e0` — **READY WITH LIMITATIONS**, storage-блокеров 06.3 нет; отдельный Recovery P3 `085a5f1` — **FIXED AND VERIFIED**. Исходный отчёт ревью и summary/hashes сохранены рядом с отчётом реализации.

Отчёты сохраняют результаты своих этапов; согласованное направление Close задано нормативными
документами выше, а не ретроспективной правкой результатов экспериментов.

## Benchmark evidence

- [cache05/](benchmarks/cache05/) — структурированные результаты задачи 03: большой round-trip, ресурсы и HDD/source-order comparison. Методика и ограничения — в [отчёте 5/5](reports/task-03/cache-performance-report.md).
- [ascxyz044b/](benchmarks/ascxyz044b/) — исходные параметры, manifests, JSON/CSV измерений и проверки импорта 04.4B. Методика и интерпретация — в [отчёте 04.4B](reports/task-04/asc-xyz-e2e-performance-check.md).
- [Density 06.0B](benchmarks/density06b/README.md) — D/E, raw JSON/CSV и audit.
- [Density 06.0C](benchmarks/density06c/README.md) — C1/C2, exact streams и audit.
- [Density 06.0D/P1](benchmarks/density-p1/README.md) — occupied traversal, refusals и independent oracle.

## Размещение будущих документов

- `docs/` — основные действующие документы.
- `docs/tools/` — спецификации инструментов.
- `docs/research/` — исследования и эксперименты.
- `docs/reports/task-XX/` — исторические отчёты по задачам.
- `docs/benchmarks/` — структурированные данные и результаты измерений.
- `tests/fixtures/` — реальные test fixtures; это входы тестов, а не документация.

Следующие задачи используют эту структуру. Новые отчёты ссылаются на нормативные
документы и evidence, сохраняя различие между действующим контрактом и историей проверок.
