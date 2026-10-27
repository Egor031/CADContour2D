#include "formats.h"
#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <tiffio.h>
#include <zlib.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

using namespace density;
using namespace density::formats;
namespace {
auto noop = [] {};
template<class T> T get(const QByteArray &b, qsizetype p) { return qFromLittleEndian<T>(b.constData()+p); }
template<class T> void put(QByteArray &b, qsizetype p,T n) { qToLittleEndian<T>(n,b.data()+p); }
std::uint32_t crc(const QByteArray &b) { return std::uint32_t(crc32(0,reinterpret_cast<const Bytef *>(b.constData()),uInt(b.size()))); }
void checksums(QByteArray &b) {
    const auto offset = get<U64>(b,48); put(b,68,crc(b.mid(qsizetype(offset))));
    put(b,64,std::uint32_t(0)); put(b,64,crc(b.first(96)));
}
QByteArray bytes(const QString &path) { FileIo io; return readFile(path,16*MiB,io); }
void replace(const QString &path, const QByteArray &b) {
    QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly|QIODevice::Truncate)); ASSERT_EQ(f.write(b),b.size());
}
// Independent minimal BigTIFF IFD parser, only for mutation of small test files.
qsizetype tag(const QByteArray &b, std::uint16_t wanted) {
    EXPECT_EQ(b.first(2),"II"); EXPECT_EQ(get<std::uint16_t>(b,2),43);
    const auto ifd = get<U64>(b,8), n = get<U64>(b,qsizetype(ifd));
    for (U64 i = 0; i < n; ++i) { const auto p = qsizetype(ifd+8+i*20);
        if (get<std::uint16_t>(b,p) == wanted) return p; }
    ADD_FAILURE() << "missing tag " << wanted; return 0;
}
qsizetype array(const QByteArray &b, std::uint16_t wanted) {
    const auto p = tag(b,wanted); return get<U64>(b,p+4) == 1 ? p+12 : qsizetype(get<U64>(b,p+12));
}
template<class Reader> void roundTrip(const Spec &s) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid()); FileIo io; auto path = root.filePath("counts");
    auto source = [&](TileKey k) { return generate(s,k); };
    if constexpr (std::is_same_v<Reader,DReader>) writeD(path,s,source,io,noop);
    else writeE(path,s,source,io,noop);
    Reader r(path,s,64*MiB);
    EXPECT_EQ(fingerprint(s,[&](TileKey k) { auto t = r.read(k); EXPECT_EQ(t,source(k)); return t; }),fingerprint(s,source));
    for (const auto &[name,plan] : readPlans(s)) visit(s,plan,[&](TileKey k) { EXPECT_EQ(r.read(k),source(k)) << name.toStdString(); });
    EXPECT_GT(io.writeBytes,0); EXPECT_GT(r.metadataBytes(),0);
}
TEST(DensityFormats, EveryScenarioExactRoundTripAndCanonicalDigest) {
    for (const auto &name : scenarioNames()) {
        auto s = makeSpec(name,16,3,6001); roundTrip<DReader>(s);
        if (name != "huge-sparse") roundTrip<EReader>(s);
    }
}
TEST(DensityFormats, IndependentDefaultGridSumsAndLiteralEntropyControls) {
    for (const auto &name : {"empty","uniform","dense","entropy"}) {
        auto s = makeSpec(name,16,3,6001); s.width = 23; s.height = 21;
        QTemporaryDir root; FileIo io;
        for (bool tiff : {false,true}) {
            auto path = root.filePath(tiff ? "e" : "d");
            if (tiff) writeE(path,s,[&](TileKey k) { return generate(s,k); },io,noop);
            else writeD(path,s,[&](TileKey k) { return generate(s,k); },io,noop);
            auto check = [&](auto &r) {
                U64 total = 0; visit(s,{},[&](TileKey k) { total = add(total,sum(r.read(k))); });
                const U64 expected = QString(name) == "empty" ? 0 : QString(name) == "uniform" ? 483
                    : QString(name) == "dense" ? 1978368 : 1080383497533ULL;
                EXPECT_EQ(total,expected);
                if (QString(name) == "entropy") {
                    const auto a = r.read({0,0});
                    EXPECT_EQ((std::vector<U64>(a.counts.begin(),a.counts.begin()+8)),
                        (std::vector<U64>{1264276649,3595836717,3226230671,2540512399,1091451317,353594682,2116651579,201916769}));
                }
            };
            if (tiff) { EReader r(path,s,32*MiB); check(r); } else { DReader r(path,s,32*MiB); check(r); }
        }
    }
}
TEST(DensityFormats, TileSizesAndPartialEdges) {
    for (auto side : {256U,512U,1024U}) {
        auto s = makeSpec("entropy",side,2,6001); s.width = side+3; s.height = side+1;
        QTemporaryDir root; FileIo io;
        writeD(root.filePath("d"),s,[&](TileKey k) { return generate(s,k); },io,noop);
        writeE(root.filePath("e"),s,[&](TileKey k) { return generate(s,k); },io,noop);
        DReader d(root.filePath("d"),s,MiB); EReader e(root.filePath("e"),s,32*MiB);
        for (U64 i = 0; i < tileCount(s); ++i) {
            auto k = keyAt(s,i); auto expected = generate(s,k);
            EXPECT_EQ(d.read(k),expected); EXPECT_EQ(e.read(k),expected);
        }
        auto t = generate(s,{1,1}); EXPECT_EQ(t.width,3); EXPECT_EQ(t.height,1);
        EXPECT_EQ(decode(encode(generate(s,{0,0}))),generate(s,{0,0}));
        EXPECT_EQ(toJson(specFromJson(toJson(s))),toJson(s));
    }
}
TEST(DensityFormats, RoiOrderAndCropValuesIndependentOfTileVisitor) {
    auto s = makeSpec("entropy",16,3,6001); QTemporaryDir root; FileIo io;
    for (bool tiff : {false,true}) {
        auto path = root.filePath(tiff ? "e" : "d");
        if (tiff) writeE(path,s,[&](TileKey k) { return generate(s,k); },io,noop);
        else writeD(path,s,[&](TileKey k) { return generate(s,k); },io,noop);
        auto check = [&](auto &r) {
            std::vector<TileKey> keys; std::vector<U64> roi(4);
            visit(s,{Access::Roi,{},15,15,17,17},[&](TileKey k) {
                keys.push_back(k); auto t = r.read(k);
                for (U64 y = 15; y < 17; ++y) for (U64 x = 15; x < 17; ++x)
                    if (x/16 == k.x && y/16 == k.y) roi[(y-15)*2+x-15] = t.counts[(y%16)*t.width+x%16];
            });
            EXPECT_EQ(keys,(std::vector<TileKey>{{0,0},{1,0},{0,1},{1,1}}));
            auto whole = s; whole.side = 64; auto reference = generate(whole,{0,0});
            EXPECT_EQ(roi,(std::vector<U64>{reference.counts[15*s.width+15],reference.counts[15*s.width+16],
                reference.counts[16*s.width+15],reference.counts[16*s.width+16]}));
            std::vector<TileKey> random{{2,2},{0,0},{2,2},{1,0}}; keys.clear();
            visit(s,{Access::Tiles,random},[&](TileKey k) { keys.push_back(r.read(k).key); }); EXPECT_EQ(keys,random);
        };
        if (tiff) { EReader r(path,s,32*MiB); check(r); } else { DReader r(path,s,32*MiB); check(r); }
    }
}
TEST(DensityFormats, ExactUint64MaximumAndUint32OverflowRefusal) {
    auto s = makeSpec("u64",16,2,1); QTemporaryDir root; FileIo io;
    auto source = [&](TileKey k) { auto t = blank(s,k); if (k == TileKey{0,0}) t.counts[0] = UINT64_MAX; return t; };
    writeD(root.filePath("d"),s,source,io,noop); writeE(root.filePath("e"),s,source,io,noop);
    DReader d(root.filePath("d"),s,MiB); EReader e(root.filePath("e"),s,32*MiB);
    EXPECT_EQ(d.read({0,0}).counts[0],UINT64_MAX); EXPECT_EQ(e.read({0,0}).counts[0],UINT64_MAX);
    std::unique_ptr<TIFF,decltype(&TIFFClose)> native(TIFFOpen(root.filePath("e").toLocal8Bit().constData(),"r"),TIFFClose);
    ASSERT_TRUE(native); std::vector<U64> decoded(s.side*s.side);
    EXPECT_EQ(TIFFReadEncodedTile(native.get(),0,decoded.data(),tmsize_t(decoded.size()*8)),tmsize_t(decoded.size()*8));
    EXPECT_EQ(decoded[0],UINT64_MAX);
    auto narrow = makeSpec("u32",16,2,1);
    EXPECT_THROW(writeD(root.filePath("d32"),narrow,source,io,noop),Failure);
    EXPECT_THROW(writeE(root.filePath("e32"),narrow,source,io,noop),Failure);
}
TEST(DensityFormats, ZeroOmissionAndHugeSparseStayBounded) {
    QTemporaryDir root; FileIo io; auto empty = makeSpec("empty",16,3,1);
    writeD(root.filePath("d0"),empty,[&](TileKey k) { return generate(empty,k); },io,noop);
    writeE(root.filePath("e0"),empty,[&](TileKey k) { return generate(empty,k); },io,noop);
    DReader d0(root.filePath("d0"),empty,MiB); EReader e0(root.filePath("e0"),empty,32*MiB);
    EXPECT_EQ(QFileInfo(root.filePath("d0")).size(),96); EXPECT_TRUE(d0.physicalKeys().empty());
    EXPECT_EQ(e0.physicalKeys().size(),9); EXPECT_EQ(sum(d0.read({1,1})),0); EXPECT_EQ(sum(e0.read({1,1})),0);
    auto huge = makeSpec("huge-sparse",256,3,6001); roundTrip<DReader>(huge);
    EXPECT_THROW(storagePlan(huge,true),Failure);
    EXPECT_THROW(writeE(root.filePath("huge-e"),huge,[&](TileKey k) { return generate(huge,k); },io,noop),Failure);
    EXPECT_FALSE(QFileInfo::exists(root.filePath("huge-e")));
}
TEST(DensityFormats, ClassicTiffAndBigTiffBothRoundTrip) {
    QTemporaryDir root; auto s = makeSpec("u64",16,3,1); FileIo io;
    for (bool big : {false,true}) {
        auto path = root.filePath(big ? "big" : "classic");
        writeE(path,s,[&](TileKey k) { return generate(s,k); },io,noop,big);
        EXPECT_EQ(get<std::uint16_t>(bytes(path),2),big ? 43 : 42);
        EReader r(path,s,32*MiB); EXPECT_EQ(r.read({2,2}),generate(s,{2,2}));
    }
}
TEST(DensityCountContainer, CorruptHeaderIndexOffsetsLengthsAndPayload) {
    QTemporaryDir root; auto s = makeSpec("entropy",16,3,1); FileIo io; auto path = root.filePath("d");
    writeD(path,s,[&](TileKey k) { return generate(s,k); },io,noop); const auto original = bytes(path);
    const auto index = qsizetype(get<U64>(original,48));
    std::vector<std::function<void(QByteArray &)>> mutations{
        [](auto &b) { b[0] = 'x'; }, [](auto &b) { put(b,8,std::uint32_t(99)); checksums(b); },
        [](auto &b) { put(b,16,UINT64_MAX); checksums(b); },
        [](auto &b) { put(b,40,UINT64_MAX); checksums(b); },
        [](auto &b) { put(b,48,UINT64_MAX); },
        [=](auto &b) { b[index] ^= 1; },
        [=](auto &b) { put(b,index+8,UINT64_MAX); checksums(b); },
        [=](auto &b) { put(b,index+16,UINT64_MAX); checksums(b); },
        [=](auto &b) { put(b,index+24,UINT64_MAX); checksums(b); },
        [=](auto &b) { put(b,index+40,get<U64>(b,index)); checksums(b); },
        [](auto &b) { b[100] ^= 0x40; },
        [](auto &b) { b.chop(1); }, [](auto &b) { b.resize(90); },
        [=](auto &b) { put(b,index+32,get<std::uint32_t>(b,index+32)^1U); checksums(b); }
    };
    for (std::size_t i = 0; i < mutations.size(); ++i) {
        auto b = original; mutations[i](b); replace(path,b);
        EXPECT_THROW({ DReader r(path,s,MiB); r.read({0,0}); },Failure) << i;
    }
}
TEST(DensityTiff, CorruptHeaderTagsOffsetsLengthsAndPayloadNeverReturnEmptySuccess) {
    QTemporaryDir root; auto s = makeSpec("entropy",16,3,1); FileIo io; auto path = root.filePath("e");
    writeE(path,s,[&](TileKey k) { return generate(s,k); },io,noop); const auto original = bytes(path);
    const auto offsets = array(original,TIFFTAG_TILEOFFSETS), lengths = array(original,TIFFTAG_TILEBYTECOUNTS);
    const auto payload = qsizetype(get<U64>(original,offsets));
    std::vector<std::function<void(QByteArray &)>> mutations{
        [](auto &b) { b[0] = 'x'; }, [](auto &b) { put(b,8,UINT64_MAX); },
        [](auto &b) { put(b,tag(b,TIFFTAG_IMAGEWIDTH)+12,std::uint32_t(0)); },
        [](auto &b) { put(b,tag(b,TIFFTAG_TILEWIDTH)+12,std::uint32_t(0xfffffff0)); },
        [=](auto &b) { put(b,offsets,UINT64_MAX); },
        [=](auto &b) { put(b,offsets,U64(0)); },
        [=](auto &b) { put(b,offsets+8,get<U64>(b,offsets)); },
        [=](auto &b) { put(b,lengths,std::uint16_t(0)); },
        [=](auto &b) { put(b,lengths,std::uint16_t(65535)); },
        [=](auto &b) { b[payload+4] ^= 0x40; },
        [](auto &b) { b.chop(1); }, [](auto &b) { b.resize(14); }
    };
    for (std::size_t i = 0; i < mutations.size(); ++i) {
        auto b = original; mutations[i](b); replace(path,b);
        EXPECT_THROW({ EReader r(path,s,32*MiB); r.read({0,0}); },Failure) << i;
    }
}
TEST(DensityFormats, RefuseIndexBudgetInvalidDimensionsAndUnsafeOverwrite) {
    QTemporaryDir root; FileIo io; auto s = makeSpec("uniform",16,3,1); auto path = root.filePath("d");
    writeD(path,s,[&](TileKey k) { return generate(s,k); },io,noop); const auto original = bytes(path);
    EXPECT_THROW(DReader(path,s,1),Failure);
    EXPECT_THROW(writeD(path,s,[&](TileKey k) { return generate(s,k); },io,noop),Failure);
    EXPECT_THROW(writeE(path,s,[&](TileKey k) { return generate(s,k); },io,noop),Failure); EXPECT_EQ(bytes(path),original);
    auto invalid = s; invalid.width = 0; EXPECT_THROW(storagePlan(invalid,false),Failure);
    invalid = s; invalid.width = UINT64_MAX; invalid.height = 2; EXPECT_THROW(storagePlan(invalid,false),Failure);
    invalid = s; invalid.width = U64(UINT32_MAX)+1; EXPECT_THROW(storagePlan(invalid,true),Failure);
    auto epath = root.filePath("e"); writeE(epath,s,[&](TileKey k) { return generate(s,k); },io,noop);
    EXPECT_THROW(EReader(epath,s,1),Failure);
}
TEST(DensityFormats, CancellationLeavesUnreadablePartialAndPreservesOtherData) {
    QTemporaryDir root; FileIo io; writeNew(root.filePath("user.data"),"keep",io); auto s = makeSpec("uniform",16,3,1);
    for (bool tiff : {false,true}) {
        int n = 0; auto check = [&] { if (++n == 3) throw Failure("cancelled","test"); };
        auto path = root.filePath(tiff ? "e" : "d");
        EXPECT_THROW({ if (tiff) writeE(path,s,[&](TileKey k) { return generate(s,k); },io,check);
            else writeD(path,s,[&](TileKey k) { return generate(s,k); },io,check); },Failure);
        EXPECT_THROW({ if (tiff) { EReader r(path,s,32*MiB); } else { DReader r(path,s,MiB); } },Failure);
    }
    EXPECT_EQ(bytes(root.filePath("user.data")),"keep");
}
int run(const QStringList &args) {
    QProcess p; p.start(DENSITY_FORMATS_EXE,args);
    if (!p.waitForFinished(80000)) { p.kill(); p.waitForFinished(5000); return -1; }
    return p.exitCode();
}
TEST(DensityFormatsCli, EndToEndBalancedProcessesAndResourceRefusal) {
    EXPECT_EQ(run({"--help"}),0);
    QTemporaryDir root; const auto dir = root.filePath("run");
    EXPECT_EQ(run({"--output-dir",dir,"--scenario","uniform","--tile-size","16","--repeats","2"}),0);
    auto j = QJsonDocument::fromJson(bytes(QDir(dir).filePath("results.json"))).object();
    EXPECT_EQ(j["status"],"ok"); auto rows = j["measurements"].toArray(); EXPECT_EQ(rows.size(),36);
    for (auto v : rows) { auto r = v.toObject(); EXPECT_EQ(r["status"],"ok"); EXPECT_TRUE(r["correctness"].toBool()); }
    EXPECT_NE(run({"--output-dir",dir}),0);
    auto refused = root.filePath("refused"); EXPECT_NE(run({"--output-dir",refused,"--ram-mib","1"}),0);
    auto failure = QJsonDocument::fromJson(bytes(QDir(refused).filePath("results.json"))).object();
    EXPECT_EQ(failure["status"],"resource_refused"); EXPECT_TRUE(failure["measurements"].toArray().isEmpty());
    EXPECT_NE(run({"--output-dir",root.filePath("disk"),"--output-mib","1"}),0);
}
TEST(DensityTiff, ShortDeflateTrailerIsRejected) {
    QTemporaryDir root; auto s = makeSpec("entropy",16,3,1); FileIo io; auto path = root.filePath("e");
    writeE(path,s,[&](TileKey k) { return generate(s,k); },io,noop); auto b = bytes(path);
    const auto p = tag(b,TIFFTAG_TILEBYTECOUNTS), pos = array(b,TIFFTAG_TILEBYTECOUNTS);
    const auto type = get<std::uint16_t>(b,p);
    if (type == TIFF_SHORT) put(b,pos,std::uint16_t(get<std::uint16_t>(b,pos)-1));
    else if (type == TIFF_LONG) put(b,pos,std::uint32_t(get<std::uint32_t>(b,pos)-1));
    else put(b,pos,get<U64>(b,pos)-1);
    replace(path,b);
    EXPECT_THROW({ EReader r(path,s,32*MiB); r.read({0,0}); },Failure);
}
} // namespace
