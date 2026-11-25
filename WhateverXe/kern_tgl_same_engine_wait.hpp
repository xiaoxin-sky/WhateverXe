// Same-engine dependency waits after a TGL host-scheduler GPU restart.
// Exact donor: AppleIntelTGLGraphics BA3AA1C0 (WhateverXe Graphics payloads).
//
// IOAccelEventMachine2 writes a dependency barrier through
// IGAccelEventMachine::writeBarrierElement, which tail-calls
// IGHardwareRingBufferMain::waitForSemaphore(stamp, index). That method always
// emits a polling MI_SEMAPHORE_WAIT on the other channel's completion stamp,
// even when the stamp index belongs to the same hardware engine. Once a restart
// has aborted a WindowServer frame, its next frame waits on the render engine
// for a same-engine client whose queued work cannot run behind it: each wait
// becomes the next hang. For a bounded window after each single-pass recovery,
// leave out only those same-engine waits so the engine drains in submission
// order. Cross-engine waits, the stamp protocol and recovery are unchanged.
#ifndef kern_tgl_same_engine_wait_hpp
#define kern_tgl_same_engine_wait_hpp
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace TGLSameEngineWait {
constexpr uint32_t waitOffset = 0x8520E;     // IGHardwareRingBufferMain::waitForSemaphore
constexpr uint32_t pollOffset = 0x852BF;     // leaq 0xe009002(...): MI_SEMAPHORE_WAIT, poll, SAD>=SDD
constexpr uint32_t blitWaitOffset = 0x4EE58; // IGHardwareRingBufferBlit::waitForSemaphore
constexpr uint32_t blitPollOffset = 0x4EF14; // leaq 0xe009002(...): MI_SEMAPHORE_WAIT, poll, SAD>=SDD
constexpr uint32_t canUseOffset = 0x42440;   // IGHardwareRingBuffer::canUseSemaphoreWaitPoll
constexpr uint32_t sciTableOffset = 0xBE8F0; // IG_STAMP_INDEX_TO_HWCS_LUT_SCI
constexpr uint32_t tableOffset = 0xBE900;    // IG_STAMP_INDEX_TO_HWCS_LUT
constexpr uint32_t tableSize = 64;
constexpr uint32_t lastIndex = 0x3B;         // donor: index > 59 has no engine table entry
constexpr uint32_t singleChannelMask = 0x18000; // SingleChannelIndex bits in owner+0x1190
constexpr uint64_t windowNs = 60ULL * 1000 * 1000 * 1000;

// Prologue copied by the Lilu trampoline: push/mov/push only, no RIP-relative.
constexpr uint8_t waitHead[] {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xEC,0x38};
constexpr uint8_t pollHeader[] {0x48,0x8D,0x84,0x0B,0x02,0x90,0x00,0x0E};
// Whole donor predicate: owner at ring+0x10, flags at owner+0x1190, engine at
// ring+0x40, and the two tables selected by the SingleChannelIndex bits.
constexpr uint8_t canUseBody[] {
	0x55,0x48,0x89,0xE5,0x83,0xFE,0x3B,0x7F,0x3D,0x4C,0x8B,0x47,0x10,0x49,0x8B,0x80,
	0x90,0x11,0x00,0x00,0x89,0xF6,0x48,0x8D,0x0D,0x93,0xC4,0x07,0x00,0x48,0x01,0xF1,
	0x48,0x8D,0x15,0x99,0xC4,0x07,0x00,0x48,0x01,0xF2,0xA9,0x00,0x80,0x01,0x00,0x48,
	0x0F,0x45,0xD1,0x0F,0xBA,0xE0,0x1E,0x73,0x0D,0x49,0x8B,0x88,0x50,0x12,0x00,0x00,
	0xF6,0x41,0x0C,0x02,0x75,0x04,0x31,0xC0,0x5D,0xC3,0x8A,0x0A,0xA9,0x00,0x80,0x01,
	0x00,0x0F,0x94,0xC2,0x38,0x4F,0x40,0x0F,0x95,0xC0,0x20,0xD0,0xEB,0xEA};
constexpr uint8_t table[tableSize] {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,
	5,5,5,5,5,5,5,5,6,6,6,6};
constexpr uint8_t sciTable[tableSize] {
	0,1,2,3,4,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4};

// Every byte the hook depends on, read from the loaded donor image.
inline bool qualified(const uint8_t *image) {
	return !memcmp(image + waitOffset, waitHead, sizeof(waitHead)) &&
		!memcmp(image + pollOffset, pollHeader, sizeof(pollHeader)) &&
		!memcmp(image + canUseOffset, canUseBody, sizeof(canUseBody)) &&
		!memcmp(image + tableOffset, table, sizeof(table)) &&
		!memcmp(image + sciTableOffset, sciTable, sizeof(sciTable));
}

// Blit ring waitForSemaphore shares the same prologue and MI_SEMAPHORE_WAIT poll header.
inline bool qualifiedBlit(const uint8_t *image) {
	return qualified(image) &&
		!memcmp(image + blitWaitOffset, waitHead, sizeof(waitHead)) &&
		!memcmp(image + blitPollOffset, pollHeader, sizeof(pollHeader));
}

// The donor predicate's table selection, without its poll-batch conditions.
// An index without a table entry is never treated as same-engine.
inline bool sameEngine(uint64_t flags, uint32_t index, uint8_t ringEngine) {
	if (index > lastIndex) return false;
	const uint8_t *selected = (flags & singleChannelMask) ? sciTable : table;
	return selected[index] == ringEngine;
}

inline uint64_t armUntil(uint64_t now, uint64_t window) {
	return now > UINT64_MAX - window ? UINT64_MAX : now + window;
}

// until == 0 means no recovery has armed the window in this boot.
inline bool skip(uint64_t now, uint64_t until, uint64_t flags, uint32_t index, uint8_t ringEngine) {
	return until && now < until && sameEngine(flags, index, ringEngine);
}
}
#endif
