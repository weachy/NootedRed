// VP9: risolve i siti del backend VP9 nella cache condivisa corrente.
// Tutte le funzioni toccate sono verificate per intero (byte e riferimenti rel32); i bersagli dati
// fuori da __TEXT passano dalla tabella SectionTargets generata insieme alle patch.
#pragma once
#include <UserCacheResolver.hpp>
#include <UserSharedCache.hpp>
#include <UserVP9DecodeData.hpp>
#include <UserVP9DecodeSequoiaData.hpp>
#include <UserVP9DecodeSonomaData.hpp>
#include <UserVP9DecodeVenturaData.hpp>

namespace UserVP9DecodeResolver
{
    using namespace UserVP9DecodeData;
    static constexpr UInt32 ImageCount   = sizeof(ResolverImages) / sizeof(ResolverImages[0]);
    static constexpr UInt32 PatchCount   = sizeof(Patches) / sizeof(Patches[0]);
    static constexpr UInt32 SectionCount = sizeof(SectionTargets) / sizeof(SectionTargets[0]);
    static constexpr UInt32 FunctionCount = sizeof(ResolverFunctions) / sizeof(ResolverFunctions[0]);
    static_assert(PatchCount > 0 && PatchCount <= 32);
    static_assert(MaxPatchSize <= 512);
    static_assert(sizeof(CacheUUID) == 16);

    constexpr UInt32 maxCount(UInt32 first, UInt32 second) { return first > second ? first : second; }

    static constexpr UInt32 MaxSites = maxCount(PatchCount, maxCount(
        sizeof(UserVP9DecodeSequoiaData::Patches) / sizeof(Patches[0]), maxCount(
        sizeof(UserVP9DecodeSonomaData::Patches) / sizeof(Patches[0]),
        sizeof(UserVP9DecodeVenturaData::Patches) / sizeof(Patches[0]))));
    static constexpr UInt32 MaxImages = maxCount(ImageCount, maxCount(
        sizeof(UserVP9DecodeSequoiaData::ResolverImages) / sizeof(ResolverImages[0]), maxCount(
        sizeof(UserVP9DecodeSonomaData::ResolverImages) / sizeof(ResolverImages[0]),
        sizeof(UserVP9DecodeVenturaData::ResolverImages) / sizeof(ResolverImages[0]))));
    static constexpr UInt32 MaxFunctions = maxCount(FunctionCount, maxCount(
        sizeof(UserVP9DecodeSequoiaData::ResolverFunctions) / sizeof(ResolverFunctions[0]), maxCount(
        sizeof(UserVP9DecodeSonomaData::ResolverFunctions) / sizeof(ResolverFunctions[0]),
        sizeof(UserVP9DecodeVenturaData::ResolverFunctions) / sizeof(ResolverFunctions[0]))));
    static_assert(MaxSites <= 32);
    static_assert(UserVP9DecodeSequoiaData::MaxPatchSize <= MaxPatchSize);
    static_assert(UserVP9DecodeSonomaData::MaxPatchSize <= MaxPatchSize);
    static_assert(UserVP9DecodeVenturaData::MaxPatchSize <= MaxPatchSize);

    struct Plan
    {
        UInt32                         count{0};
        UserVideoDecodeData::Patch     patches[MaxSites]{};
        UInt8                          original[MaxSites][MaxPatchSize]{};
        UInt8                          patched[MaxSites][MaxPatchSize]{};
        UserCacheResolver::ImageBounds images[MaxImages]{};
        UserCacheResolver::KnownLocation locations[MaxFunctions]{};
    };

    template<size_t Images, size_t Functions, size_t Sites, size_t Sections, typename Slot, size_t Slots>
    inline bool resolveSet(UserSharedCache::Input& input, UInt64 base, Plan& plan,
                           const UserCacheResolver::ImageReference (&images)[Images],
                           const UserCacheResolver::Function (&functions)[Functions],
                           const UserVideoDecodeData::Patch (&patches)[Sites],
                           const UserCacheResolver::Location (&locations)[Sites],
                           const UserCacheSectionData::Target (&sections)[Sections], const Slot (&slots)[Slots])
    {
        static_assert(Images <= MaxImages && Functions <= MaxFunctions && Sites <= MaxSites);
        if (!UserCacheResolver::resolveImages(input, base, images, plan.images)
            || !UserCacheResolver::resolveFunctions(input, base, images, functions, patches, locations, plan.patches,
                                                   &plan.original[0][0], &plan.patched[0][0], MaxPatchSize, plan.images,
                                                   true, sections, Sections, nullptr, plan.locations))
        {
            return false;
        }
        for (const auto& slot : slots) {
            UInt8      field[4];
            UInt64     table, target;
            const auto address = plan.locations[slot.constructor].address + slot.field;
            if (!input.read(address, field, sizeof(field))
                || !UserCacheResolver::relativeTarget(address, 0, field, table)
                || !input.read(table + slot.slot, &target, sizeof(target))
                || target != plan.locations[slot.function].address + slot.offset)
            {
                return false;
            }
        }
        plan.count = Sites;
        return true;
    }

    // 版本只选择参考数据；安装仍以完整函数、节引用和 ABI 核验为前提。
    inline bool resolve(UserSharedCache::Input& input, UInt64 base, Plan& plan, UInt32 major = 25)
    {
        plan.count = 0;
        switch (major) {
            case 22: {
                namespace Data = UserVP9DecodeVenturaData;
                return resolveSet(input, base, plan, Data::ResolverImages, Data::ResolverFunctions, Data::Patches,
                                  Data::ResolverLocations, Data::SectionTargets, Data::ABISlots);
            }
            case 23: {
                namespace Data = UserVP9DecodeSonomaData;
                return resolveSet(input, base, plan, Data::ResolverImages, Data::ResolverFunctions, Data::Patches,
                                  Data::ResolverLocations, Data::SectionTargets, Data::ABISlots);
            }
            case 24: {
                namespace Data = UserVP9DecodeSequoiaData;
                return resolveSet(input, base, plan, Data::ResolverImages, Data::ResolverFunctions, Data::Patches,
                                  Data::ResolverLocations, Data::SectionTargets, Data::ABISlots);
            }
            case 25:
                return resolveSet(input, base, plan, ResolverImages, ResolverFunctions, Patches, ResolverLocations,
                                  SectionTargets, ABISlots);
            default: return false;
        }
    }
}    // namespace UserVP9DecodeResolver
