#ifndef WASMCOMPONENTABI_HPP
#define WASMCOMPONENTABI_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentAbi.hpp
  \brief The Component Model Canonical ABI: lifting and lowering values.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmTypeStore.hpp"
#include <cstddef>
#include <cstdint>
#include <map>

namespace WASM {

//! \brief Per-instance Canonical ABI ownership (TYPE_MODEL TM-11/TM-15)
//!
//! The Store derives and caches CanonicalAbiSignature metadata once per
//! (TypeId, CanonicalAbiOptions). That cache is process-wide and inert. A
//! component instance also needs an *instance-owned* view: the signatures its
//! lifts/lowers actually use, with an identity that belongs to this instance
//! rather than to the Store. Keeping the two apart is what lets two instances
//! of the same definition with different resource substitutions stay distinct
//! while never mutating type identity.
//!
//! The cache is index-keyed by (TypeId, options) and grows only on a miss, so a
//! pointer to an entry is stable for the lifetime of this object. This class is
//! backend-agnostic: it names only Store metadata (docs/LAYERING.md).

/**
 * \brief Owns per-component-instance Canonical ABI signature metadata.
 *
 * Store-level signatures remain inert and shared; this cache gives each
 * component instance stable, separately owned signature objects.
 */
class ComponentAbiInstance {
public:
	//! \param store Process-wide type metadata this instance caches against
	//! \param instanceId Opaque host identity for this component instance
	/**
	 * \brief Creates an instance cache associated with a canonical type store.
	 * \param store Store whose canonical ABI metadata is queried; must outlive this object.
	 * \param instanceId Opaque host identity assigned to this component instance.
	 */
	explicit ComponentAbiInstance(const TypeStore& store, uint64_t instanceId = 0)
		: store_(store), instanceId_(instanceId) {}

	//! Host-assigned identity of this component instance.
	/** \brief Returns the host-assigned component instance identity. \return Instance ID. */
	uint64_t instanceId() const { return instanceId_; }
	//! Sets the host-assigned identity of this component instance.
	/** \brief Updates the host-assigned component instance identity. \param id New instance ID. */
	void setInstanceId(uint64_t id) { instanceId_ = id; }

	//! Resolve and take ownership of one instance's signature. Returns nullptr
	//! when the type is not a function or uses a shape this ABI slice cannot
	//! flatten. The returned pointer is stable until this object is destroyed.
	/**
	 * \brief Resolves and caches the Canonical ABI signature for a type/options pair.
	 * \param type Canonical type identity to resolve.
	 * \param options Canonical ABI flattening options.
	 * \return Stable pointer to this instance's cached signature, or nullptr if
	 *         the type is not a supported function signature.
	 */
	const CanonicalAbiSignature* resolve(TypeId type,
			const CanonicalAbiOptions& options) const
	{
		if (type.isNone())
			return nullptr;
		const Key key{type, options};
		const auto found = signatures_.find(key);
		if (found != signatures_.end()) {
			++hits_;
			return &found->second;
		}
		const CanonicalAbiSignature* storeSignature =
				store_.canonicalAbi(type, options);
		if (storeSignature == nullptr)
			return nullptr;
		++misses_;
		const auto inserted = signatures_.emplace(key, *storeSignature);
		return &inserted.first->second;
	}

	//! Number of instance-owned signatures currently cached.
	/** \brief Returns the number of instance-owned signatures. \return Cache entry count. */
	std::size_t signatureCount() const { return signatures_.size(); }
	//! Cache hits since construction.
	/** \brief Returns the number of successful cache hits. \return Hit count. */
	std::size_t hits() const { return hits_; }
	//! Cache misses that caused a Store lookup and insertion.
	/** \brief Returns the number of cache misses that required Store lookup. \return Miss count. */
	std::size_t misses() const { return misses_; }

	//! Instance-owned signatures are not type identity: two instances with the
	//! same TypeId/options both resolve, but their entries are distinct objects.
	/**
	 * \brief Looks up an already-cached signature without consulting the Store.
	 * \param type Canonical type identity.
	 * \param options Canonical ABI flattening options.
	 * \return Pointer to the instance-owned signature, or nullptr if absent.
	 */
	const CanonicalAbiSignature* find(TypeId type,
			const CanonicalAbiOptions& options) const
	{
		const auto found = signatures_.find(Key{type, options});
		return found == signatures_.end() ? nullptr : &found->second;
	}
private:
	//! Map key: a Store type plus the Canonical ABI options that flatten it.
	/** \brief Ordered key for the per-instance signature cache. */
	struct Key {
		TypeId type{TypeId::kNone}; //!< Closed function type being flattened
		CanonicalAbiOptions options; //!< Encoding/memory/realloc/async choices

		//! Strict weak ordering for `std::map`.
		bool operator<(const Key& other) const
		{
			if (type != other.type)
				return type < other.type;
			if (options.stringEncoding != other.options.stringEncoding)
				return options.stringEncoding < other.options.stringEncoding;
			if (options.memory != other.options.memory)
				return options.memory < other.options.memory;
			if (options.realloc != other.options.realloc)
				return options.realloc < other.options.realloc;
			if (options.postReturn != other.options.postReturn)
				return options.postReturn < other.options.postReturn;
			if (options.async != other.options.async)
				return options.async < other.options.async;
			return options.pointerSize < other.options.pointerSize;
		}
	};

	const TypeStore& store_; //!< Process-wide type metadata
	uint64_t instanceId_ = 0; //!< Host-assigned instance identity
	mutable std::map<Key, CanonicalAbiSignature> signatures_; //!< Owned cache
	mutable std::size_t hits_ = 0; //!< resolve() hits
	mutable std::size_t misses_ = 0; //!< resolve() misses
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTABI_HPP
