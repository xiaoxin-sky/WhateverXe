#include "kern_tgl_metal_posh.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    using namespace TGLMetalPosh;
    std::vector<uint8_t> page(pageSize);
    for (size_t i = 0; i < page.size(); ++i) page[i] = static_cast<uint8_t>(i * 7 + 3);
    memcpy(page.data() + deallocOffset, dealloc, sizeof(dealloc));
    const auto original = page;

    // The guard ends at the operator delete call guarded by the patched branch:
    // cmpb $0,(%r14,%rax); jne 0x7ed96; call operator delete; movq $0,(%r14,%rbx).
    const size_t at = branchOffset - deallocOffset;
    const uint8_t compare[] {0x41,0x80,0x3c,0x06,0x00};
    assert(!memcmp(dealloc + at - sizeof(compare), compare, sizeof(compare)));
    assert(!memcmp(dealloc + at, branch, sizeof(branch)));
    assert(dealloc[at + sizeof(branch)] == 0xe8);
    assert(deallocOffset + sizeof(dealloc) == 0xd96 && branchOffset + sizeof(branch) + 5 + 8 == 0xd96);

    assert(candidate(pageOffset, page.data()));
    for (uint64_t offset : {0ULL, pageOffset - pageSize, pageOffset + pageSize, pageOffset + 1, 0x107e000ULL})
        assert(!candidate(offset, page.data()));
    assert(!candidate(pageOffset, nullptr));
    for (size_t i = 0; i < sizeof(dealloc); ++i) {
        page[deallocOffset + i] ^= 0x01;
        assert(!candidate(pageOffset, page.data()));
        page[deallocOffset + i] ^= 0x01;
    }
    assert(page == original);

    apply(page.data());
    assert(!memcmp(page.data() + branchOffset, release, sizeof(release)));
    for (size_t i = 0; i < page.size(); ++i)
        if (i < branchOffset || i >= branchOffset + sizeof(release)) assert(page[i] == original[i]);
    assert(!candidate(pageOffset, page.data())); // A patched page is never patched again.

    const std::string path = "/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver";
    assert(driver(path.c_str()));
    assert(driver(("/System/Volumes/Data" + path).c_str()));
    for (auto other : {path + ".old", path.substr(0, path.size() - 1), "/tmp" + path,
                       "/System/Volumes/Database" + path, "/System/Volumes/Data/System/Volumes/Data" + path,
                       std::string("/Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle/Contents/MacOS/TGLVA"),
                       std::string("/System/Library/Extensions/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver"),
                       std::string()})
        assert(!driver(other.c_str()));
    assert(!driver(nullptr));

    // Sequoia 24H32 cs_validate_page: push rbp; mov rbp,rsp; push r15; push r14;
    // push rbx; sub rsp,0x18; mov rbx,r9 (16 bytes) before any memory operand.
    const uint8_t sequoia[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x18,0x4c,0x89,0xcb,0x4d,0x89,0xc6,0x48};
    assert(routable(sequoia));
    const uint8_t wide[] {0x55,0x48,0x89,0xe5,0x41,0x57,0x48,0x81,0xec,0x00,0x01,0x00,0x00,0x41,0x56,0x48,0x8b,0x05,0,0,0,0};
    assert(routable(wide)); // The RIP-relative load starts after the copied span.
    const uint8_t chained[] {0xff,0x25,0,0,0,0};
    assert(routable(chained));
    const uint8_t shortRoute[] {0xe9,0,0,0,0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
    assert(!routable(shortRoute));
    const uint8_t ripLoad[] {0x55,0x48,0x89,0xe5,0x48,0x8b,0x05,0,0,0,0,0x90,0x90,0x90,0x90,0x90};
    assert(!routable(ripLoad));
    const uint8_t call[] {0x55,0x48,0x89,0xe5,0x41,0x57,0xe8,0,0,0,0,0x90,0x90,0x90,0x90,0x90};
    assert(!routable(call));
    const uint8_t memoryMov[] {0x55,0x48,0x89,0xe5,0x48,0x89,0x45,0xf8,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
    assert(!routable(memoryMov));
    assert(!routable(nullptr));
    puts("PASS: Metal POSH page identity, single in-page release, driver path and PIC route prologue");
}
