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
| [Project Manifest v1](project-manifest-format.md) | Строгая JSON schema, точные числа, metadata budgets, пути и retention 05.1 |

План реализации не имеет приоритета над требованиями, архитектурой и алгоритмами.
UX/UI Design действует в пределах их контрактов; согласование интерфейса не означает
реализацию функций и не меняет статусы задач.
Обзор проекта и проверенные команды сборки — в [корневом README](../README.md).

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
- [05.5A GUI Save/Open и lifecycle](reports/task-05/gui-save-open.md) — **ACCEPTED WITH LIMITATIONS** после native Windows GUI-приёмки; R1/O1/L1 FIXED. Последний полный production CTest: 761 PASS / 0 FAIL / 1 SKIP в Debug/Release; адресный L1: 8/8 в Debug/Release. Ограничения: системный DPI 150%/200% не проверен (Qt scaling проверен, но не заменяет системный DPI), отдельный native-сценарий длительного Save с поздней правкой, нестабильность Recovery `Applied`/`NoChange` и предупреждение C4702; перенос между мониторами — N/A.
- [05.5B GUI Recovery/startup и сквозные проверки](reports/task-05/gui-recovery.md) — **READY FOR INDEPENDENT REVIEW**; новые проверки и оставшаяся ручная приёмка отделены от принятой 05.4/05.5A.
- [Наблюдение по Operations-тесту](reports/task-05/persistence-foundation.md#operations-test-observation) — UNRESOLVED OBSERVATION / NOT CONFIRMED DEFECT.

05.1–05.4 — **ACCEPTED**; 05.5A — **ACCEPTED WITH LIMITATIONS**; 05.5B — **READY FOR INDEPENDENT REVIEW**.
Принята C++ реализация Autosave/Recovery с чтением v1 и публикацией v2;
GUI Save/Open, Save/Discard/Cancel и partial availability добавлены в 05.5A;
GUI Recovery/startup decision и сквозные проверки реализованы в 05.5B;
независимое ревью и оставшаяся ручная приёмка ещё нужны.
**05.5 — IN PROGRESS / NOT ACCEPTED**.

## Benchmark evidence

- [cache05/](benchmarks/cache05/) — структурированные результаты задачи 03: большой round-trip, ресурсы и HDD/source-order comparison. Методика и ограничения — в [отчёте 5/5](reports/task-03/cache-performance-report.md).
- [ascxyz044b/](benchmarks/ascxyz044b/) — исходные параметры, manifests, JSON/CSV измерений и проверки импорта 04.4B. Методика и интерпретация — в [отчёте 04.4B](reports/task-04/asc-xyz-e2e-performance-check.md).

## Размещение будущих документов

- `docs/` — основные действующие документы.
- `docs/tools/` — спецификации инструментов.
- `docs/research/` — исследования и эксперименты.
- `docs/reports/task-XX/` — исторические отчёты по задачам.
- `docs/benchmarks/` — структурированные данные и результаты измерений.
- `tests/fixtures/` — реальные test fixtures; это входы тестов, а не документация.

Следующие задачи используют эту структуру. Новые отчёты ссылаются на нормативные
документы и evidence, сохраняя различие между действующим контрактом и историей проверок.
