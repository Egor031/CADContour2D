# Point Cache Binary Format

## Статус и область

Внутренний предварительный формат **v1**, реализация 4/5 задачи 03. Не заморожен:
итоговая нагрузочная приёмка и сравнение производительности остаются этапу 5/5.
Обоснованные изменения layout до завершения задачи 03 требуют согласованного
обновления этого документа, codec и тестов. Опубликованных версий и миграций пока нет.

`cadcontour_cache` зависит только от `cadcontour_core` и Qt Core. Здесь реализованы
типы, codec отдельных элементов, проверяемый layout, точный файловый I/O и протокол
публикации файла, streaming ingest/external sort, CacheWriter, packed BBox hierarchy
и PendingCache, CacheReader со spatial queries и полной validation. Интеграции
с project state пока нет. Небольшие файлы базовых файловых тестов имеют собственную
тестовую сигнатуру и не являются datasets.

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
Поля описывают packed BBox hierarchy, построенную по Morton-порядку sorter.
Построение описано ниже; ранняя проверка выборочности I/O описана в
[cache-reader-check.md](cache-reader-check.md). Итоговой нагрузочной приёмки ещё нет.

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

CacheWriter заполняет все point pages кроме последней полностью, что
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

Leaves level = 1 ссылаются на Points; level ≥ 2 — на Index. Writer проверяет
согласованность своего дерева при sealing, используя descriptors записанных point
pages и readback index pages. Полная validation произвольного стороннего dataset
выполняется Reader. Codec одной entry/page не проверяет согласованность дерева.

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
records произвольного большого файла выполняется Reader; ограниченная
проверка собственного результата writer описана ниже.

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
   только читать staged device, проверять свои I/O и возвращать structured result без
   исключений. Перегрузка с `ContextValidator` синхронно заимствует caller context для
   рабочих буферов, cancellation и cleanup собственных scratch-файлов перед rename;
   исходная перегрузка без context сохраняется.
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
возврата callback; sink может передавать записи в Point Page builder CacheWriter,
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

## CacheWriter, BBox hierarchy и PendingCache (3/5)

### API и поток страниц

`CacheWriter::create(finalPath, limits, temporaryParent, cancel, context)` создаёт
одну headless операцию. `append(span<const Measurement>)` передаёт batches
существующему PointStreamSort; ID, Morton key и исходная metadata принадлежат sorter.
`finish()` однократно возвращает `Result<PendingCache>`. Дополнительной сортировки
или полного файла отсортированных точек нет.

Sink накапливает до 2726 PointRecord, вызывает `encodePointPage()` и пишет страницу
в StagedFile. Непустая последняя страница записывается после успешного завершения
sorter. Point ordinals идут от 0, все страницы кроме последней полны. Фактические
bounds выбираются по исходным binary64 с явными правилами signed zero, как в ingest;
нет epsilon, квантования или расширения bounds. Count и побитовый BBox всего
выданного потока сравниваются с metadata успешного `PointStreamSort::finish()`.
Ошибка sorter после последнего sink, включая cleanup, запрещает публикацию.

Для каждой записанной point page сохраняется descriptor: 64 байта существующего
encoded IndexEntry и 32 байта SHA-256 этих 64 байт. Это приватный scratch framing,
не новое поле v1. Один unbuffered `points.desc` в уникальном каталоге содержит
ровно `96 * pointPages` байт; длина, digest, ordinal, kind и count проверяются.

### Построение и размещение уровней

Leaf pages объединяют последовательные группы до 1022 point descriptors. Все
листья записываются подряд в index section с level=1. Следующий проход читает
предыдущий уровень из уже записанных index pages: bounds и checked sum counts
каждой страницы становятся descriptor для родителя. Полного массива descriptors
и дополнительных файлов уровней нет. Один буфер страницы переиспользуется для
чтения дочерней страницы и записи родительской; bounded массив entries удерживает
только одну родительскую группу.

Уровни следуют снизу вверх, ordinals index section непрерывны от 0. В каждой
группе children идут в исходном порядке; root — последняя index page. Height равен
root level. Bounds родителя — точное объединение дочерних bounds, pointCount —
checked sum исходных записей. Повторять проходы нужно до единственного root.
Для пустого dataset страниц и root нет; layout/header/footer остаются допустимыми.
Это текущий инженерный вариант packed hierarchy; сравнение эффективности индексов
и HDD performance ещё не выполнено.

### Совместный ресурсный бюджет

Пусть `M/T/H` — caller memoryBytes/temporaryDiskBytes/maxOpenFiles, а `p` — сумма
длин absolute final path и absolute temporary parent в UTF-16 units. Writer резервирует:

```text
W = 65536 + 65536
    + max(2726 * sizeof(PointRecord), 1022 * sizeof(IndexEntry)) + 16*p
sorter.memoryBytes        = M - W
writer.temporaryDiskBytes = floor(T/2)
sorter.temporaryDiskBytes = T - floor(T/2)
sorter.maxOpenFiles       = H - 2
```

Первая часть W — reserve для control objects, ограниченного стека, путей и
служебных расходов; вторая — единственный encoded page buffer. На текущем x64
`sizeof(PointRecord)=24`, `sizeof(IndexEntry)=56`; phase-specific массивы имеют
соответственно 65424 и 57232 байта. Point array и sorter уничтожаются до allocation
index array. Validation использует те же page/entries buffers. Размер массива
не зависит от общего количества точек, страниц или уровней.

Бюджет sorter дополнительно включает его собственный fixed reserve, описанный
выше. Слишком малые M/T или H<4 отклоняются. Во время сортировки одновременно
открыты staged cache, descriptor file и до двух runs; при index/sealing — не более
двух файлов. Directory не удерживает постоянный file handle.

Перед каждым ростом проверяются checked sizes, qint64 offsets и сумма логических
длин cache+descriptors в writer half; sorter отдельно ограничивает source+destination
другой половиной. Финальный cache учитывается, пока существуют runs. Деление
консервативное: свободный бюджет одной половины не заимствуется другой. Фактическая
запись/flush могут отказать независимо от планового бюджета. Нехватка свободного
места обрабатывается как I/O failure; предварительный snapshot свободного места
не гарантировал бы успех последующих записей.
Filesystem metadata/allocation granularity, входные batches, общий Qt/runtime и
файловый кэш ОС не являются контролируемыми рабочими allocations. W и тестовые
счётчики не означают измеренный peak RSS или жёсткий лимит памяти процесса.

### Sealing, validation и владение

Состояния writer: Accepting → Finishing → Succeeded либо Failed/Cancelled.
Повторный вход отклоняется без изменения выполняемой операции. После failure
сохраняется первая ошибка; после success дальнейшие append/finish дают InvalidState.
bad_alloc преобразуется в ResourceExhausted, в том числе внутри sink.

После успешной сортировки/индексации writer записывает окончательные header и
completion footer через существующие codecs/calculateLayout. Затем
`StagedFile::publish(expectedSize, validator, context)` проверяет flush и точный
размер. Validator перечитывает header/footer, вызывает validateEnvelope и сравнивает
идентичность/metadata с исходным потоком. Все index pages проверяются по framing,
checksum/padding, count, ordinal и level. Leaves сравниваются с checksummed point
descriptors и ожидаемой вместимостью исходных страниц. Parent entries сравниваются
с readback summaries children. Проверяются непрерывное покрытие каждого уровня,
отсутствие пропусков/дубликатов/циклов, section ranges, root, bounds и subtree counts.
Корректный SHA-256 сам по себе не заменяет эти проверки.

Validator не перечитывает все point pages: он проверяет собственный результат
writer по descriptors сформированных страниц. Это не общий validator произвольного
cache и не проверка случайного повреждения всех point payloads после записи.
Независимые headless tests дополнительно читают все records небольших datasets,
проверяют IDs, биты X/Y, порядок и BBox, затем обходят дерево отдельным DFS.

Cancellation проверяется при выдаче потока, на границах страниц, каждого child/
группы/уровня, sealing/readback и непосредственно перед завершением validator.
После успешной validation scratch закрывается и удаляется до rename; отказ удаления
даёт CleanupFailed. Используется прежний один QTemporaryFile::rename без overwrite
и copy fallback. Synchronous I/O и rename не имеют гарантированной latency отмены;
успешный завершающий gate является границей публикации, отмена после него не
отменяет состоявшийся success. Гарантии при отключении питания не добавлены.

Владение непрерывно: StagedFile → PublishedFile → move-only PendingCache.
PendingCache предоставляет path/metadata/index/layout; уничтожение означает
Discard. `releaseToOwner()` передаёт путь принятому владельцу через существующий
PublishedFile, отключает autoRemove без allocations или большого I/O. Повторная
передача и moved-from объект имеют пустой path. Долговременное удаление принятого
dataset и application publication gate здесь не реализованы.

При failure/cancellation PendingCache не возвращается, временные handles/буферы
освобождаются, удаления не меняют первую ошибку. Если удаление scratch sorter
блокирует ОС, writer сохраняет его cleanup-владение до destructor retry; собственный
descriptor directory также остаётся под RAII. Если блокировка сохраняется при
уничтожении владельцев или процесс аварийно завершён, временные файлы могут остаться;
recovery/scavenger относится к дальнейшим задачам.

## CacheReader и полная validation (4/5)

### API, открытие и владение

`CacheReader::open(path, ResourceLimits, cancel, context)` возвращает
`Result<unique_ptr<CacheReader>>`. Metadata/index/layout доступны только у успешно
открытого объекта. Reader владеет одним `QFile` в ReadOnly | Unbuffered, encoded
page buffer и рабочими массивами. Он не владеет удалением dataset, не вызывает
`PendingCache::releaseToOwner()` и не запускает потоки. Владелец файла обеспечивает
его неизменность и время жизни. Несколько Reader могут открыть тот же файл, в том
числе до application Accept. Leases и защита от внешней подмены не реализуются.

Быстрое открытие проверяет реальный размер, оба header subblocks, footer и
`validateEnvelope()`. Из pointPages вычисляется форма существующей packed hierarchy:
число страниц каждого уровня, начало уровня и его children. Height/pageCount/root
должны ей соответствовать; root — последняя index page. Это вычисление по числам,
а не чтение index section. Затем проверяется root: framing/checksum, kind, ordinal,
level, count, children и агрегированные bounds/counts относительно metadata.
Для пустого dataset root не читается. Открытие читает 8192 байта envelope и одну
65536-байтную root page для непустого dataset, **ни одной point page**.

Успешное открытие не подтверждает исправность непрочитанных страниц. Обычный scan
проверяет использованные страницы; полная validation проверяет весь dataset.
Изменение файла другим процессом во время чтения нарушает immutable-контракт;
Reader не обещает согласованный snapshot такого изменения.

### Scans и ошибки

`scanAll(sink, cancel, context)`, `scanBox(box, sink, cancel, context)` и
`scanBoxes(span<const BoundingBox>, sink, cancel, context)` возвращают
`Result<ReadStatistics>`. Sink имеет существующий тип
`std::function<Status(span<const PointRecord>)>`. Внутренний batch содержит до
2726 записей; span действителен только внутри callback. Ошибка Status сохраняется
без переклассификации, любое исключение sink становится ConsumerFailed. Внутренний
bad_alloc становится ResourceExhausted. Отказ, исключение или cancellation могут
следовать после выданного префикса; rollback выдачи отсутствует. Потребитель нового
dataset публикует его только после успешного завершения scan.

Атомарный operation guard отклоняет concurrent/reentrant scan/validation с
InvalidState до доступа к mutable state. Ошибка callback и обычная отмена не
делают Reader терминальным. Следующий запрос сбрасывает счётчики и device error,
заново выполняет checked seeks и проверки страниц. После I/O/corruption failure
это также разрешено, но success возможен лишь при реально исправном доступном
файле. Reader ничего не исправляет. Уничтожать Reader во время активного вызова
нельзя. Sink/cancel contexts и входной span областей заимствуются на время вызова;
caller не должен менять области во время обхода.

Все query bounds валидируются до чтения/выдачи. Требуются конечные значения и
min <= max по обеим осям. Принадлежность точная и inclusive, без epsilon,
расширения или изменения X/Y. Вырожденные rectangles и пустая union допустимы.
Повторные measurements различаются по ID, а перекрывающиеся/repeated rectangles
не дублируют выдачу. scanAll выдаёт физический порядок point section. Пространственный
DFS идёт по children в том же порядке, выдавая подпоследовательность scanAll.

При чтении проверяются codec страницы и ожидаемые kind/ordinal/level, checked
offset/range, полнота point pages кроме последней, ID < N и containment координат
в metadata. Индекс дополнительно проверяет packed child ordinals, вместимость
группы, leaf counts, checked subtree sums и точные bounds относительно parent.
Point page до callback сверяется с её parent descriptor, включая bounds по битам
и signed-zero extrema. Непересекающиеся поддеревья не читаются; полная проверка
отсечённых bounds не подразумевается. Fallback на scanAll при ошибке индекса нет.

### Обход и ресурсы

Один итеративный DFS обслуживает всю union. Frame хранит одну декодированную index
page на активный уровень; encoded buffer общий для всех чтений. Packed grouping
задаёт единственного parent каждой страницы, исключает повторный child и пропуски
без visited set. Уровень строго уменьшается при спуске. Проверка формы ограничена
64 slots; в qint64 layout v1 реально возможны не более пяти уровней. Повреждённый
height не управляет allocations до проверки формы. Полный index в RAM не читается.
Проверка membership/intersection линейна по числу входных rectangles, без planner.

Для `p = absolute dataset path length` в UTF-16 units и height `h` open резервирует:

```text
R = 65536 + 65536 + 2726*sizeof(PointRecord) + h*sizeof(Frame) + 16*p
```

Первая часть — control/stack/Qt/path reserve, вторая — encoded buffer.
На текущем x64 `sizeof(Frame)=57240`, `sizeof(PointRecord)=24`: рабочие массивы
составляют 130960 байт при h=0, 188200 при h=1, 245440 при h=2. Ни query, ни
scanAll не выделяют массив результата или массив размера N. Требуются ненулевой
memory budget >= R и минимум один файл; temporaryDiskBytes для open/scans не нужен.
Это бюджет рабочих allocations, не предел RSS, памяти Qt/runtime или OS file cache.

`ReadStatistics` относится к текущему успешному вызову, без предшествующего open.
Он содержит прочитанные index/point pages, успешно переданные байты readExact,
число вызовов readExact/seek, число успешно выданных записей, размер постоянных
рабочих массивов Reader и число frames. Это логические операции, не syscalls и
не физические HDD seeks. Validation включает envelope I/O; её returnedRecords
считает записи, переданные внутреннему ID-проверяющему sink. bufferBytes не включает
временный массив validation и память sorter.

### Строгая validation произвольного файла

После успешного open вызывается `validate(totalLimits, temporaryParent, cancel,
context)`. Она повторно читает envelope, сравнивает с metadata открытия и обходит
всё дерево без пространственного отсечения. Все pages, references и локальные
инварианты проверяются; packed coverage и итоговые числа посещённых point/index
pages доказывают полную достижимость, отсутствие cycles/reuse/пропусков. Сравнение
каждого parent descriptor с фактическим child summary связывает subtree counts
и bounds с реальными records вплоть до root/metadata, включая signed zero.

Для строгой проверки ID используется существующий PointStreamSort с локальными
искусственными measurements `{double(id >> 32), double(id & UINT32_MAX), 0}`.
Обе половины точно представимы в binary64. Отображение ID в такую пару и её Morton
key инъективно; исходные ID здесь не заменяются ID, назначенными sorter. После
сортировки одинаковые исходные ID обязательно соседние, в том числе через границы
sink batches. Отсутствие повторов, N записей и ранее проверенное `0 <= id < N`
строго доказывают полноту диапазона 0..N-1. Сумма/XOR/min/max для этого не используются.
Source records/координаты и алгоритм production Morton sorting не изменяются.

Для `t = absolute temporary parent length` validation резервирует
`V = R + 2726*sizeof(Measurement) + 16*t`; sorter получает `M - V` памяти,
весь temporaryDiskBytes и `H - 1` файлов. Непустая validation требует минимум
трёх файлов в общем бюджете. Sorter отдельно учитывает собственный control reserve,
run buffers, source+destination disk budget и bounded merge fan-in. Scratch entry
занимает 40 байт, header каждой run — 32; при merge одновременно могут существовать
два поколения. Пустой dataset требует только R/один handle, без scratch.

Scratch создаётся только внутри уникального каталога текущей validation и удаляется
sorter через прежний RAII/cleanup protocol. Нехватка бюджета — structured failure;
фактический I/O отказ также запрещает success. CleanupFailed на пути успеха
сохраняется; после основной ошибки cleanup её не подменяет. Если ОС запрещает
удаление до уничтожения операции, scratch может остаться, как и у writer/sorter.
Dataset во всех случаях открыт только для чтения и не изменяется/не удаляется.
Cancellation проверяется на границах страниц/children/batches и внутри external
sort, а также перед success; гарантированной latency синхронного I/O нет.

Проверки и ранние измерения: [cache-reader-check.md](cache-reader-check.md).

## Что ещё не реализовано

Нет ASC/XYZ parser, application/OperationRunner integration,
project serialization, Density Map и Reduced Point Cloud. Данный формат не означает
завершения или принятия всей задачи 03.
