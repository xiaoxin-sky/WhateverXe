#!/usr/bin/env python3
"""Run the production same-engine wait wrapper and arm with mocked kernel calls.
With the accelerator binary as argument, also check every qualified byte."""
import pathlib, re, struct, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
source = (root/'WhateverXe/kern_xe.cpp').read_text()
header = (root/'WhateverXe/kern_tgl_same_engine_wait.hpp').read_text()
body = source[source.index('uintptr_t tglOrigMainWait {};'):source.index('bool tglSameEngineSerialize')]

# Arming happens before the native reset/resume, and only from the recovery.
recovery = source[source.index('void XE::wrapTGLRecovery(void *channel) {'):]
recovery = recovery[:recovery.index('\n}\n')]
assert recovery.index('tglArmSameEngineWindow();') < recovery.index('TGLRecovery::resetAndResume(channel)')
assert source.count('tglArmSameEngineWindow();') == 1
# Install gate: exact recovery, opt-out, exact symbols and bytes, not admission.
install = source[source.index('bool sameEngineOK=false;'):source.index('"TGLSameEngineBlitWaitRelaxReady",sameEngineBlitOK);')]
for needle in ['recoveryRequested && recoveryOK && !checkKernelArgument("-wex-strict-gpu-wait")',
               '__ZN24IGHardwareRingBufferMain16waitForSemaphoreEjj',
               '__ZN24IGHardwareRingBufferBlit16waitForSemaphoreEjj',
               'address+TGLSameEngineWait::waitOffset', 'address+TGLSameEngineWait::blitWaitOffset',
               'address+TGLSameEngineWait::canUseOffset',
               'TGLSameEngineWait::qualified(reinterpret_cast<const uint8_t *>(address))',
               'TGLSameEngineWait::qualifiedBlit(reinterpret_cast<const uint8_t *>(address))',
               '{mainWait,tglMainWaitForSemaphore,tglOrigMainWait}',
               '{blitWait,tglBlitWaitForSemaphore,tglOrigBlitWait}']:
    assert needle in install, needle
assert install.index('qualified(') < install.index('routeMultiple(')
assert install.index('qualifiedBlit(') < install.index('routeMultiple(')
assert 'tglSameEngineReady=sameEngineOK;' in source
admission = re.search(r'tglAcceleratorReady = [^;]+;', source).group()
assert 'sameEngine' not in admission

harness = r'''
#include "kern_tgl_same_engine_wait.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
uint64_t clockNow;
void clock_get_uptime(uint64_t *t) { *t = clockNow; }
template <typename T> T FunctionCast(T, uintptr_t address) { return reinterpret_cast<T>(address); }
int originals, blitOriginals; void *lastRing; uint32_t lastStamp, lastIndexSeen;
void original(void *ring, uint32_t stamp, uint32_t index) { ++originals; lastRing = ring; lastStamp = stamp; lastIndexSeen = index; }
void originalBlit(void *ring, uint32_t stamp, uint32_t index) { ++blitOriginals; lastRing = ring; lastStamp = stamp; lastIndexSeen = index; }
namespace {
''' + body + r'''
}
alignas(16) uint8_t ring[0x80], owner[0x1200];
void setFlags(uint64_t flags) { memcpy(owner + 0x1190, &flags, sizeof(flags)); }
int main() {
    tglOrigMainWait = reinterpret_cast<uintptr_t>(&original);
    tglOrigBlitWait = reinterpret_cast<uintptr_t>(&originalBlit);
    *reinterpret_cast<uint8_t **>(ring + 0x10) = owner; ring[0x40] = 0; setFlags(0x40000000);
    // Not installed: a recovery cannot arm anything.
    tglArmSameEngineWindow(); assert(!tglSameEngineUntil && !tglSameEngineArms);
    tglMainWaitForSemaphore(ring, 0x7264, 13);
    assert(originals == 1 && lastRing == ring && lastStamp == 0x7264 && lastIndexSeen == 13);
    ring[0x40] = 1; tglBlitWaitForSemaphore(ring, 0x7264, 24);
    assert(blitOriginals == 1 && !tglSameEngineBlitSkips);
    ring[0x40] = 0;
    // Installed, no recovery yet: every wait is native.
    tglSameEngineReady = true; tglSameEngineWindow = 600; clockNow = 1000;
    tglMainWaitForSemaphore(ring, 0x7264, 13); assert(originals == 2 && !tglSameEngineSkips);
    // A recovery arms the window: same-engine waits are left out, others kept.
    tglArmSameEngineWindow(); assert(tglSameEngineUntil == 1600 && tglSameEngineArms == 1);
    tglMainWaitForSemaphore(ring, 0x7265, 13); assert(originals == 2 && tglSameEngineSkips == 1);
    tglMainWaitForSemaphore(ring, 0x10, 24); assert(originals == 3 && lastIndexSeen == 24 && lastStamp == 0x10);
    tglMainWaitForSemaphore(ring, 0x11, 60); assert(originals == 4 && lastIndexSeen == 60);
    ring[0x40] = 1; tglBlitWaitForSemaphore(ring, 0x12, 24); assert(blitOriginals == 1 && tglSameEngineSkips == 2 && tglSameEngineBlitSkips == 1);
    tglBlitWaitForSemaphore(ring, 0x13, 13); assert(blitOriginals == 2 && lastIndexSeen == 13);
    // SingleChannelIndex selects the SCI table: index 1 is engine 1 there.
    setFlags(0x40008000); ring[0x40] = 0;
    tglMainWaitForSemaphore(ring, 0x14, 1); assert(originals == 5 && lastIndexSeen == 1);
    tglMainWaitForSemaphore(ring, 0x15, 13); assert(originals == 5 && tglSameEngineSkips == 3);
    setFlags(0x40000000);
    // The window expires; a later recovery re-arms it from that time.
    clockNow = 1600; tglMainWaitForSemaphore(ring, 0x7266, 13); assert(originals == 6 && tglSameEngineSkips == 3);
    ring[0x40] = 1; tglBlitWaitForSemaphore(ring, 0x7266, 24); assert(blitOriginals == 3 && tglSameEngineBlitSkips == 1);
    ring[0x40] = 0;
    clockNow = 2000; tglArmSameEngineWindow(); assert(tglSameEngineUntil == 2600 && tglSameEngineArms == 2);
    tglMainWaitForSemaphore(ring, 0x7267, 13); assert(originals == 6 && tglSameEngineSkips == 4);
    puts("PASS: production same-engine wait wrapper: unarmed native, armed window, blit, cross-engine, SCI table, re-arm");
}
'''
with tempfile.TemporaryDirectory(prefix='wxe-same-engine-') as d:
    p = pathlib.Path(d); (p/'test.cpp').write_text(harness)
    subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-I'+str(root/'WhateverXe'),
                    str(p/'test.cpp'),'-o',str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)

if len(sys.argv) > 1:
    # Exact donor bytes, read through the Mach-O section map (code and __const).
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
    def const(name):
        return int(re.search(r'constexpr uint32_t ' + name + r' = (0x[0-9A-Fa-f]+);', header)[1], 16)
    def array(name):
        values = re.search(r'constexpr uint8_t ' + name + r'\[[^\]]*\] \{([^}]+)\}', header)[1]
        return bytes(int(v.strip(), 0) for v in values.split(',') if v.strip())
    for name, at in [('waitHead', 'waitOffset'), ('pollHeader', 'pollOffset'),
                     ('waitHead', 'blitWaitOffset'), ('pollHeader', 'blitPollOffset'),
                     ('canUseBody', 'canUseOffset'), ('table', 'tableOffset'), ('sciTable', 'sciTableOffset')]:
        want = array(name); assert read(const(at), len(want)) == want, (name, at)
    # The poll is the native emitter's MI_SEMAPHORE_WAIT (opcode 0x1C, poll, SAD>=SDD).
    assert struct.unpack_from('<I', array('pollHeader'), 4)[0] == 0x0E009002
    # The two tables are the ones the native predicate's RIP-relative LEAs select.
    body = read(const('canUseOffset'), len(array('canUseBody')))
    base = const('canUseOffset')
    for lea, target in [(0x16, 'sciTableOffset'), (0x20, 'tableOffset')]:
        assert body[lea:lea + 3] in (b'\x48\x8d\x0d', b'\x48\x8d\x15')
        assert base + lea + 7 + struct.unpack_from('<i', body, lea + 3)[0] == const(target), target
    print('PASS: same-engine wait bytes match the accelerator binary')
