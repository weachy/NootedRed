// 用户态补丁共用的页面查询、去重和写权限管理。
#pragma once
#include <IOKit/IOTypes.h>
#include <mach/i386/vm_param.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>

namespace UserPatchMemory
{

    using RegionQuery = kern_return_t (*)(vm_map_t, mach_vm_address_t*, mach_vm_size_t*, natural_t*,
                                          vm_region_recurse_info_t, mach_msg_type_number_t*);
    using ReportError = void          (*)(kern_return_t);

    struct Page
    {
        mach_vm_address_t address{0};
        vm_prot_t         protection{VM_PROT_NONE};
        bool              writable{false};
        bool              maximumPending{false};
    };

    inline bool leafProtection(vm_map_t map, mach_vm_address_t page, vm_prot_t& protection, RegionQuery query,
                               ReportError report, vm_prot_t* maximum = nullptr)
    {
        natural_t depth{0};
        for (UInt32 attempt = 0; attempt < 8; ++attempt) {
            auto           address = page;
            mach_vm_size_t size{0};
            // 只查询映射与权限，避免完整查询逐页统计整个共享缓存区域。
            vm_region_submap_short_info_data_64_t info{};
            mach_msg_type_number_t                count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
            const auto                            result =
                query(map, &address, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
            if (result != KERN_SUCCESS) {
                report(result);
                return false;
            }
            if (count < VM_REGION_SUBMAP_SHORT_INFO_COUNT_64 || address > page || page - address >= size
                || size - (page - address) < PAGE_SIZE)
            {
                return false;
            }
            if (!info.is_submap) {
                protection = info.protection;
                if (maximum) { *maximum = info.max_protection; }
                return true;
            }
            if (depth == ~0U) { break; }
            ++depth;
        }
        return false;
    }

    inline Page* findPage(Page* pages, UInt32 count, mach_vm_address_t address)
    {
        for (UInt32 i = 0; i < count; ++i) {
            if (pages[i].address == address) { return &pages[i]; }
        }
        return nullptr;
    }

    template<size_t Capacity>
    inline bool collectPages(Page (&pages)[Capacity], UInt32& count, UInt64 address, UInt32 size)
    {
        const auto last = (address + size - 1) & ~static_cast<UInt64>(PAGE_SIZE - 1);
        for (auto page = address & ~static_cast<UInt64>(PAGE_SIZE - 1); page <= last; page += PAGE_SIZE) {
            if (findPage(pages, count, page)) { continue; }
            if (count == Capacity) { return false; }
            pages[count++].address = page;
        }
        return true;
    }

    inline bool makeWritable(vm_map_t map, Page& page, ReportError report)
    {
        if (page.writable) { return true; }
        const auto result =
            vm_protect(map, page.address, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (result != KERN_SUCCESS) {
            report(result);
            return false;
        }
        page.writable = true;
        return true;
    }

    inline bool prepareRollback(vm_map_t map, Page* pages, UInt32 count, UInt64 address, UInt32 size,
                                ReportError report)
    {
        const auto last  = (address + size - 1) & ~static_cast<UInt64>(PAGE_SIZE - 1);
        bool       ready = true;
        for (auto current = address & ~static_cast<UInt64>(PAGE_SIZE - 1); current <= last; current += PAGE_SIZE) {
            auto* page = findPage(pages, count, current);
            if (!page || !makeWritable(map, *page, report)) { ready = false; }
        }
        return ready;
    }

    inline bool restorePages(vm_map_t map, Page* pages, UInt32 count, ReportError report)
    {
        bool restored = true;
        for (UInt32 i = 0; i < count; ++i) {
            auto& page = pages[i];
            if (!page.writable && !page.maximumPending) { continue; }
            // 先恢复当前权限，再下调最大权限；set_maximum 会取当前权限与新最大权限的交集。
            if (page.writable) {
                auto result = vm_protect(map, page.address, PAGE_SIZE, FALSE, page.protection);
                if (result != KERN_SUCCESS) {
                    report(result);
                    result = vm_protect(map, page.address, PAGE_SIZE, FALSE, page.protection);
                }
                if (result != KERN_SUCCESS) {
                    restored = false;
                    continue;
                }
                page.writable       = false;
                page.maximumPending = true;
            }
            const auto maximum = vm_protect(map, page.address, PAGE_SIZE, TRUE, page.protection);
            if (maximum != KERN_SUCCESS) {
                report(maximum);
                restored = false;
            }
            else {
                page.maximumPending = false;
            }
        }
        return restored;
    }

    // 共享缓存执行器适配器：检查、COW、回滚与权限恢复只保留一份实现。
    template<typename Read, typename Write>
    struct ExecutorOps
    {
        vm_map_t    map;
        RegionQuery query;
        ReportError report;
        Read        readUser;
        Write       writeUser;

        bool check(Page& page)
        {
            return leafProtection(map, page.address, page.protection, query, report)
                   && page.protection == (VM_PROT_READ | VM_PROT_EXECUTE);
        }
        bool makeWritable(Page& page) { return UserPatchMemory::makeWritable(map, page, report); }
        bool prepareRollback(Page* pages, const UInt32 count, const UInt64 address, const UInt32 size)
        {
            return UserPatchMemory::prepareRollback(map, pages, count, address, size, report);
        }
        bool restore(Page* pages, const UInt32 count)
        {
            return UserPatchMemory::restorePages(map, pages, count, report);
        }
        int write(const UInt8* bytes, const UInt64 address, const UInt32 size)
        {
            return writeUser(bytes, address, size);
        }
        int read(const UInt64 address, UInt8* bytes, const UInt32 size) { return readUser(address, bytes, size); }
        template<size_t Capacity>
        bool collectPages(Page (&pages)[Capacity], UInt32& count, const UInt64 address, const UInt32 size)
        {
            return UserPatchMemory::collectPages(pages, count, address, size);
        }
    };

}    // namespace UserPatchMemory
