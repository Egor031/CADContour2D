# Project Manifest v1 — Persistence Foundation 05.1

## Статус и область

Реализованы metadata snapshot, UTF-8 JSON codec на Qt Core и безопасное разрешение
project-owned paths. Это основа хранения, а не пользовательские Save/Open.
Binary Point Cache v1 не изменён. Несовместимое расширение manifest, включая новые
семантические поля или dataset kinds, требует увеличения `version` и явной поддержки
codec; миграции и молчаливое игнорирование неизвестного содержимого отсутствуют.

## Точная схема

Все показанные поля обязательны. В v1 ровно один descriptor `pointCache`, потому что
других datasets в domain model ещё нет. Это ограничение схемы v1, а не ограничение
числа будущих Density Map versions. Пустые sections для будущих подсистем не хранятся.

```json
{
  "format": "CADContour2D",
  "version": 1,
  "projectId": "e3fa43cb-04a2-42be-ae60-91263ea74616",
  "storageId": "bd1422a6-a982-471b-800d-b466154a2637",
  "snapshotId": "a5b3c605-a6c7-40af-94d3-4096eb536293",
  "units": "mm",
  "parameters": { "cell": "0.10000000000000002" },
  "source": { "path": "D:/Измерения/Деталь.xyz", "bytes": "1234", "lines": "20" },
  "state": { "pointCacheId": "e7f25d97-cb8e-44d3-8760-682ad575c958" },
  "datasets": [{
    "kind": "pointCache",
    "formatVersion": 1,
    "cacheId": "e7f25d97-cb8e-44d3-8760-682ad575c958",
    "path": "data/bd1422a6-a982-471b-800d-b466154a2637/datasets/e7f25d97-cb8e-44d3-8760-682ad575c958.pc2dcache",
    "pointCount": "3",
    "hasNonZeroZ": false,
    "bounds": { "minX": "-0", "minY": "-4", "maxX": "3", "maxY": "2" }
  }]
}
```

Порядок полей и whitespace несущественны. Повторяющиеся decoded property names,
в том числе `a` и `\u0061`, запрещены до построения основного Qt DOM.
Неизвестные поля на любом уровне, kinds, единицы и версии отклоняются.
Отсутствующие поля не получают default values. `null` не заменяет обязательное поле.

UUID — ненулевая каноническая lowercase строка `8-4-4-4-12` без braces. Это непрозрачные
128-битные значения; существующий CacheId не требует конкретных UUID version/variant.
Новый `projectId` генерируется Qt при подготовке import candidate до publication gate.
Constructor domain state принимает явный `ProjectId`, чтобы последующий Open мог
восстановить постоянную идентичность независимо от нового runtime stamp.

| Значение | Контракт |
|---|---|
| `projectId` | Логический проект; сохраняется при cell/Undo/Redo |
| runtime identity / input revision | Только applicability текущего сеанса, не сериализуются |
| `cacheId` / `state.pointCacheId` | Один immutable Point Cache; reference обязан совпадать с descriptor |
| `storageId` | Конкретное хранилище назначения snapshot, может отличаться от текущего AppData storage |
| `snapshotId` | Конкретный prospective snapshot, задаётся вызывающим кодом |
| OperationId, progress, worker, history, drafts | Не входят в manifest |

`captureSnapshot(state, storageId, snapshotId)` проецирует только domain metadata и
формирует references назначения. Она не подтверждает наличие файлов назначения,
не устанавливает published save baseline и не меняет текущую сессию.

## Числа и согласованность

- `version` и `formatVersion` — unsigned integer JSON tokens без fraction/exponent;
  диапазон uint32, поддерживается 1. Raw tokens проверяются до Qt binary64 conversion:
  `1.00000000000000000001` не может округлиться в поддерживаемую версию 1.
- Все `uint64_t` — десятичные строки без знака, exponent и ведущих нулей, кроме `0`.
- Все значимые binary64 (`cell`, bounds) — десятичные строки с грамматикой
  `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`.
- Encoder использует `to_chars(general, max_digits10)`, decoder — `from_chars` с полным
  потреблением строки. Конечные subnormal и signed zero сохраняются побитово.
  Overflow/underflow вне представимого диапазона возвращают NumericRange; NaN/Infinity
  и нечисловые строки отклоняются. Десятичного округления до фиксированного числа знаков нет.
- `cell` конечен и строго положителен; bounds конечны и упорядочены по обеим осям.
- Импортированный cache непуст; `0 < pointCount <= source.lines <= source.bytes`.
  Source path — непустой корректный Unicode без NUL; исходный ASC/XYZ не читается.
- Для pointCount проверяется представимость packed Point Cache v1 в qint64 file range
  через checked arithmetic. `uint64_t` metadata сохраняют весь диапазон, но UINT64_MAX
  точек не описывают физически представимый cache и отклоняются.
- Dataset path должен совпадать с `data/<storageId>/datasets/<cacheId>.pc2dcache`.

Числа JSON вместо числовых строк, `"1x"`, `"+1"`, `"NaN"`, `"1e999"`, несовпадающий
CacheId, повторный descriptor и `../cache` — примеры недопустимого содержимого.
Отсутствующий либо повреждённый dataset не делает синтаксически и структурно
корректный manifest повреждённым: проверка содержимого — отдельная будущая операция.

## Ресурсы и ошибки

Все точки входа codec получают явный `ManifestLimits {maxBytes, maxTokens, maxDepth}`.
Нуль не означает unlimited. Бюджет выбирает вызывающий код по доступным ресурсам;
универсального фиксированного маленького лимита или лимита числа версий нет.
Тестовый профиль 1 MiB / 4096 tokens / depth 32 не является production policy.

Перед DOM проверяются фактические UTF-8 bytes, число tokens (включая property names,
скалярные значения и открывающие containers), глубина и duplicate keys. Qt отдельно
проверяет JSON grammar. Его собственные ограничения parser также действуют.
Память зависит от metadata, а не от числа точек; DOM и duplicate-key sets имеют
накладные расходы. Это admission limits, не гарантия peak RSS; bad_alloc даёт
ResourceExhausted. Encoder проверяет размер единственной неограниченной source string
до создания DOM и фактический JSON после создания.

`readManifest` принимает уже открытый readable nonsequential binary QIODevice в
позиции 0. Size проверяется до первого read; чтение идёт блоками до 16 KiB, с
одним дополнительным байтом для обнаружения превышения бюджета при росте файла.
Проверяются отрицательный read, размеры и их изменение. Sequential devices
отклоняются: временный нулевой read не доказывает окончательный конец потока.
Изменение содержимого с тем же размером не исключается этим API.

`Result<T>` возвращает `Error {code, field, detail}` без GUI. Категории:
InvalidJson, UnsupportedVersion, UnsupportedFormat, MissingField, InvalidType,
NumericRange, InvalidValue, InconsistentState, InvalidReference, ResourceExhausted,
FileIO. Некорректные значения не маскируются пустым успешным snapshot.

## Storage, пути и retention

```text
ProjectName/
├── ProjectName.pc2dproj
└── data/<storageId>/datasets/<CacheId>.pc2dcache
```

Persistent references не содержат AppData или абсолютный root проекта. Весь каталог
можно перенести. Исходный `source.path` справочный и может быть абсолютным/UNC.

`validateDatasetPath` проверяет лексическую переносимость без файлового I/O:
forward slash, непустые components, запрет absolute/UNC/drive-relative, backslash,
`.`/`..`, ADS (`:`), управляющих и недопустимых Win32 characters, trailing dots/spaces,
некорректного UTF-16 и components длиннее 255 UTF-16 units. Запрещены device names,
включая имена с расширением, COM/LPT 1–9 и superscript ¹²³, CONIN$/CONOUT$.

`resolveDatasetPath` требует существующий абсолютный project root и использует его
canonical location как доверенную границу. Каждый существующий descendant проверяется
через GetFileAttributesW; **все** reparse points отклоняются, даже внутренние links.
Родители должны быть каталогами, конечный существующий элемент — файлом.
Проверяются и существующие родители отсутствующего leaf. Отсутствие допустимо и
возвращается как `exists=false`; access/inspection failure — отдельная ошибка.
Путь root сам может быть выбран через link, границей становится его canonical target.

Это наблюдение filesystem, не secure open по handle. Внешняя подмена между проверкой
и использованием (TOCTOU), hard-link aliases и изменение содержимого другим процессом
не исключаются. Для 05.2 обязательны повторные проверки перед I/O, уникальное создание
без overwrite, проверка completion dataset и публикация manifest только после datasets.
Полный протокол копирования/rename и crash durability в 05.1 не реализуются.

Единственный физический binding принятого domain dataset —
`shared_ptr<const DatasetLocation>` (storage identity, directory, relative path).
ProjectSession не дублирует расположение. Будущие worker/Undo владельцы удерживают эту
ссылку; Save может подготовить новый binding и атомарно заменить current reference,
при этом старые snapshots продолжают удерживать прежнее расположение. Само переключение
не реализовано. Логическая CacheId не меняется при переносе тех же immutable bytes.

DatasetLocation ничего не удаляет, даже при уничтожении последней ссылки. Принятые
AppData и будущие project-owned файлы сохраняются после Close, замены, очистки Undo и
уничтожения controller. Scratch/PendingCache/QTemporaryDir до Accept принадлежат
создавшему worker и сохраняют прежние cleanup правила. Неопределённые файлы не удаляются.
Save/Recovery persistent reachability и GC не реализованы; принятую retention policy
нельзя заменить удалением по одному лишь shared_ptr reference count.
