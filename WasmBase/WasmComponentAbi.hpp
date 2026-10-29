#ifndef WASMCOMPONENTABI_HPP
#define WASMCOMPONENTABI_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentAbi.hpp
  \brief The Component Model Canonical ABI: lifting and lowering values.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

//! Opens the exported-visibility block; every declaration below is exported
//! (docs/EMBEDDING_SURFACE.md ES-4).
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
	//! Public surface: construction, host identity and the signature queries.
public:
	//! \param [in] store Process-wide type metadata this instance caches against
	//! \param [in] instanceId Opaque host identity for this component instance
	explicit ComponentAbiInstance(const TypeStore& store, uint64_t instanceId = 0)
		: store_(store), instanceId_(instanceId) {}

	//! Host-assigned identity of this component instance.
	//! \return The id passed to the constructor or set by setInstanceId(): 0
	//! when the embedder never assigned one.
	uint64_t instanceId() const { return instanceId_; }
	//! Sets the host-assigned identity of this component instance.
	//! \param [in] id Opaque embedder-defined number; this class only stores it
	//! and never interprets it, so a reused id changes nothing about type
	//! identity.
	void setInstanceId(uint64_t id) { instanceId_ = id; }

	//! Resolve and take ownership of one instance's signature. Returns nullptr
	//! when the type is not a function or uses a shape this ABI slice cannot
	//! flatten. The returned pointer is stable until this object is destroyed.
	//! \param [in] type Store id of the (closed) function type to flatten; a
	//! `kNone` id resolves to nullptr without touching the Store.
	//! \param [in] options Encoding, memory, realloc, post-return, async and
	//! pointer-size choices that determine the flattened signature; the same
	//! type under different options is a separate cache entry.
	//! \return The instance-owned signature, or nullptr when `type` is `kNone`,
	//! is not a function type, or needs a shape this slice cannot flatten.
	const CanonicalAbiSignature* resolve(TypeId type,
			const CanonicalAbiOptions& options) const
	{
		if (type.isNone())
			return nullptr;
		//! Probe key: the Store type plus the options that flatten it.
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
	//! \return How many (type, options) pairs this instance has resolved.
	std::size_t signatureCount() const { return signatures_.size(); }
	//! Cache hits since construction.
	//! \return The number of resolve() calls answered from the instance cache
	//! without a Store lookup.
	std::size_t hits() const { return hits_; }
	//! Cache misses that caused a Store lookup and insertion.
	//! \return The number of resolve() calls that consulted the Store and
	//! inserted the result; 0 for a fresh object.
	std::size_t misses() const { return misses_; }

	//! Instance-owned signatures are not type identity: two instances with the
	//! same TypeId/options both resolve, but their entries are distinct objects.
	//! \param [in] type Store id of the function type to look up.
	//! \param [in] options The flattening options the entry must have been
	//! cached under.
	//! \return The cached signature, or nullptr when this instance never
	//! resolved that (type, options) pair — `find` never inserts.
	const CanonicalAbiSignature* find(TypeId type,
			const CanonicalAbiOptions& options) const
	{
		//! Cache-only probe: a miss deliberately stays a miss.
		const auto found = signatures_.find(Key{type, options});
		return found == signatures_.end() ? nullptr : &found->second;
	}
	//! Internals: the cache key and this instance's own signature cache.
private:
	//! Map key: a Store type plus the Canonical ABI options that flatten it.
	struct Key {
		TypeId type{TypeId::kNone}; //!< Closed function type being flattened
		CanonicalAbiOptions options; //!< Encoding/memory/realloc/async choices

		//! Strict weak ordering for `std::map`.
		bool operator<(const Key& other) const
		{
			//! The Store type id decides first.
			if (type != other.type)
				return type < other.type;
			//! Then the string encoding, so UTF-8 and UTF-16 keys differ.
			if (options.stringEncoding != other.options.stringEncoding)
				return options.stringEncoding < other.options.stringEncoding;
			//! Then the linear-memory id the strings/pointers live in.
			if (options.memory != other.options.memory)
				return options.memory < other.options.memory;
			//! Then the realloc function used for lowering.
			if (options.realloc != other.options.realloc)
				return options.realloc < other.options.realloc;
			//! Then the post-return function used after a lift.
			if (options.postReturn != other.options.postReturn)
				return options.postReturn < other.options.postReturn;
			//! Then the sync/async canonical mode.
			if (options.async != other.options.async)
				return options.async < other.options.async;
			//! Pointer size is the last discriminator there is.
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
