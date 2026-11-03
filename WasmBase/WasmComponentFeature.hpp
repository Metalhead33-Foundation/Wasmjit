#ifndef WASMCOMPONENTFEATURE_HPP
#define WASMCOMPONENTFEATURE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentFeature.hpp
  \brief Component-model feature gates and per-load component options.

  This header is part of the embedder surface: every declaration in it is
  exported (docs/EMBEDDING_SURFACE.md ES-4). A component is accepted only when
  every proposal feature it uses is enabled. The validator turns a disabled
  feature into a named error before the runtime sees the component, so a
  validated component is never rejected later for a feature the caller did not
  enable (docs/proposals/COMPONENT_MODEL.md CM-20).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>
#include <string_view>

namespace WASM {

/** \brief One independently gateable Component Model feature. */
enum class ComponentFeature : std::uint32_t {
	Baseline = 1u << 0, //!< Component-model 1.0 binary, types, canon lift/lower and resources.
	Values = 1u << 1, //!< Value definitions, imports, exports and component `start`.
	Async = 1u << 2, //!< Async function types/canonopts and task/stream/future/waitable built-ins.
	Threads = 1u << 3, //!< The thread.* canon built-ins.
	ComponentImports = 1u << 4, //!< component-sort imports, exports and bag re-exports.
	Memory64 = 1u << 5, //!< 64-bit core memories used through the Canonical ABI.
	ErrorContext = 1u << 6 //!< error-context values and built-ins.
};

/** \brief Returns the stable spelling of a feature. \param feature Feature to name. \return Lower-case feature name. */
inline constexpr std::string_view componentFeatureName(ComponentFeature feature) noexcept
{
	switch (feature) {
	case ComponentFeature::Baseline: return "baseline";
	case ComponentFeature::Values: return "values";
	case ComponentFeature::Async: return "async";
	case ComponentFeature::Threads: return "threads";
	case ComponentFeature::ComponentImports: return "component-imports";
	case ComponentFeature::Memory64: return "memory64";
	case ComponentFeature::ErrorContext: return "error-context";
	}
	return "unknown";
}

/** \brief An immutable bit set of Component Model features. */
class ComponentFeatures {
public:
	/** \brief Creates an empty feature set. */
	constexpr ComponentFeatures() noexcept = default;
	/** \brief Creates a set from an already-combined bit mask. \param bits Combined `ComponentFeature` bits. */
	explicit constexpr ComponentFeatures(std::uint32_t bits) noexcept
		: bits_(bits) {}

	/** \brief The empty set: no component is accepted. \return Empty features. */
	static constexpr ComponentFeatures none() noexcept { return ComponentFeatures{}; }
	/** \brief The component-model 1.0 baseline. \return Baseline features. */
	static constexpr ComponentFeatures baseline() noexcept
	{
		return ComponentFeatures(static_cast<std::uint32_t>(ComponentFeature::Baseline));
	}
	/** \brief Every feature the engine can execute today. \return Implemented features. */
	static constexpr ComponentFeatures implemented() noexcept
	{
		return ComponentFeatures(
				static_cast<std::uint32_t>(ComponentFeature::Baseline)
				| static_cast<std::uint32_t>(ComponentFeature::Values)
				| static_cast<std::uint32_t>(ComponentFeature::Async)
				| static_cast<std::uint32_t>(ComponentFeature::Threads)
				| static_cast<std::uint32_t>(ComponentFeature::ComponentImports)
				| static_cast<std::uint32_t>(ComponentFeature::Memory64)
				| static_cast<std::uint32_t>(ComponentFeature::ErrorContext));
	}
	/** \brief Every feature the parser understands. \return All features; currently the same set as implemented(). */
	static constexpr ComponentFeatures all() noexcept
	{
		return ComponentFeatures(
				static_cast<std::uint32_t>(ComponentFeature::Baseline)
				| static_cast<std::uint32_t>(ComponentFeature::Values)
				| static_cast<std::uint32_t>(ComponentFeature::Async)
				| static_cast<std::uint32_t>(ComponentFeature::Threads)
				| static_cast<std::uint32_t>(ComponentFeature::ComponentImports)
				| static_cast<std::uint32_t>(ComponentFeature::Memory64)
				| static_cast<std::uint32_t>(ComponentFeature::ErrorContext));
	}

	/** \brief Tests whether a feature is enabled. \param feature Feature to test. \return true when the bit is set. */
	constexpr bool contains(ComponentFeature feature) const noexcept
	{
		return (bits_ & static_cast<std::uint32_t>(feature)) != 0;
	}
	/** \brief Returns the raw bit mask. \return Combined feature bits. */
	constexpr std::uint32_t bits() const noexcept { return bits_; }
	/** \brief True when this set enables everything in `other`. \param other Required features. \return true when all bits of `other` are present. */
	constexpr bool contains(ComponentFeatures other) const noexcept
	{
		return (bits_ & other.bits_) == other.bits_;
	}
	/** \brief Union of two feature sets. \param other Features to add. \return Combined set. */
	constexpr ComponentFeatures operator|(ComponentFeatures other) const noexcept
	{
		return ComponentFeatures(bits_ | other.bits_);
	}
	/** \brief Intersection of two feature sets. \param other Features to keep. \return Intersection. */
	constexpr ComponentFeatures operator&(ComponentFeatures other) const noexcept
	{
		return ComponentFeatures(bits_ & other.bits_);
	}
	/** \brief True when both sets contain the same features. \param other Set to compare. \return true on equality. */
	constexpr bool operator==(const ComponentFeatures& other) const noexcept
	{
		return bits_ == other.bits_;
	}
	/** \brief True when the sets differ. \param other Set to compare. \return true on inequality. */
	constexpr bool operator!=(const ComponentFeatures& other) const noexcept
	{
		return bits_ != other.bits_;
	}

private:
	std::uint32_t bits_ = 0; //!< Combined feature bits.
};

/** \brief Per-load Component Model policy. */
struct ComponentOptions {
	//! The features this load may use. The default is `all()` for the internal
	//! parser/validator; the public `WASM::Runtime::loadComponent*` surface
	//! defaults to `implemented()` instead.
	ComponentFeatures features = ComponentFeatures::all();
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTFEATURE_HPP
