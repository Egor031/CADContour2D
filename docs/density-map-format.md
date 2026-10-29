# Density Map D v1

Нормативный самостоятельный binary format этапа 06.2. Это dataset identity,
а не identity версии Project State (06.4). Point Cache, source paths и manifest
для чтения не нужны. Экспериментальный `D06BCT01` несовместим и не принимается.

## Wire types и compatibility

Все integers unsigned, little-endian, фиксированной ширины. `f64` — исходные
IEEE-754 binary64 bits, включая signed zero. UUID — 16 RFC 4122 bytes, как CacheId;
native structs/enums/QUuid/padding не сериализуются. X быстрее Y, logical Y вверх.
Любое изменение layout требует новой format version. Неизвестные version,
обязательные flags, units, convention, codec, sample/build contract — UnsupportedFormat.
Reserved bytes обязаны быть нулями; нарушение — CorruptData.

CRC везде CRC-32/ISO-HDLC (reflected polynomial 0xedb88320, init 0xffffffff,
xorout 0xffffffff; zlib crc32 API с initial value 0; check `123456789` = cbf43926).
CRC поля header/footer при вычислении заменяются четырьмя нулями, остальные bytes
включаются. CRC index покрывает все его bytes без исключений; пустой index имеет
CRC 0. Decoded CRC покрывает packed LE samples, без padding. Это контроль случайного
повреждения, без криптографической аутентификации.

## Header (320 bytes)

| Offset | Size | Type | Значение |
|---:|---:|---|---|
| 0 | 8 | bytes | ASCII `PC2DDN01` |
| 8 | 4 | u32 | format version = 1 |
| 12 | 4 | u32 | header size = 320 |
| 16 | 4 | u32 | mandatory flags = 0 |
| 20 | 4 | u32 | units = 1 (mm) |
| 24 | 4 | u32 | mapping convention = 1 (06.1 Binary64SeparateRoundNearestEven) |
| 28 | 4 | u32 | storage tile side, 256 или 512 |
| 32 | 4 | u32 | compression = 1 (RFC 1950 zlib / Deflate) |
| 36 | 4 | u32 | writer compression policy = 6 |
| 40 | 4 | u32 | allowed samples bit mask = 3 (u32 и u64) |
| 44 | 4 | u32 | build contract: 1 SuppliedTiles, 2 CompletePointCache |
| 48 | 16 | UUID | non-null permanent dataset identity |
| 64 | 16 | UUID | non-null source CacheId |
| 80 | 8 | u64 | declared source measurements |
| 88, 96 | 8 each | f64 | original minimum X, Y |
| 104, 112 | 8 each | f64 | original maximum X, Y (не terminal boundary) |
| 120 | 8 | f64 | nominal cell |
| 128, 136 | 8 each | f64 | origin X, Y, bitwise равны соответствующему minimum |
| 144, 152 | 8 each | u64 | width, height |
| 160 | 8 | u64 | physical nonzero tile count T |
| 168 | 8 | u64 | checked total counts |
| 176 | 8 | u64 | payload offset = 320 |
| 184 | 8 | u64 | payload byte length |
| 192 | 8 | u64 | index offset = payload end |
| 200 | 8 | u64 | index byte length = T * 80 |
| 208 | 8 | u64 | footer offset = index end |
| 216 | 8 | u64 | footer length = 64 |
| 224 | 8 | u64 | exact file size = footer end |
| 232 | 4 | u32 | index CRC |
| 236 | 4 | u32 | header CRC |
| 240 | 4 | u32 | build contract version = 1 |
| 244 | 76 | bytes | reserved zero |

Contract 1 сохраняет предоставленные counts; их соответствие source measurements
не утверждается. Contract 2 дополнительно требует total == source measurements.
Он проверяет заявленную сумму, но достоверность измерений/распределения относится
к builder 06.3. Source measurements может быть 0. Timestamp не используется.

Reader один раз при open вызывает `raster::Mapping::create` с явной
ValidationPolicy и callback, затем сверяет dimensions/convention. Численные ошибки,
unsupported FP environment и resource refusal различаются. Formula bypass нет;
10 млн boundary checks — начальная caller policy, не storage dimension limit.

## Payload и sparse disk index

Payload — независимые zlib streams в порядке index, без gaps/padding. Каждая
запись описывает ровно действительный прямоугольник, включая partial edges.
Fully zero tiles отсутствуют. Никакого linear ordinal или требования
представимости width*height / tileRows*tileColumns нет.

Index содержит T фиксированных записей по 80 bytes, строго по (tileY, tileX).

| Offset | Size | Type | Значение |
|---:|---:|---|---|
| 0, 8 | 8 each | u64 | tileX, tileY |
| 16 | 8 | u64 | absolute payload offset |
| 24 | 8 | u64 | compressed length > 0 |
| 32 | 8 | u64 | raw length = actualWidth * actualHeight * sample bytes |
| 40 | 8 | u64 | checked tile total > 0 |
| 48 | 8 | u64 | tile maximum > 0 |
| 56 | 4 | u32 | decoded CRC |
| 60 | 4 | u32 | sample type: 1=u32, 2=u64 |
| 64, 68 | 4 each | u32 | actual width, height |
| 72 | 8 | bytes | reserved zero |

Key обязан быть в logical tile bounds. Shape вычисляется из проверенной сетки.
Первый payload начинается в 320, каждый следующий — точно в конце предыдущего,
последний — точно в index offset. Дубликаты/неправильный порядок запрещены.
Все размеры/offsets проверяются до арифметики, allocations и seeks, включая
qint64/size_t/zlib widths. Compressed length не больше compressBound(expected raw).
Encoding канонический: u32 iff maximum <= UINT32_MAX, иначе u64; никаких narrowing,
float, saturation. Sum/max/encoding сверяются с decoded values. UINT64_MAX допустим
в одной cell; overflow любой суммы — явный отказ.

## Completion footer (64 bytes)

| Offset | Size | Type | Значение |
|---:|---:|---|---|
| 0 | 8 | bytes | ASCII `PC2DEND1` |
| 8 | 4 | u32 | version = 1 |
| 12 | 4 | u32 | flags = 0 |
| 16 | 8 | u64 | exact file size |
| 24 | 8 | u64 | T |
| 32 | 8 | u64 | total |
| 40, 44 | 4 each | u32 | header CRC, index CRC |
| 48 | 4 | u32 | footer CRC |
| 52 | 12 | bytes | reserved zero |

Отсутствие/укорочение completion — IncompleteDataset. Наличие finalPath ничего не
доказывает. Лишние bytes, несогласованные sections, metadata или footer — отказ.
Zero-only dataset: payload/index length 0, T=total=0, размер 384 bytes.

## Integrity и ownership API

Open читает header/footer и весь structural index O(T), без payload, проверяет
CRCs/порядок/shape/offsets/checked totals и mapping. Только после этого корректный
отсутствующий key означает zero. ReadTile читает один payload, строго требует один
завершённый stream, ровно ожидаемый decoded size и полное потребление compressed
bytes, запрещает dictionary/trailing/concatenation; проверяет decoded CRC/max/total.
FullValidate обходит только physical tiles и проверяет те же payload invariants,
canonical omission и общий count/provenance contract. Ошибка не выдаёт partial tile.

Reader владеет read-only handle, но не файлом. Его owner сохраняет immutable файл.
Concurrent/reentrant operations одного reader дают InvalidState; metadata/statistics
нельзя читать одновременно с операцией. Независимые readers имеют независимые handles.
Произвольная внешняя модификация после open не гарантированно обнаруживается.
Metadata/mapping references живут до уничтожения reader; readTile передаёт caller
владение vector результата, удерживаемые caller результаты не входят в owned budget
следующей операции reader. Streaming callback заимствует key/shape/counts только до возврата. Его отказ
возвращается consumer как failure; ранее переданный prefix не откатывается.

Writer single-use, non-reentrant, без ProjectState. addTile заимствует counts только
на время вызова; caller сохраняет их неизменными до возврата, включая cancellation
callbacks. Writer принимает keys в любом порядке, в том числе zero submissions.
Дубликаты (включая zero) выявляются при canonical sorting в finish и терминальны.
Finish: spool → bounded initial runs → two-way disk merge → canonical payload/index →
flush → open/full readback → non-overwriting rename → move-only PendingDataset.
Pending discard удаляет только собственный результат; releaseToOwner передаёт
ответственность caller без application Accept. Failure/cancel/resource refusal
терминальны; повторные вызовы дают InvalidState. Reentrant rejection не меняет
состояние внешней операции. Cleanup errors передаются отдельному diagnostic callback,
не заменяя первопричину. Callback/context должны жить до destruction owner/pending,
быть noexcept и не вызывать операции владельца.

## ResourcePolicy

Политика задаётся явно, начальные значения — caller examples, не geometry limits.
Ноль никогда не unlimited: buffer/codec/sort/handle/work/mapping budgets отказывают
при первой необходимой расходной единице; scratch/output zero запрещают создание
writer, index validation zero разрешает только T=0. Production reader использует
одну stack descriptor и вообще не выделяет index cache (его расход всегда ноль;
отдельного настраиваемого cache budget пока нет).

Buffer budget учитывает simultaneously owned decoded u64, packed bytes, compressed
bound и I/O buffer. Codec allocations идут через checked zalloc/zfree с отдельным
hard budget; sort array ограничен отдельным budget и максимумом 4096 записей на run.
Fixed owner/Qt/allocator bookkeeping не является OS memory cap. Borrowed caller tile
не включён. Scratch/output budgets — logical file bytes; combined disk учитывает
spool + два descriptor tapes + candidate одновременно; physical allocation, NTFS
metadata, Qt runtime и process-wide handles не выдаются за внутренний accounting.
Writer требует 5 dataset handles (spool, две tapes, candidate, readback reader),
reader — 1. Scratch и final output ограничены отдельно, combined disk — дополнительно.
Index records и прочая work (codec/I/O/sample blocks/sort/merge checkpoints) имеют
отдельные budgets на операцию; writer work накопителен до finish с учётом readback.
Mapping budget передаётся factory отдельно, не расширяется скрыто.

Cancellation проверяется на входе, каждые <=4096 samples, между bounded codec/I/O
chunks, runs/merge records, index records, readback и непосредственно перед success.
Callback noexcept; FP-контракт mapping сохраняется. Wall-clock deadline не обещается.
Flush не является гарантией power-loss durability. Filesystem crash recovery,
hostile concurrent mutation и application persistence остаются вне 06.2.
