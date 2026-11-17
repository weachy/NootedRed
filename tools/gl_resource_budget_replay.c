#include "budget-bytes.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* CPU-only execution of the host driver's actual resource-list routines.
 * All external calls are redirected to the stubs below. GPU APIs and command
 * submission pages are never executable in this replay.
 */
struct Resource {
    uint8_t shared[0x140], client[0x40];
    uint64_t size;
    unsigned retains;
};
static void* shared(void* p, void** out)
{
    struct Resource* r = p;
    *out = r->client;
    return r->shared;
}
static uint64_t size(void* p) { return ((struct Resource*)p)->size; }
static void retain(void* p) { ++((struct Resource*)p)->retains; }
static void put(void* dst, size_t off, const void* src, size_t n)
{
    memcpy((uint8_t*)dst + off, src, n);
}
static uint32_t u32(const void* p, size_t o)
{
    uint32_t v;
    memcpy(&v, (const uint8_t*)p + o, 4);
    return v;
}
static void stub(uint8_t* image, size_t at, size_t slot, void* target)
{
    assert(image[at] == 0xff && image[at + 1] == 0x25);
    int32_t delta = (int32_t)(slot - at - 6);
    put(image, at + 2, &delta, 4);
    put(image, slot, &target, 8);
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    FILE* f = fopen(argv[1], "rb");
    assert(f);
    uint8_t* image = mmap(NULL, 0xf0000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(image != MAP_FAILED);
    assert(fread(image, 1, 0xf0000, f) == 0xf0000);
    fclose(f);
    assert(memcmp(image + 0x572a, budget_find, sizeof(budget_find)) == 0);
    stub(image, 0xec4e4, 0xedef0, shared);
    stub(image, 0xec4f6, 0xedef8, size);
    stub(image, 0xec484, 0xedf00, retain);
    assert(mprotect(image, 0xf0000, PROT_NONE) == 0);
    assert(mprotect(image + 0xec000, 0x1000, PROT_READ | PROT_EXEC) == 0);
    assert(mprotect(image + 0xed000, 0x1000, PROT_READ) == 0);
    void (*init)(void*, void*) = (void*)(image + 0x572a);
    uint32_t (*add)(void*, void*, int) = (void*)(image + 0x58db);
    unsigned failures = 0, checks = 0;
    unsigned amounts[] = {128, 256, 384, 512, 513, 768, 1024, 2048, 4096, 16384};
    for (unsigned patched = 0; patched < 2; ++patched) {
        assert(mprotect(image + 0x5000, 0x1000, PROT_READ | PROT_WRITE) == 0);
        memcpy(image + 0x572a, patched ? budget_replace : budget_find, 50);
        assert(mprotect(image + 0x5000, 0x1000, PROT_READ | PROT_EXEC) == 0);
        for (unsigned i = 0; i < sizeof(amounts) / sizeof(amounts[0]); ++i) {
            uint8_t ctx[0x12e0] = {0}, hw[0xb4] = {0};
            uint8_t list[0x28], expected[0x28] = {0};
            void* hwptr = hw;
            void* ctxptr = ctx;
            memset(list, 0xa5, sizeof(list));
            put(ctx, 0x12d8, &hwptr, 8);
            put(hw, 0x14, &amounts[i], 4);
            put(expected, 0x10, &ctxptr, 8);
            uint32_t total = amounts[i] * 1024;
            uint32_t original = total - 0x80000;
            uint32_t policy = total / 2;
            if (total > 0x80000 && total - 0x80000 > policy)
                policy = total - 0x80000;
            uint32_t budget = patched ? policy : original;
            put(expected, 0x20, &budget, 4);
            init(list, ctx);
            assert(memcmp(list, expected, sizeof(list)) == 0);
            ++checks;
            if (amounts[i] != 512) continue;
            uint32_t ids[256] = {0};
            void* ptrs[256] = {0};
            void* idptr = ids;
            void* resources = ptrs;
            uint16_t capacity = 256;
            put(list, 2, &capacity, 2);
            put(list, 8, &idptr, 8);
            put(list, 0x18, &resources, 8);
            struct Resource r = { .size = 4096 };
            uint32_t id = 7;
            put(r.shared, 0x100, &id, 4);
            uint32_t index = add(list, &r, 0);
            assert(index == (patched ? 0 : 65535));
            assert((u32(list, 0) & 0xffff) == patched);
            assert(ids[0] == (patched ? 7 : 0));
            assert(r.retains == patched);
            assert(u32(r.client, 0x18) == patched);
            checks += 5;
            if (patched) {
                assert(ptrs[0] == &r);
                assert(u32(list, 0x24) == 4);
                assert(add(list, &r, 1) == 0);
                assert(ids[0] == 0x80000007);
                assert(r.retains == 1);
                checks += 5;
            } else ++failures;
            printf("%s 512MiB: budget=%uKiB add-scratch-index=%u count=%u\n",
                patched ? "fixed" : "original", budget, index, u32(list, 0) & 0xffff);
        }
    }
    assert(failures == 1);
    munmap(image, 0xf0000);
    printf("PASS: %u CPU checks; original zero-budget rejection reproduced; "
           "fixed actual AddRsrc accepts scratch. No GPU submission.\n", checks);
}
