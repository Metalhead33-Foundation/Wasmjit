#ifndef WASMCALLCACHE_HPP
#define WASMCALLCACHE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmCallCache.hpp
  \brief Optional libffi-backed caches for module and component exports.

  The cache is a convenience layer over the public descriptor and component
  instance APIs. It never changes the `Callable` ABI and never owns an instance:
  the instance must outlive the cache, and unloading or re-instantiating that
  instance invalidates the cached entry points.
*/

WASMJIT_VISIBLE_BEGIN

//! The libffi type record is named only through pointers in this header, so an
//! embedder does not need libffi's include directory to include the cache.
typedef struct _ffi_type ffi_type;

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "WasmComponentInstance.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmTrap.hpp"
#include "WasmValue.hpp"

namespace WASM {

//! One precomputed libffi callable for a core module export.
struct ModuleFfiEntry {
	std::string name; //!< The exported function name.
	Callable callable; //!< The instance's callable, including its context.
	std::vector<ffi_type*> argTypes; //!< Argument types including the context pointer.
	ffi_type* returnType = nullptr; //!< The libffi result type.
	std::size_t paramCount = 0; //!< Wasm parameters, excluding VMContext*.
	//! Declared kind of each Wasm parameter, in order; a `V128` occupies two
	//! argument slots at the canonical two-word convention (decision note §28).
	std::vector<WASM::ValueTypeCode> paramKinds;
	bool multiResult = false; //!< True when the ABI has a trailing result buffer.
	//! Declared kind of the single result; `Void` when there is none. The frame
	//! packer needs the *Wasm* kind to know the copy width, which an `ffi_type`
	//! cannot express (docs/backends/EXPORT_SIGNATURE.md section 21).
	WASM::ValueTypeCode resultKind = WASM::ValueTypeCode::Void;
	std::size_t resultCount = 0; //!< Number of declared results.
};

//! A name-indexed ffi cache over one module instance's function exports.
class WASMJIT_API ModuleCallCache {
public:
	/** \brief Builds a cache over one module instance's function exports. \param instance Instance whose exports are cached; it must outlive the cache. */
	explicit ModuleCallCache(ModuleInstance& instance);
	/** \brief Releases the prepared ffi call interfaces. */
	~ModuleCallCache();

	ModuleCallCache(const ModuleCallCache&) = delete;
	ModuleCallCache& operator=(const ModuleCallCache&) = delete;
	ModuleCallCache(ModuleCallCache&&) = delete;
	ModuleCallCache& operator=(ModuleCallCache&&) = delete;

	/** \brief Finds a cached export by name. \param name Export name. \return Entry pointer, or nullptr when the name is not a function export. */
	const ModuleFfiEntry* find(std::string_view name) const;

	/** \brief Returns every cached function export. \return Entries in module export order. */
	const std::vector<ModuleFfiEntry>& entries() const noexcept { return entries_; }

	/** \brief Invokes one entry with a caller-owned result and libffi argument pointers. \param entry Cached entry to call. \param result Result storage; null is allowed for a void/multi-result entry. \param args Pointers to Wasm arguments, excluding VMContext*. \throws WASM::Trap when the call reports a pending trap. */
	void call(const ModuleFfiEntry& entry, void* result, void* const* args) const;

	/** \brief Typed convenience wrapper around an entry's libffi call. \tparam Ret Expected result type. \tparam Args Wasm argument types. \param name Export name. \param args Arguments forwarded by address to libffi. \return The result value. \throws std::runtime_error when the export is absent or the arity differs, and WASM::Trap when the call traps. */
	template <typename Ret, typename... Args>
	Ret call(std::string_view name, Args&&... args) const
	{
		const ModuleFfiEntry* entry = find(name);
		if (entry == nullptr)
			throw std::runtime_error("module call cache: no export named " + std::string(name));
		if (entry->paramCount != sizeof...(Args))
			throw std::runtime_error("module call cache: wrong argument count for " + std::string(name));
		if (entry->multiResult)
			throw std::runtime_error("module call cache: multi-result export needs callWithResults");
		std::vector<void*> argv{ const_cast<void*>(static_cast<const void*>(&args))... };
		if constexpr (std::is_void_v<Ret>) {
			call(*entry, nullptr, argv.data());
		} else {
			Ret result{};
			call(*entry, &result, argv.data());
			return result;
		}
	}

	/** \brief Invokes a multi-result entry into a caller-owned ABI result buffer. \param name Export name. \param result Result buffer. \param args Pointers to Wasm arguments. \throws std::runtime_error when the export is absent or not multi-result, and WASM::Trap when the call traps. */
	void callWithResults(std::string_view name, void* result, void* const* args) const;

private:
	ModuleInstance* instance_ = nullptr; //!< Borrowed instance.
	std::vector<ModuleFfiEntry> entries_; //!< Entries in export order.
	std::vector<void*> cifs_; //!< One prepared ffi_cif per entry, type-erased.
	std::unordered_map<std::string, std::size_t> byName_; //!< Name to entry index.
};

namespace Detail {

//! Converts one typed host value into the flat `Value` a component function
//! expects. This is deliberately limited to core scalar/reference cells.
template <typename T>
Value makeValue(T value)
{
	Value result{};
	if constexpr (std::is_same_v<std::decay_t<T>, float>) {
		result.kind = ValueTypeCode::F32;
		result.f32 = value;
	} else if constexpr (std::is_same_v<std::decay_t<T>, double>) {
		result.kind = ValueTypeCode::F64;
		result.f64 = value;
	} else if constexpr (std::is_pointer_v<std::decay_t<T>>) {
		result.kind = ValueTypeCode::ExternRef;
		result.ref = const_cast<void*>(static_cast<const void*>(value));
	} else if constexpr (std::is_integral_v<std::decay_t<T>>
			&& sizeof(std::decay_t<T>) > 4) {
		result.kind = ValueTypeCode::I64;
		result.i64 = static_cast<std::int64_t>(value);
	} else {
		result.kind = ValueTypeCode::I32;
		result.i32 = static_cast<std::int32_t>(value);
	}
	return result;
}

//! Reads one flat component result as the requested host type.
template <typename T>
T readValue(const Value& value)
{
	if constexpr (std::is_same_v<T, float>)
		return value.f32;
	else if constexpr (std::is_same_v<T, double>)
		return value.f64;
	else if constexpr (std::is_pointer_v<T>)
		return static_cast<T>(value.ref);
	else if constexpr (std::is_integral_v<T> && sizeof(T) > 4)
		return static_cast<T>(value.i64);
	else
		return static_cast<T>(value.i32);
}

} // namespace Detail

//! One precomputed component function entry.
struct ComponentFfiEntry {
	std::string name; //!< The exported component function name.
	ComponentFunction function; //!< The component's flat-value callable.
	std::vector<ffi_type*> argTypes; //!< Argument types (no context pointer).
	ffi_type* returnType = nullptr; //!< The libffi result type.
	std::size_t paramCount = 0; //!< Number of flat core parameters.
	bool multiResult = false; //!< True when the function has several flat results.
};

//! A name-indexed ffi cache over a component instance's function exports.
class WASMJIT_API ComponentCallCache {
public:
	/** \brief Builds a cache over one component instance's function exports. \param instance Instance whose exports are cached; it must outlive the cache. */
	explicit ComponentCallCache(ComponentInstanceHandle& instance);

	ComponentCallCache(const ComponentCallCache&) = delete;
	ComponentCallCache& operator=(const ComponentCallCache&) = delete;
	ComponentCallCache(ComponentCallCache&&) noexcept = default;
	ComponentCallCache& operator=(ComponentCallCache&&) noexcept = default;

	/** \brief Finds a cached component export by name. \param name Export name. \return Entry pointer, or nullptr when absent. */
	const ComponentFfiEntry* find(std::string_view name) const;

	/** \brief Returns every cached component function export. \return Entries in runtime map order; callers must not depend on it. */
	const std::vector<ComponentFfiEntry>& entries() const noexcept { return entries_; }

	/** \brief Invokes one component entry through its flat-value callable. \param entry Cached entry. \param args Flat core argument values. \return Flat core result values. \throws WASM::Trap when the component reports a pending trap. */
	std::vector<Value> call(const ComponentFfiEntry& entry,
			std::span<const Value> args) const;

	/** \brief Typed convenience wrapper for a single-result component function. \tparam Ret Expected core result type. \tparam Args Core argument types. \param name Export name. \param args Arguments. \return The single result. \throws std::runtime_error when absent, arity differs, or the function has no single result; WASM::Trap when it traps. */
	template <typename Ret, typename... Args>
	Ret call(std::string_view name, Args&&... args) const
	{
		const ComponentFfiEntry* entry = find(name);
		if (entry == nullptr)
			throw std::runtime_error("component call cache: no export named " + std::string(name));
		if (entry->paramCount != sizeof...(Args))
			throw std::runtime_error("component call cache: wrong argument count for " + std::string(name));
		if (entry->multiResult || entry->function.results.size() != 1u)
			throw std::runtime_error("component call cache: export needs the multi-result API");
		std::vector<Value> values;
		values.reserve(sizeof...(Args));
		(values.push_back(Detail::makeValue(args)), ...);
		const std::vector<Value> results = call(*entry, values);
		return Detail::readValue<Ret>(results.front());
	}

private:
	ComponentInstanceHandle* instance_ = nullptr; //!< Borrowed instance.
	std::vector<ComponentFfiEntry> entries_; //!< Entries in export order.
	std::unordered_map<std::string, std::size_t> byName_; //!< Name to entry index.
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCALLCACHE_HPP

