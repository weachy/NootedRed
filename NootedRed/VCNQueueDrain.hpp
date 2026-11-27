#pragma once

#include <IOKit/IOTypes.h>
#include <libkern/libkern.h>

namespace VCNQueueDrain
{
    constexpr UInt32 DecodeChannel = 8;
    constexpr UInt32 EncodeChannel = 9;

    struct Layout
    {
        UInt32 users[2];
        UInt32 accelerator;
        UInt32 channels[3];
        UInt32 getChannelIndex;
        UInt32 getAccelChannel;
        UInt32 submitted;
    };

    inline UInt32 read32(const UInt8* data)
    {
        UInt32 value;
        memcpy(&value, data, sizeof(value));
        return value;
    }

    inline bool calls(const UInt8* instruction, const UInt8* target)
    {
        return instruction[0] == 0xE8
            && reinterpret_cast<uintptr_t>(instruction + 5)
                    + static_cast<uintptr_t>(static_cast<SInt32>(read32(instruction + 1)))
                == reinterpret_cast<uintptr_t>(target);
    }

    // Matches the branch from entry to the first releaseHardware call and extracts native queue refcounts.
    inline bool readStop(const UInt8* code, size_t size, Layout& layout)
    {
        constexpr UInt8 prefix[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB,
                                    0x83, 0xFE, 0x08, 0x75, 0x2C, 0xFF, 0x8B};
        constexpr UInt8 decodeRelease[] = {0xBE, 8, 0, 0, 0};
        constexpr UInt8 testEnabled[] = {0xF6, 0x43, 0x10, 1, 0x0F, 0x84};
        constexpr UInt8 compareEnabled[] = {0x80, 0x7B, 0x10, 0, 0x0F, 0x84};
        constexpr UInt8 releaseCall[] = {0x48, 0x8B, 0x03, 0x48, 0x89, 0xDF, 0xFF, 0x90, 0xD8, 2, 0, 0};
        constexpr UInt8 encode[] = {0x41, 0x89, 0xF6, 0x41, 0x8D, 0x46, 0xF7, 0x83, 0xF8, 1, 0x77};
        constexpr UInt8 testEncode[] = {0x8A, 0x43, 0x10, 0xA8, 1, 0x74, 0x14};
        constexpr UInt8 compareEncode[] = {0x8A, 0x43, 0x10, 0x3C, 1, 0x75, 0x14};
        constexpr UInt8 secondEncode[] = {0x8A, 0x43, 0x10, 0xBE, 10, 0, 0, 0, 0xA8, 1, 0x74, 0x38};
        constexpr UInt8 sharedEncodeGuard[] = {0x72, 0x80, 0x7B, 0x10, 0, 0x74, 0x6C};
        constexpr UInt8 sharedEncodeJump[] = {0xBE, 10, 0, 0, 0, 0xEB, 0x42};
        constexpr UInt8 sharedReleaseGuard[] = {0x80, 0x7B, 0x10, 0, 0x74, 0x0C};
        if (size < 0x7F || memcmp(code, prefix, sizeof(prefix)) || code[0x15] != 0x0F || code[0x16] != 0x85
            || memcmp(code + 0x1B, decodeRelease, sizeof(decodeRelease))
            || (memcmp(code + 0x20, testEnabled, sizeof(testEnabled))
                && memcmp(code + 0x20, compareEnabled, sizeof(compareEnabled)))
            || memcmp(code + 0x2A, releaseCall, sizeof(releaseCall))
            || code[0x36] != 0xE9 || memcmp(code + 0x3B, encode, sizeof(encode))
            || code[0x47] != 0xFF || code[0x48] != 0x8B || code[0x4D] != 0x75)
        {
            return false;
        }
        const bool inlineRelease = code[0x46] == 0x3A && code[0x4E] == 0x5C
            && (!memcmp(code + 0x4F, testEncode, sizeof(testEncode))
                || !memcmp(code + 0x4F, compareEncode, sizeof(compareEncode)))
            && !memcmp(code + 0x56, releaseCall, 6) && code[0x5C] == 0xBE && read32(code + 0x5D) == EncodeChannel
            && !memcmp(code + 0x61, releaseCall + 6, 6) && !memcmp(code + 0x67, secondEncode, sizeof(secondEncode))
            && !memcmp(code + 0x73, releaseCall, sizeof(releaseCall));
        // Sonoma's second encode channel invokes releaseHardware through a shared release branch.
        const bool sharedRelease = size >= 0xC1 && code[0x46] == 0x26
            && !memcmp(code + 0x4E, sharedEncodeGuard, sizeof(sharedEncodeGuard))
            && !memcmp(code + 0x55, releaseCall, 6) && code[0x5B] == 0xBE && read32(code + 0x5C) == EncodeChannel
            && !memcmp(code + 0x60, releaseCall + 6, 6)
            && !memcmp(code + 0x66, sharedEncodeJump, sizeof(sharedEncodeJump))
            && !memcmp(code + 0xAF, sharedReleaseGuard, sizeof(sharedReleaseGuard))
            && !memcmp(code + 0xB5, releaseCall, sizeof(releaseCall));
        if (!inlineRelease && !sharedRelease) { return false; }
        const auto idle = 0x1BU + read32(code + 0x17);
        const auto decode = read32(code + 0x11);
        const auto encodeUsers = read32(code + 0x49);
        if (idle < 0x7B || idle >= size || 0x2AU + read32(code + 0x26) != idle
            || 0x3BU + read32(code + 0x37) != idle || decode < 0x20 || decode > 0xFFF8
            || decode % alignof(UInt32) || encodeUsers != decode + sizeof(UInt32))
        {
            return false;
        }
        layout.users[0] = decode;
        layout.users[1] = encodeUsers;
        return true;
    }

    // Native suspend path obtains the accel channel from the hardware queue and waits on its submitted timestamp.
    inline bool readSuspend(const UInt8* code, size_t size, const UInt8* wait, Layout& layout)
    {
        constexpr UInt8 loadChannel[] = {0x48, 0x8B, 0xBB};
        constexpr UInt8 lookup[] = {0x48, 0x85, 0xFF, 0x74, 0x2E, 0x4C, 0x8B, 0x73};
        constexpr UInt8 getIndex[] = {0x48, 0x8B, 0x07, 0xFF, 0x90};
        constexpr UInt8 getChannel[] = {0x49, 0x8B, 0x0E, 0x4C, 0x89, 0xF7, 0x89, 0xC6, 0xFF, 0x91};
        constexpr UInt8 loadStamp[] = {0x48, 0x85, 0xC0, 0x74, 0x0E, 0x8B, 0xB0};
        constexpr UInt8 callWait[] = {0x48, 0x89, 0xC7, 0xE8};
        constexpr UInt8 release[] = {0x48, 0x8B, 0x03, 0x48, 0x89, 0xDF, 0xBE};
        constexpr UInt8 releaseCall[] = {0xFF, 0x90, 0xD8, 2, 0, 0};
        size_t matches = 0;
        for (size_t i = 0; i + 75 <= size; ++i) {
            const auto* block = code + i;
            if (memcmp(block, loadChannel, sizeof(loadChannel))
                || memcmp(block + 7, lookup, sizeof(lookup))
                || memcmp(block + 16, getIndex, sizeof(getIndex))
                || memcmp(block + 25, getChannel, sizeof(getChannel))
                || memcmp(block + 39, loadStamp, sizeof(loadStamp))
                || memcmp(block + 50, callWait, sizeof(callWait))
                || memcmp(block + 58, release, sizeof(release))
                || memcmp(block + 69, releaseCall, sizeof(releaseCall))) { continue; }
            const auto channel = read32(block + 65);
            if (channel < DecodeChannel || channel > EncodeChannel + 1 || !calls(block + 53, wait)) { continue; }
            const auto index = channel - DecodeChannel;
            const auto field = read32(block + 3);
            const auto getIndexSlot = read32(block + 21);
            const auto getChannelSlot = read32(block + 35);
            const auto stamp = read32(block + 46);
            if ((matches & (1U << index)) || !field || field > 0xFFF8 || field % sizeof(void*)
                || block[15] < sizeof(void*) || block[15] % sizeof(void*)
                || getIndexSlot != 0x120 || getChannelSlot != 0x1A0
                || !stamp || stamp > 0xFFFC || stamp % alignof(UInt32)
                || (matches && (layout.accelerator != block[15] || layout.submitted != stamp))) { return false; }
            layout.accelerator = block[15];
            layout.channels[index] = field;
            layout.getChannelIndex = getIndexSlot;
            layout.getAccelChannel = getChannelSlot;
            layout.submitted = stamp;
            matches |= 1U << index;
        }
        return matches == 7 && layout.channels[1] == layout.channels[0] + sizeof(void*)
            && layout.channels[2] == layout.channels[1] + sizeof(void*);
    }

    inline bool readsSubmittedStamp(const UInt8* replay, size_t size, const UInt8* wait, UInt32 submitted)
    {
        constexpr UInt8 loadStamp[] = {0x8B, 0xB7};
        constexpr UInt8 nextChannel[] = {0x49, 0xFF, 0xC6, 0x4D, 0x39, 0xF7};
        for (size_t i = 0; i + 17 <= size; ++i) {
            const auto* block = replay + i;
            if (!memcmp(block, loadStamp, sizeof(loadStamp)) && read32(block + 2) == submitted
                && calls(block + 6, wait) && !memcmp(block + 11, nextChannel, sizeof(nextChannel))) { return true; }
        }
        return false;
    }
}
