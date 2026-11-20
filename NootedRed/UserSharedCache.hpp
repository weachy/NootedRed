// 有读取预算的 dyld 缓存与 Mach-O 解析；仅读取当前进程，供用户态补丁共用。
#pragma once
#include <IOKit/IOTypes.h>
#include <string.h>
namespace UserSharedCache
{

    using Reader                   = int (*)(UInt64, void*, size_t);
    using ContextReader            = int (*)(void*, UInt64, void*, size_t);
    constexpr UInt64   UserLimit   = 0x0000800000000000ULL;
    constexpr UInt32   MaxTextSize = 8 * 1024 * 1024;
    struct ImageReference
    {
        const char* path;
        UInt32      size;
    };

    struct Range
    {
        UInt64 address{0}, size{0};
        bool   contains(UInt64 value, UInt64 length) const
        {
            return value >= address && value - address <= size && length <= size - (value - address);
        }
    };

    struct Input
    {
        Reader                reader;
        Range                 cache;
        UInt32                reads{0}, bytes{0}, candidates{0};
        bool                  failed{false}, tableRead{false};
        const ImageReference* imageRefs{nullptr};
        UInt32                imageCount{0};
        UInt64                imageAddresses[16]{};
        void*                 readerContext{nullptr};
        ContextReader         contextReader{nullptr};
        // 由单次事务的堆工作区提供，不保存任何进程的匹配结果。
        UInt8*                scanBuffer{nullptr};
        static constexpr UInt32 ScanSize = 4096;
        void selectImages(UInt64 base, const ImageReference* refs, UInt32 count)
        {
            bool reusable = tableRead && cache.address == base && refs && imageRefs && count <= 16 && imageCount <= 16;
            for (UInt32 i = 0; reusable && i < count; ++i) {
                if (!refs[i].path || refs[i].size == 0 || refs[i].size > 256) { reusable = false; break; }
                bool found = false;
                for (UInt32 j = 0; j < imageCount; ++j) {
                    if (refs[i].size == imageRefs[j].size
                        && memcmp(refs[i].path, imageRefs[j].path, refs[i].size) == 0) { found = true; break; }
                }
                reusable = found;
            }
            if (reusable) { return; }
            imageRefs = refs;
            imageCount = count;
            tableRead = false;
            memset(imageAddresses, 0, sizeof(imageAddresses));
        }
        bool                  read(UInt64 address, void* value, UInt32 size)
        {
            if (failed || !cache.contains(address, size) || reads >= 32768 || size > 32 * 1024 * 1024 - bytes) {
                failed = true;
                return false;
            }
            ++reads;
            bytes += size;
            if ((contextReader ? contextReader(readerContext, address, value, size) : reader(address, value, size)) != 0) {
                failed = true;
                return false;
            }
            return true;
        }
    };

    inline UInt32 word(const UInt8* value)
    {
        UInt32 result;
        memcpy(&result, value, sizeof(result));
        return result;
    }

    inline UInt64 wide(const UInt8* value)
    {
        UInt64 result;
        memcpy(&result, value, sizeof(result));
        return result;
    }

    inline bool relativeTarget(UInt64 address, UInt32 offset, const UInt8* bytes, UInt64& target)
    {
        if (bytes == nullptr || address >= UserLimit - 4 || offset >= UserLimit - address - 4) { return false; }
        int displacement;
        memcpy(&displacement, bytes + offset, sizeof(displacement));
        const auto next = address + offset + 4;
        if (displacement < 0) {
            const auto magnitude = static_cast<UInt64>(-static_cast<long long>(displacement));
            if (next < magnitude) { return false; }
            target = next - magnitude;
        }
        else {
            if (next >= UserLimit || static_cast<UInt64>(displacement) >= UserLimit - next) { return false; }
            target = next + static_cast<UInt64>(displacement);
        }
        return true;
    }

    inline bool setRelative(UInt8* bytes, UInt32 offset, UInt64 address, UInt64 target)
    {
        if (bytes == nullptr || address >= UserLimit - 4 || offset >= UserLimit - address - 4) { return false; }
        const auto next = address + offset + 4;
        if (next >= UserLimit || target >= UserLimit) { return false; }
        const auto magnitude = target >= next ? target - next : next - target;
        if (magnitude > (target >= next ? 0x7FFFFFFFULL : 0x80000000ULL)) { return false; }
        const auto signedValue =
            target >= next ? static_cast<long long>(magnitude) : -static_cast<long long>(magnitude);
        const auto value = static_cast<int>(signedValue);
        memcpy(bytes + offset, &value, sizeof(value));
        return true;
    }

    struct Image
    {
        UInt64 address{0};
        Range  text;
        Range  constants[8]{};
        UInt32 constantCount{0};
        Range  segments[16]{};
        UInt32 segmentCount{0};
        bool   contains(UInt64 value, UInt32 size) const
        {
            for (UInt32 i = 0; i < segmentCount; ++i) {
                if (segments[i].contains(value, size)) { return true; }
            }
            return false;
        }
        bool constant(UInt64 value) const
        {
            for (UInt32 i = 0; i < constantCount; ++i) {
                if (constants[i].contains(value, 4)) { return true; }
            }
            return false;
        }
    };

    __attribute__((noinline))
    inline bool imageForCache(Input& input, UInt64 base, Image& image, const char* pathName, UInt32 pathSize)
    {
        UInt8 header[456];
        if (!pathName || pathSize == 0 || pathSize > 256 || !input.reader || base >= UserLimit - sizeof(header)
            || (base & 4095) != 0 || input.reader(base, header, sizeof(header)) != 0
            || memcmp(header, "dyld_v1 x86_64h", 16) != 0)
        {
            return false;
        }
        // Darwin 25 的现代 image table；所有元数据都必须落在主缓存的首个只读映射内。
        const auto mappingOffset = word(header + 16), mappingCount = word(header + 20);
        const auto unslid = wide(header + 224), size = wide(header + 232), maxSlide = wide(header + 240);
        if (mappingOffset < sizeof(header) || mappingOffset > 65536 || mappingCount == 0 || mappingCount > 64
            || unslid == 0 || unslid > base || base - unslid > maxSlide || size == 0 || size > 0x1000000000ULL
            || size > UserLimit - base)
        {
            return false;
        }
        input.cache = {base, size};
        UInt8 mapping[32];
        if (!input.read(base + mappingOffset, mapping, sizeof(mapping)) || wide(mapping) != unslid
            || wide(mapping + 16) != 0 || (word(mapping + 28) & 1) == 0)
        {
            return false;
        }
        const auto metadataSize = wide(mapping + 8);
        if (metadataSize == 0 || metadataSize > size) { return false; }
        const Range metadata{base, metadataSize < 16 * 1024 * 1024 ? metadataSize : 16 * 1024 * 1024};
        const auto  imagesOffset = word(header + 448), imageCount = word(header + 452);
        if (imageCount == 0 || imageCount > 8192 || imagesOffset < sizeof(header)
            || !metadata.contains(base + imagesOffset, static_cast<UInt64>(imageCount) * 32))
        {
            return false;
        }
        const auto slide = base - unslid;
        UInt64     imageAddress{0};
        UInt32     pathReadSize = pathSize;
        if (input.imageCount > 16 || (input.imageCount && !input.imageRefs)) { return false; }
        for (UInt32 p = 0; p < input.imageCount; ++p) {
            if (!input.imageRefs[p].path || input.imageRefs[p].size > 256 || input.imageRefs[p].size == 0) {
                return false;
            }
            if (pathReadSize < input.imageRefs[p].size) { pathReadSize = input.imageRefs[p].size; }
        }
        UInt8 entries[512];
        Range pathWindow;
        for (UInt32 i = 0; !input.tableRead && i < imageCount;) {
            const auto count = imageCount - i < 16 ? imageCount - i : 16;
            if (!input.read(base + imagesOffset + static_cast<UInt64>(i) * 32, entries, count * 32)) { return false; }
            for (UInt32 j = 0; j < count; ++j) {
                const auto* entry      = entries + j * 32;
                const auto  pathOffset = word(entry + 24);
                if (!metadata.contains(base + pathOffset, pathReadSize)) { return false; }
                char path[256];
                if (input.scanBuffer) {
                    // 路径密集存放于只读元数据区；复用事务缓冲，避免每个目录项单独 copyin。
                    const auto address = base + pathOffset;
                    if (!pathWindow.contains(address, pathReadSize)) {
                        const auto remaining = metadata.size - pathOffset;
                        const auto length    = remaining < Input::ScanSize ? UInt32(remaining) : Input::ScanSize;
                        if (!input.read(address, input.scanBuffer, length)) { return false; }
                        pathWindow = {address, length};
                    }
                    memcpy(path, input.scanBuffer + (address - pathWindow.address), pathReadSize);
                }
                else if (!input.read(base + pathOffset, path, pathReadSize)) {
                    return false;
                }
                for (UInt32 p = 0; p < input.imageCount; ++p) {
                    if (memcmp(path, input.imageRefs[p].path, input.imageRefs[p].size) != 0) { continue; }
                    const auto candidate = wide(entry);
                    if (input.imageAddresses[p] || candidate < unslid || candidate - unslid >= size) { return false; }
                    input.imageAddresses[p] = candidate + slide;
                }
                if (memcmp(path, pathName, pathSize) != 0) { continue; }
                const auto address = wide(entry);
                if (imageAddress != 0 || address < unslid || address - unslid >= size) { return false; }
                imageAddress = address + slide;
            }
            i += count;
        }
        if (input.imageCount) {
            input.tableRead = true;
            for (UInt32 p = 0; p < input.imageCount; ++p) {
                if (!input.imageAddresses[p]) { return false; }
                if (input.imageRefs[p].size == pathSize && memcmp(input.imageRefs[p].path, pathName, pathSize) == 0) {
                    imageAddress = input.imageAddresses[p];
                }
            }
        }
        if (imageAddress == 0) { return false; }
        image.address = imageAddress;
        UInt8 mach[32];
        if (!input.read(imageAddress, mach, sizeof(mach)) || word(mach) != 0xFEEDFACF || word(mach + 4) != 0x01000007
            || (word(mach + 12) != 6 && word(mach + 12) != 8))
        {
            return false;
        }
        const auto commands = word(mach + 16), commandSize = word(mach + 20);
        if (commands == 0 || commands > 128 || commandSize > 65536
            || !input.cache.contains(imageAddress + 32, commandSize))
        {
            return false;
        }
        UInt32 cursor{0};
        for (UInt32 i = 0; i < commands; ++i) {
            UInt8 command[72];
            if (cursor > commandSize || commandSize - cursor < 8 || !input.read(imageAddress + 32 + cursor, command, 8))
            {
                return false;
            }
            const auto kind = word(command), length = word(command + 4);
            if (length < 8 || (length & 7) != 0 || length > commandSize - cursor) { return false; }
            if (kind == 0x19) {
                if (length < sizeof(command) || !input.read(imageAddress + 32 + cursor, command, sizeof(command))) {
                    return false;
                }
                const auto sections = word(command + 64);
                if (sections > 64 || length != 72 + sections * 80) { return false; }
                const auto segmentStart = wide(command + 24), segmentSize = wide(command + 32);
                if (segmentStart < unslid || segmentStart - unslid >= size
                    || segmentSize > size - (segmentStart - unslid))
                {
                    return false;
                }
                const Range segment{segmentStart + slide, segmentSize};
                if (image.segmentCount == 16) { return false; }
                image.segments[image.segmentCount++] = segment;
                for (UInt32 j = 0; j < sections; ++j) {
                    UInt8 section[80];
                    if (!input.read(imageAddress + 32 + cursor + 72 + j * 80, section, sizeof(section))) {
                        return false;
                    }
                    const auto address = wide(section + 32), sectionSize = wide(section + 40);
                    if (address < unslid || address - unslid >= size || !segment.contains(address + slide, sectionSize))
                    {
                        return false;
                    }
                    if (memcmp(section, "__text\0", 7) == 0 && (word(command + 60) & 5) == 5) {
                        if (image.text.size != 0 || sectionSize == 0 || sectionSize > 64 * 1024 * 1024) {
                            return false;
                        }
                        image.text = {address + slide, sectionSize};
                    }
                    if (memcmp(section, "__const\0", 8) == 0 && (word(command + 60) & 1) != 0) {
                        if (image.constantCount == 8) { return false; }
                        image.constants[image.constantCount++] = {address + slide, sectionSize};
                    }
                }
            }
            cursor += length;
        }
        return cursor == commandSize && image.text.size != 0;
    }

}    // namespace UserSharedCache
