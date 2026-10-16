# Point Cache Binary Format

## Статус и область

Внутренний предварительный формат **v1**, реализация 2/5 задачи 03. Не заморожен:
writer, spatial index, reader и производительность ещё должны быть проверены.
Обоснованные изменения layout до завершения задачи 03 требуют согласованного
обновления этого документа, codec и тестов. Опубликованных версий и миграций пока нет.

`cadcontour_cache` зависит только от `cadcontour_core` и Qt Core. Здесь реализованы
типы, codec отдельных элементов, проверяемый layout, точный файловый I/O и протокол
публикации файла, streaming ingest и external sort. Это не готовый Persistent Point
Cache. Небольшие файлы файловых тестов имеют собственную тестовую сигнатуру и не
являются datasets.

Канонические обязательства: [architecture.md, §§10, 12–14](architecture.md#12-persistent-point-cache),
[algorithms.md, §3](algorithms.md#3-общие-численные-правила).

## Организация контейнера

Все offsets в байтах. Integer fields — unsigned фиксированной ширины,
little-endian. Нет native struct serialization, compression, квантования,
изменения координат или промежуточного `float`. Координаты в миллиметрах, Y вверх.

| Элемент | Размер | Положение |
|---|---:|---|
| Header/data subblock | 2048 | 0 |
| Header/index subblock | 2048 | 2048 |
| Point pages | 65536 каждая | 4096 |
| Index pages | 65536 каждая | сразу после point pages |
| Completion footer | 4096 | сразу после index pages, конец файла |
| Point record | 24 | внутри point page |
| Index entry | 64 | внутри index page |

В странице 64 байта заголовка и 32 байта checksum. Вместимость point page —
**2726** записей (остаются 16 байт padding при полной странице), index page —
**1022** entries (32 байта padding при полной странице). Остальная неиспользованная
часть страницы также padding, всегда нулевая. Между разделами зазоров нет.

```text
pointPages   = pointCount / 2726 + (pointCount % 2726 != 0)
indexOffset  = 4096 + pointPages * 65536
footerOffset = indexOffset + indexPageCount * 65536
fileSize     = footerOffset + 4096
```

Сложения, умножения, диапазоны и преобразования проверяются до вычисления,
включая представимость конечного размера в `qint64`. `pageCount` не использует
переполняющееся `(count + capacity - 1)`. `checked::range` допускает пустой диапазон
на конце файла. `checked::memorySize` проверяет диапазон `size_t`; в целевом x64
он совпадает с `uint64_t`, поэтому непредставимого `uint64_t` здесь не существует.
Это не проверка фактической доступности RAM.

## Идентичность и координаты

`CacheId` — 16 непрозрачных байт UUID в порядке RFC 4122, как `QUuid::toRfc4122()`.
Нулевой ID запрещён. Сериализуется массив байт, не native memory QUuid и не его
смешанное integer-представление. Генератор использует `QUuid::createUuid()`.
Dataset сохраняет ID при перемещении файла; новый dataset получает новый ID.

`RecordId` — `uint64_t`, весь диапазон допустим. Полная идентичность записи —
`{CacheId, RecordId}`. Повторные измерения одинаковых X/Y остаются отдельными
записями. Ingest назначает уникальные последовательные ID; codec не
назначает ID и не ищет дубликаты. Пространственная сортировка может менять порядок,
но должна сохранять ID и биты X/Y каждой записи.

| Point record offset | Размер | Поле |
|---:|---:|---|
| 0 | 8 | RecordId |
| 8 | 8 | X, IEEE 754 binary64 |
| 16 | 8 | Y, IEEE 754 binary64 |

Конечные X/Y сохраняются побитово, включая signed zero и subnormal. Codec
использует `std::bit_cast`, ширина и свойства double проверяются `static_assert`.
Низкоуровневые `binary::getDouble/putDouble` передают любые 64-битные представления;
`encodeRecord/decodeRecord` отдельно отклоняют NaN и ±Infinity как координаты dataset.

## Header/data subblock

Offsets относительно начала файла (начала data subblock).

| Offset | Размер | Поле / v1 значение |
|---:|---:|---|
| 0 | 8 | Magic, ASCII `PC2DDATA`, без NUL |
| 8 | 4 | Version = 1 |
| 12 | 4 | Byte-order marker = `0x01020304` (байты `04 03 02 01`) |
| 16 | 4 | Header size = 4096 |
| 20 | 4 | Page size = 65536 |
| 24 | 4 | Point record size = 24 |
| 28 | 4 | Encoding = 1: uint64 ID + binary64 X/Y в мм, без compression |
| 32 | 4 | Checksum algorithm = 1: SHA-256 |
| 36 | 4 | Flags: bit 0 = hasNonZeroZ, остальные 0 |
| 40 | 16 | CacheId |
| 56 | 8 | Point count |
| 64 | 8 | BBox minX |
| 72 | 8 | BBox minY |
| 80 | 8 | BBox maxX |
| 88 | 8 | BBox maxY |
| 96 | 8 | Point section offset = 4096 |
| 104 | 8 | Point page count, вычислен из point count |
| 112 | 1904 | Reserved, нули |
| 2016 | 32 | SHA-256 байтов [0, 2016) |

BBox конечный и упорядоченный: minX ≤ maxX, minY ≤ maxY; вырожденные размеры
допустимы. Для пустого dataset count = 0, BBox состоит из четырёх **+0**, flag Z = 0.
Для непустого dataset ingest вычисляет точный BBox; codec проверяет
допустимость полей, но не их соответствие всему набору точек.

`CacheMetadata` содержит ID, count, bounds и Z flag; version/encoding и остальные
обязательные размеры заданы константами v1. `cell`, raster mapping и Rough Geometry
к исходному cache не относятся.

## Header/index subblock

Абсолютный offset = 2048 + относительный offset из таблицы.

| Offset | Размер | Поле / v1 значение |
|---:|---:|---|
| 0 | 8 | Magic `PC2DINDX` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Flags = 0 |
| 16 | 4 | Page size = 65536 |
| 20 | 4 | Index entry size = 64 |
| 24 | 4 | Checksum algorithm = 1 |
| 28 | 4 | Kind: 0 = None, 1 = BoundingBoxTree |
| 32 | 8 | Index section offset |
| 40 | 8 | Index page count |
| 48 | 8 | Root page ordinal в index section |
| 56 | 4 | Height = level корневой страницы |
| 60 | 1956 | Reserved, нули |
| 2016 | 32 | SHA-256 относительных байтов [0, 2016) |

Для пустого dataset kind/count/height = 0, root = `UINT64_MAX` (noPage).
Для непустого: kind = 1, pageCount > 0, root < pageCount, 1 ≤ height ≤ pageCount.
Поля описывают будущую BBox hierarchy. Morton-порядок реализован в sorter ниже;
метод построения hierarchy и практическая эффективность spatial access ещё
не реализованы и не подтверждены.

## Point/index page

Offsets относительно начала страницы. Page ID — ordinal от нуля внутри своего
раздела; `UINT64_MAX` запрещён. Point и index ordinals имеют отдельные пространства.

| Offset | Размер | Поле |
|---:|---:|---|
| 0 | 8 | Magic `PC2DPAGE` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Kind: 1 = Points, 2 = Index |
| 16 | 4 | Flags = 0 |
| 20 | 4 | Количество records/entries, 1..capacity |
| 24 | 8 | Page ID |
| 32 | 4 | Level: point = 0; index leaves = 1; parents ≥ 2 |
| 36 | 4 | Payload bytes = count × record/entry size |
| 40 | 24 | Reserved, нули |
| 64 | count × size | Records/entries |
| после payload | до 65504 | Нулевой padding |
| 65504 | 32 | SHA-256 байтов [0, 65504) |

Будущий writer заполняет все point pages кроме последней полностью, что
согласуется с формулой pointPages. Самостоятельный page codec допускает частичную
страницу, не решая её положение в файле. Пустых страниц нет.

### Index entry

| Offset | Размер | Поле |
|---:|---:|---|
| 0, 8, 16, 24 | по 8 | BBox minX, minY, maxX, maxY |
| 32 | 8 | Child page ordinal в соответствующем разделе |
| 40 | 8 | Число исходных записей в дочерней странице/поддереве, ограничения ниже |
| 48 | 4 | Child kind: 1 = Points, 2 = Index |
| 52 | 12 | Reserved, нули |

Для Points-child `1 <= pointCount <= pointCapacity`: ссылка описывает одну Points-страницу.
Для Index-child `pointCount > 0` — число исходных записей в поддереве; оно может превышать
вместимость одной Points-страницы при соблюдении остальных ограничений формата.
Эти локальные ограничения проверяются encoder, decoder и validation отдельной страницы.
Неверный count на входе encoder — `InvalidInput`, в бинарном элементе — `CorruptCache`.

Leaves level = 1 ссылаются на Points; level ≥ 2 — на Index. Будущий index validator
проверит level дочерних страниц, section ranges, BBox coverage, subtree counts и
отсутствие циклов, включая равенство count фактическому числу записей дочерней страницы.
Эта полная проверка согласованности не является обязанностью codec одной entry/page.

## Completion footer

Offsets относительно начала footer, расположенного точно на footerOffset.

| Offset | Размер | Поле |
|---:|---:|---|
| 0 | 8 | Magic `PC2DDONE` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Completion marker = 1 |
| 16 | 16 | CacheId |
| 32 | 8 | Point count |
| 40 | 8 | Полный размер файла |
| 48 | 8 | Point page count |
| 56 | 8 | Index page count |
| 64 | 8 | Footer offset |
| 72 | 3992 | Reserved, нули |
| 4064 | 32 | SHA-256 байтов [0, 4064) |

Footer содержит только признак завершения и согласующие поля; flags для иных
состояний нет. `validateEnvelope` сравнивает header/footer с реальным размером.
Он не читает файл и не доказывает корректность всех страниц dataset.

## Checksums, совместимость и validation

Checksum каждого блока — 32 raw bytes SHA-256 в стандартном порядке digest.
Хэшируется весь блок **до** checksum, включая reserved и padding. Само checksum-поле
исключено, а не заполнено нулями для hashing. Header имеет ровно два отдельных
хэша, без aggregate header hash. Нет section hashes и полного file hash.

Encoder записывает все reserved/padding нулями. Decoder сначала проверяет размер
и SHA-256, затем magic/совместимость и смысловые поля. Поэтому неизвестная версия
при неверном checksum классифицируется как повреждение; при корректном checksum —
`UnsupportedFormat`. Неизвестные encoding, checksum algorithm, flags, page/index
kind и byte-order marker отклоняются как unsupported. Неверные magic, известные
размеры/offsets, BBox, уровни, reserved/padding или checksum — `CorruptCache`.
Неожиданный размер отдельного header/page/footer, отсутствующий completion marker
и усечённый container — `IncompleteCache`. Лишние байты контейнера — corruption.

Codec принимает буфер ровно размера элемента. Primitive integer/double операции
проверяют диапазон до доступа и не требуют alignment. Контроль finite coordinates
отдельный от raw bits. Полная проверка IDs, topology индекса, покрытия BBox и всех
records большого файла остаётся будущим writer/reader.

SHA-256 выявляет случайное повреждение; это не подпись и не аутентификация.

## File I/O, публикация и владение

`file::readExact/writeExact` требуют открытое устройство с нужным направлением доступа
и выключенным текстовым режимом. Активный `QIODevice::Text`, в том числе включённый
через `setTextModeEnabled(true)` после открытия, отклоняется как `InvalidInput` до
передачи данных: содержимое файла, позиция устройства и read buffer не меняются.
Операции работают по переданному span без буфера размера файла,
повторяют short transfers и завершаются при нулевом прогрессе. Read различает
отрицательный результат (ReadFailed), EOF до конца span (IncompleteCache) и нулевой
прогресс без EOF (ReadFailed). Write ≤ 0 — WriteFailed. Seek проверяет qint64,
открытость и random access. Size/flush проверяются отдельно.

`Error` хранит категорию, phase, operation, optional offset, deviceCode и detail
до 256 UTF-16 units. Qt отдаёт `QFileDevice::FileError`, не native errno/GetLastError;
нулевой deviceCode означает отсутствие такого кода. Нативный код не угадывается.
`Result<T>` хранит либо значение, либо ошибку; low-level код не показывает dialogs.
`ResourceLimits` — входные бюджеты caller (memory, temporary disk, open files), а не
глобальный RAM-limit. Streaming sorter проверяет их до выделения рабочих массивов
и создания поколений; нули не означают unlimited.

Протокол `StagedFile`:

1. `create(finalPath)` требует существующий каталог назначения. Создаёт уникальный
   `.pc2d-XXXXXX.tmp` в этом каталоге через `QTemporaryFile::open()` с autoRemove=true.
2. Caller формирует файл через checked write/read/seek/size/flush. Первая ошибка
   запоминается, запрещает последующие операции и публикацию; cleanup её не заменяет.
3. `publish(expectedSize, validator)` — терминальная попытка, включая отказ validation.
   Требует непустой validator и размер в диапазоне qint64. Проверяет flush, точный
   размер, seek(0), затем вызывает validator требуемой структуры. Validator обязан
   только читать, проверять свои I/O и возвращать structured result без исключений.
4. Только после этих проверок однократно вызывает именно
   `QTemporaryFile::rename(finalPath)`. Замена существующего пути запрещена;
   copy/delete fallback, `QFile::rename` и повторный rename отсутствуют.
5. Успех передаёт **тот же** QTemporaryFile в move-only `PublishedFile` без отключения
   autoRemove. Guard владеет cleanup уже опубликованного файла; уничтожение guard
   означает discard и удаляет его. Старый StagedFile больше файлом не владеет.
6. `PublishedFile::releaseToOwner()` возвращает заранее сохранённый finalPath,
   отключает autoRemove и освобождает guard. Caller становится постоянным владельцем
   и отвечает за сохранение/удаление файла. Путь подготовлен до отключения cleanup,
   передача не требует allocations. Это ещё не application publication gate.

При отказе объект удаляет только созданный им temp при уничтожении. Уже существующий
target не приобретается во владение и не удаляется. Cleanup использует RAII Qt;
отдельная ошибка удаления при destructor не сообщается и не заменяет первый failure.
Если ОС не позволяет удалить файл, возможен оставшийся temp: отдельного repair/
scavenger workflow здесь нет. Каталог назначения должен быть подконтролен приложению;
защита от враждебной подмены файлов/каталогов вне контракта.

Поведение rename/autoRemove проверено по [официальной документации Qt 6.12](https://doc.qt.io/qt-6.12/qtemporaryfile.html#rename)
и тестами установленного Qt 6.12.0 на Windows. Atomic rename обеспечивает видимость
сформированного файла, checksums — обнаружение повреждений. Успешный flush не
обещает долговечность при внезапном отключении питания: fsync/directory sync и такая
гарантия в текущий контракт не входят.

## Streaming ingest и external sort (2/5)

`PointStreamSort::create(limits, temporaryParent, cancel, context)` создаёт одну
headless операцию без потоков. `append(span<const Measurement>)` принимает X/Y/Z
как конечные `double`, сохраняет исходные X/Y и назначает ID от нуля в порядке
приёма. Z хранится только как `hasNonZeroZ` (`-0` считается нулём). Count проверяется
перед batch; предельный representable count — `UINT64_MAX`. Пустые batches — no-op
с проверкой cancellation. Пустой dataset даёт четыре +0 и Z=false. BBox выбирает
-0 для минимума и +0 для максимума, если соответствующие знаки встречались во входе;
это не изменяет биты записей.

Сортировка использует `(Morton128, RecordId)`. Для ключа нули канонизируются в +0;
у negative binary64 инвертируются все биты, у nonnegative переключается sign bit.
Из монотонных unsigned слов чередуются биты от старших к младшим: X перед Y.
Ключ состоит из `high/low uint64_t`, сравниваемых лексикографически; `__int128`,
нормализация BBox и grid quantization не используются. Только scratch-запись
содержит ключ; выходной `PointRecord` остаётся 24-байтовым по codec v1.

### Бюджеты и поколения

В RAM нет directory всего набора. На диске не более двух файлов `a.runs/b.runs`
в уникальном `QTemporaryDir`. Каждый run начинается с 32-байтового private header:
magic, count, payload length и FNV-1a-64 всего payload (по 8 little-endian байт).
Payload — последовательность 40-байтовых записей: high, low, ID, X, Y. Headers
образуют дисковую directory; следующий header находится checked skip по length.
Это scratch framing, без persistent versioning и без изменения layout v1.
Ingest не создаёт пустых runs; reader/merge допускают count=length=0 с checksum
пустого payload и пропускают их без вызова sink.
Checksum выявляет случайные повреждения, не предназначен для враждебного ввода.
Проверяются точные размеры файлов, диапазоны, finite coordinates, ID range,
соответствие ключей координатам, строгий порядок внутри runs и checksum.

План памяти резервирует `R = 65536 + 16 * absoluteParentPathLength` байт для control
objects, копий путей, allocator/Qt overhead и ограниченного стека. Остаток `A` задаёт
размер блока `b` (1..1024) и fan-in `F` (2..64). Применимость проверяется по фактическим
`sizeof`, включая выравнивание:

- ingest/sort/write: `40*b + sizeof(Entry)*runCapacity`, где
  `runCapacity = floor((A - 40*b) / sizeof(Entry))`, `sizeof(Entry)=40`;
- intermediate/final merge: `40*b + sizeof(PointRecord)*b +
  F*(sizeof(Entry)*b + sizeof(Cursor) + sizeof(size_t))`.

`std::sort` установленного MSVC выполняет in-place introsort без второго массива;
его стек входит в reserve. Перед merge массив ingest уничтожается. Массивы имеют
точные capacities; входной batch, память sink, общие runtime allocations и файловый
кэш ОС в рабочий бюджет операции не входят. Это план контролируемой рабочей памяти,
а не ограничитель process RSS. Слишком малый бюджет отвергается до allocation;
`bad_alloc` преобразуется в `ResourceExhausted`.

Все inputs поколения читаются через один unbuffered `QFile`, с отдельными bounded
буферами runs и min-heap индексов. Поэтому один открытый файл достаточен для ingest
и final merge; intermediate merge требует два. `maxOpenFiles=1` допустим, но при
необходимости промежуточного поколения операция завершится `ResourceExhausted`.
QFile уничтожается перед удалением; закрытые QTemporaryFile с удерживаемым native
handle не используются. Directory не удерживает постоянного открытого handle.

Disk budget — сумма логических длин принадлежащих операции файлов (без filesystem
metadata/allocation granularity). Перед каждым run и merge проверяются размеры,
qint64 и одновременное существование source+destination. Реальный write/flush failure
остаётся ошибкой независимо от прогноза. Новое поколение полностью перечитывается
и проверяется до удаления старого. Во время final merge дополнительный полный файл
не создаётся. Случай одной run использует тот же последовательный buffered reader.
Размеры буферов и cap fan-in — инженерные настройки; HDD/seeks performance ещё не
подтверждены большими прогонами.

### Выдача, состояния и cleanup

`finish(sink)` однократно передаёт ограниченные `span<const PointRecord>` в глобальном
порядке, затем возвращает `Result<CacheMetadata>`. Span действителен только до
возврата callback; sink может передавать записи в будущий Point Page builder,
накапливающий до `format::pointCapacity` для `encodePointPage()`. Sink должен
подготавливать результат временно: после выданного префикса возможны I/O failure,
отмена или отказ sink. Только успешный `finish` разрешает дальнейшую публикацию.
Пустой dataset не вызывает sink. Callback возвращает существующий `Status`;
его ошибка сохраняется, исключение переводится в `ConsumerFailed`.

Состояния: Accepting → Consuming → Succeeded либо Failed/Cancelled. Ошибка batch
может оставить принятый префикс внутри операции, но терминально запрещает его
продолжение/потребление и немедленно освобождает временные данные. После failure
повторные вызовы возвращают первую ошибку, после success — `InvalidState`.
Повторный вход из callback отклоняется без изменения выполняемой операции.
Metadata доступны как результат только при успехе; входной префикс не становится dataset.

Cancellation callback должен быть `noexcept`, его context живёт до завершения
операции. Checkpoints стоят между batches и блоками ingest, до/после локальной
сортировки, при run I/O, refill/output, verification и между поколениями, а также
перед терминальным успехом. Жёсткая latency не гарантируется. Sorter не создаёт
потоков и не интегрирован с OperationRunner.

При любом завершении уничтожаются буферы и файловые объекты, затем удаляется только
уникальный каталог операции. Отказ явного удаления на пути успеха — `CleanupFailed`.
После основного failure ошибка cleanup его не заменяет; destructor повторяет RAII
cleanup без возможности вернуть вторую ошибку. При запрете удаления ОС каталог может
остаться; crash recovery/scavenger не реализованы. Parent должен быть доверенным,
защита от подмены scratch-файлов сторонним процессом вне контракта.

## Что ещё не реализовано

Нет ASC/XYZ parser, финального streaming cache writer,
упаковки потока страниц, index builder/tree traversal, CacheReader/spatial queries,
полной dataset validation, PendingCache, application/OperationRunner integration,
project serialization, Density Map и Reduced Point Cloud. Данный формат не означает
завершения или принятия всей задачи 03.
