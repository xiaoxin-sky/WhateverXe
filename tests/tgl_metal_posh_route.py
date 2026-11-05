#!/usr/bin/env python3
"""Run the production cs_validate_page wrapper and route gate with mocked kernel calls."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
source = (root/'WhateverXe/kern_tgl_metal_posh.cpp').read_text()
body = source.split('#include <libkern/OSAtomic.h>\n', 1)[1]
harness = r'''
#include "kern_tgl_metal_posh.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using mach_vm_address_t = uintptr_t;
using SInt32 = int32_t;
using thread_call_param_t = void *;
using thread_call_t = void *;
using thread_call_func_t = void (*)(thread_call_param_t, thread_call_param_t);
namespace KernelVersion { enum { Catalina=19, BigSur, Monterey, Ventura, Sonoma, Sequoia, Tahoe }; }
int version = KernelVersion::Sequoia; int getKernelVersion() { return version; }
std::set<std::string> args; bool checkKernelArgument(const char *name) { return args.count(name); }
#define SYSLOG(...) ((void)0)
template <typename T> T FunctionCast(T, mach_vm_address_t address) { return reinterpret_cast<T>(address); }
SInt32 OSIncrementAtomic(volatile SInt32 *value) { return (*value)++; }
thread_call_func_t publisher; bool allocateCalls = true; int entered = 0;
thread_call_t thread_call_allocate(thread_call_func_t f, thread_call_param_t) { publisher = f; return allocateCalls ? (void *)1 : nullptr; }
bool thread_call_enter(thread_call_t) { ++entered; return true; }
int routeProperty = -1; unsigned long long pagesProperty = ~0ULL;
struct IORegistryEntry {
    static IORegistryEntry *getRegistryRoot() { static IORegistryEntry r; return &r; }
    void setProperty(const char *key, bool value) { assert(!strcmp(key, "WhateverXeMetalPoshRoute")); routeProperty = value; }
    void setProperty(const char *key, unsigned long long value, unsigned bits) {
        assert(!strcmp(key, "WhateverXeMetalPoshPages") && bits == 32); pagesProperty = value; }
};
int originals = 0; std::vector<uint8_t> *expectAtOriginal;
void original(void *vnode, void *pager, uint64_t offset, const void *data, int *validated, int *tainted, int *nx) {
    ++originals;
    assert(vnode == (void *)0x10 && pager == (void *)0x20 && validated && tainted && nx);
    // The hash check always sees the signed bytes.
    if (expectAtOriginal) assert(!memcmp(data, expectAtOriginal->data(), TGLMetalPosh::pageSize));
    *validated = 1; (void)offset;
}
const char *path = "/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver";
int pathReads = 0; int pathError = 0;
int readPath(void *vnode, char *out, int *length) {
    ++pathReads; assert(vnode == (void *)0x10 && *length == 256);
    if (pathError) return pathError;
    strcpy(out, path); return 0;
}
std::vector<uint8_t> prologue {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x18,0x4c,0x89,0xcb,0x4d,0x89,0xc6,0x48};
bool haveVnGetPath = true, haveValidate = true, routeResult = true; int routes = 0;
class KernelPatcher {
public:
    static constexpr size_t KernelID = 0;
    struct RouteRequest {
        const char *symbol; mach_vm_address_t to; mach_vm_address_t *org;
        template <class T> RouteRequest(const char *s, T t, mach_vm_address_t &o) : symbol(s), to(reinterpret_cast<mach_vm_address_t>(t)), org(&o) {}
    };
    mach_vm_address_t solveSymbol(size_t id, const char *name) {
        assert(id == KernelID);
        if (!strcmp(name, "_vn_getpath")) return haveVnGetPath ? reinterpret_cast<mach_vm_address_t>(&readPath) : 0;
        if (!strcmp(name, "_cs_validate_page")) return haveValidate ? reinterpret_cast<mach_vm_address_t>(prologue.data()) : 0;
        assert(false); return 0;
    }
    bool routeMultipleLong(size_t id, RouteRequest *r, size_t n) {
        assert(id == KernelID && n == 1 && !strcmp(r->symbol, "_cs_validate_page"));
        ++routes; *r->org = reinterpret_cast<mach_vm_address_t>(&original); return routeResult;
    }
    void clearError() {}
};
''' + body + r'''
void reset() { haveVnGetPath = haveValidate = routeResult = allocateCalls = true; routes = 0; routeProperty = -1; args.clear(); }
int main() {
    using namespace TGLMetalPosh;
    // Route gate: OS range, opt-out, missing symbols, prologue and Lilu result.
    for (version = 19; version <= 26; ++version) {
        reset(); KernelPatcher p; processKernel(p);
        const bool known = version >= 20 && version <= 25;
        assert(routes == (known ? 1 : 0) && routeProperty == (known ? 1 : -1));
    }
    version = KernelVersion::Sequoia;
    reset(); args.insert("-wex-no-posh-free"); { KernelPatcher p; processKernel(p); } assert(!routes && routeProperty == 0);
    reset(); haveVnGetPath = false; { KernelPatcher p; processKernel(p); } assert(!routes && routeProperty == 0);
    reset(); haveValidate = false; { KernelPatcher p; processKernel(p); } assert(!routes && routeProperty == 0);
    reset(); routeResult = false; { KernelPatcher p; processKernel(p); } assert(routes == 1 && routeProperty == 0);
    auto saved = prologue;
    reset(); prologue = {0xe9,0,0,0,0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
    { KernelPatcher p; processKernel(p); } assert(!routes && routeProperty == 0); // Shared short route would panic Lilu.
    reset(); prologue = {0x55,0x48,0x89,0xe5,0x48,0x8b,0x05,0,0,0,0,0x90,0x90,0x90,0x90,0x90};
    { KernelPatcher p; processKernel(p); } assert(!routes && routeProperty == 0);
    reset(); prologue = {0xff,0x25,0,0,0,0}; { KernelPatcher p; processKernel(p); } assert(routes == 1 && routeProperty == 1);
    prologue = saved;
    reset(); { KernelPatcher p; processKernel(p); } assert(routes == 1 && routeProperty == 1 && publisher);

    // Page wrapper: original first, path only for exact candidates, one release.
    std::vector<uint8_t> page(pageSize, 0xcc);
    memcpy(page.data() + deallocOffset, dealloc, sizeof(dealloc));
    const auto signedPage = page;
    int validated = 0, tainted = 0, nx = 0;
    auto run = [&](uint64_t offset) { validatePage((void *)0x10, (void *)0x20, offset, page.data(), &validated, &tainted, &nx); };
    expectAtOriginal = &page;
    run(pageOffset + pageSize); assert(originals == 1 && pathReads == 0 && page == signedPage);
    auto other = page; other[deallocOffset] ^= 1; page = other;
    run(pageOffset); assert(originals == 2 && pathReads == 0 && page == other);
    page = signedPage;
    for (const char *wrong : {"/tmp/AppleIntelTGLGraphicsMTLDriver",
        "/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver.old"}) {
        path = wrong; run(pageOffset); assert(page == signedPage && !entered);
    }
    path = "/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver";
    pathError = 63; run(pageOffset); assert(page == signedPage && !entered); pathError = 0;
    const int reads = pathReads;
    run(pageOffset);
    assert(validated == 1 && pathReads == reads + 1 && entered == 1);
    assert(!memcmp(page.data() + branchOffset, release, sizeof(release)));
    for (size_t i = 0; i < page.size(); ++i)
        if (i < branchOffset || i >= branchOffset + sizeof(release)) assert(page[i] == signedPage[i]);
    publisher(nullptr, nullptr); assert(pagesProperty == 1);
    // A page already in the cache is not patched or path-checked again.
    expectAtOriginal = nullptr; const auto patched = page;
    run(pageOffset); assert(page == patched && pathReads == reads + 1 && entered == 1);
    // The Data-volume spelling of the same file is accepted.
    page = signedPage; expectAtOriginal = &page;
    path = "/System/Volumes/Data/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver";
    run(pageOffset); assert(page == patched && entered == 2);
    publisher(nullptr, nullptr); assert(pagesProperty == 2);
    puts("PASS: production page wrapper and route gate: exact page, exact driver path, original hash first, PIC prologue");
}
'''
with tempfile.TemporaryDirectory(prefix='wxe-posh-') as d:
    p = pathlib.Path(d); (p/'test.cpp').write_text(harness)
    subprocess.run(['clang++','-std=c++17','-Wno-mismatched-tags','-fsanitize=address,undefined','-I'+str(root/'WhateverXe'),str(p/'test.cpp'),'-o',str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
