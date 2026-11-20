// 完整函数匹配与 rel32 重建；不认识的实现或外部目标布局一律拒绝。
#pragma once
#include <UserCacheSectionData.hpp>
#include <UserSharedCache.hpp>

namespace UserCacheResolver
{

    using namespace UserSharedCache;

    // 模块只提供字节约束与候选核验；读取、预算、扫描边界及歧义拒绝统一处理。
    template<typename ByteMatches>
    inline bool matchBytes(Input& input, UInt64 address, UInt32 size, ByteMatches byteMatches)
    {
        UInt8 bytes[256];
        for (UInt32 offset = 0; offset < size;) {
            const auto length = size - offset < sizeof(bytes) ? size - offset : UInt32(sizeof(bytes));
            if (!input.read(address + offset, bytes, length)) { return false; }
            for (UInt32 i = 0; i < length; ++i) {
                if (!byteMatches(offset + i, bytes[i])) { return false; }
            }
            offset += length;
        }
        return !input.failed;
    }

    struct ScanAnchor
    {
        UInt32 offset;
        UInt8  value;
    };

    inline const UInt8* findScanByte(const UInt8* bytes, UInt8 value, UInt32 count)
    {
        // 内核 memchr 逐字节扫描；用通用寄存器先跳过无命中字宽，不越过有效尾部。
        constexpr UInt64 LowBits = 0x0101010101010101ULL, HighBits = 0x8080808080808080ULL;
        const auto       repeated = UInt64(value) * LowBits;
        while (count >= sizeof(UInt64)) {
            const auto different = wide(bytes) ^ repeated;
            if (((different - LowBits) & ~different & HighBits) != 0) { break; }
            bytes += sizeof(UInt64);
            count -= sizeof(UInt64);
        }
        // 按字节确认首个命中，同时消除相邻字节借位产生的附加标记。
        for (; count != 0; --count, ++bytes) {
            if (*bytes == value) { return bytes; }
        }
        return nullptr;
    }

    // 由 tools/analyze_anchor_frequency.py 从生成函数数据统计的字节稀有度秩：
    // 0 最少见。仅决定预筛选字节的选择，不参与候选匹配语义。
    inline constexpr UInt8 AnchorRarity[256] = {
        0xFF, 0xF7, 0xF0, 0xE9, 0xF2, 0xD0, 0xC0, 0xD8, 0xED, 0xDD, 0xAA, 0xAB, 0xD4, 0xA3, 0x75, 0xF9, 0xE1, 0xC2,
        0x76, 0x5D, 0xB0, 0x7E, 0x45, 0x39, 0xB5, 0x49, 0x24, 0x3A, 0x6C, 0x5E, 0x35, 0xBD, 0xDC, 0xA9, 0xA6, 0x8C,
        0xE6, 0xB8, 0x28, 0x4A, 0xBE, 0xA4, 0x2D, 0x1D, 0x6E, 0x1F, 0x65, 0x7F, 0xC1, 0xEA, 0x7C, 0x61, 0xB3, 0x82,
        0x0C, 0x52, 0x9B, 0xA8, 0x73, 0x6D, 0x87, 0x89, 0x36, 0x47, 0xE4, 0xFA, 0xA1, 0xB6, 0xE8, 0xE7, 0xB1, 0x94,
        0xFD, 0xEE, 0x64, 0x37, 0xEF, 0xBA, 0x95, 0x54, 0xBF, 0x32, 0x33, 0x81, 0x83, 0xA2, 0x67, 0x80, 0xAC, 0x08,
        0x0D, 0x88, 0x5C, 0x96, 0x78, 0x4B, 0x98, 0x42, 0x29, 0x3B, 0x2E, 0x20, 0xB2, 0x10, 0x97, 0x11, 0x02, 0x09,
        0x3D, 0x17, 0x18, 0x12, 0x9D, 0x19, 0x8A, 0x84, 0xE5, 0xE3, 0x25, 0x8E, 0xB7, 0x26, 0x1A, 0x40, 0x43, 0x57,
        0x4E, 0x9C, 0xDB, 0xD7, 0x8D, 0xF8, 0xEB, 0xF6, 0x9F, 0x5F, 0xCE, 0xFC, 0x53, 0xFB, 0x93, 0xF3, 0x6A, 0x58,
        0x79, 0x2F, 0x41, 0x85, 0xBC, 0xD9, 0x3E, 0x22, 0x60, 0x0A, 0x05, 0x0E, 0x23, 0x74, 0x14, 0x13, 0x68, 0x1B,
        0x03, 0x62, 0x27, 0x59, 0x04, 0x00, 0x8B, 0x6F, 0x01, 0x0B, 0x44, 0x38, 0x0F, 0x63, 0x99, 0x06, 0x2A, 0x4C,
        0x5A, 0xA0, 0x9A, 0x55, 0xCB, 0xC6, 0xC8, 0x7D, 0x69, 0xC3, 0xB4, 0xA7, 0xF1, 0xF4, 0xC9, 0xC7, 0xCC, 0xAE,
        0xCA, 0xDA, 0xD5, 0xD2, 0xC4, 0x3F, 0x48, 0x2B, 0x8F, 0x51, 0xC5, 0xA5, 0xAF, 0x15, 0xB9, 0x16, 0x7B, 0x4F,
        0x6B, 0x30, 0x1E, 0x77, 0x1C, 0x31, 0x46, 0x9E, 0xE0, 0xDE, 0xCD, 0x3C, 0x56, 0x86, 0x90, 0x66, 0xEC, 0xD3,
        0x70, 0xDF, 0x5B, 0x34, 0x4D, 0x50, 0xAD, 0x72, 0x7A, 0x2C, 0x07, 0x21, 0xE2, 0xD6, 0xD1, 0xCF, 0xBB, 0x91,
        0x92, 0x71, 0xF5, 0xFE,
    };

    template<typename PrefixMatches, typename CandidateMatches>
    __attribute__((noinline)) inline bool scanBuffered(Input& input, const Range& text, UInt32 size, UInt32 prefixSize,
                                                       const PrefixMatches&    prefixMatches,
                                                       const CandidateMatches& candidateMatches, UInt64& found,
                                                       UInt8* scan, UInt32 capacity, const ScanAnchor* anchor = nullptr)
    {
        found = 0;
        for (UInt64 offset = 0; offset <= text.size - size;) {
            const auto length = text.size - offset < capacity ? UInt32(text.size - offset) : capacity;
            if (!input.read(text.address + offset, scan, length)) { return false; }
            const auto positions = length - prefixSize + 1;
            for (UInt32 j = 0; j < positions && offset + j <= text.size - size; ++j) {
                if (anchor) {
                    const auto  remaining = text.size - size - offset - j + 1;
                    const auto  count     = remaining < positions - j ? UInt32(remaining) : positions - j;
                    const auto* next      = findScanByte(scan + j + anchor->offset, anchor->value, count);
                    if (!next) { break; }
                    j = UInt32(next - scan) - anchor->offset;
                }
                if (!prefixMatches(scan + j)) { continue; }
                if (++input.candidates > 128) { return false; }
                const auto address = text.address + offset + j;
                if (!candidateMatches(address)) { continue; }
                if (found) { return false; }
                found = address;
            }
            offset += positions;
        }
        return found != 0 && !input.failed;
    }

    template<typename PrefixMatches, typename CandidateMatches>
    __attribute__((noinline)) inline bool scanUnbuffered(Input& input, const Range& text, UInt32 size,
                                                         UInt32 prefixSize, const PrefixMatches& prefixMatches,
                                                         const CandidateMatches& candidateMatches, UInt64& found,
                                                         const ScanAnchor* anchor = nullptr)
    {
        UInt8 scan[1024];
        return scanBuffered(input, text, size, prefixSize, prefixMatches, candidateMatches, found, scan, sizeof(scan),
                            anchor);
    }

    template<typename PrefixMatches, typename CandidateMatches>
    __attribute__((noinline)) inline bool scanFunction(Input& input, const Range& text, UInt32 size, UInt32 prefixSize,
                                                       const PrefixMatches&    prefixMatches,
                                                       const CandidateMatches& candidateMatches, UInt64& found,
                                                       UInt64 hint = 0, const ScanAnchor* anchor = nullptr)
    {
        found = 0;
        if (prefixSize == 0 || prefixSize > 32 || (anchor && anchor->offset >= prefixSize) || size < prefixSize
            || size > text.size || text.size > MaxTextSize)
        {
            return false;
        }
        // 渲染基线保留完整核验后的参考 RVA 快路径；其他调用默认验证扫描唯一性。
        if (hint && text.contains(hint, size) && candidateMatches(hint)) {
            found = hint;
            return !input.failed;
        }
        if (input.scanBuffer) {
            return scanBuffered(input, text, size, prefixSize, prefixMatches, candidateMatches, found, input.scanBuffer,
                                Input::ScanSize, anchor);
        }
        return scanUnbuffered(input, text, size, prefixSize, prefixMatches, candidateMatches, found, anchor);
    }

    struct Prefix
    {
        UInt64 values[4]{}, masks[4]{};
        void   set(UInt32 index, UInt8 value, bool skipped)
        {
            if (skipped) { return; }
            const auto shift   = (index % 8) * 8;
            masks[index / 8]  |= UInt64(0xFF) << shift;
            values[index / 8] |= UInt64(value) << shift;
        }
        bool commonAnchor(const Prefix& other, ScanAnchor& anchor) const
        {
            // 在所有共同固定字节中选择生成数据里最少见的一个；相同稀有度保留最早位置。
            UInt32 bestOffset = 32;
            UInt32 bestCost   = 0x100;
            for (UInt32 i = 0; i < 32; ++i) {
                const auto shift = (i % 8) * 8;
                if (((masks[i / 8] >> shift) & (other.masks[i / 8] >> shift) & 0xFF) == 0xFF
                    && ((values[i / 8] >> shift) & 0xFF) == ((other.values[i / 8] >> shift) & 0xFF))
                {
                    const auto cost = AnchorRarity[UInt8(values[i / 8] >> shift)];
                    if (cost < bestCost) {
                        bestCost   = cost;
                        bestOffset = i;
                    }
                }
            }
            if (bestOffset == 32) { return false; }
            anchor = {bestOffset, UInt8(values[bestOffset / 8] >> (bestOffset % 8) * 8)};
            return true;
        }
        bool matches(const UInt8* bytes) const
        {
            for (UInt32 i = 0; i < 4; ++i) {
                if ((wide(bytes + i * 8) & masks[i]) != values[i]) { return false; }
            }
            return true;
        }
    };
    using UserSharedCache::ImageReference;
    struct Reference
    {
        UInt32 offset, image;
        UInt64 rva;
        UInt32 indirect;
    };
    struct Function
    {
        UInt32           image;
        UInt64           rva;
        const UInt8*     original;
        UInt32           size;
        const Reference* originalRefs;
        UInt32           originalCount;
        const Reference* patchedRefs;
        UInt32           patchedCount;
    };
    struct Location
    {
        UInt32 function, offset;
    };
    struct ImageBounds
    {
        UInt64 address;
        Range  text, segments[8];
        UInt32 segmentCount;
        bool   contains(UInt64 value, UInt64 size) const
        {
            for (UInt32 i = 0; i < segmentCount; ++i) {
                if (segments[i].contains(value, size)) { return true; }
            }
            return false;
        }
    };

    inline bool skipped(UInt32 index, const Reference* refs, UInt32 count)
    {
        for (UInt32 i = 0; i < count; ++i) {
            if (index >= refs[i].offset && index - refs[i].offset < 4) { return true; }
        }
        return false;
    }

    template<typename Patch>
    inline UInt8 expected(const Function& function, UInt32 index, UInt32 f, bool patched, const Patch* patches,
                          const Location* locations, UInt32 count)
    {
        if (patched) {
            for (UInt32 i = 0; i < count; ++i) {
                if (locations[i].function == f && index >= locations[i].offset
                    && index - locations[i].offset < patches[i].size)
                {
                    return patches[i].patched[index - locations[i].offset];
                }
            }
        }
        return function.original[index];
    }

    inline bool referencesOrdered(const Reference* refs, UInt32 count)
    {
        for (UInt32 i = 1; i < count; ++i) {
            if (refs[i - 1].offset > refs[i].offset) { return false; }
        }
        return true;
    }

    template<typename Patch>
    inline bool matches(Input& input, UInt64 address, const Function& function, UInt32 f, bool patched,
                        const Patch* patches, const Location* locations, UInt32 count, bool ordered)
    {
        const auto* refs     = patched ? function.patchedRefs : function.originalRefs;
        const auto  refCount = patched ? function.patchedCount : function.originalCount;
        UInt32      cursor{0};
        return matchBytes(input, address, function.size,
                          [&](UInt32 index, UInt8 value)
                          {
                              if (!ordered) {
                                  return skipped(index, refs, refCount)
                                         || value == expected(function, index, f, patched, patches, locations, count);
                              }
                              while (cursor < refCount && index >= refs[cursor].offset
                                     && index - refs[cursor].offset >= 4) {
                                  ++cursor;
                              }
                              return (cursor < refCount && index >= refs[cursor].offset)
                                     || value == expected(function, index, f, patched, patches, locations, count);
                          });
    }

    template<typename Patch>
    inline bool matches(Input& input, UInt64 address, const Function& function, UInt32 f, bool patched,
                        const Patch* patches, const Location* locations, UInt32 count)
    {
        const auto* refs     = patched ? function.patchedRefs : function.originalRefs;
        const auto  refCount = patched ? function.patchedCount : function.originalCount;
        return matches(input, address, function, f, patched, patches, locations, count,
                       referencesOrdered(refs, refCount));
    }

    template<typename Patch, typename CandidateFilter>
    inline bool find(Input& input, const Range& text, const Function& function, UInt32 f, const Patch* patches,
                     const Location* locations, UInt32 count, UInt64& address, bool& patched, CandidateFilter filter)
    {
        if (function.size < 32) { return false; }
        Prefix prefixes[2];
        bool   ordered[2]{};
        // 扫描热点只比较已准备的前缀与引用顺序，避免对每个候选重新遍历补丁表。
        for (UInt32 variant = 0; variant < 2; ++variant) {
            const auto* refs      = variant ? function.patchedRefs : function.originalRefs;
            const auto  countRefs = variant ? function.patchedCount : function.originalCount;
            for (UInt32 i = 0; i < 32; ++i) {
                prefixes[variant].set(i, expected(function, i, f, variant != 0, patches, locations, count),
                                      skipped(i, refs, countRefs));
            }
            ordered[variant] = referencesOrdered(refs, countRefs);
        }
        const bool samePrefix    = memcmp(prefixes, prefixes + 1, sizeof(Prefix)) == 0;
        const auto prefixMatches = [&](const UInt8* bytes)
        { return prefixes[0].matches(bytes) || (!samePrefix && prefixes[1].matches(bytes)); };
        const auto candidateMatches = [&](UInt64 candidate)
        {
            for (UInt32 variant = 0; variant < 2; ++variant) {
                if (matches(input, candidate, function, f, variant != 0, patches, locations, count, ordered[variant])
                    && filter(candidate, variant != 0))
                {
                    patched = variant != 0;
                    return true;
                }
            }
            return false;
        };
        ScanAnchor anchor{};
        return scanFunction(input, text, function.size, 32, prefixMatches, candidateMatches, address, 0,
                            prefixes[0].commonAnchor(prefixes[1], anchor) ? &anchor : nullptr);
    }

    template<typename Patch>
    inline bool find(Input& input, const Range& text, const Function& function, UInt32 f, const Patch* patches,
                     const Location* locations, UInt32 count, UInt64& address, bool& patched)
    {
        return find(input, text, function, f, patches, locations, count, address, patched,
                    [](UInt64, bool) { return true; });
    }

    inline bool sectionTarget(Input& input, const ImageBounds& image, const UserCacheSectionData::Target& target,
                              UInt64& address)
    {
        UInt8 header[32], unslid[8];
        if (!input.read(image.address, header, sizeof(header))
            || !input.read(input.cache.address + 224, unslid, sizeof(unslid)))
        {
            return false;
        }
        const auto commandSize = word(header + 20), commandCount = word(header + 16);
        if (commandSize > 65536 || commandCount > 128 || wide(unslid) > input.cache.address) { return false; }
        const auto slide = input.cache.address - wide(unslid);
        const auto named = [](const UInt8* bytes, const char* name)
        {
            const auto size = strlen(name);
            return size <= 16 && memcmp(bytes, name, size) == 0 && (size == 16 || bytes[size] == 0);
        };
        const auto inspect = [&](UInt32 cursor, bool hinted)
        {
            UInt8 command[72], section[80];
            if (cursor < 32 || cursor - 32 > commandSize || commandSize - (cursor - 32) < sizeof(command)
                || !input.read(image.address + cursor, command, sizeof(command)) || word(command) != 0x19
                || !named(command + 8, target.segment))
            {
                return false;
            }
            const auto count = word(command + 64), length = word(command + 4);
            if (count > 64 || length != 72 + count * 80 || length > commandSize - (cursor - 32)) { return false; }
            for (UInt32 j = 0; j < count; ++j) {
                const auto index = hinted ? target.index : j;
                if (index >= count || !input.read(image.address + cursor + 72 + index * 80, section, sizeof(section))) {
                    return false;
                }
                if (named(section, target.section) && named(section + 16, target.segment)) {
                    const auto start = wide(section + 32), size = wide(section + 40);
                    if (start >= UserLimit - slide || target.offset > size || size - target.offset < 4
                        || !image.contains(start + slide, size))
                    {
                        return false;
                    }
                    address = start + slide + target.offset;
                    return true;
                }
                if (hinted) { break; }
            }
            return false;
        };
        // 命令位置只是提示；命令或节顺序改变后仍按名称和完整边界查找。
        if (inspect(target.command, true)) { return true; }
        for (UInt32 cursor = 32, i = 0; i < commandCount; ++i) {
            UInt8 command[8];
            if (cursor - 32 > commandSize || commandSize - (cursor - 32) < 8
                || !input.read(image.address + cursor, command, sizeof(command)))
            {
                return false;
            }
            const auto size = word(command + 4);
            if (size < 8 || (size & 7) != 0 || size > commandSize - (cursor - 32)) { return false; }
            if (word(command) == 0x19 && inspect(cursor, false)) { return true; }
            cursor += size;
        }
        return false;
    }

    inline bool target(Input& input, const Reference& ref, const ImageBounds* images, UInt32 imageCount,
                       const ImageReference* imageRefs, const Function* functions, const UInt64* addresses,
                       UInt32 functionCount, UInt64& address, bool sectionRelative,
                       const UserCacheSectionData::Target* scopedTargets = nullptr, UInt32 scopedTargetCount = 0)
    {
        if (ref.image >= imageCount || ref.rva >= UserLimit - images[ref.image].address) { return false; }
        address = images[ref.image].address + ref.rva;
        // 已完整核验的目标函数也可搬迁；其他目标必须保持映像内参考 RVA。
        for (UInt32 i = 0; i < functionCount; ++i) {
            if (functions[i].image == ref.image && ref.rva >= functions[i].rva
                && ref.rva - functions[i].rva < functions[i].size)
            {
                address = addresses[i] + ref.rva - functions[i].rva;
                return images[ref.image].contains(address, 4);
            }
        }
        if (sectionRelative) {
            for (UInt32 i = 0; i < scopedTargetCount; ++i) {
                const auto& entry = scopedTargets[i];
                if (entry.rva == ref.rva && strcmp(entry.path, imageRefs[ref.image].path) == 0) {
                    return sectionTarget(input, images[ref.image], entry, address);
                }
            }
            for (const auto& entry : UserCacheSectionData::Targets) {
                if (entry.rva == ref.rva && strcmp(entry.path, imageRefs[ref.image].path) == 0) {
                    return sectionTarget(input, images[ref.image], entry, address);
                }
            }
        }
        return images[ref.image].contains(address, 4);
    }

    inline bool binding(Input& input, UInt64 address, UInt32 kind, UInt64 expected)
    {
        if (kind == 2) {
            UInt8  stub[6];
            UInt64 slot;
            if (!input.read(address, stub, 6) || stub[0] != 0xFF || stub[1] != 0x25
                || !relativeTarget(address, 2, stub, slot))
            {
                return false;
            }
            address = slot;
        }
        UInt8 pointer[8];
        return (kind == 1 || kind == 2) && input.read(address, pointer, 8) && wide(pointer) == expected;
    }

    // 使用已匹配函数实际引用的 GOT / 调用桩位置，按绑定目标身份关联两份载荷。
    inline bool observedTarget(Input& input, const Reference* requested, UInt32 index, UInt32 f,
                               const Function* functions, const UInt64* addresses, const bool* variants,
                               UInt64& address)
    {
        const auto& ref     = requested[index];
        UInt32      ordinal = 0;
        for (UInt32 i = 0; i < index; ++i) {
            if (requested[i].image == ref.image && requested[i].rva == ref.rva && requested[i].indirect == ref.indirect)
            {
                ++ordinal;
            }
        }
        const auto& fn    = functions[f];
        const auto* refs  = variants[f] ? fn.patchedRefs : fn.originalRefs;
        const auto  count = variants[f] ? fn.patchedCount : fn.originalCount;
        for (UInt32 i = 0; i < count; ++i) {
            if (refs[i].image != ref.image || refs[i].rva != ref.rva || refs[i].indirect != ref.indirect) { continue; }
            if (ordinal) {
                --ordinal;
                continue;
            }
            UInt8 field[4];
            return input.read(addresses[f] + refs[i].offset, field, 4)
                   && relativeTarget(addresses[f] + refs[i].offset, 0, field, address);
        }
        return false;
    }

    // 已完整核验的函数位置；patched 表示命中的是生成数据中的补丁变体。
    struct KnownLocation
    {
        UInt64 address;
        bool   patched;
    };

    template<size_t ImageCount, size_t ImageCapacity>
    __attribute__((noinline)) inline bool resolveImages(Input& input, UInt64 base,
                                                        const ImageReference (&imageRefs)[ImageCount],
                                                        ImageBounds          (&images)[ImageCapacity])
    {
        static_assert(ImageCount > 0 && ImageCount <= 16 && ImageCapacity >= ImageCount);
        input.selectImages(base, imageRefs, ImageCount);
        for (UInt32 i = 0; i < ImageCount; ++i) {
            Image image;
            if (!imageForCache(input, base, image, imageRefs[i].path, imageRefs[i].size) || image.segmentCount > 8) {
                return false;
            }
            images[i].address      = image.address;
            images[i].text         = image.text;
            images[i].segmentCount = image.segmentCount;
            memcpy(images[i].segments, image.segments, image.segmentCount * sizeof(Range));
        }
        return true;
    }

    // known 只允许在映像同源且无私有页已证明时使用；跳过唯一性扫描但保留全部函数与引用核验。
    template<typename Patch, size_t ImageCount, size_t FunctionCount, size_t PatchCount, size_t ImageCapacity>
    __attribute__((noinline)) inline bool
        resolveFunctions(Input& input, UInt64 base, const ImageReference (&imageRefs)[ImageCount],
                         const Function (&functions)[FunctionCount], const Patch* patches,
                         const Location (&locations)[PatchCount], Patch* output, UInt8* originals, UInt8* replacements,
                         UInt32 stride, ImageBounds (&images)[ImageCapacity], bool sectionRelative,
                         const UserCacheSectionData::Target* scopedTargets, UInt32 scopedTargetCount,
                         const KnownLocation* known, KnownLocation* located)
    {
        static_assert(ImageCapacity >= ImageCount);
        static_assert(ImageCount > 0 && ImageCount <= 16 && FunctionCount > 0 && FunctionCount <= 32);
        if (scopedTargetCount > 128 || (scopedTargets == nullptr) != (scopedTargetCount == 0)) { return false; }
        UInt64 addresses[FunctionCount]{};
        bool   variants[FunctionCount]{};
        for (UInt32 i = 0; i < FunctionCount; ++i) {
            if (functions[i].image >= ImageCount) { return false; }
            const auto dataTargetsMatch = [&](UInt64 candidate, bool variant)
            {
                const auto& fn    = functions[i];
                const auto* refs  = variant ? fn.patchedRefs : fn.originalRefs;
                const auto  count = variant ? fn.patchedCount : fn.originalCount;
                for (UInt32 r = 0; r < count; ++r) {
                    if (refs[r].image >= ImageCount) { return false; }
                    bool resolved = false;
                    for (UInt32 k = 0; k < i; ++k) {
                        const auto& prior = functions[k];
                        if (prior.image == refs[r].image && refs[r].rva >= prior.rva
                            && refs[r].rva - prior.rva < prior.size)
                        {
                            resolved = true;
                            break;
                        }
                    }
                    bool scoped = false;
                    for (UInt32 k = 0; !resolved && k < scopedTargetCount; ++k) {
                        const auto& entry = scopedTargets[k];
                        if (entry.rva == refs[r].rva && strcmp(entry.path, imageRefs[refs[r].image].path) == 0) {
                            scoped = true;
                            break;
                        }
                    }
                    if (!resolved && !scoped) { continue; }
                    UInt8  field[4];
                    UInt64 actual, destination;
                    if (refs[r].offset > fn.size || fn.size - refs[r].offset < sizeof(field)
                        || !input.read(candidate + refs[r].offset, field, sizeof(field))
                        || !relativeTarget(candidate + refs[r].offset, 0, field, actual)
                        || !target(input, refs[r], images, ImageCount, imageRefs, functions, addresses, i, destination,
                                   sectionRelative, scopedTargets, scopedTargetCount)
                        || (refs[r].indirect ? !binding(input, actual, refs[r].indirect, destination) :
                                               actual != destination))
                    {
                        return false;
                    }
                }
                return true;
            };
            if (known) {
                const auto& location = known[i];
                if (location.address == 0 || location.patched > 1
                    || !images[functions[i].image].text.contains(location.address, functions[i].size))
                {
                    return false;
                }
                addresses[i] = location.address;
                variants[i]  = location.patched != 0;
                if (!matches(input, addresses[i], functions[i], i, variants[i], patches, locations, PatchCount)
                    || !dataTargetsMatch(addresses[i], variants[i]))
                {
                    return false;
                }
            }
            else if (!find(input, images[functions[i].image].text, functions[i], i, patches, locations, PatchCount,
                           addresses[i], variants[i], dataTargetsMatch))
            {
                return false;
            }
            if (located) { located[i] = {addresses[i], variants[i]}; }
            for (UInt32 j = 0; j < i; ++j) {
                if (Range{addresses[j], functions[j].size}.contains(addresses[i], 1)
                    || Range{addresses[i], functions[i].size}.contains(addresses[j], 1))
                {
                    return false;
                }
            }
        }
        for (UInt32 i = 0; i < FunctionCount; ++i) {
            const auto& fn    = functions[i];
            const auto* refs  = variants[i] ? fn.patchedRefs : fn.originalRefs;
            const auto  count = variants[i] ? fn.patchedCount : fn.originalCount;
            for (UInt32 r = 0; r < count; ++r) {
                UInt8  field[4];
                UInt64 actual, destination;
                if (refs[r].offset > fn.size || fn.size - refs[r].offset < 4
                    || !input.read(addresses[i] + refs[r].offset, field, 4)
                    || !relativeTarget(addresses[i] + refs[r].offset, 0, field, actual)
                    || !target(input, refs[r], images, ImageCount, imageRefs, functions, addresses, FunctionCount,
                               destination, sectionRelative, scopedTargets, scopedTargetCount)
                    || (refs[r].indirect ? !binding(input, actual, refs[r].indirect, destination) :
                                           actual != destination))
                {
                    return false;
                }
            }
        }
        for (UInt32 p = 0; p < PatchCount; ++p) {
            const auto loc = locations[p];
            if (loc.function >= FunctionCount || patches[p].size == 0 || patches[p].size > stride) { return false; }
            const auto& fn = functions[loc.function];
            if (loc.offset > fn.size || patches[p].size > fn.size - loc.offset) { return false; }
            const auto address = addresses[loc.function] + loc.offset;
            if (!input.cache.contains(address, patches[p].size)) { return false; }
            auto* original    = originals + p * stride;
            auto* replacement = replacements + p * stride;
            memcpy(original, patches[p].original, patches[p].size);
            memcpy(replacement, patches[p].patched, patches[p].size);
            for (UInt32 variant = 0; variant < 2; ++variant) {
                const auto* refs  = variant ? fn.patchedRefs : fn.originalRefs;
                const auto  count = variant ? fn.patchedCount : fn.originalCount;
                for (UInt32 r = 0; r < count; ++r) {
                    if (refs[r].offset + 4 <= loc.offset || refs[r].offset >= loc.offset + patches[p].size) {
                        continue;
                    }
                    UInt64 destination;
                    if (refs[r].offset < loc.offset || refs[r].offset + 4 > loc.offset + patches[p].size
                        || (refs[r].indirect ?
                                !observedTarget(input, refs, r, loc.function, functions, addresses, variants,
                                                destination) :
                                !target(input, refs[r], images, ImageCount, imageRefs, functions, addresses,
                                        FunctionCount, destination, sectionRelative, scopedTargets, scopedTargetCount))
                        || !setRelative(variant ? replacement : original, refs[r].offset - loc.offset, address,
                                        destination))
                    {
                        return false;
                    }
                }
            }
            output[p] = {address - base, original, replacement, patches[p].size};
            for (UInt32 j = 0; j < p; ++j) {
                if (Range{base + output[j].offset, output[j].size}.contains(address, 1)
                    || Range{address, patches[p].size}.contains(base + output[j].offset, 1))
                {
                    return false;
                }
            }
        }
        return true;
    }

    template<typename Patch, size_t ImageCount, size_t FunctionCount, size_t PatchCount, size_t ImageCapacity>
    __attribute__((noinline)) inline bool
        resolveWithImages(Input& input, UInt64 base, const ImageReference (&imageRefs)[ImageCount],
                          const Function (&functions)[FunctionCount], const Patch* patches,
                          const Location (&locations)[PatchCount], Patch* output, UInt8* originals, UInt8* replacements,
                          UInt32 stride, ImageBounds (&images)[ImageCapacity], bool sectionRelative = false,
                          const UserCacheSectionData::Target* scopedTargets = nullptr, UInt32 scopedTargetCount = 0)
    {
        static_assert(ImageCapacity >= ImageCount);
        if (!resolveImages(input, base, imageRefs, images)) { return false; }
        return resolveFunctions(input, base, imageRefs, functions, patches, locations, output, originals, replacements,
                                stride, images, sectionRelative, scopedTargets, scopedTargetCount, nullptr, nullptr);
    }

    // 显式热点入口：只核验已定位的函数及其引用/绑定，不执行全域唯一性扫描。
    template<typename Patch, size_t ImageCount, size_t FunctionCount, size_t PatchCount, size_t ImageCapacity>
    __attribute__((noinline)) inline bool
        verifyAtKnownRVA(Input& input, UInt64 base, const ImageReference (&imageRefs)[ImageCount],
                         const Function (&functions)[FunctionCount], const Patch* patches,
                         const Location (&locations)[PatchCount], const KnownLocation (&known)[FunctionCount],
                         Patch* output, UInt8* originals, UInt8* replacements, UInt32 stride,
                         ImageBounds (&images)[ImageCapacity], bool sectionRelative = false,
                         const UserCacheSectionData::Target* scopedTargets = nullptr, UInt32 scopedTargetCount = 0)
    {
        static_assert(ImageCapacity >= ImageCount);
        return resolveFunctions(input, base, imageRefs, functions, patches, locations, output, originals, replacements,
                                stride, images, sectionRelative, scopedTargets, scopedTargetCount, known, nullptr);
    }

    template<typename Patch, size_t ImageCount, size_t FunctionCount, size_t PatchCount>
    inline bool resolve(Input& input, UInt64 base, const ImageReference (&imageRefs)[ImageCount],
                        const Function (&functions)[FunctionCount], const Patch* patches,
                        const Location (&locations)[PatchCount], Patch* output, UInt8* originals, UInt8* replacements,
                        UInt32 stride, bool sectionRelative = false,
                        const UserCacheSectionData::Target* scopedTargets = nullptr, UInt32 scopedTargetCount = 0)
    {
        ImageBounds images[ImageCount]{};
        return resolveWithImages(input, base, imageRefs, functions, patches, locations, output, originals, replacements,
                                 stride, images, sectionRelative, scopedTargets, scopedTargetCount);
    }

}    // namespace UserCacheResolver
