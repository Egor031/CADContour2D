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
| [Implementation Plan](implementation-plan.md) | Последовательность реализации, границы задач и актуальные статусы |
| [Persistent Point Cache Format](point-cache-format.md) | Binary layout v1, codec, Writer/Reader, spatial queries и validation |
| [Project Manifest v1](project-manifest-format.md) | Строгая JSON schema, точные числа, metadata budgets, пути и retention 05.1 |

План реализации не имеет приоритета над требованиями, архитектурой и алгоритмами.
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

- [05.1 Persistence Foundation](reports/task-05/persistence-foundation.md#independent-verification-and-acceptance) — **ACCEPTED** после независимой проверки исправлений H1/H2; вся задача 05 не завершена, 05.2–05.5 — NOT STARTED.
- [Наблюдение по Operations-тесту](reports/task-05/persistence-foundation.md#operations-test-observation) — UNRESOLVED OBSERVATION / NOT CONFIRMED DEFECT.

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
