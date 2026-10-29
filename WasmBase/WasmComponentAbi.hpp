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

class ComponentAbiInstance {
public:
	//! \param store Process-wide type metadata this instance caches against
	//! \param instanceId Opaque host identity for this component instance
	explicit ComponentAbiInstance(const TypeStore& store, uint64_t instanceId = 0)
		: store_(store), instanceId_(instanceId) {}

	//! Host-assigned identity of this component instance.
	uint64_t instanceId() const { return instanceId_; }
	//! Sets the host-assigned identity of this component instance.
	void setInstanceId(uint64_t id) { instanceId_ = id; }

	//! Resolve and take ownership of one instance's signature. Returns nullptr
	//! when the type is not a function or uses a shape this ABI slice cannot
	//! flatten. The returned pointer is stable until this object is destroyed.
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
	std::size_t signatureCount() const { return signatures_.size(); }
	//! Cache hits since construction.
	std::size_t hits() const { return hits_; }
	//! Cache misses that caused a Store lookup and insertion.
	std::size_t misses() const { return misses_; }

	//! Instance-owned signatures are not type identity: two instances with the
	//! same TypeId/options both resolve, but their entries are distinct objects.
	const CanonicalAbiSignature* find(TypeId type,
			const CanonicalAbiOptions& options) const
	{
		const auto found = signatures_.find(Key{type, options});
		return found == signatures_.end() ? nullptr : &found->second;
	}
private:
	//! Map key: a Store type plus the Canonical ABI options that flatten it.
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
