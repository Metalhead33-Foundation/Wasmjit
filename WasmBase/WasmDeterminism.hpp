#ifndef WASMDETERMINISM_HPP
#define WASMDETERMINISM_HPP
#include <cmath>
#include <cstdint>
#include <cstring>

namespace WASM {

// Canonical NaN payloads for the deterministic profile
// (docs/proposals/PROFILES.md PR-3): a positive quiet NaN with a zero payload.
constexpr uint32_t kCanonicalF32Bits = 0x7fc00000u;
constexpr uint64_t kCanonicalF64Bits = 0x7ff8000000000000ull;

// Replace a NaN with the canonical NaN of the same width; every other value
// (including infinity and -0) passes through bit-exactly. Used by the backend
// at the points where a float becomes observable to the embedder or to memory.
inline float deterministicF32(float value) noexcept
{
	if (!std::isnan(value))
		return value;
	float canonical;
	std::memcpy(&canonical, &kCanonicalF32Bits, sizeof(canonical));
	return canonical;
}

inline double deterministicF64(double value) noexcept
{
	if (!std::isnan(value))
		return value;
	double canonical;
	std::memcpy(&canonical, &kCanonicalF64Bits, sizeof(canonical));
	return canonical;
}

} // namespace WASM

#endif // WASMDETERMINISM_HPP
