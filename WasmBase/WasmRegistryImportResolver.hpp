#ifndef WASMREGISTRYIMPORTRESOLVER_HPP
#define WASMREGISTRYIMPORTRESOLVER_HPP

#include "WasmApi.hpp"

/*!
  \file WasmRegistryImportResolver.hpp
  \brief Resolving (module, field) imports against store-owned entities.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"

#include "WasmImport.hpp"
#include "WasmMemory.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace WASM {

//! A name → object map with heterogeneous lookup, so a `std::string_view` lookup
//! does not allocate and does not need the vendored container that used to be
//! included here (docs/EMBEDDING_SURFACE.md §6.2: the resolver is embedder tier,
//! and an embedder header should not pull in the utility library for a map).
template<typename T>
/** \brief String-keyed registry map supporting allocation-free string_view lookup. \tparam T Registered value type. */
using NameMap = std::map<std::string, T, std::less<>>;

//! Generic registry-backed resolver for embedders that want to pre-register
//! imports by module/field name. This is backend-independent storage; any
//! backend-specific ABI concerns belong in how `Callable::fnPtr` values are
//! produced before registration.
/** \brief Registry-backed implementation of both import resolution and export registration. */
class WASMJIT_API RegistryImportResolver final : public ImportResolver, public ImportRegistrar
{
public:
	//! A registered global is a borrowed *cell*. A module export publishes its
	//! own cell (`owned` is null); a host registration copies the value into
	//! `owned` so the pointer stays valid for the registry's lifetime. The type
	//! is published so an import can be matched (mutability included, MG-4).
	/** \brief Registry representation of a global cell and its declared type. */
	struct GlobalEntry {
		Value* cell = nullptr;
		GlobalType type{};
		std::unique_ptr<Value> owned; //!< Field `owned` of GlobalEntry.
	};

	/** \brief Set of registered importable entities under one module name. */
	struct ModuleRegistry {
		NameMap<Callable> functions;
		NameMap<GlobalEntry> globals;
		//! Borrowed views into memories owned by the (global) Store. Registering
		//! the same pointer under two names is how memory sharing happens.
		NameMap<LinearMemory*> memories;
		NameMap<ImportedTable> tables;
		NameMap<TagId> tags;
		NameMap<TypeId> types; //!< Field `types` of ModuleRegistry.

		/** \brief Registers a callable in this module namespace. \param fieldName Import/export field name. \param callable Callable descriptor to publish. */
	void registerFunction(std::string_view fieldName, const Callable& callable);
		/** \brief Registers a copied host global value. \param fieldName Field name. \param value Initial value copied into registry-owned storage. */
	void registerGlobal(std::string_view fieldName, const Value& value);
		/** \brief Registers a borrowed global cell. \param fieldName Field name. \param cell Global storage cell; must outlive its registration. \param type Declared type and mutability of the cell. */
	void registerGlobalCell(std::string_view fieldName, Value* cell, const GlobalType& type);
		/** \brief Registers a borrowed Store-owned linear memory. \param fieldName Field name. \param memory Memory descriptor to publish; its owner must outlive use. */
	void registerMemory(std::string_view fieldName, LinearMemory* memory);
		/** \brief Registers a borrowed table and its matching metadata. \param fieldName Field name. \param table Table view and optional declared type. */
	void registerTable(std::string_view fieldName, const ImportedTable& table);
		/** \brief Registers an exception tag identity. \param fieldName Field name. \param tag Store-owned canonical tag ID. */
	void registerTag(std::string_view fieldName, TagId tag);
		/** \brief Registers a canonical type identity. \param fieldName Field name. \param type Store-owned canonical type ID. */
	void registerType(std::string_view fieldName, TypeId type);

		std::optional<Callable> resolveFunction(std::string_view fieldName, TypeId expectedType,
				bool exact, Store& store);
		std::optional<Value*> resolveGlobal(std::string_view fieldName, const GlobalType& type);
		std::optional<LinearMemory*> resolveMemory(std::string_view fieldName, const MemoryType& type);
		std::optional<ImportedTable> resolveTable(std::string_view fieldName, const TableType& type,
											 TypeId elementTypeId, Store& store);
		std::optional<TagId> resolveTag(std::string_view fieldName, TypeId expectedType, Store& store);
		std::optional<TypeId> resolveType(std::string_view fieldName, AbstractHeapType bound,
				Store& store);
	};

	void registerFunction(std::string_view moduleName, std::string_view fieldName,
						  const Callable& callable) override; //!< Field `override` of type.
	//! Host overload: copies `value` into a registry-owned cell.
	void registerGlobal(std::string_view moduleName, std::string_view fieldName,
						const Value& value);
	//! Exporter override: publishes the module's cell (non-owning).
	void registerGlobal(std::string_view moduleName, std::string_view fieldName,
						Value* cell, const GlobalType& type) override;
	void registerMemory(std::string_view moduleName, std::string_view fieldName,
						LinearMemory* memory) override; //!< Field `override` of type.
	void registerTable(std::string_view moduleName, std::string_view fieldName,
					   const ImportedTable& table) override; //!< Field `override` of type.
	void registerTag(std::string_view moduleName, std::string_view fieldName,
					 TagId tag) override; //!< Field `override` of type.
	void registerType(std::string_view moduleName, std::string_view fieldName,
					  TypeId type) override; //!< Field `override` of type.

	std::optional<Callable> resolveFunction(
		std::string_view moduleName,
		std::string_view fieldName,
		TypeId expectedType,
		bool exact = false) override; //!< Field `exact` of type.

	std::optional<Value*> resolveGlobal(
		std::string_view moduleName,
		std::string_view fieldName,
		const GlobalType& type) override; //!< Field `override` of type.

	std::optional<LinearMemory*> resolveMemory(
		std::string_view moduleName,
		std::string_view fieldName,
		const MemoryType& type) override; //!< Field `override` of type.

	std::optional<ImportedTable> resolveTable(
		std::string_view moduleName,
		std::string_view fieldName,
		const TableType& type,
		TypeId           elementTypeId) override; //!< Field `override` of type.

	std::optional<TagId> resolveTag(
		std::string_view moduleName,
		std::string_view fieldName,
		TypeId           expectedType) override; //!< Field `override` of type.

	std::optional<TypeId> resolveType(
		std::string_view moduleName,
		std::string_view fieldName,
		AbstractHeapType bound) override; //!< Field `override` of type.

	//! docs/EMBEDDING_SURFACE.md §4.3 option B: type matching reads an explicit
	//! store, bound by the instance being resolved.
	/** \brief Binds the Store used for canonical type matching. \param store Store that owns the importing instance's type identities. */
	void bindStore(Store& store) override { store_ = &store; }
	/** \brief Returns the Store previously bound to this resolver. \return Reference to the bound Store. \throws std::logic_error if no Store has been bound. */
	Store& boundStore() const;

private:
	Store* store_ = nullptr; //!< Field `store_` of type.
	NameMap<ModuleRegistry> registeredModules; //!< Field `registeredModules` of type.
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMREGISTRYIMPORTRESOLVER_HPP
