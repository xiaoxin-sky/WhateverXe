// Copyright (c) 2026 WhateverXe contributors. BSD-3-Clause.
#include "kern_tgl_metal_posh.hpp"
#include <Headers/kern_api.hpp>
#include <IOKit/IORegistryEntry.h>
#include <kern/thread_call.h>
#include <libkern/OSAtomic.h>

namespace {
mach_vm_address_t originalValidate;
int (*getPath)(void *, char *, int *);
thread_call_t publishCall;
volatile SInt32 releases;

void publish(thread_call_param_t, thread_call_param_t) {
    IORegistryEntry::getRegistryRoot()->setProperty("WhateverXeMetalPoshPages",
        static_cast<unsigned long long>(releases), 32);
}
// Keep the path buffer out of the per-page frame.
__attribute__((noinline)) bool driverVnode(void *vnode) {
    char path[256];
    int length = sizeof(path);
    // vn_getpath does not enter the filesystem; the pager holds the vnode.
    return vnode && !getPath(vnode, path, &length) && TGLMetalPosh::driver(path);
}
// Big Sur and later: cs_validate_page(vnode, pager, offset, data, validated, tainted, nx).
void validatePage(void *vnode, void *pager, uint64_t offset, const void *data,
                  int *validated, int *tainted, int *nx) {
    FunctionCast(validatePage, originalValidate)(vnode, pager, offset, data, validated, tainted, nx);
    // Patch after the hash check, in the page the pager shares with every
    // mapping. The file and its signature stay unchanged; a re-read page is
    // validated and patched again, and a patched page no longer matches.
    auto page = static_cast<uint8_t *>(const_cast<void *>(data));
    if (!TGLMetalPosh::candidate(offset, page) || !driverVnode(vnode)) return;
    TGLMetalPosh::apply(page);
    OSIncrementAtomic(&releases);
    if (publishCall) thread_call_enter(publishCall);
}
}

void TGLMetalPosh::processKernel(KernelPatcher &p) {
    const auto v = getKernelVersion();
    if (v < KernelVersion::BigSur || v > KernelVersion::Tahoe) return;
    auto root = IORegistryEntry::getRegistryRoot();
    bool ready = false;
    if (!checkKernelArgument("-wex-no-posh-free")) {
        getPath = reinterpret_cast<decltype(getPath)>(p.solveSymbol(KernelPatcher::KernelID, "_vn_getpath"));
        p.clearError();
        const auto validate = p.solveSymbol(KernelPatcher::KernelID, "_cs_validate_page");
        p.clearError();
        publishCall = thread_call_allocate(publish, nullptr);
        if (getPath && validate && routable(reinterpret_cast<const uint8_t *>(validate))) {
            // RestrictEvents and WhateverGreen share this route; Lilu requires long jumps.
            KernelPatcher::RouteRequest route("_cs_validate_page", validatePage, originalValidate);
            ready = p.routeMultipleLong(KernelPatcher::KernelID, &route, 1);
            p.clearError();
        }
    }
    root->setProperty("WhateverXeMetalPoshRoute", ready);
    SYSLOG("wex", "Metal POSH release route ready=%d", ready);
}
