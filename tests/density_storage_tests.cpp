#include "density/density_storage.h"

#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <zlib.h>
#include <bit>
#include <cfenv>
#include <functional>
#include <numeric>
#define NOMINMAX
#include <Windows.h>

namespace cadcontour::density {
class DensityStorageTestAccess {
public:
    static void hook(Writer &w, Status (*h)(Phase, void *) noexcept, void *c) { w.setFaultHook(h, c); }
};
}
namespace {
using namespace cadcontour;
using namespace cadcontour::density;
using B = std::vector<unsigned char>;
// Independent LE decoder/encoder and bitwise CRC: no production codec/layout helpers.
U64 get(const B &b, std::size_t p, unsigned bytes = 8)
{
    U64 value = 0; for (unsigned i = 0; i < bytes; ++i) value |= U64(b.at(p + i)) << (i * 8); return value;
}
void put(B &b, std::size_t p, U64 v, unsigned bytes = 8)
{ for (unsigned i = 0; i < bytes; ++i) b.at(p + i) = static_cast<unsigned char>(v >> (i * 8)); }
std::uint32_t checksum(const B &b)
{
    std::uint32_t v = 0xffffffffU;
    for (auto x : b) { v ^= x; for (int i = 0; i < 8; ++i) v = (v >> 1) ^ ((v & 1) ? 0xedb88320U : 0); }
    return v ^ 0xffffffffU;
}
B slice(const B &b, std::size_t p, std::size_t n) { return {b.begin() + p, b.begin() + p + n}; }
void copy(B &b, std::size_t p, const B &v) { std::copy(v.begin(), v.end(), b.begin() + p); }
void magic(B &b, std::size_t p, const char *s) { std::copy_n(s, 8, b.begin() + p); }
B zip(const B &b)
{
    uLongf n = compressBound(static_cast<uLong>(b.size())); B out(n);
    if (compress2(out.data(), &n, b.data(), static_cast<uLong>(b.size()), 6) != Z_OK) throw std::runtime_error("fixture compression");
    out.resize(n); return out;
}
struct Input { TileKey key; std::vector<U64> values; };
struct Spec {
    U64 width = 257, height = 257;
    unsigned side = 256;
    double minX = -0.0, minY = 0.0, cell = 1.0;
    double maxX = 257, maxY = 257;
    unsigned contract = 1;
    U64 source = 123;
    std::vector<Input> inputs;
};
Shape dimensions(const Spec &s, TileKey k)
{ return {static_cast<unsigned>(std::min<U64>(s.side, s.width - k.x * s.side)), static_cast<unsigned>(std::min<U64>(s.side, s.height - k.y * s.side))}; }
Input input(const Spec &s, TileKey key, U64 value)
{
    auto d = dimensions(s, key); Input i{key, std::vector<U64>(std::size_t(d.width) * d.height)}; i.values.front() = value; return i;
}
Spec mixed()
{
    Spec s; s.inputs.push_back(input(s, {0, 0}, 7)); s.inputs.push_back(input(s, {1, 1}, U64(UINT32_MAX) + 1)); return s;
}
void repair(B &b)
{
    // Physical fixture sections stay put when testing forged header offsets.
    const auto footer = b.size() - 64, indexLength = static_cast<std::size_t>(get(b, 200));
    const auto index = footer - indexLength;
    put(b, 232, checksum(slice(b, index, indexLength)), 4); put(b, 236, 0, 4); put(b, 236, checksum(slice(b, 0, 320)), 4);
    put(b, footer + 16, get(b, 224)); put(b, footer + 24, get(b, 160)); put(b, footer + 32, get(b, 168));
    put(b, footer + 40, get(b, 236, 4), 4); put(b, footer + 44, get(b, 232, 4), 4);
    put(b, footer + 48, 0, 4); put(b, footer + 48, checksum(slice(b, footer, 64)), 4);
}
B fixture(const Spec &s)
{
    B b(320), index;
    U64 total = 0, tiles = 0;
    for (const auto &i : s.inputs) {
        const U64 max = *std::max_element(i.values.begin(), i.values.end()); if (!max) continue;
        const unsigned bytes = max > UINT32_MAX ? 8 : 4; const auto d = dimensions(s, i.key);
        B raw(i.values.size() * bytes), entry(80); U64 sum = 0;
        for (std::size_t n = 0; n < i.values.size(); ++n) { put(raw, n * bytes, i.values[n], bytes); sum += i.values[n]; }
        const auto payload = zip(raw);
        put(entry, 0, i.key.x); put(entry, 8, i.key.y); put(entry, 16, b.size()); put(entry, 24, payload.size()); put(entry, 32, raw.size());
        put(entry, 40, sum); put(entry, 48, max); put(entry, 56, checksum(raw), 4); put(entry, 60, bytes == 4 ? 1 : 2, 4);
        put(entry, 64, d.width, 4); put(entry, 68, d.height, 4);
        b.insert(b.end(), payload.begin(), payload.end()); index.insert(index.end(), entry.begin(), entry.end()); total += sum; ++tiles;
    }
    const U64 indexOffset = b.size(); b.insert(b.end(), index.begin(), index.end()); const U64 footer = b.size(); b.resize(b.size() + 64);
    magic(b, 0, "PC2DDN01"); put(b, 8, 1, 4); put(b, 12, 320, 4); put(b, 20, 1, 4); put(b, 24, 1, 4);
    put(b, 28, s.side, 4); put(b, 32, 1, 4); put(b, 36, 6, 4); put(b, 40, 3, 4); put(b, 44, s.contract, 4);
    for (unsigned j = 0; j < 16; ++j) { b[48 + j] = static_cast<unsigned char>(j + 1); b[64 + j] = static_cast<unsigned char>(32 + j); }
    put(b, 80, s.source);
    for (const auto [offset, v] : {std::pair{88, s.minX}, {96, s.minY}, {104, s.maxX}, {112, s.maxY}, {120, s.cell}, {128, s.minX}, {136, s.minY}})
        put(b, offset, std::bit_cast<U64>(v));
    put(b, 144, s.width); put(b, 152, s.height); put(b, 160, tiles); put(b, 168, total); put(b, 176, 320);
    put(b, 184, indexOffset - 320); put(b, 192, indexOffset); put(b, 200, index.size()); put(b, 208, footer); put(b, 216, 64); put(b, 224, b.size()); put(b, 240, 1, 4);
    magic(b, footer, "PC2DEND1"); put(b, footer + 8, 1, 4); repair(b); return b;
}
void write(const QString &path, const B &b)
{
    QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(f.write(reinterpret_cast<const char *>(b.data()), static_cast<qint64>(b.size())), b.size());
}
B read(const QString &path)
{
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; auto b = f.readAll(); return {b.begin(), b.end()};
}
Metadata metadata(const Spec &s)
{
    auto m = raster::Mapping::create({s.minX, s.minY}, {s.maxX, s.maxY}, s.cell, raster::ValidationPolicy{10'000'000});
    if (!m) throw std::runtime_error("test mapping");
    DatasetId id; cache::CacheId source; for (unsigned j = 0; j < 16; ++j) { id.bytes[j] = std::byte(j + 1); source.bytes[j] = std::byte(32 + j); }
    return {id, source, s.source, s.contract == 1 ? BuildContract::SuppliedTiles : BuildContract::CompletePointCache, s.side, m.value()};
}
struct D : testing::Test {
    QTemporaryDir dir;
    QString path() const { return dir.filePath("result.dmap"); }
    ResourcePolicy policy = initialResourcePolicy();
    Result<std::unique_ptr<Reader>> open() { return Reader::openReader(path(), policy); }
    Result<std::unique_ptr<Writer>> writer(const Spec &s) { return Writer::create(path(), metadata(s), policy, dir.path()); }
    void clean() { EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty()); }
};
TEST_F(D, IndependentMixedFixtureAndSelectiveZero)
{
    const auto s = mixed(); write(path(), fixture(s)); auto r = open(); ASSERT_TRUE(r);
    EXPECT_EQ(r.value()->statistics().decodedTiles, 0); EXPECT_EQ(r.value()->statistics().readBytes, 384 + 160);
    for (const auto &i : s.inputs) { auto t = r.value()->readTile(i.key); ASSERT_TRUE(t); EXPECT_EQ(t.value().counts, i.values); }
    const auto before = r.value()->statistics().decodedTiles;
    auto zero = r.value()->readTile({1, 0}); ASSERT_TRUE(zero);
    EXPECT_TRUE(std::all_of(zero.value().counts.begin(), zero.value().counts.end(), [](U64 v) { return v == 0; }));
    EXPECT_EQ(r.value()->statistics().decodedTiles, before); EXPECT_TRUE(r.value()->fullValidate());
    auto invalid = r.value()->readTile({2, 0}); ASSERT_FALSE(invalid); EXPECT_EQ(invalid.error().code, ErrorCode::InvalidInput);
}
class Count : public D, public testing::WithParamInterface<U64> {};
TEST_P(Count, IndependentFixtureAndWriterByteOracle)
{
    Spec s; s.width = s.height = 1; s.maxX = s.maxY = 0; s.inputs.push_back(input(s, {0, 0}, GetParam()));
    write(path(), fixture(s)); auto r = open(); ASSERT_TRUE(r); auto tile = r.value()->readTile({0, 0}); ASSERT_TRUE(tile); EXPECT_EQ(tile.value().counts[0], GetParam()); r.value().reset();
    ASSERT_TRUE(QFile::remove(path())); auto w = writer(s); ASSERT_TRUE(w); ASSERT_TRUE(w.value()->addTile({0, 0}, {1, 1}, s.inputs[0].values));
    auto pending = w.value()->finish(); ASSERT_TRUE(pending);
    // Full bytes equal independent generator, including UUIDs, metadata and all CRCs.
    EXPECT_EQ(read(path()), fixture(s));
}
INSTANTIATE_TEST_SUITE_P(Exact, Count, testing::Values(U64(0), U64(1), U64(65535), U64(65536), U64(UINT32_MAX), U64(UINT32_MAX) + 1, UINT64_MAX));
TEST_F(D, AllPartialEdgesAndUnsortedConstructionGroups)
{
    for (unsigned side : {256U, 512U}) {
        Spec s; s.side = side; s.width = side * 2 + 1; s.height = side * 2 + 3; s.maxX = double(s.width); s.maxY = double(s.height);
        for (U64 y = 0; y < 3; ++y) for (U64 x = 0; x < 3; ++x) {
            if (x == 1 && y == 1) continue;
            auto t = input(s, {x, y}, 1 + x + 3 * y); t.values.back() += 17;
            if(t.values.size()>1)t.values[1]+=5;
            const auto d=dimensions(s,{x,y}); if(d.height>1)t.values[d.width]+=23;
            s.inputs.push_back(std::move(t));
        }
        auto w = writer(s); ASSERT_TRUE(w);
        // Group-major submissions for 512 construction groups / 256 storage tiles
        // are intentionally not global row-major; reverse forces sorting too.
        for (auto i = s.inputs.rbegin(); i != s.inputs.rend(); ++i) ASSERT_TRUE(w.value()->addTile(i->key, dimensions(s, i->key), i->values));
        auto p = w.value()->finish(); ASSERT_TRUE(p); EXPECT_EQ(read(path()), fixture(s));
        auto r = open(); ASSERT_TRUE(r); EXPECT_TRUE(r.value()->fullValidate());
        for (const auto &i : s.inputs) { auto t = r.value()->readTile(i.key); ASSERT_TRUE(t); EXPECT_EQ(t.value().counts, i.values); }
    }
}
TEST_F(D, Binary64MetadataOriginalMaximumAndSignedZero)
{
    Spec s; s.width = 3; s.height = 1; s.cell = 0.1; s.maxX = 3 * s.cell; s.maxY = 0;
    auto w = writer(s); ASSERT_TRUE(w); auto p = w.value()->finish(); ASSERT_TRUE(p); EXPECT_EQ(read(path()), fixture(s));
    auto r = open(); ASSERT_TRUE(r); EXPECT_EQ(std::bit_cast<U64>(r.value()->mapping().minimum().x), std::bit_cast<U64>(-0.0));
    EXPECT_EQ(std::bit_cast<U64>(r.value()->mapping().maximum().x), std::bit_cast<U64>(s.maxX)); EXPECT_EQ(r.value()->mapping().width(), 3);
}
TEST_F(D, TotalOverflowAndCompleteProvenance)
{
    Spec s; auto w = writer(s); ASSERT_TRUE(w); auto a = input(s, {0, 0}, UINT64_MAX);
    ASSERT_TRUE(w.value()->addTile(a.key, dimensions(s, a.key), a.values)); auto b = input(s, {1, 1}, 1);
    auto failure = w.value()->addTile(b.key, dimensions(s, b.key), b.values); ASSERT_FALSE(failure); EXPECT_EQ(failure.error().code, ErrorCode::InvalidInput);
    EXPECT_FALSE(w.value()->finish()); clean();
    s.contract = 2; s.source = 0; w = writer(s); ASSERT_TRUE(w); auto p = w.value()->finish(); ASSERT_TRUE(p); EXPECT_TRUE(open());
}
TEST_F(D, WithinTileOverflowAndCountContractRefusal)
{
    auto s = mixed(); s.inputs[0].values[0] = UINT64_MAX; s.inputs[0].values[1] = 1;
    auto w = writer(s); ASSERT_TRUE(w); EXPECT_FALSE(w.value()->addTile({0, 0}, {256, 256}, s.inputs[0].values)); clean();
    s.contract = 2; s.source = 9; w = writer(s); ASSERT_TRUE(w); auto p = w.value()->finish(); ASSERT_FALSE(p); EXPECT_EQ(p.error().code, ErrorCode::InvalidInput); clean();
}
class Duplicate : public D, public testing::WithParamInterface<std::pair<U64, U64>> {};
TEST_P(Duplicate, ZeroAndNonzeroSubmissionsNeverMerge)
{
    Spec s; auto w = writer(s); ASSERT_TRUE(w);
    for (auto n : {GetParam().first, GetParam().second}) { auto i = input(s, {1, 1}, n); ASSERT_TRUE(w.value()->addTile(i.key, {1, 1}, i.values)); }
    auto p = w.value()->finish(); ASSERT_FALSE(p); EXPECT_EQ(p.error().code, ErrorCode::InvalidInput); clean();
}
INSTANTIATE_TEST_SUITE_P(All, Duplicate, testing::Values(std::pair<U64,U64>{0,0}, std::pair<U64,U64>{0,1}, std::pair<U64,U64>{1,0}, std::pair<U64,U64>{1,1}));

struct Mutation { const char *name; std::function<void(B &)> change; };
std::vector<Mutation> mutations()
{
    std::vector<Mutation> v;
    for (auto offset : {8,12,16,20,24,28,32,36,40,44,240}) v.push_back({"unsupported", [offset](B &b) { put(b, offset, 99, 4); repair(b); }});
    for (auto offset : {128,136,144,152,176,184,216,224,244}) v.push_back({"header invariant", [offset](B &b) { put(b, offset, get(b, offset) + 1); repair(b); }});
    v.push_back({"magic", [](B &b) { magic(b,0,"D06BCT01"); }});
    v.push_back({"header CRC", [](B &b) { b[236] ^= 1; }});
    v.push_back({"index CRC", [](B &b) { b[static_cast<std::size_t>(get(b,192))] ^= 1; }});
    v.push_back({"footer CRC", [](B &b) { b[b.size()-16] ^= 1; }});
    v.push_back({"footer missing", [](B &b) { b.resize(b.size()-64); }});
    v.push_back({"footer magic", [](B &b) { b[b.size()-64] ^= 1; repair(b); }});
    v.push_back({"footer flags", [](B &b) { put(b,b.size()-52,1,4); repair(b); }});
    v.push_back({"footer reserved", [](B &b) { b.back()=1; repair(b); }});
    v.push_back({"trailing", [](B &b) { b.push_back(0); }});
    v.push_back({"huge index count", [](B &b) { put(b,160,UINT64_MAX); repair(b); }});
    v.push_back({"index offset overflow", [](B &b) { put(b,192,UINT64_MAX); repair(b); }});
    v.push_back({"footer offset overflow", [](B &b) { put(b,208,UINT64_MAX); put(b,236,0,4); put(b,236,checksum(slice(b,0,320)),4); }});
    for (auto [field, value] : {std::pair{0, U64(2)}, {8,U64(2)}, {16,U64(319)}, {16,U64(321)}, {16,UINT64_MAX},
             {24,U64(0)}, {24,UINT64_MAX}, {32,U64(1)}, {40,U64(0)}, {48,U64(0)}, {64,U64(255)}, {72,U64(1)}})
        v.push_back({"index invariant", [field,value](B &b) { auto p=static_cast<std::size_t>(get(b,192)); put(b,p+field,value,field==64 ? 4 : 8); repair(b); }});
    v.push_back({"unsupported sample", [](B &b) { put(b,static_cast<std::size_t>(get(b,192))+60,9,4); repair(b); }});
    v.push_back({"duplicate key", [](B &b) { auto p=static_cast<std::size_t>(get(b,192)); put(b,p+80,0); put(b,p+88,0); repair(b); }});
    v.push_back({"unordered keys", [](B &b) { auto p=static_cast<std::size_t>(get(b,192)); put(b,p,1); put(b,p+8,1); put(b,p+80,0); put(b,p+88,0); repair(b); }});
    v.push_back({"count contract", [](B &b) { put(b,44,2,4); repair(b); }});
    v.push_back({"null dataset", [](B &b) { std::fill(b.begin()+48,b.begin()+64,0); repair(b); }});
    v.push_back({"null source", [](B &b) { std::fill(b.begin()+64,b.begin()+80,0); repair(b); }});
    v.push_back({"nonfinite cell", [](B &b) { put(b,120,0x7ff8000000000000ULL); repair(b); }});
    v.push_back({"collapsed cell", [](B &b) { put(b,120,0); repair(b); }});
    v.push_back({"aggregate overflow", [](B &b) { auto p=static_cast<std::size_t>(get(b,192)); put(b,p+40,UINT64_MAX); repair(b); }});
    v.push_back({"narrowed encoding", [](B &b) { auto p=static_cast<std::size_t>(get(b,192)); put(b,p+80+60,1,4); repair(b); }});
    return v;
}
class Corrupt : public D, public testing::WithParamInterface<unsigned> {};
TEST_P(Corrupt, RejectStructuralViolationsEvenWithRecomputedCRC)
{
    auto b = fixture(mixed()); const auto m = mutations().at(GetParam()); SCOPED_TRACE(m.name); m.change(b); write(path(),b); auto r = open(); EXPECT_FALSE(r);
}
INSTANTIATE_TEST_SUITE_P(Layout, Corrupt, testing::Range(0U,static_cast<unsigned>(mutations().size())));

TEST_F(D, DuplicateAndUnorderedIndexKeysWithOtherwiseValidShapesAndCRCs)
{
    Spec s; s.width=512; s.height=256; s.maxX=512; s.maxY=256;
    s.inputs={input(s,{0,0},7),input(s,{1,0},9)};
    for(bool duplicate:{true,false}) {
        auto b=fixture(s); const auto p=static_cast<std::size_t>(get(b,192));
        if(duplicate)put(b,p+80,0);
        else {put(b,p,1);put(b,p+80,0);}
        repair(b);write(path(),b);auto r=open();ASSERT_FALSE(r);EXPECT_EQ(r.error().code,ErrorCode::CorruptData);
        EXPECT_EQ(r.error().detail,QStringLiteral("Duplicate/unordered index key"));
    }
}

// Replace a single payload while independently repairing all layout/CRC records.
B replacePayload(B b, const B &payload)
{
    auto index = slice(b, static_cast<std::size_t>(get(b,192)), 80); auto h = slice(b,0,320); auto footer = slice(b,b.size()-64,64);
    b = h; b.insert(b.end(),payload.begin(),payload.end()); const auto p=b.size(); put(index,24,payload.size()); b.insert(b.end(),index.begin(),index.end());
    const auto f=b.size(); b.insert(b.end(),footer.begin(),footer.end()); put(b,184,payload.size()); put(b,192,p); put(b,208,f); put(b,224,b.size()); repair(b); return b;
}
class Payload : public D, public testing::WithParamInterface<unsigned> {};
TEST_P(Payload, OpenIsStructuralSelectiveAndFullDetectPayloadFailure)
{
    Spec s; s.width=s.height=1; s.maxX=s.maxY=0; s.inputs={input(s,{0,0},7)};
    auto b=fixture(s); auto compressed=slice(b,320,static_cast<std::size_t>(get(b,184)));
    switch (GetParam()) {
    case 0: compressed[2]^=1; break;
    case 1: compressed.pop_back(); break;
    case 2: compressed.push_back(0); break;
    case 3: { auto second=compressed; compressed.insert(compressed.end(),second.begin(),second.end()); break; }
    case 4: compressed=zip(B{7,0,0}); break;
    case 5: compressed=zip(B{7,0,0,0,0}); break;
    case 6: compressed=zip(B{8,0,0,0}); break;
    case 7: { // Valid RFC1950 stream requiring a preset dictionary.
        z_stream z{}; ASSERT_EQ(deflateInit(&z,6),Z_OK); unsigned char dict[]={7,0,0,0}; ASSERT_EQ(deflateSetDictionary(&z,dict,4),Z_OK);
        B out(64); z.next_in=dict; z.avail_in=4; z.next_out=out.data(); z.avail_out=64; ASSERT_EQ(deflate(&z,Z_FINISH),Z_STREAM_END);
        out.resize(z.total_out); deflateEnd(&z); compressed=out; break;
    }
    case 8: { auto p=static_cast<std::size_t>(get(b,192)); put(b,p+56,123,4); repair(b); break; }
    }
    if(GetParam()!=8) b=replacePayload(b,compressed); write(path(),b); auto r=open();
    // Concatenation/dictionary may already exceed compressBound and be refused structurally.
    if(!r) { EXPECT_TRUE(GetParam()==3 || GetParam()==7); return; }
    EXPECT_EQ(r.value()->statistics().decodedTiles,0); auto t=r.value()->readTile({0,0}); ASSERT_FALSE(t); EXPECT_EQ(t.error().code,ErrorCode::CorruptData);
    EXPECT_FALSE(r.value()->fullValidate());
}
INSTANTIATE_TEST_SUITE_P(Strict, Payload, testing::Range(0U,9U));

TEST_F(D, ConcatenationWithinCompressedBoundAndCanonicalPayloadSummaries)
{
    Spec s; s.inputs={input(s,{0,0},7)};
    for(unsigned scenario=0;scenario<5;++scenario) {
        auto b=fixture(s); auto p=static_cast<std::size_t>(get(b,192));
        if(scenario==0) {
            auto payload=slice(b,320,static_cast<std::size_t>(get(b,184))); const auto second=payload;
            payload.insert(payload.end(),second.begin(),second.end()); b=replacePayload(b,payload);
        } else if(scenario==1) { put(b,p+40,8); put(b,168,8); repair(b); }
        else if(scenario==2) { put(b,p+48,6); repair(b); }
        else if(scenario==3) { // All-zero decoded samples with a structurally nonzero descriptor.
            B raw(256*256*4); b=replacePayload(b,zip(raw)); p=static_cast<std::size_t>(get(b,192)); put(b,p+56,checksum(raw),4); repair(b);
        } else { // Valid decoded checksum with a changed sample, stale max/total.
            B raw(256*256*4); raw[0]=9; b=replacePayload(b,zip(raw)); p=static_cast<std::size_t>(get(b,192)); put(b,p+56,checksum(raw),4); repair(b);
        }
        write(path(),b); auto r=open(); ASSERT_TRUE(r); EXPECT_FALSE(r.value()->readTile({0,0})); EXPECT_FALSE(r.value()->fullValidate());
    }
}

TEST_F(D, LargeSparseAreaAndUint64KeyArithmeticWithoutOrdinal)
{
    const TileKey farKey{U64(UINT32_MAX) + 7, U64(UINT32_MAX) + 9};
    auto sh = tileShape(UINT64_MAX, UINT64_MAX, 512, farKey); ASSERT_TRUE(sh); EXPECT_EQ(sh.value(), (Shape{512,512}));
    auto last = tileShape(UINT64_MAX, UINT64_MAX, 256, {(UINT64_MAX-1)/256,(UINT64_MAX-1)/256}); ASSERT_TRUE(last); EXPECT_EQ(last.value(),(Shape{255,255}));
    EXPECT_FALSE(tileShape(UINT64_MAX,1,256,{UINT64_MAX,0})); EXPECT_FALSE(tileShape(0,1,256,{0,0}));
    Spec huge; huge.width=U64(256)*(U64(UINT32_MAX)+9); huge.maxX=double(huge.width); write(path(),fixture(huge));
    auto refused=open(); ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code,ErrorCode::ResourceRefused); ASSERT_TRUE(refused.error().mapping); EXPECT_GT(refused.error().required,10'000'000);
    ASSERT_TRUE(QFile::remove(path()));
    Spec s; s.width=1'000'003; s.height=1'000'001; s.maxX=double(s.width); s.maxY=double(s.height);
    s.inputs={input(s,{0,0},1),input(s,{(s.width-1)/256,(s.height-1)/256},9)};
    auto w=writer(s); ASSERT_TRUE(w); for(auto i=s.inputs.rbegin();i!=s.inputs.rend();++i) ASSERT_TRUE(w.value()->addTile(i->key,dimensions(s,i->key),i->values));
    auto p=w.value()->finish(); ASSERT_TRUE(p); EXPECT_EQ(read(path()),fixture(s)); auto r=open(); ASSERT_TRUE(r); EXPECT_EQ(r.value()->statistics().physicalTiles,2); EXPECT_TRUE(r.value()->fullValidate());
}
TEST_F(D, SortSpillsAndMergesUnderOneDescriptorBudget)
{
    Spec s; s.width=256*33; s.height=1; s.maxX=double(s.width); s.maxY=0; policy.sortBytes=96;
    for(U64 x=0;x<33;++x) s.inputs.push_back(input(s,{x,0},x+1));
    auto w=writer(s); ASSERT_TRUE(w); for(auto i=s.inputs.rbegin();i!=s.inputs.rend();++i) ASSERT_TRUE(w.value()->addTile(i->key,dimensions(s,i->key),i->values));
    auto p=w.value()->finish(); ASSERT_TRUE(p); EXPECT_EQ(read(path()),fixture(s)); EXPECT_GE(w.value()->statistics().mergePasses,5); EXPECT_LE(w.value()->statistics().sortReserve,policy.sortBytes);
}
using Limit = U64 ResourcePolicy::*;
const std::array<Limit,9> limits={&ResourcePolicy::bufferBytes,&ResourcePolicy::codecBytes,&ResourcePolicy::sortBytes,&ResourcePolicy::scratchBytes,
    &ResourcePolicy::outputBytes,&ResourcePolicy::combinedDiskBytes,&ResourcePolicy::indexRecords,&ResourcePolicy::workUnits,nullptr};
class Limits : public D, public testing::WithParamInterface<unsigned> {};
TEST_P(Limits, ZeroAndSmallBudgetsRefuseWithoutLeaking)
{
    const auto n=GetParam(); auto s=mixed();
    if(n<8) policy.*limits[n]=0; else policy.openFiles=4;
    auto w=writer(s);
    if(w) { auto result=w.value()->addTile(s.inputs[0].key,dimensions(s,s.inputs[0].key),s.inputs[0].values); EXPECT_FALSE(result); w.value().reset(); }
    else EXPECT_EQ(w.error().code,ErrorCode::ResourceRefused);
    clean();
}
INSTANTIATE_TEST_SUITE_P(Budgets,Limits,testing::Range(0U,9U));
TEST_F(D, ExactBufferBudgetAndOneByteShort)
{
    Spec s; auto w=writer(s); ASSERT_TRUE(w); const auto reserve=w.value()->statistics().bufferReserve; w.value().reset(); clean();
    policy.bufferBytes=reserve-1; EXPECT_FALSE(writer(s)); clean();
    policy.bufferBytes=reserve; w=writer(s); ASSERT_TRUE(w); auto p=w.value()->finish(); ASSERT_TRUE(p); auto r=open(); EXPECT_TRUE(r);
}
TEST_F(D, RuntimeScratchOutputCodecAndWorkRefusals)
{
    Spec s; auto i=input(s,{0,0},1);
    for(unsigned scenario=0;scenario<5;++scenario) {
        policy=initialResourcePolicy();
        if(scenario==0)policy.scratchBytes=80;
        if(scenario==1)policy.outputBytes=384;
        if(scenario==2)policy.combinedDiskBytes=384;
        if(scenario==3)policy.codecBytes=16;
        if(scenario==4)policy.workUnits=8;
        auto w=writer(s); ASSERT_TRUE(w); auto a=w.value()->addTile(i.key,dimensions(s,i.key),i.values);
        if(a) { auto p=w.value()->finish(); ASSERT_FALSE(p); EXPECT_EQ(p.error().code,ErrorCode::ResourceRefused); }
        else EXPECT_EQ(a.error().code,ErrorCode::ResourceRefused);
        w.value().reset(); clean();
    }
}
TEST_F(D, ReaderBudgetsAndMappingErrorsRemainDistinct)
{
    auto bad=fixture(mixed());put(bad,24,2,4);repair(bad);write(path(),bad);auto unsupported=open();
    ASSERT_FALSE(unsupported);EXPECT_EQ(unsupported.error().code,ErrorCode::UnsupportedFormat);
    write(path(),fixture(mixed())); policy.indexRecords=1; auto r=open(); ASSERT_FALSE(r); EXPECT_EQ(r.error().code,ErrorCode::ResourceRefused);
    policy=initialResourcePolicy(); policy.mapping=raster::ValidationPolicy{0}; r=open(); ASSERT_FALSE(r); EXPECT_EQ(r.error().code,ErrorCode::ResourceRefused); ASSERT_TRUE(r.error().mapping);
    policy=initialResourcePolicy(); std::fesetround(FE_UPWARD); r=open(); std::fesetround(FE_TONEAREST); ASSERT_FALSE(r); EXPECT_EQ(r.error().code,ErrorCode::Mapping);
    policy.codecBytes=0; r=open(); ASSERT_TRUE(r); auto t=r.value()->readTile({0,0}); ASSERT_FALSE(t); EXPECT_EQ(t.error().code,ErrorCode::ResourceRefused);
}
TEST_F(D, InvalidShapeKeyAndTerminalStates)
{
    Spec s; for(bool badKey:{false,true}) {
        auto w=writer(s); ASSERT_TRUE(w); std::array<U64,1> counts{1};
        auto a=w.value()->addTile(badKey?TileKey{2,0}:TileKey{0,0},{1,1},counts); ASSERT_FALSE(a); EXPECT_EQ(a.error().code,ErrorCode::InvalidInput);
        auto p=w.value()->finish(); ASSERT_FALSE(p); EXPECT_EQ(p.error().code,ErrorCode::InvalidState); clean();
    }
    auto w=writer(s); ASSERT_TRUE(w); auto p=w.value()->finish(); ASSERT_TRUE(p);
    auto twice=w.value()->finish(); ASSERT_FALSE(twice); EXPECT_EQ(twice.error().code,ErrorCode::InvalidState);
    std::array<U64,1> counts{1}; auto a=w.value()->addTile({1,1},{1,1},counts); ASSERT_FALSE(a); EXPECT_EQ(a.error().code,ErrorCode::InvalidState);
}
struct Fault { Phase phase; ErrorCode code; unsigned hits=0; };
Status fault(Phase phase,void *context) noexcept
{
    auto &f=*static_cast<Fault *>(context); if(phase!=f.phase)return success(); ++f.hits;
    return Error{f.code,phase,QStringLiteral("Injected bounded failure")};
}
class Failures : public D, public testing::WithParamInterface<Phase> {};
TEST_P(Failures, IOFailureAndCancellationAreTerminalAndClean)
{
    for(auto code:{ErrorCode::IO,ErrorCode::Cancelled}) {
        Spec s; s.width=1024; s.height=1; s.maxX=1024; s.maxY=0; policy.sortBytes=96;
        auto w=writer(s); ASSERT_TRUE(w); Fault f{GetParam(),code}; DensityStorageTestAccess::hook(*w.value(),fault,&f);
        bool failed=false;
        for(U64 x=4;x>0;--x) { auto i=input(s,{x-1,0},x); auto a=w.value()->addTile(i.key,dimensions(s,i.key),i.values); if(!a) { EXPECT_EQ(a.error().code,code); failed=true; break; } }
        if(!failed){auto p=w.value()->finish();ASSERT_FALSE(p);EXPECT_EQ(p.error().code,code);}
        EXPECT_GT(f.hits,0); auto again=w.value()->finish(); ASSERT_FALSE(again); EXPECT_EQ(again.error().code,ErrorCode::InvalidState); clean();
    }
}
INSTANTIATE_TEST_SUITE_P(Phases,Failures,testing::Values(Phase::Ingest,Phase::Codec,Phase::Sort,Phase::Merge,Phase::Copy,Phase::Seal,Phase::Flush,Phase::Readback,Phase::Publish));
TEST_F(D, CollisionPreservesExistingDatasetAndDiscardTransfer)
{
    Spec s; auto w=writer(s); ASSERT_TRUE(w); B previous{1,2,3}; write(path(),previous);
    auto p=w.value()->finish(); ASSERT_FALSE(p); EXPECT_EQ(read(path()),previous); EXPECT_FALSE(writer(s)); ASSERT_TRUE(QFile::remove(path())); clean();
    w=writer(s); ASSERT_TRUE(w); { auto pending=w.value()->finish(); ASSERT_TRUE(pending); EXPECT_TRUE(QFileInfo::exists(path())); } clean();
    w=writer(s); ASSERT_TRUE(w); { auto pending=w.value()->finish(); ASSERT_TRUE(pending); EXPECT_EQ(pending.value().releaseToOwner(),path()); }
    EXPECT_TRUE(open()); ASSERT_TRUE(QFile::remove(path())); clean();
}
TEST_F(D, CleanupFailureHasSeparateDiagnostic)
{
    Spec s; unsigned diagnostics=0;
    auto callback=[](const Error &e,void *p) noexcept { if(e.code==ErrorCode::CleanupFailed)++*static_cast<unsigned *>(p); };
    auto w=Writer::create(path(),metadata(s),policy,dir.path(),nullptr,nullptr,+callback,&diagnostics); ASSERT_TRUE(w);
    auto p=w.value()->finish(); ASSERT_TRUE(p); auto owned=std::make_unique<PendingDataset>(std::move(p.value()));
    HANDLE h=CreateFileW(reinterpret_cast<LPCWSTR>(path().utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    ASSERT_NE(h,INVALID_HANDLE_VALUE); owned.reset(); EXPECT_EQ(diagnostics,1); EXPECT_TRUE(QFileInfo::exists(path())); CloseHandle(h); ASSERT_TRUE(QFile::remove(path())); clean();
}
TEST_F(D, CompletedCandidateIsFullyReadBackBeforePublication)
{
    struct Tamper { QString directory; bool done=false; } context{dir.path()};
    auto corrupt=[](Phase phase,void *p) noexcept -> Status {
        auto &c=*static_cast<Tamper *>(p); if(phase!=Phase::Readback || c.done)return success(); c.done=true;
        for(const auto &name:QDir(c.directory).entryList({QStringLiteral(".pc2d-*.tmp")},QDir::Files|QDir::Hidden)) {
            QFile f(QDir(c.directory).filePath(name)); if(!f.open(QIODevice::ReadWrite))continue;
            if(f.read(8)!="PC2DDN01")continue;
            if(!f.seek(322))break; auto b=f.read(1); if(b.size()!=1)break; b[0]=char(b[0]^1); f.seek(322); f.write(b); f.flush(); return success();
        }
        return Error{ErrorCode::IO,Phase::Readback,QStringLiteral("tamper fixture setup failed")};
    };
    Spec s; auto w=writer(s); ASSERT_TRUE(w); auto i=input(s,{0,0},7); ASSERT_TRUE(w.value()->addTile(i.key,dimensions(s,i.key),i.values));
    DensityStorageTestAccess::hook(*w.value(),+corrupt,&context); auto p=w.value()->finish(); ASSERT_FALSE(p); EXPECT_EQ(p.error().code,ErrorCode::CorruptData); EXPECT_TRUE(context.done); clean();
}
TEST_F(D, CleanupFailureDoesNotHidePrimaryIOFailure)
{
    struct Locked { QString directory,path; HANDLE handle=INVALID_HANDLE_VALUE; unsigned diagnostics=0; } context{dir.path()};
    auto diagnostic=[](const Error &e,void *p) noexcept {if(e.code==ErrorCode::CleanupFailed)++static_cast<Locked *>(p)->diagnostics;};
    auto lock=[](Phase phase,void *p) noexcept -> Status {
        auto &c=*static_cast<Locked *>(p); if(phase!=Phase::Codec)return success();
        const auto names=QDir(c.directory).entryList({QStringLiteral(".pc2d-density-*.tmp")},QDir::Files|QDir::Hidden);
        if(!names.empty()) {c.path=QDir(c.directory).filePath(names[0]);c.handle=CreateFileW(reinterpret_cast<LPCWSTR>(c.path.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);}
        return Error{ErrorCode::IO,Phase::Codec,QStringLiteral("primary I/O fault")};
    };
    Spec s; auto w=Writer::create(path(),metadata(s),policy,dir.path(),nullptr,nullptr,+diagnostic,&context); ASSERT_TRUE(w);
    DensityStorageTestAccess::hook(*w.value(),+lock,&context); auto i=input(s,{0,0},7); auto a=w.value()->addTile(i.key,dimensions(s,i.key),i.values);
    ASSERT_FALSE(a); EXPECT_EQ(a.error().code,ErrorCode::IO); EXPECT_EQ(a.error().detail,QStringLiteral("primary I/O fault")); ASSERT_NE(context.handle,INVALID_HANDLE_VALUE);
    EXPECT_GE(context.diagnostics,1); CloseHandle(context.handle); ASSERT_TRUE(QFile::remove(context.path)); clean();
}
bool cancel(void *p) noexcept { return *static_cast<bool *>(p); }
TEST_F(D, CancellationCallbackAtEveryWriterPhase)
{
    for(auto phase:{Phase::Ingest,Phase::Codec,Phase::Sort,Phase::Merge,Phase::Copy,Phase::Seal,Phase::Flush,Phase::Readback,Phase::Publish}) {
        struct Control { Phase phase; bool stop=false; } control{phase};
        auto check=[](void *p) noexcept {return static_cast<Control *>(p)->stop;};
        auto arm=[](Phase p,void *context) noexcept -> Status {auto &v=*static_cast<Control *>(context);if(p==v.phase)v.stop=true;return success();};
        Spec s; s.width=1024; s.height=1; s.maxX=1024; s.maxY=0; policy.sortBytes=96;
        auto w=Writer::create(path(),metadata(s),policy,dir.path(),+check,&control); ASSERT_TRUE(w); DensityStorageTestAccess::hook(*w.value(),+arm,&control);
        bool stopped=false;
        for(U64 x=4;x>0;--x){auto i=input(s,{x-1,0},x);auto a=w.value()->addTile(i.key,dimensions(s,i.key),i.values);if(!a){EXPECT_EQ(a.error().code,ErrorCode::Cancelled);stopped=true;break;}}
        if(!stopped){auto p=w.value()->finish();ASSERT_FALSE(p);EXPECT_EQ(p.error().code,ErrorCode::Cancelled);} clean();
    }
}
TEST_F(D, CancellationOnEntryReaderAndBeforeCreation)
{
    Spec s; bool stop=true; auto w=Writer::create(path(),metadata(s),policy,dir.path(),cancel,&stop); ASSERT_FALSE(w); EXPECT_EQ(w.error().code,ErrorCode::Cancelled); clean();
    write(path(),fixture(mixed())); auto r=Reader::openReader(path(),policy,cancel,&stop); ASSERT_FALSE(r); EXPECT_EQ(r.error().code,ErrorCode::Cancelled);
    stop=false; r=Reader::openReader(path(),policy); ASSERT_TRUE(r); stop=true; auto t=r.value()->readTile({0,0},cancel,&stop); ASSERT_FALSE(t); EXPECT_EQ(t.error().code,ErrorCode::Cancelled);
    EXPECT_FALSE(r.value()->fullValidate(cancel,&stop)); EXPECT_TRUE(r.value()->fullValidate());
}
TEST_F(D, MidIndexAndStreamingCancellationPreserveOnlyEmittedPrefix)
{
    Spec s; s.width=256*33; s.height=1; s.maxX=double(s.width); s.maxY=0;
    for(U64 x=0;x<33;++x)s.inputs.push_back(input(s,{x,0},7));write(path(),fixture(s));
    unsigned calls=0;auto later=[](void *p) noexcept {return ++*static_cast<unsigned *>(p)==50;};
    auto cancelled=Reader::openReader(path(),policy,+later,&calls);ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code,ErrorCode::Cancelled);EXPECT_EQ(cancelled.error().phase,Phase::Index);
    auto r=open();ASSERT_TRUE(r);struct Consumer {bool stop=false;unsigned received=0;} consumer;
    auto check=[](void *p) noexcept {return static_cast<Consumer *>(p)->stop;};
    auto sink=[](const Tile &,void *p) noexcept -> Status {auto &c=*static_cast<Consumer *>(p);++c.received;c.stop=true;return success();};
    auto result=r.value()->forEachTile(+sink,&consumer,+check,&consumer);ASSERT_FALSE(result);EXPECT_EQ(result.error().code,ErrorCode::Cancelled);
    EXPECT_EQ(consumer.received,1);EXPECT_TRUE(r.value()->fullValidate());
}
TEST_F(D, CancellationAfterPartialCodecSortMergeCopyAndReadbackWork)
{
    for(auto phase:{Phase::Codec,Phase::Sort,Phase::Merge,Phase::Copy,Phase::Readback}) {
        struct Control {Phase phase;unsigned hits=0,readbackChecks=0;bool readback=false,stop=false;} control{phase};
        auto check=[](void *p) noexcept {auto &c=*static_cast<Control *>(p);if(c.readback && ++c.readbackChecks==50)c.stop=true;return c.stop;};
        auto arm=[](Phase phase,void *p) noexcept -> Status {auto &c=*static_cast<Control *>(p);if(phase==c.phase){++c.hits;if(phase==Phase::Readback)c.readback=true;else if(c.hits==20)c.stop=true;}return success();};
        Spec s;s.width=1024;s.height=256;s.maxX=1024;s.maxY=256;policy.sortBytes=96;
        auto w=Writer::create(path(),metadata(s),policy,dir.path(),+check,&control);ASSERT_TRUE(w);DensityStorageTestAccess::hook(*w.value(),+arm,&control);
        bool stopped=false;for(U64 x=4;x>0;--x){auto i=input(s,{x-1,0},7);auto a=w.value()->addTile(i.key,dimensions(s,i.key),i.values);if(!a){EXPECT_EQ(a.error().code,ErrorCode::Cancelled);stopped=true;break;}}
        if(!stopped){auto p=w.value()->finish();ASSERT_FALSE(p);EXPECT_EQ(p.error().code,ErrorCode::Cancelled);if(phase==Phase::Readback)EXPECT_EQ(p.error().phase,Phase::Validate);}
        if(phase!=Phase::Readback)EXPECT_GE(control.hits,20);clean();
    }
}
TEST_F(D, StreamingConsumerFailureReentryAndIndependentReaders)
{
    write(path(),fixture(mixed())); auto a=open(),b=open(); ASSERT_TRUE(a); ASSERT_TRUE(b);
    auto sink=[](const Tile &,void *context) noexcept -> Status { auto r=static_cast<Reader *>(context)->readTile({0,0});
        if(r || r.error().code!=ErrorCode::InvalidState)return Error{ErrorCode::ConsumerFailed,Phase::Read,QStringLiteral("reentry accepted")}; return success(); };
    EXPECT_TRUE(a.value()->forEachTile(+sink,a.value().get()));
    auto reject=[](const Tile &,void *) noexcept -> Status {return Error{ErrorCode::ConsumerFailed,Phase::Read,QStringLiteral("consumer refused")};};
    auto result=a.value()->forEachTile(+reject,nullptr); ASSERT_FALSE(result); EXPECT_EQ(result.error().code,ErrorCode::ConsumerFailed);
    auto t=b.value()->readTile({1,1}); ASSERT_TRUE(t); EXPECT_EQ(t.value().counts[0],U64(UINT32_MAX)+1); EXPECT_TRUE(a.value()->fullValidate());
}
struct Reenter { Writer *writer=nullptr; bool observed=false; };
bool reenter(void *context) noexcept
{
    auto &r=*static_cast<Reenter *>(context); if(r.writer) { auto p=r.writer->finish(); r.observed=!p && p.error().code==ErrorCode::InvalidState; } return false;
}
TEST_F(D, WriterReentryRejectedWithoutTerminatingOuterOperation)
{
    Spec s; Reenter context; auto w=Writer::create(path(),metadata(s),policy,dir.path(),reenter,&context); ASSERT_TRUE(w); context.writer=w.value().get();
    auto p=w.value()->finish(); ASSERT_TRUE(p); EXPECT_TRUE(context.observed); context.writer=nullptr;
}
} // namespace
