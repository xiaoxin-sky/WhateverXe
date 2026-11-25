// Fallback channel selection when IGAccelEventMachine::eventTimeout returns
// nullptr on a stalled TGL host-scheduler engine (donor BA3AA1C0).
//
// In multi-channel host scheduling, IOAccelEventMachine2::restart_channel calls
// IGAccelEventMachine::eventTimeout(stamp_idx). When it returns nullptr, IOAccel
// logs GPURestartSkipped ("no channel associated with stamp_idx") and eventually
// GPURestartDeferred, leaving the GPU permanently hung while the hardware cursor
// still moves. Two native exits cause this after checkForProgress(~3.2 s) fails:
//   - Reason 4: the reported engine (e.g. RCS 0) is idle because a cross-engine
//     dependency on BCS (engine 1) is the actual hung ring.
//   - Reason 5: an active context on the reported engine has a different
//     stampIndex and elapsedNs < 5 s, so repeated timeouts keep skipping reset.
//
// This helper runs only after the native eventTimeout returns nullptr and spent
// >= minProgressWaitNs (ruling out display stamps, disabled recovery, or early
// progress). It preserves the native pause -> halt -> getActiveContexts ->
// prepareGPUReset contract and never alters non-null native returns.
#ifndef kern_tgl_event_timeout_hpp
#define kern_tgl_event_timeout_hpp
#include "kern_tgl_same_engine_wait.hpp"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace TGLEventTimeout {
constexpr uint32_t eventTimeoutOffset = 0x15F22;   // IGAccelEventMachine::eventTimeout(int)
constexpr uint32_t fiveSecImmOffset = 0x160C1;     // movabsq $0x12a05f200, %rax
constexpr uint32_t haltOffset = 0x56978;           // IGScheduler::haltCommandStreamer(IGHwCsType)
constexpr uint32_t resumeOffset = 0x56A2E;         // IGScheduler::resumeCommandStreamer(IGHwCsType)
constexpr uint32_t getActiveOffset = 0x380EC;      // IGScheduler5::getActiveContexts(...)
constexpr uint32_t prepareResetOffset = 0x390D8;   // IGScheduler5::prepareGPUReset(IGHwCsType)
constexpr uint32_t lutSciOffset = 0xA0280;         // IG_STAMP_INDEX_TO_HWCS_LUT_SCI (in eventTimeout)
constexpr uint32_t lutOffset = 0xA0290;            // IG_STAMP_INDEX_TO_HWCS_LUT (in eventTimeout)

constexpr uint64_t minProgressWaitNs = 2ULL * 1000 * 1000 * 1000; // 2 s: rules out early return
constexpr uint64_t repeatWindowNs = 15ULL * 1000 * 1000 * 1000;   // 15 s: consecutive null window
constexpr uint32_t repeatThreshold = 2;                           // 2 consecutive nulls (> ~6.4 s)
constexpr uint8_t rcsEngine = 0;
constexpr uint8_t bcsEngine = 1;
constexpr uint8_t maxTrackedEngines = 8;

// 23-byte prologue of IGAccelEventMachine::eventTimeout(int): no RIP-relative.
constexpr uint8_t eventTimeoutHead[] {
	0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
	0x48,0x83,0xEC,0x18,0x41,0x89,0xF5,0x49,0x89,0xFE};
constexpr uint8_t fiveSecImm[] {0x48,0xB8,0x00,0xF2,0x05,0x2A,0x01,0x00,0x00,0x00};
constexpr uint8_t haltHead[] {
	0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x41,0x89,0xF6,0x49,0x89};
constexpr uint8_t resumeHead[] {
	0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x41,0x89,0xF6,0x49,0x89};
constexpr uint8_t getActiveHead[] {
	0x55,0x48,0x89,0xE5,0x89,0xF0,0x48,0x8B,0xBC,0xC7,0xA0,0x0C,0x00,0x00,0x48,0x89};
constexpr uint8_t prepareResetHead[] {
	0x55,0x48,0x89,0xE5,0x5D,0xC3};

inline bool qualified(const uint8_t *image) {
	return !memcmp(image + eventTimeoutOffset, eventTimeoutHead, sizeof(eventTimeoutHead)) &&
		!memcmp(image + fiveSecImmOffset, fiveSecImm, sizeof(fiveSecImm)) &&
		!memcmp(image + haltOffset, haltHead, sizeof(haltHead)) &&
		!memcmp(image + resumeOffset, resumeHead, sizeof(resumeHead)) &&
		!memcmp(image + getActiveOffset, getActiveHead, sizeof(getActiveHead)) &&
		!memcmp(image + prepareResetOffset, prepareResetHead, sizeof(prepareResetHead)) &&
		!memcmp(image + lutOffset, TGLSameEngineWait::table, TGLSameEngineWait::tableSize) &&
		!memcmp(image + lutSciOffset, TGLSameEngineWait::sciTable, TGLSameEngineWait::tableSize);
}

inline bool engineForStamp(uint64_t flags, int32_t stampIdx, uint8_t &engineOut) {
	if (stampIdx < 0 || static_cast<uint32_t>(stampIdx) > TGLSameEngineWait::lastIndex) return false;
	const uint8_t *selected = (flags & TGLSameEngineWait::singleChannelMask) ?
		TGLSameEngineWait::sciTable : TGLSameEngineWait::table;
	engineOut = selected[stampIdx];
	return engineOut < maxTrackedEngines;
}

inline bool isDisplayStamp(int32_t stampIdx, int32_t minStamp, int32_t maxStamp) {
	return stampIdx >= minStamp && stampIdx <= maxStamp;
}

enum class Action : uint32_t {
	None = 0,
	FallbackBCS = 1,      // Reason 4: RCS idle while BCS holds an active context
	FallbackRepeated = 2, // Reason 5: repeated native nulls on the same engine
};

struct State {
	uint64_t calls {};
	uint64_t nativeNonNull {};
	uint64_t nativeNull {};
	uint64_t fallbackBCS {};
	uint64_t fallbackRepeated {};
	uint64_t lastDurationNs {};
	uint64_t lastElapsedNs {};
	uint32_t lastStampIdx {};
	uint32_t lastEngine {};
	uint32_t lastAction {};
	uint32_t lastChannelStamp {};
	uint32_t consecutiveNulls[maxTrackedEngines] {};
	uint64_t lastNullAt[maxTrackedEngines] {};
};

inline void noteNonNull(State &s, uint8_t engine) {
	++s.nativeNonNull;
	if (engine < maxTrackedEngines) {
		s.consecutiveNulls[engine] = 0;
		s.lastNullAt[engine] = 0;
	}
}

inline uint32_t noteNull(State &s, uint8_t engine, uint64_t now, uint64_t repeatWindow) {
	++s.nativeNull;
	if (engine >= maxTrackedEngines) return 0;
	const bool within = s.lastNullAt[engine] && now >= s.lastNullAt[engine] &&
		(now - s.lastNullAt[engine]) <= repeatWindow;
	s.consecutiveNulls[engine] = within ? (s.consecutiveNulls[engine] + 1) : 1;
	s.lastNullAt[engine] = now;
	return s.consecutiveNulls[engine];
}

inline bool shouldProbeFallback(uint64_t flags, int32_t stampIdx, int32_t displayMin,
		int32_t displayMax, uint64_t duration, uint64_t minWait) {
	if (flags & 0x20ULL) return false; // hang recovery disabled on accelerator
	if (isDisplayStamp(stampIdx, displayMin, displayMax)) return false;
	return duration >= minWait;
}

inline bool canFallbackRepeated(uint32_t streak, void *ctx0, void *channel) {
	return streak >= repeatThreshold && ctx0 != nullptr && channel != nullptr;
}

inline bool canFallbackBCS(uint8_t engine, void *rcsCtx0, void *rcsCtx1, void *bcsCtx0, void *bcsChannel) {
	return engine == rcsEngine && rcsCtx0 == nullptr && rcsCtx1 == nullptr &&
		bcsCtx0 != nullptr && bcsChannel != nullptr;
}
}
#endif
