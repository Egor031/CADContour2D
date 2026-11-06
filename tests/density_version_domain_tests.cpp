#include "core/project_state.h"
#include "project_test_access.h"

#include <gtest/gtest.h>

#include <bit>
#include <limits>
#include <type_traits>

using namespace cadcontour;

namespace {
template<class Id> Id identity(unsigned value)
{
    Id result;
    result.bytes[0] = std::byte(value);
    return result;
}

ImportedDataset source()
{
    ImportedDataset data;
    data.cacheId = identity<cache::CacheId>(1);
    data.pointCount = 7;
    data.minimum = {-1234.123456, -0.0};
    data.maximum = {-1233.123455, 1.0};
    return data;
}

ProjectState project()
{
    return ProjectState(1, identity<ProjectId>(2), source());
}

std::shared_ptr<const DensityVersion> version(unsigned versionId, unsigned datasetId, double cell = 0.5)
{
    const auto data = source();
    const auto mapping = raster::Mapping::create(data.minimum, data.maximum, cell, raster::ValidationPolicy{100});
    if (!mapping) throw std::runtime_error("Domain fixture mapping failed");
    DensityBuildMetadata build;
    build.tileSide = 256;
    build.groupSide = 512;
    build.eventCapacity = 16384;
    build.statistics.scanned = 7;
    build.statistics.eventsWritten = 7;
    build.statistics.eventsRead = 7;
    build.statistics.accumulated = 7;
    build.statistics.tileTotal = 7;
    build.statistics.scanAllCalls = 1;
    build.statistics.density.total = 7;
    return std::make_shared<const DensityVersion>(DensityVersionDescriptor{
        identity<VersionId>(versionId),
        {identity<density::DatasetId>(datasetId),
         std::make_shared<const DensityDatasetLocation>(DensityDatasetLocation{
             std::filesystem::path(L"C:\\domain-fixture\\density-" + std::to_wstring(datasetId) + L".d")})},
        data.cacheId, data.pointCount, mapping.value(), build});
}

void append(ProjectState &state, std::shared_ptr<const DensityVersion> entry)
{
    auto prepared = state.prepareDensityAppend(std::move(entry));
    ASSERT_TRUE(state.densityAppendApplicable(prepared));
    state.commitDensityAppend(prepared);
    EXPECT_FALSE(state.densityAppendApplicable(prepared));
}
} // namespace

TEST(DensityVersionDomain, IdentityTypesAndEmptyCatalogue)
{
    static_assert(!std::is_same_v<VersionId, ProjectId>);
    static_assert(!std::is_same_v<VersionId, density::DatasetId>);
    static_assert(!std::is_same_v<VersionId, cache::CacheId>);
    auto state = project();
    EXPECT_TRUE(VersionId{}.isNull());
    EXPECT_FALSE(identity<VersionId>(1).isNull());
    EXPECT_TRUE(state.densityVersions().empty());
    EXPECT_FALSE(state.activeDensityVersion());
    EXPECT_EQ(state.densityVersion({}), nullptr);
    EXPECT_FALSE(state.hasUnpersistableV1State());
    EXPECT_EQ(state.selectDensityVersion(std::nullopt).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(state.inputRevision(), 0);
}

TEST(DensityVersionDomain, PreparationPublishesNothingAndCommitSelectsOnce)
{
    auto state = project();
    const auto first = version(10, 20);
    auto prepared = state.prepareDensityAppend(first);
    EXPECT_TRUE(state.densityVersions().empty());
    EXPECT_FALSE(state.activeDensityVersion());
    EXPECT_EQ(state.inputRevision(), 0);
    ASSERT_TRUE(state.densityAppendApplicable(prepared));
    state.commitDensityAppend(prepared);
    ASSERT_EQ(state.densityVersions().size(), 1);
    EXPECT_EQ(state.densityVersions().front(), first);
    EXPECT_EQ(state.activeDensityVersion(), first->descriptor().id);
    EXPECT_EQ(state.densityVersion(first->descriptor().id), first.get());
    EXPECT_EQ(state.inputRevision(), 1);
    EXPECT_TRUE(state.hasUnpersistableV1State());
    EXPECT_FALSE(state.densityAppendApplicable(prepared));
}

TEST(DensityVersionDomain, IdenticalBuildsKeepDifferentDatasetsAndIndependentRoughOwners)
{
    auto state = project();
    const auto first = version(10, 20);
    const auto second = version(11, 21);
    append(state, first);
    const auto *reference = state.densityVersion(first->descriptor().id);
    const auto *rough = &reference->rough();
    append(state, second);
    ASSERT_EQ(state.densityVersions().size(), 2);
    EXPECT_EQ(state.densityVersions()[0].get(), first.get());
    EXPECT_EQ(state.densityVersions()[1].get(), second.get());
    EXPECT_EQ(state.densityVersion(first->descriptor().id), reference);
    EXPECT_EQ(&first->rough(), rough);
    EXPECT_NE(&first->rough(), &second->rough());
    EXPECT_EQ(first->rough().revision(), 0);
    EXPECT_EQ(second->rough().revision(), 0);
    EXPECT_NE(first->descriptor().id, second->descriptor().id);
    EXPECT_NE(first->descriptor().dataset.id, second->descriptor().dataset.id);
    EXPECT_NE(first->descriptor().dataset.location, second->descriptor().dataset.location);
    EXPECT_EQ(first->descriptor().mapping.cell(), second->descriptor().mapping.cell());
    EXPECT_EQ(first->descriptor().build.statistics.density.total, 7);
    EXPECT_EQ(state.activeDensityVersion(), second->descriptor().id);
}

TEST(DensityVersionDomain, MappingBitsAndFutureCellAreIndependent)
{
    auto state = project();
    const auto first = version(10, 20, 0.5);
    const auto second = version(11, 21, 0.25);
    append(state, first);
    append(state, second);
    ASSERT_EQ(state.changeCell(0.125).outcome, CommandOutcome::Applied);
    EXPECT_EQ(first->descriptor().mapping.cell(), 0.5);
    EXPECT_EQ(second->descriptor().mapping.cell(), 0.25);
    EXPECT_NE(first->descriptor().mapping.width(), second->descriptor().mapping.width());
    EXPECT_EQ(std::bit_cast<std::uint64_t>(first->descriptor().mapping.minimum().y),
              std::bit_cast<std::uint64_t>(-0.0));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(first->descriptor().mapping.minimum().x),
              std::bit_cast<std::uint64_t>(source().minimum.x));
    EXPECT_EQ(state.cell(), 0.125);
}

TEST(DensityVersionDomain, InvalidDuplicateAndUnknownIdentitiesPreserveContent)
{
    auto state = project();
    const auto first = version(10, 20);
    append(state, first);
    EXPECT_EQ(state.checkDensityAppend(*version(0, 21)).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(state.checkDensityAppend(*version(11, 0)).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(state.checkDensityAppend(*version(10, 21)).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(state.checkDensityAppend(*version(11, 20)).outcome, CommandOutcome::Rejected);
    EXPECT_THROW(state.prepareDensityAppend(nullptr), std::invalid_argument);
    EXPECT_THROW(state.prepareDensityAppend(first), std::invalid_argument);
    EXPECT_EQ(state.selectDensityVersion(VersionId{}).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(state.selectDensityVersion(identity<VersionId>(99)).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(state.inputRevision(), 1);
    EXPECT_EQ(state.densityVersions().size(), 1);
    EXPECT_EQ(state.activeDensityVersion(), first->descriptor().id);
}

TEST(DensityVersionDomain, SelectionIsOnlyAStableReferenceAndRepeatedChoiceIsNoop)
{
    auto state = project();
    const auto first = version(10, 20);
    const auto second = version(11, 21);
    append(state, first);
    append(state, second);
    EXPECT_EQ(state.selectDensityVersion(first->descriptor().id).outcome, CommandOutcome::Applied);
    const auto revision = state.inputRevision();
    EXPECT_EQ(state.selectDensityVersion(first->descriptor().id).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(state.inputRevision(), revision);
    EXPECT_EQ(state.selectDensityVersion(std::nullopt).outcome, CommandOutcome::Applied);
    EXPECT_FALSE(state.activeDensityVersion());
    EXPECT_EQ(state.densityVersions().size(), 2);
    EXPECT_TRUE(state.hasUnpersistableV1State());
    EXPECT_EQ(state.selectDensityVersion(second->descriptor().id).outcome, CommandOutcome::Applied);
    EXPECT_EQ(state.densityVersions()[0], first);
    EXPECT_EQ(state.densityVersions()[1], second);
}

TEST(DensityVersionDomain, LateChangesAndAnotherProjectInvalidatePreparedAppend)
{
    auto state = project();
    auto other = project();
    auto prepared = state.prepareDensityAppend(version(10, 20));
    EXPECT_FALSE(other.densityAppendApplicable(prepared));
    ASSERT_EQ(state.changeCell(0.25).outcome, CommandOutcome::Applied);
    EXPECT_FALSE(state.densityAppendApplicable(prepared));
    EXPECT_TRUE(state.densityVersions().empty());
    EXPECT_EQ(state.cell(), 0.25);
    auto retry = state.prepareDensityAppend(version(10, 20));
    state.commitDensityAppend(retry);
    EXPECT_EQ(state.cell(), 0.25);
    auto late = state.prepareDensityAppend(version(11, 21));
    ASSERT_EQ(state.selectDensityVersion(std::nullopt).outcome, CommandOutcome::Applied);
    EXPECT_FALSE(state.densityAppendApplicable(late));
    EXPECT_FALSE(state.activeDensityVersion());
    EXPECT_EQ(state.densityVersions().size(), 1);
}

TEST(DensityVersionDomain, ForeignSourceAndChangedBinary64BoundsAreRejected)
{
    auto state = project();
    auto descriptor = version(10, 20)->descriptor();
    descriptor.source = identity<cache::CacheId>(90);
    EXPECT_EQ(state.checkDensityAppend(DensityVersion(descriptor)).outcome, CommandOutcome::Rejected);
    descriptor = version(10, 20)->descriptor();
    descriptor.sourceMeasurements = 8;
    EXPECT_EQ(state.checkDensityAppend(DensityVersion(descriptor)).outcome, CommandOutcome::Rejected);
    descriptor = version(10, 20)->descriptor();
    const auto mapping = raster::Mapping::create({source().minimum.x, +0.0}, source().maximum,
                                                0.5, raster::ValidationPolicy{100});
    ASSERT_TRUE(mapping);
    descriptor.mapping = mapping.value();
    EXPECT_EQ(state.checkDensityAppend(DensityVersion(descriptor)).outcome, CommandOutcome::Rejected);
    EXPECT_TRUE(state.densityVersions().empty());
    EXPECT_EQ(state.inputRevision(), 0);
}

TEST(DensityVersionDomain, DescriptorReferencesOutliveProjectWithoutAnyFileOwner)
{
    std::shared_ptr<const DensityVersion> retained;
    const RoughState *rough = nullptr;
    {
        auto state = project();
        append(state, version(10, 20));
        retained = state.densityVersions().front();
        rough = &retained->rough();
    }
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->descriptor().id, identity<VersionId>(10));
    EXPECT_EQ(&retained->rough(), rough);
    EXPECT_EQ(retained->descriptor().sourceMeasurements, 7);
    EXPECT_EQ(retained->descriptor().dataset.location->path.filename(), L"density-20.d");
}

TEST(DensityVersionDomain, RevisionExhaustionIsCheckedBeforeAppendOrSelection)
{
    auto state = project();
    const auto first = version(10, 20);
    ProjectStateTestAccess::setRevision(state, std::numeric_limits<std::uint64_t>::max() - 1);
    append(state, first);
    EXPECT_EQ(state.inputRevision(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(state.checkDensityAppend(*version(11, 21)).outcome, CommandOutcome::Rejected);
    EXPECT_THROW(state.prepareDensityAppend(version(11, 21)), std::invalid_argument);
    EXPECT_EQ(state.selectDensityVersion(std::nullopt).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(state.selectDensityVersion(first->descriptor().id).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(state.densityVersions().size(), 1);
    EXPECT_EQ(state.activeDensityVersion(), first->descriptor().id);
    EXPECT_EQ(state.inputRevision(), std::numeric_limits<std::uint64_t>::max());
}

TEST(DensityVersionDomain, MovingPreparationTransfersItsSingleCommitAuthority)
{
    auto state = project();
    auto original = state.prepareDensityAppend(version(10, 20));
    auto transferred = std::move(original);
    EXPECT_FALSE(state.densityAppendApplicable(original));
    ASSERT_TRUE(state.densityAppendApplicable(transferred));
    state.commitDensityAppend(transferred);
    EXPECT_FALSE(state.densityAppendApplicable(original));
    EXPECT_FALSE(state.densityAppendApplicable(transferred));
    ASSERT_EQ(state.densityVersions().size(), 1);
    EXPECT_EQ(state.activeDensityVersion(), identity<VersionId>(10));
}
