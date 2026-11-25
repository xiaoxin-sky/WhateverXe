#include "../WhateverXe/kern_tgl_event_timeout.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace TGLEventTimeout;

int main() {
	uint8_t engine = 0xFF;
	assert(engineForStamp(0, 1, engine) && engine == 0);
	assert(engineForStamp(0, 12, engine) && engine == 0);
	assert(engineForStamp(0, 24, engine) && engine == 1);
	assert(engineForStamp(0x8000ULL, 1, engine) && engine == 1);
	assert(!engineForStamp(0, -1, engine) && !engineForStamp(0, 60, engine));

	assert(isDisplayStamp(50, 48, 55));
	assert(!isDisplayStamp(1, 48, 55));

	assert(!shouldProbeFallback(0x20, 1, 48, 55, 3000, 2000));
	assert(!shouldProbeFallback(0, 50, 48, 55, 3000, 2000));
	assert(!shouldProbeFallback(0, 1, 48, 55, 1999, 2000));
	assert(shouldProbeFallback(0, 1, 48, 55, 2000, 2000));

	State s {};
	assert(noteNull(s, 0, 1000, 500) == 1);
	assert(noteNull(s, 0, 1400, 500) == 2);
	assert(noteNull(s, 0, 2000, 500) == 1); // expired window resets streak
	noteNonNull(s, 0);
	assert(s.consecutiveNulls[0] == 0 && s.nativeNonNull == 1 && s.nativeNull == 3);

	int dummyCtx = 1, dummyChan = 2;
	assert(!canFallbackRepeated(1, &dummyCtx, &dummyChan));
	assert(canFallbackRepeated(2, &dummyCtx, &dummyChan));
	assert(!canFallbackRepeated(2, &dummyCtx, nullptr));

	assert(canFallbackBCS(rcsEngine, nullptr, nullptr, &dummyCtx, &dummyChan));
	assert(!canFallbackBCS(bcsEngine, nullptr, nullptr, &dummyCtx, &dummyChan));
	assert(!canFallbackBCS(rcsEngine, &dummyCtx, nullptr, &dummyCtx, &dummyChan));
	assert(!canFallbackBCS(rcsEngine, nullptr, nullptr, &dummyCtx, nullptr));

	std::vector<uint8_t> image(lutOffset + TGLSameEngineWait::tableSize, 0xCC);
	memcpy(image.data() + eventTimeoutOffset, eventTimeoutHead, sizeof(eventTimeoutHead));
	memcpy(image.data() + fiveSecImmOffset, fiveSecImm, sizeof(fiveSecImm));
	memcpy(image.data() + haltOffset, haltHead, sizeof(haltHead));
	memcpy(image.data() + resumeOffset, resumeHead, sizeof(resumeHead));
	memcpy(image.data() + getActiveOffset, getActiveHead, sizeof(getActiveHead));
	memcpy(image.data() + prepareResetOffset, prepareResetHead, sizeof(prepareResetHead));
	memcpy(image.data() + lutOffset, TGLSameEngineWait::table, TGLSameEngineWait::tableSize);
	memcpy(image.data() + lutSciOffset, TGLSameEngineWait::sciTable, TGLSameEngineWait::tableSize);
	assert(qualified(image.data()));
	for (auto [at, size] : {
			std::pair<uint32_t, size_t>{eventTimeoutOffset, sizeof(eventTimeoutHead)},
			{fiveSecImmOffset, sizeof(fiveSecImm)},
			{haltOffset, sizeof(haltHead)},
			{resumeOffset, sizeof(resumeHead)},
			{getActiveOffset, sizeof(getActiveHead)},
			{prepareResetOffset, sizeof(prepareResetHead)},
			{lutOffset, TGLSameEngineWait::tableSize},
			{lutSciOffset, TGLSameEngineWait::tableSize}})
		for (size_t i = 0; i < size; ++i) {
			image[at + i] ^= 1;
			assert(!qualified(image.data()));
			image[at + i] ^= 1;
		}
	assert(qualified(image.data()));
	puts("event timeout: engine LUT, display guard, streak window, BCS/repeated fallback, exact bytes");
}
