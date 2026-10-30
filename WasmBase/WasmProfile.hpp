#ifndef WASMPROFILE_HPP
#define WASMPROFILE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmProfile.hpp
  \brief The language-subset profiles that gate a load and an instantiation.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>
namespace WASM {

//! Language-profile capability statement (docs/proposals/PROFILES.md).
//!
//! A profile is a coarse language subset that may only *restrict* the engine:
//! it never changes behaviour (PROFILES.md §2/§3). This engine implements the
//! full profile `{}` of the features it supports and claims nothing narrower:
//! the deterministic profile needs NaN normalisation and pinned relaxed-SIMD
//! choices (PR-3), and the scalar profile needs the vector gates (PR-4). A
//! false claim would be worse than a missing one (PRD2).
//!
//! The proposal defines no binary metadata section and no runtime API; a
//! declaration is an out-of-band implementation statement (§3, PRD4), which is
//! exactly what this constexpr header is. It names no backend, so it belongs in
//! the backend-agnostic core (docs/LAYERING.md).
enum class Profile : uint8_t {
	Full, //!< `{}` — supports everything the engine implements
	Deterministic, //!< the spec's `{N,T}`; not claimed alone (see below)
	Scalar, //!< `{V}` — excludes vector types and instructions
	//! The deterministic∩scalar intersection `{N,T,V}`. The proposal allows an
	//! implementation to declare an intersection, and this engine does: SIMD
	//! float NaN payloads can be observed byte-wise (`v128.store`), so a
	//! deterministic SIMD module needs per-lane normalisation this engine does
	//! not implement. `Deterministic` alone is therefore *not* claimed.
	DeterministicScalar, //!< Scalar float results are deterministic; SIMD ones are not.
};

//! A marker names a language subset to *exclude*. A profile is the intersection
//! of its markers' exclusions (docs/proposals/PROFILES.md §2).
enum class ProfileMarker : uint32_t {
	None = 0, //!< No exclusion requested.
	NonDeterministic = 1u << 0, //!< N: relaxed SIMD / non-deterministic rules
	Threads = 1u << 1, //!< T: shared memory, atomics, shared types
	Vectors = 1u << 2, //!< V: v128 and every SIMD instruction
};

//! Union of two markers, so `ProfileMarker::Threads | ProfileMarker::Vectors`
//! names the exclusion set `{T,V}`.
/** \brief Combines two profile-exclusion marker sets. \param lhs Left marker set. \param rhs Right marker set. \return Bitwise union of both marker sets. */
inline constexpr ProfileMarker operator|(ProfileMarker lhs, ProfileMarker rhs) noexcept
{
	return static_cast<ProfileMarker>(
			static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

//! The markers a configuration excludes, and the predicates the validator and
//! the backend use. `profileConfigX()` helpers below build the named profiles.
/** \brief Set of language-subset exclusions requested by an embedder. */
struct ProfileConfig {
	//! Bit set of the `ProfileMarker` values this configuration excludes; zero
	//! means the full profile.
	uint32_t excludedMarkers = 0;

	//! The full profile `{}`: the engine claims no exclusion.
	constexpr ProfileConfig() noexcept = default;
	//! A profile that excludes exactly the subsets `marker` names.
	/** \brief Constructs a profile excluding one marker. \param marker Subset marker to exclude. */
	explicit constexpr ProfileConfig(ProfileMarker marker) noexcept
		: excludedMarkers(static_cast<uint32_t>(marker)) {}
	//! A profile from an already-combined marker bit set.
	/** \brief Constructs a profile from a combined exclusion bitmask. \param markers Combined ProfileMarker bits. */
	explicit constexpr ProfileConfig(uint32_t markers) noexcept
		: excludedMarkers(markers) {}

	//! True when this profile excludes the subset `marker` names.
	/** \brief Tests whether a subset is excluded. \param marker Marker to test. \return true when the marker's bit is set. */
	inline constexpr bool excludes(ProfileMarker marker) const noexcept
	{
		return (excludedMarkers & static_cast<uint32_t>(marker)) != 0;
	}
	//! True when nothing is excluded, i.e. the full profile.
	/** \brief Tests whether this configuration excludes no features. \return true for the full profile. */
	inline constexpr bool isFull() const noexcept { return excludedMarkers == 0; }
	//! True when the deterministic profile's exclusions apply.
	/** \brief Tests whether nondeterministic profile features are excluded. \return true when the deterministic marker is set. */
	inline constexpr bool deterministic() const noexcept
	{
		return excludes(ProfileMarker::NonDeterministic);
	}
	//! True when the threads subset is excluded.
	/** \brief Tests whether the threads subset is excluded. \return true when the Threads marker is set. */
	inline constexpr bool excludesThreads() const noexcept
	{
		return excludes(ProfileMarker::Threads);
	}
	//! True when the scalar subset is excluded (no vectors).
	/** \brief Tests whether vector types/instructions are excluded. \return true for the scalar profile. */
	inline constexpr bool scalar() const noexcept
	{
		return excludes(ProfileMarker::Vectors);
	}
};

//! The full profile `{}`: no exclusions.
/** \brief Returns the profile with no exclusions. \return Full profile configuration. */
inline constexpr ProfileConfig fullProfile() noexcept
{
	return ProfileConfig{};
}

//! The spec's deterministic profile `{N,T}`. This engine does not support it
//! alone (see supportsProfileConfig); it is exposed so a caller can ask.
/** \brief Returns the spec's deterministic exclusion configuration. \return Configuration excluding nondeterminism and threads. */
inline constexpr ProfileConfig deterministicProfile() noexcept
{
	return ProfileConfig{ProfileMarker::NonDeterministic | ProfileMarker::Threads};
}

//! The scalar profile `{V}`: excludes every vector type and SIMD instruction.
/** \brief Returns the scalar-only profile configuration. \return Configuration excluding vector types and SIMD. */
inline constexpr ProfileConfig scalarProfile() noexcept
{
	return ProfileConfig{ProfileMarker::Vectors};
}

//! The `{N,T,V}` intersection this engine actually supports.
/** \brief Returns the deterministic-plus-scalar profile configuration. \return Configuration excluding nondeterminism, threads, and vectors. */
inline constexpr ProfileConfig deterministicScalarProfile() noexcept
{
	return ProfileConfig{ProfileMarker::NonDeterministic | ProfileMarker::Threads
			| ProfileMarker::Vectors};
}

//! The coarsest profile this engine claims today: the full profile.
/** \brief Returns the broadest named profile claimed by this engine. \return Profile::Full. */
inline constexpr Profile supportedProfile() noexcept
{
	return Profile::Full;
}

//! True when the engine claims the named profile (see `supportsProfileConfig`).
/** \brief Tests whether the engine claims support for a named profile. \param profile Profile to query. \return true when the named profile is supported. */
inline constexpr bool supportsProfile(Profile profile) noexcept
{
	switch (profile) {
	case Profile::Full:                return true;
	case Profile::Scalar:              return true;
	case Profile::DeterministicScalar: return true;
	case Profile::Deterministic:       return false; //!< needs v128 NaN normalisation
	}
	return false;
}

//! True when the engine can honour every exclusion in `config`. The only
//! unsupported shape is a deterministic configuration that still admits
//! vectors: `{N,T}` and any variant without V.
/** \brief Tests whether the engine can honour every exclusion in a configuration. \param config Requested exclusion set. \return true when the configuration is supported. */
inline constexpr bool supportsProfileConfig(const ProfileConfig& config) noexcept
{
	if (config.excludes(ProfileMarker::NonDeterministic)
			|| config.excludes(ProfileMarker::Threads))
		return config.excludes(ProfileMarker::Vectors);
	return true;
}

//! Stable short name for diagnostics and embedder logs.
/** \brief Returns the stable diagnostic spelling of a profile. \param profile Profile to format. \return Static null-terminated profile name. */
inline constexpr const char* profileName(Profile profile) noexcept
{
	switch (profile) {
	case Profile::Full:                return "full";
	case Profile::Deterministic:       return "deterministic";
	case Profile::Scalar:              return "scalar";
	case Profile::DeterministicScalar: return "deterministic+scalar";
	}
	return "unknown";
}

} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMPROFILE_HPP
