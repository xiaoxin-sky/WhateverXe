#!/usr/bin/env python3
"""Run the production eventTimeout fallback wrapper with mocked kernel calls.
With the accelerator binary as argument, also check every qualified byte."""
import pathlib, re, struct, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
source = (root/'WhateverXe/kern_xe.cpp').read_text()
header = (root/'WhateverXe/kern_tgl_event_timeout.hpp').read_text()
wait_header = (root/'WhateverXe/kern_tgl_same_engine_wait.hpp').read_text()
body = source[source.index('uintptr_t tglOrigEventTimeout {};'):source.index('bool tglEventTimeoutSerialize')]

install = source[source.index('bool eventTimeoutFallbackOK=false;'):source.index('"TGLEventTimeoutFallbackReady",eventTimeoutFallbackOK);')]
for needle in ['recoveryRequested && recoveryOK && !checkKernelArgument("-wex-no-timeout-fallback")',
               '__ZN19IGAccelEventMachine12eventTimeoutEi',
               '__ZN11IGScheduler19haltCommandStreamerE10IGHwCsType',
               '__ZN11IGScheduler21resumeCommandStreamerE10IGHwCsType',
               '__ZN12IGScheduler517getActiveContextsE10IGHwCsTypePP17IGHardwareContextPyS3_',
               '__ZN12IGScheduler515prepareGPUResetE10IGHwCsType',
               'TGLEventTimeout::qualified(reinterpret_cast<const uint8_t *>(address))',
               '{timeoutSym,tglEventTimeout,tglOrigEventTimeout}']:
    assert needle in install, needle
assert install.index('qualified(') < install.index('routeMultiple(')
admission = re.search(r'tglAcceleratorReady = [^;]+;', source).group()
assert 'eventTimeout' not in admission

harness = r'''
#include "kern_tgl_event_timeout.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
uint64_t clockNow, origAdvance;
void clock_get_uptime(uint64_t *t) { *t = clockNow; }
template <typename T> T FunctionCast(T, uintptr_t address) { return reinterpret_cast<T>(address); }
void *origReturn;
void *origEventTimeout(void *, int32_t) { clockNow += origAdvance; return origReturn; }
int pauses, resumes, halts[8], csResumes[8], prepares[8];
void *activeCtx0[8], *activeCtx1[8];
uint64_t activeElapsed[8];
void mockPause(void *) { ++pauses; }
void mockResume(void *) { ++resumes; }
void mockPrepare(void *, uint32_t e) { ++prepares[e]; }
void mockGetActive(void *, uint32_t e, void **c0, uint64_t *el, void **c1) {
    *c0 = activeCtx0[e]; *el = activeElapsed[e]; *c1 = activeCtx1[e];
}
void mockHalt(void *, uint32_t e) { ++halts[e]; }
void mockResumeCs(void *, uint32_t e) { ++csResumes[e]; }
namespace {
''' + body + r'''
}
alignas(16) uint8_t em[0x40], accel[0x1300], rcsCtx[0x100], bcsCtx[0x100], rcsChan[0x40], bcsChan[0x40];
uintptr_t schedVtab[0x200 / 8];
uintptr_t *schedObj = schedVtab;
void resetCounters() {
    pauses = resumes = 0;
    memset(halts, 0, sizeof(halts));
    memset(csResumes, 0, sizeof(csResumes));
    memset(prepares, 0, sizeof(prepares));
    memset(activeCtx0, 0, sizeof(activeCtx0));
    memset(activeCtx1, 0, sizeof(activeCtx1));
    memset(activeElapsed, 0, sizeof(activeElapsed));
}
int main() {
    tglOrigEventTimeout = reinterpret_cast<uintptr_t>(&origEventTimeout);
    tglHaltStreamer = reinterpret_cast<uintptr_t>(&mockHalt);
    tglResumeStreamer = reinterpret_cast<uintptr_t>(&mockResumeCs);
    tglEventTimeoutMinWait = 2000;
    tglEventTimeoutRepeatWindow = 15000;
    schedVtab[0x178 / 8] = reinterpret_cast<uintptr_t>(&mockPause);
    schedVtab[0x180 / 8] = reinterpret_cast<uintptr_t>(&mockResume);
    schedVtab[0x188 / 8] = reinterpret_cast<uintptr_t>(&mockPrepare);
    schedVtab[0x198 / 8] = reinterpret_cast<uintptr_t>(&mockGetActive);
    *reinterpret_cast<uint8_t **>(em + 0x10) = accel;
    *reinterpret_cast<void **>(accel + 0x1250) = &schedObj;
    uint64_t flags = 0; int32_t dmin = 48, dmax = 55;
    memcpy(accel + 0x1190, &flags, sizeof(flags));
    memcpy(accel + 0x11A0, &dmin, sizeof(dmin));
    memcpy(accel + 0x11A4, &dmax, sizeof(dmax));
    *reinterpret_cast<void **>(rcsCtx + 0xB8) = rcsChan;
    *reinterpret_cast<void **>(bcsCtx + 0xB8) = bcsChan;
    *reinterpret_cast<uint32_t *>(rcsChan + 0x20) = 19;
    *reinterpret_cast<uint32_t *>(bcsChan + 0x20) = 24;

    // 1. Native non-null passes straight through without touching scheduler.
    resetCounters(); clockNow = 1000; origAdvance = 3200; origReturn = rcsChan;
    assert(tglEventTimeout(em, 1) == rcsChan);
    assert(pauses == 0 && tglEventTimeoutState.nativeNonNull == 1);

    // 2. Native null with short duration (< 2s, e.g. progress resumed) does not probe.
    resetCounters(); origAdvance = 500; origReturn = nullptr;
    assert(tglEventTimeout(em, 1) == nullptr);
    assert(pauses == 0 && tglEventTimeoutState.nativeNull == 1);

    // 3. Reason 4: RCS idle while BCS has hung context -> returns bcsChan and leaves scheduler paused.
    resetCounters(); clockNow = 50000; origAdvance = 3200; origReturn = nullptr;
    activeCtx0[1] = bcsCtx; activeElapsed[1] = 4000;
    assert(tglEventTimeout(em, 1) == bcsChan);
    assert(pauses == 1 && resumes == 0 && halts[0] == 1 && csResumes[0] == 1 && halts[1] == 1 && prepares[1] == 1);
    assert(tglEventTimeoutState.fallbackBCS == 1);

    // 4. Reason 5: first RCS null with mismatched stamp returns nullptr and resumes cleanly;
    //    second consecutive RCS null within 15s triggers repeated fallback.
    resetCounters(); clockNow = 100000; origAdvance = 3200; origReturn = nullptr;
    activeCtx0[0] = rcsCtx; activeElapsed[0] = 1000;
    assert(tglEventTimeout(em, 1) == nullptr);
    assert(pauses == 1 && resumes == 1 && halts[0] == 1 && csResumes[0] == 1 && prepares[0] == 0);
    resetCounters(); clockNow += 100; origAdvance = 3200; origReturn = nullptr;
    activeCtx0[0] = rcsCtx; activeElapsed[0] = 1000;
    assert(tglEventTimeout(em, 12) == rcsChan);
    assert(pauses == 1 && resumes == 0 && halts[0] == 1 && csResumes[0] == 0 && prepares[0] == 1);
    assert(tglEventTimeoutState.fallbackRepeated == 1);

    // 5. Both RCS and BCS idle -> resumes cleanly and returns nullptr.
    resetCounters(); clockNow = 200000; origAdvance = 3200; origReturn = nullptr;
    assert(tglEventTimeout(em, 1) == nullptr);
    assert(pauses == 1 && resumes == 1 && halts[0] == 1 && csResumes[0] == 1 && halts[1] == 1 && csResumes[1] == 1);
    puts("PASS: production eventTimeout wrapper: passthrough, early-null, Reason4-BCS, Reason5-repeat, idle-clean");
}
'''
with tempfile.TemporaryDirectory(prefix='wxe-event-timeout-') as d:
    p = pathlib.Path(d); (p/'test.cpp').write_text(harness)
    subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-I'+str(root/'WhateverXe'),
                    str(p/'test.cpp'),'-o',str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)

if len(sys.argv) > 1:
    data = pathlib.Path(sys.argv[1]).read_bytes()
    ncmds = struct.unpack_from('<I', data, 16)[0]; off = 32; sections = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x19:
            for i in range(struct.unpack_from('<I', data, off + 64)[0]):
                s = off + 72 + i * 80; addr, sz, fo = struct.unpack_from('<QQI', data, s + 32)
                sections.append((addr, sz, fo))
        off += size
    def read(va, n):
        for addr, sz, fo in sections:
            if addr <= va and va + n <= addr + sz: return data[fo + va - addr:fo + va - addr + n]
        raise AssertionError(hex(va))
    def const(src, name):
        return int(re.search(r'constexpr uint32_t ' + name + r' = (0x[0-9A-Fa-f]+);', src)[1], 16)
    def array(src, name):
        values = re.search(r'constexpr uint8_t ' + name + r'\[[^\]]*\] \{([^}]+)\}', src)[1]
        return bytes(int(v.strip(), 0) for v in values.split(',') if v.strip())
    for name, at, src in [
        ('eventTimeoutHead', 'eventTimeoutOffset', header),
        ('fiveSecImm', 'fiveSecImmOffset', header),
        ('haltHead', 'haltOffset', header),
        ('resumeHead', 'resumeOffset', header),
        ('getActiveHead', 'getActiveOffset', header),
        ('prepareResetHead', 'prepareResetOffset', header),
    ]:
        want = array(src, name); assert read(const(header, at), len(want)) == want, name
    assert read(const(header, 'lutOffset'), 64) == array(wait_header, 'table')
    assert read(const(header, 'lutSciOffset'), 64) == array(wait_header, 'sciTable')
    print('PASS: eventTimeout bytes match the accelerator binary')
