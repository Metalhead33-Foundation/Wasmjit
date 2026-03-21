#ifndef LIBJITIMPORTRESOLVER_HPP
#define LIBJITIMPORTRESOLVER_HPP
#include "../Base/WasmImport.hpp"
#include "../Io/ElvStringhashMap.hpp"

namespace LibJIT {

// Registry-backed resolver for LibJIT instantiation.
//
// The base WASM::ImportResolver interface is backend-agnostic, but function
// imports are effectively backend-shaped because `Callable::fnPtr` must obey
// the active backend's calling convention. For LibJIT that means imported
// functions should be registered as C-callable entry points with the same ABI
// the JIT emits for Wasm calls:
//
//   ret (*)(WASM::VMContext* ctx, wasm_params...)
//
// `ctx` is the owning instance for plain internal calls, or the imported
// callable's bound context for imported/indirect/ref calls.
class ImportResolver final : public WASM::ImportResolver
{
public:
	// Represents a single module's worth of exportable items
	struct ModuleRegistry {
		Elv::Util::UnorderedStrMap<WASM::Callable> functions;
		Elv::Util::UnorderedStrMap<WASM::Value> globals;
		Elv::Util::UnorderedStrMap<WASM::ImportedMemory> memories;
		Elv::Util::UnorderedStrMap<WASM::ImportedTable> tables;
		Elv::Util::UnorderedStrMap<uint32_t> tags;
		void registerFunction(std::string_view fieldName,
							  const WASM::Callable& callable);
		void registerGlobal(std::string_view fieldName,
							const WASM::Value& value);
		void registerMemory(std::string_view fieldName,
							const WASM::ImportedMemory& memory);
		void registerTable(std::string_view fieldName,
						   const WASM::ImportedTable& table);
		void registerTag(std::string_view fieldName,
						 uint32_t tagValue);
		std::optional<WASM::Callable> resolveFunction(
			std::string_view fieldName,
			uint32_t typeIdx);
		std::optional<WASM::Value> resolveGlobal(
			std::string_view fieldName,
			const WASM::GlobalType& type);
		std::optional<WASM::ImportedMemory> resolveMemory(
			std::string_view fieldName,
			const WASM::MemoryType& type);
		std::optional<WASM::ImportedTable> resolveTable(
			std::string_view fieldName,
			const WASM::TableType& type);
		std::optional<uint32_t> resolveTag(
			std::string_view fieldName,
			uint32_t typeIdx);
	};
	void registerFunction(std::string_view moduleName, std::string_view fieldName,
						  const WASM::Callable& callable);
	void registerGlobal(std::string_view moduleName, std::string_view fieldName,
						const WASM::Value& value);
	void registerMemory(std::string_view moduleName, std::string_view fieldName,
						const WASM::ImportedMemory& memory);
	void registerTable(std::string_view moduleName, std::string_view fieldName,
					   const WASM::ImportedTable& table);
	void registerTag(std::string_view moduleName, std::string_view fieldName,
					 uint32_t tagValue);

	std::optional<WASM::Callable> resolveFunction(
		std::string_view moduleName,
		std::string_view fieldName,
		uint32_t typeIdx) override;

	std::optional<WASM::Value> resolveGlobal(
		std::string_view moduleName,
		std::string_view fieldName,
		const WASM::GlobalType& type) override;

	std::optional<WASM::ImportedMemory> resolveMemory(
		std::string_view moduleName,
		std::string_view fieldName,
		const WASM::MemoryType& type) override;

	std::optional<WASM::ImportedTable> resolveTable(
		std::string_view moduleName,
		std::string_view fieldName,
		const WASM::TableType& type) override;

	std::optional<uint32_t> resolveTag(
		std::string_view moduleName,
		std::string_view fieldName,
		uint32_t typeIdx) override;

private:
	Elv::Util::UnorderedStrMap<ModuleRegistry> registered_modules;
};

} // namespace LibJIT

#endif // LIBJITIMPORTRESOLVER_HPP
