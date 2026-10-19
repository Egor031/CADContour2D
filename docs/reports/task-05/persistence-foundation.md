# 05.1 — Persistence Foundation

Первоначальная реализация — commit `80eff06`. Независимое ревью потребовало
исправлений H1/H2 (**FIXES REQUIRED**); воспроизведение и результаты исправления
описаны в [дополнении H1/H2](#h1-h2-review-fixes). Разделы A–I ниже сохраняют
исторические результаты первоначальной реализации. Окончательная приёмка 05.1 не объявлена.

## A. Baseline

- Дата: 2026-10-19, Windows x64.
- Branch: `main`.
- Исходный HEAD: `58b058b350f1347f8423bdc145e801ef17ffc69c`.
- Рабочее дерево и index перед работой чистые; пользовательских изменений не было.
- Применены пользовательские инструкции и корневой AGENTS.md: C++20/Qt Quick,
  Qt-независимый domain, metadata-only persistence, сохранение принятых cache,
  проверки до commit, отсутствие push без поручения. Работа ограничена 05.1.
- Изучены README, implementation-plan, requirements §§6, 8, 31–33, architecture
  §§12, 14–15, 18, 24–31, 44–45, численные/импортные контракты algorithms,
  point-cache-format; ProjectState, ImportedDataset, controller/import operation,
  OperationRunner/types, CacheReader и связанные тесты.
- Исходный lifetime: PendingCache и QTemporaryDir принадлежат worker до Accept;
  atomic gate публикует candidate и Accept, затем worker отключает autoRemove.
  Принятые файлы уже сохранялись после Close/замены/controller destruction.

## B. Architecture Decisions

1. `ProjectId` — отдельный Qt-независимый value type из 16 UUID bytes. Генерация
   находится в fallible подготовке import candidate; атомарный commit не расширен.
   Constructor domain state принимает явный ID для будущего восстановления.
2. `ProjectSnapshot` отделён от domain state. Он содержит persistent IDs, выбранный
   cell, source metadata, Point Cache descriptor и state reference. Runtime stamp,
   revision, worker, OperationId и history отсутствуют.
3. `cadcontour_persistence` зависит от core/cache metadata и Qt Core. Codec и storage
   paths разделены на две пары файлов; отдельные service/repository/transaction
   frameworks, зависимости, миграции и фиктивные future sections не добавлены.
4. В v1 поддержан один pointCache descriptor. Будущие kinds требуют новой schema
   version; storage/path mechanism и retention binding не зависят от их числа.
5. Физическое расположение удерживает единственный shared immutable DatasetLocation;
   исчезновение ссылки не означает удаления файла. Логическая CacheId отделена от
   места хранения. Persistent reference database и GC не вводились.

## C. Implementation

Production-файлы:

| Файлы | Изменение |
|---|---|
| `src/core/project_id.h` | Persistent UUID value type без Qt |
| `src/core/project_state.h/.cpp` | Явный persistent ID в constructor импортированного состояния; неизменяемый getter |
| `src/core/imported_dataset.h` | DatasetLocation и shared const binding вместо отдельных физических полей |
| `src/application/project_controller.h/.cpp` | Генерация projectId и binding до commit; удалено дублирование путей в private PersistentStorage |
| `src/persistence/project_manifest.h/.cpp` | Snapshot projection, JSON encoding/decoding, bounded QIODevice read, structured errors |
| `src/persistence/project_storage.h/.cpp` | Канонические имена назначения, lexical validation и Win32 filesystem resolution |
| `CMakeLists.txt` | Самостоятельный headless persistence target, существующие presets и зависимости сохранены |

Внутренний C++ API импортированного ProjectState теперь требует ProjectId;
ImportedDataset хранит location reference. Все существующие consumers cachePath()
сохранены. Пользовательские API/GUI и поведение импорта не расширены.
ProjectState не получил QFile/QJson/I/O. OperationRunner, Accept/Discard,
binary Point Cache v1, parser/importer и QML не изменены.

Tests: `tests/persistence_tests.cpp`, дополнения import_application_tests.cpp и
tests/CMakeLists.txt. Обновлены адресные документация, README и индекс docs.

## D. Manifest Format

Нормативная [точная JSON schema с полным примером](../../project-manifest-format.md#точная-схема)
содержит обязательные поля:

```text
format = "CADContour2D", version = 1, projectId, storageId, snapshotId, units = "mm"
parameters: { cell }
source: { path, bytes, lines }
state: { pointCacheId }
datasets: [{ kind = "pointCache", formatVersion = 1, cacheId, path,
             pointCount, hasNonZeroZ, bounds: { minX, minY, maxX, maxY } }]
```

UUID — canonical lowercase nonzero; state CacheId совпадает с descriptor.
Uint64 записаны десятичными строками; binary64 — строками `to_chars(general,
max_digits10)` с полным `from_chars` parse. Побитово проверены соседние значения,
положительный/отрицательный zero, subnormal, DBL_MIN/MAX и большие координаты.
UINT64_MAX сохраняется в source metadata; невозможный по file range pointCount
отклоняется. Корректный count выше 2^53 проходит exact round-trip без giant fixture.

Неизвестные fields/kinds/units/versions отклоняются. Duplicate decoded keys
выявляются до основного Qt DOM. Нет default при missing field, numeric truncation
или молчаливого удаления содержимого. Несовместимое расширение увеличивает version.
Version fields требуют unsigned integer tokens без fraction/exponent. Финальная
проверка нашла и закрыла rounding case `1.00000000000000000001` → Qt double `1`:
исходный токен проверяется до преобразования и отклоняется. Добавлен regression test.

Ошибки различают invalid JSON/UTF-8, version/format, missing/type, range/value,
state/reference inconsistency, resource admission и file I/O.
`cell="0.5"` допустим; `cell=0.5`, `"NaN"`, `"1x"`, `"1e999"`, повторный descriptor,
неизвестный ключ или `../cache` отклоняются. Отсутствующий dataset не повреждает manifest.

ManifestLimits задаётся caller: bytes, tokens, depth, без универсального малого cap
и без лимита числа будущих версий. Device size проверяется до read, чтение bounded;
перед DOM выполняется structural admission. Это не гарантия process RSS.

## E. Storage and Ownership

```text
ProjectName/ProjectName.pc2dproj
ProjectName/data/<storageId>/datasets/<CacheId>.pc2dcache
```

Snapshot содержит только relative internal references, исходный source path справочный.
Snapshot projection формирует место назначения без копирования, открытия dataset
или изменения текущего binding. Наличие/целостность cache проверяются отдельно
на последующих этапах, не во время codec.

Paths проверяются на absolute/UNC/drive-relative, traversal, empty/dot components,
Win32 ambiguous endings/device names, ADS, invalid characters/Unicode. Resolver
использует canonical root и проверяет атрибуты каждого существующего descendant,
включая parents отсутствующего файла; все reparse points запрещены. Directory вместо
конечного файла и файл вместо parent отклоняются. Никакие каталоги не создаются.

DatasetLocation удерживает storage ID + physical location. Session не дублирует
эти данные; будущий worker или Undo snapshot может удерживать ту же const shared_ptr.
Будущий Save подготовит новый binding и заменит current reference после успешной
публикации; старые holders сохранят прежний binding. В 05.1 переключения нет.
Ни Close, ни replacement, ни последняя ссылка не удаляют принятый файл. Scratch и
непринятые результаты продолжают удаляться прежними worker guards.

Контракт подготовки/публикации для 05.2: новые уникальные immutable datasets создаются
без overwrite, проверяются completion/identity, и только затем manifest может стать
published. Проекция metadata этого не доказывает и не является save baseline.
Фактические copy/Save/Open/partial availability/Recovery остаются 05.2–05.5.

## F. Tests

Финальные результаты в **каждой** конфигурации Debug и Release:

| Набор | Покрытие | Passed | Failed | Skipped |
|---|---|---:|---:|---:|
| Persistence.Manifest | Полная схема, source/UUID/cache/storage/cell, bit-exact числа, missing/types/version tokens, unknown/duplicate keys, ranges/references, budgets/device errors, metadata projection | 15 | 0 | 0 |
| Persistence.StoragePath | Unicode/перенос/missing, lexical Win32 safety, directory/file mismatch, junction escape с missing leaf, file symlink | 4 | 0 | 1 |
| AscApplication | 3 новые проверки ID/Undo/failure/runtime separation/shared retention плюс 22 прежних import/lifecycle tests | 25 | 0 | 0 |
| Все production CTest | Вышеуказанные и весь прежний production baseline, включая cache/core/operations/confirmation/GUI/QML/dependency smoke | 474 | 0 | 1 |

FileSymlinkEscape пропущен с Win32 `ERROR_PRIVILEGE_NOT_HELD (1314)`;
он **не** считается PASS. Junction test реально создаёт directory reparse point,
проверяет escape/missing leaf и удаляет только link, сохраняя target.

Проверка отсутствия скрытой работы: capture/codec проверены с отсутствующим binding,
без доступного исходного облака; count выше 2^53 обрабатывается только как metadata.
Просмотр production dependencies/source подтвердил отсутствие CacheReader calls,
scanAll/validate, cache copy, Writer, PNG/index creation и загрузки точек.
Representability проверяется checked metadata arithmetic, не чтением dataset.

## G. Regression

Использован существующий локальный `build/task02-env.cmd`: MSVC 19.51.36260,
Qt 6.12.0, CMake 3.30.5, Ninja 1.12.1; глобальный PATH не менялся.
В обеих конфигурациях CMake reconfigure выполнен автоматически build-командой;
vcpkg сообщил, что все пакеты уже установлены. Новых project dependencies нет.

Фактически выполнены:

```bat
build\task02-env.cmd cmake --build --preset windows-debug --target all all_qmllint --parallel 4
build\task02-env.cmd cmake --build --preset windows-release --target all all_qmllint --parallel 4
build\task02-env.cmd ctest --preset windows-debug -L production --output-on-failure --output-log build/task05-debug-production-final.log
build\task02-env.cmd ctest --preset windows-release -L production --output-on-failure --output-log build/task05-release-production-final.log
git diff --check
```

- Debug: build/lint PASS; production 474 passed, 0 failed, 1 skipped; 92.93 s.
- Release: build/lint PASS; production 474 passed, 0 failed, 1 skipped; 45.81 s.
- Предварительная полная регрессия до version-token regression: в каждой конфигурации
  473 passed, 0 failed, 1 skipped из 474. После исправления повторены обе полные
  сборки/lint и production CTest; таблица F отражает финальный код.
- До полной регрессии: адресный Debug Persistence/AscApplication, 41 passed,
  0 failed, 1 skipped (до добавления двух дополнительных manifest tests).
- Diff/область: whitespace check PASS; src/cache, src/io, OperationRunner,
  import_operation, src/ui и qml не менялись. Никакого пользовательского Save/Open нет.
- Configure сохраняет известные optional Qt6TaskTree/WrapVulkanHeaders diagnostics.
  При Debug rebuild существующего operation_diagnostics.cpp была warning C4702
  в стороннем fmt/base.h; новый persistence code собран без предупреждений.

Первоначальные sandbox configure/build были заблокированы доступом к vcpkg cache,
файловые tests — записью в Qt temp directory. Эти прогоны не считаются успешными;
выше приведены реальные повторные прогоны вне sandbox. Зависшие sandbox build
processes остановлены через их exec sessions, исходники/тесты для обхода не ослаблялись.

Opt-in CacheReaderIntegration с многомиллионным fixture, giant/HDD benchmarks,
generator workloads, ручной GUI smoke и power-loss tests не запускались: binary
cache/алгоритмы/GUI не менялись, полная production regression выполнена.

## H. Risks and Limitations

- File symlink fixture не проверен из-за Windows privilege; junction/reparse branch
  проверен реальным filesystem test. Path handling привязан к Windows baseline;
  все descendant reparse points запрещены консервативно, даже внутренние links.
- Resolver — path observation, не handle lease. TOCTOU, hard-link aliases, внешнее
  изменение datasets и same-size rewrite manifest не исключаются. Следующие I/O
  операции повторяют проверки непосредственно перед использованием.
- Manifest validity не подтверждает dataset availability, ID из реального файла
  или checksums. Полная validation никогда не запускается codec автоматически.
- Retention консервативен: принятые файлы накапливаются; shared_ptr не заменяет
  persistent reachability save/recovery. Удаление/GC, crash scavenger и backups отсутствуют.
- Caller должен выбрать разумный metadata admission budget; оценка реального RSS
  для будущих больших списков версий не выполнена. Ограничения Qt parser действуют.
- Для 05.2 остаются copy/progress/cancel, уникальное no-overwrite создание, dataset
  completion и атомарная manifest publication, безопасная передача session binding.
  В 05.1 это закреплённые обязанности будущего протокола, а не готовая транзакция.

## I. Final Status

**READY FOR INDEPENDENT REVIEW** — реализована только 05.1.
Задача 05 целиком не завершена и не принята. Следующее действие — независимое
техническое ревью; к 05.2 работа не переходила.

Branch остаётся `main`. Результат предназначен для одного commit с сообщением
`implement project persistence foundation and strict manifest v1`; итоговый HEAD —
commit, содержащий этот отчёт (точный hash приводится в финальном сообщении).
После commit ожидается чистый index/рабочее дерево; фактический Git status проверяется
после commit и сообщается отдельно. Push не выполняется.

## H1-H2 review fixes

### A. Baseline

Дата: 2026-10-19. Branch `main`, исходный HEAD
`80eff061ef56ad783de7ae220819d1f3af4314eb`. Рабочее дерево и index чистые.
Применены действующие AGENTS.md и Git-политика: адресное исправление, проверки и
отдельный commit без изменения истории или push. Повторный архитектурный аудит
и работы 05.2 не выполнялись.

### B. Verification of Findings

**H1 подтверждено.** В preflight backslash без проверки пропускал следующий байт;
raw control bytes также пропускались. Qt 6.12 parser принимал повреждённые strings:
`source.path="\q"` превращался в `q`, `units="m\m"` — в `mm`, `cell="\1"` — в `1`.
Также принят key `"unit\s"` как `units`. Из 32 raw control bytes в тестовом source.path
U+0001–U+001F принимались; raw NUL уже отклонялся поздней semantic validation как
InvalidValue, а не как повреждённый JSON. Escaped unpaired surrogates также ранее
отклонялись поздно как InvalidValue; теперь они выявляются до Qt.

**H2 подтверждено для обоих полей.** Условие
`decimalSyntax(raw, false) && !decimalSyntax(raw, true)` не отклоняло `1.` и `1.e0`,
поскольку первая проверка возвращала false. Permissive Qt parser затем превращал
эти tokens в 1. Та же причина давала неверную диагностику malformed numbers в
других полях: например, `hasNonZeroZ:1.` доходил до проверки JSON-типа как число.

Сначала изменены **только tests**, production codec оставался исходным. Запущены
6 новых regression tests и 3 прежних round-trip controls: **4 Passed / 5 Failed /
0 Skipped**, exit 1. Пять отрицательных групп обнаружили дефекты, positive escapes
и все три numeric/full round-trip controls прошли. Это ожидаемые failures
воспроизведения, а не failures итоговой реализации. Локальные доказательства:
`build/task05-h1h2-before.log` и `.xml`.

### C. Fixes

Изменены только:

- `src/persistence/project_manifest.cpp` — lexical preflight;
- `tests/persistence_tests.cpp` — raw-input fixtures и regression tests;
- `docs/project-manifest-format.md` — уточнение фактически обеспечиваемой validation;
- этот отчёт и README — результаты и текущий статус.

H1: общий string scanner проверяет **все** names/values до любого Qt parsing.
Разрешён точный набор JSON escapes; проверяются четыре hex digits у `\uXXXX`,
согласованность UTF-16 surrogate pairs, string/escape termination и запрет raw
U+0000–U+001F. Значения не исправляются и не нормализуются. Literal UTF-8 и корректные
escapes остаются допустимыми. Существующая semantic NUL validation сохранена.

H2: malformed JSON number теперь даёт InvalidJson; корректный fraction/exponent
или signed token в поле unsigned version — NumericRange. Диапазон uint32 проверяется
по исходной целочисленной строке до Qt conversion, включая очень длинный overflow.
`1` принимается, корректные integers неподдерживаемой версии дают UnsupportedVersion,
а неверный JSON-тип по-прежнему даёт InvalidType. Общая проверка raw numeric tokens
устраняет тот же permissive parsing вне version fields.

Это дополнение существующего ограниченного preflight; Qt по-прежнему разбирает
структуру JSON и строит DOM. Новый полноценный JSON parser, интерфейсы или зависимости
не добавлены. Схема не расширялась: ранее запрещённый вход теперь действительно отклоняется.

### D. Regression Tests

Набор `ManifestReviewRegression` (6 tests):

| Test | Проверка |
|---|---|
| H1RejectsInvalidStringEscapesInKeysAndValues | Неизвестные escapes в source, units, cell и key |
| H1RejectsEveryUnescapedControlByte | Все 32 raw U+0000–U+001F |
| H1RejectsMalformedUnicodeAndTruncatedStrings | Короткие/non-hex Unicode escapes, unpaired/wrong surrogates, усечение strings/escapes |
| H1AcceptsStandardEscapesAndUnicodeWithoutChangingValues | Все 9 JSON escape forms, literal Unicode, supplementary pair, escaped keys/cell, literal backslash перед q; semantic NUL rejection |
| H2VersionTokensAndTypesAreStrict | Оба поля: 1/whitespace, 1., 1.0, 1e0, 01, +1, .1, -1, -0, malformed exponent/fraction, rounding, uint32 boundaries/overflow, unsupported versions и неверные types |
| H2NumericGrammarIsCheckedBeforeFieldTypes | Malformed raw numbers вне versions дают InvalidJson; корректное число неверного типа сохраняет InvalidType |

Raw tokens вставляются в уже сериализованный корректный fixture, чтобы Qt encoder
не мог исправить повреждение до теста. Positive controls сравнивают фактические
decoded значения, не только success. В обеих конфигурациях новый набор: **6 Passed /
0 Failed / 0 Skipped**. Полный persistence-набор: **25 Passed / 0 Failed / 1 Skipped**;
побитовые binary64/uint64/signed-zero и прежние validation tests прошли.

### E. Debug / Release

Использованы существующие CMake presets и штатный локальный MSVC/Qt environment
из `build/task02-env.cmd`, без изменения зависимостей/toolchain.

```bat
build\task02-env.cmd cmake --build --preset windows-debug --target all all_qmllint --parallel 4
build\task02-env.cmd cmake --build --preset windows-release --target all all_qmllint --parallel 4
build\task02-env.cmd ctest --preset windows-debug -R ^Persistence --output-on-failure --output-log build/task05-h1h2-debug-persistence.log
build\task02-env.cmd ctest --preset windows-release -R ^Persistence --output-on-failure --output-log build/task05-h1h2-release-persistence.log
build\task02-env.cmd ctest --preset windows-debug -L production --output-on-failure --output-log build/task05-h1h2-debug-production.log
build\task02-env.cmd ctest --preset windows-release -L production --output-on-failure --output-log build/task05-h1h2-release-production.log
git diff --check
```

Debug: build и QML lint PASS; production **480 Passed / 0 Failed / 1 Skipped**
из 481, 91.78 s. Release: build и QML lint PASS; production **480 Passed / 0 Failed /
1 Skipped** из 481, 50.85 s. Адресный persistence-набор в каждой конфигурации:
**25 Passed / 0 Failed / 1 Skipped**. Новый code/tests собраны с `/W4` без предупреждений.
`git diff --check` PASS. Просмотр полного diff подтвердил адресную область изменений.

### F. Scope and Compatibility

Схема `.pc2dproj` v1, Point Cache v1, numeric representation и error enum не менялись.
Из production изменён только manifest codec. Domain/application, Import,
OperationRunner, worker lifecycle, storage paths/ownership и GUI/QML не менялись.
Полные scans/validate/copy cache и другие тяжёлые операции не добавлены.
Гигабайтные benchmarks, opt-in CacheReaderIntegration и ручной GUI smoke не запускались.
05.2, Save/Save As и расширение persistence не начинались.

### G. Remaining Risks

FileSymlinkEscape остаётся SKIP с Windows `ERROR_PRIVILEGE_NOT_HELD (1314)`;
политика теста не менялась. Реальный junction escape проверен. Прежние ограничения
TOCTOU, conservative retention и metadata admission budgets сохранены. Исправления
не добавляют гарантий dataset integrity/availability. Новых дефектов вне H1/H2
в ходе этой целевой проверки не обнаружено.

### H. Final Status

**H1/H2 FIXED — READY FOR VERIFICATION.**
Branch остаётся `main`; исправления предназначены для нового отдельного commit
`fix strict JSON string and version token validation` без изменения `80eff06`.
Точный итоговый HEAD и проверенный Git status сообщаются после commit.
Окончательная приёмка 05.1 и всей задачи 05 не объявляется; push не выполняется.
