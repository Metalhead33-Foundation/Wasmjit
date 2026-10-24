#ifndef WASMPROFILE_HPP
#define WASMPROFILE_HPP
#include <cstdint>
namespace WASM {

// Language-profile capability statement (docs/proposals/PROFILES.md).
//
// A profile is a coarse language subset that may only *restrict* the engine:
// it never changes behaviour (PROFILES.md §2/§3). This engine implements the
// full profile `{}` of the features it supports and claims nothing narrower:
// the deterministic profile needs NaN normalisation and pinned relaxed-SIMD
// choices (PR-3), and the scalar profile needs the vector gates (PR-4). A
// false claim would be worse than a missing one (PRD2).
//
// The proposal defines no binary metadata section and no runtime API; a
// declaration is an out-of-band implementation statement (§3, PRD4), which is
// exactly what this constexpr header is. It names no backend, so it belongs in
// the backend-agnostic core (docs/LAYERING.md).
enum class Profile : uint8_t {
	Full,          // `{}` — supports everything the engine implements
	Deterministic, // excludes non-deterministic behaviour (not claimed yet)
	Scalar         // excludes vector types and instructions (not claimed yet)
};

inline constexpr Profile supportedProfile() noexcept
{
	return Profile::Full;
}

inline constexpr bool supportsProfile(Profile profile) noexcept
{
	return profile == supportedProfile();
}

inline constexpr const char* profileName(Profile profile) noexcept
{
	switch (profile) {
	case Profile::Full:          return "full";
	case Profile::Deterministic: return "deterministic";
	case Profile::Scalar:        return "scalar";
	}
	return "unknown";
}

} // namespace WASM
#endif // WASMPROFILE_HPP
