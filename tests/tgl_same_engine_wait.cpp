#include "../WhateverXe/kern_tgl_same_engine_wait.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace TGLSameEngineWait;

int main() {
	// Table selection matches the donor predicate: default and SingleChannelIndex.
	for (uint32_t i = 0; i <= lastIndex; ++i) {
		assert(sameEngine(0, i, table[i]) && !sameEngine(0, i, table[i] + 1));
		for (uint64_t sci : {0x8000ULL, 0x10000ULL, 0x18000ULL})
			assert(sameEngine(sci, i, sciTable[i]) && !sameEngine(sci, i, sciTable[i] + 1));
	}
	// Observed 2026-11-06 loop: WindowServer index 1 on RCS waits on index 13.
	assert(table[1] == 0 && table[13] == 0 && sameEngine(0x40000000, 13, 0));
	// Cross-engine dependencies (BCS 24, VCS 32) keep their native wait.
	assert(!sameEngine(0, 24, 0) && !sameEngine(0, 32, 0) && sameEngine(0, 32, 2));
	// Indexes without a table entry, including a negative int, never qualify.
	for (uint32_t i : {lastIndex + 1, 63U, 64U, 0xFFFFFFFFU})
		for (uint8_t engine = 0; engine < 8; ++engine) assert(!sameEngine(0, i, engine));
	// Other owner flag bits do not change the table.
	assert(sameEngine(~0ULL & ~uint64_t(singleChannelMask), 13, 0));

	// Window: unarmed boot never skips; armed skips only before it expires.
	assert(!skip(0, 0, 0, 13, 0) && !skip(UINT64_MAX - 1, 0, 0, 13, 0));
	const uint64_t until = armUntil(1000, 600);
	assert(until == 1600 && skip(1000, until, 0, 13, 0) && skip(1599, until, 0, 13, 0));
	assert(!skip(1600, until, 0, 13, 0) && !skip(5000, until, 0, 13, 0));
	assert(!skip(1000, until, 0, 24, 0));
	assert(armUntil(UINT64_MAX - 5, 600) == UINT64_MAX && armUntil(0, 0) == 0);

	// Qualification reads every byte the hook depends on, and only those.
	std::vector<uint8_t> image(tableOffset + tableSize, 0xCC);
	memcpy(image.data() + waitOffset, waitHead, sizeof(waitHead));
	memcpy(image.data() + pollOffset, pollHeader, sizeof(pollHeader));
	memcpy(image.data() + canUseOffset, canUseBody, sizeof(canUseBody));
	memcpy(image.data() + tableOffset, table, sizeof(table));
	memcpy(image.data() + sciTableOffset, sciTable, sizeof(sciTable));
	assert(qualified(image.data()));
	assert(!qualifiedBlit(image.data()));
	memcpy(image.data() + blitWaitOffset, waitHead, sizeof(waitHead));
	memcpy(image.data() + blitPollOffset, pollHeader, sizeof(pollHeader));
	assert(qualifiedBlit(image.data()));
	for (auto [at, size] : {std::pair<uint32_t, size_t>{waitOffset, sizeof(waitHead)},
			{pollOffset, sizeof(pollHeader)}, {canUseOffset, sizeof(canUseBody)},
			{tableOffset, sizeof(table)}, {sciTableOffset, sizeof(sciTable)}})
		for (size_t i = 0; i < size; ++i) {
			image[at + i] ^= 1;
			assert(!qualified(image.data()));
			assert(!qualifiedBlit(image.data()));
			image[at + i] ^= 1;
		}
	for (auto [at, size] : {std::pair<uint32_t, size_t>{blitWaitOffset, sizeof(waitHead)},
			{blitPollOffset, sizeof(pollHeader)}})
		for (size_t i = 0; i < size; ++i) {
			image[at + i] ^= 1;
			assert(qualified(image.data()));
			assert(!qualifiedBlit(image.data()));
			image[at + i] ^= 1;
		}
	assert(qualified(image.data()) && qualifiedBlit(image.data()));
	// The copied prologue has no RIP-relative operand and is longer than a rel32 jump.
	assert(sizeof(waitHead) >= 5 && waitHead[0] == 0x55);
	puts("same-engine wait: donor tables, unarmed boot, bounded window, cross-engine waits, exact bytes");
}
