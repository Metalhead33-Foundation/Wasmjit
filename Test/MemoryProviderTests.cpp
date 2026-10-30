#include <catch2/catch_all.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <Euphemy/Io/EuphFile.hpp>

#include "TestBackend.hpp"
#include "WasmBase/WasmMemory.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmParseOptions.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmStore.hpp"
#include "WasmBase/WasmVMContext.hpp"

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

/*!
  \file MemoryProviderTests.cpp
  \brief The host-chosen allocation policy for linear memory.

  The design, the measurements it is based on and the vocabulary are in
  docs/proposals/MEMORY_PROVIDER.md (`MP-1`). The point of the suite is that the
  policy is invisible to a module and to every backend: the `LinearMemory` view
  is the same POD either way, and only two host-observable properties change —
  whether `memoryBase` can move, and whether the declared maximum costs resident
  memory.
*/

namespace {

constexpr std::uint64_t kPage = 0x10000;

//! The byte at `offset`, for the zero-fill assertions.
std::uint8_t byteAt(const WASM::LinearMemory* memory, std::uint64_t offset)
{
	return memory->memoryBase[offset];
}

//! Loads the `[mem-provider]` fixture into `store`, which is what makes the
//! store's policy reach `ModuleInstance::instantiateMemories`.
WASM::Module loadProviderFixture(WASM::Store& store)
{
	WASM::Module module;
	const std::string path = std::string(WASM_TEST_DIR) + "memory_provider.wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	module.fromFile(file, WASM::ParseOptions{}, store);
	return module;
}

} // namespace

TEST_CASE("the default allocation policy keeps every existing behaviour", "[mem-provider]")
{
	WASM::Store store;
	// Nothing sets a policy here: this is what a build predating MP-1 does.
	REQUIRE(store.memoryAllocationPolicy() == WASM::MemoryAllocationPolicy::Growable);

	const WASM::Store::MemoryId id = store.createLinearMemory(1, 4, false);
	WASM::LinearMemory* memory = store.memory(id);
	REQUIRE(memory != nullptr);
	REQUIRE(memory->memorySize == kPage);

	// Growth is all-or-nothing, commits are what the budget sees, and a new
	// page reads as zeros whichever backing produced it.
	REQUIRE(store.growMemory(memory, 1));
	REQUIRE(memory->memorySize == 2 * kPage);
	REQUIRE(byteAt(memory, kPage) == 0);
	REQUIRE(store.allocatedBytes() == 2 * kPage);

	// Past the declared maximum: no growth, and no change to the accounting.
	const std::uint64_t charged = store.allocatedBytes();
	REQUIRE_FALSE(store.growMemory(memory, 3));
	REQUIRE(memory->memorySize == 2 * kPage);
	REQUIRE(store.allocatedBytes() == charged);

	// A zero delta always succeeds (spec conformance).
	REQUIRE(store.growMemory(memory, 0));
}

TEST_CASE("a reserved memory never moves and is not capped at four gigabytes", "[mem-provider]")
{
	WASM::Store store;
	// Six gigabytes of declared maximum: above the shared provider's cap, so a
	// rewritten maximum would be visible here.
	const std::uint64_t maxPages = 98304; // 98304 * 64 KiB = 6 GiB
	const WASM::Store::MemoryId id = store.createLinearMemory(1, maxPages, false, false, kPage,
			WASM::MemoryAllocationPolicy::Reserved);
	WASM::LinearMemory* memory = store.memory(id);
	REQUIRE(memory != nullptr);

	// The declared maximum survives verbatim, and the reservation itself is not
	// charged: only committed bytes are.
	REQUIRE(memory->memoryMax == maxPages * kPage);
	REQUIRE(memory->memorySize == kPage);
	REQUIRE(store.allocatedBytes() == kPage);

	// The base is the same pointer for the memory's whole life, across grows
	// that a growable buffer would have had to relocate.
	WASM::LinearMemory* const view = memory;
	const std::uint8_t* base = memory->memoryBase;
	REQUIRE(base != nullptr);
	for (int i = 1; i <= 8; ++i) {
		REQUIRE(store.growMemory(memory, 1));
		REQUIRE(memory->memorySize == static_cast<std::uint64_t>(i + 1) * kPage);
		REQUIRE(memory->memoryBase == base);
		REQUIRE(byteAt(memory, static_cast<std::uint64_t>(i) * kPage) == 0);
	}
	REQUIRE(store.allocatedBytes() == 9 * kPage);

	// Growing past the maximum fails without touching size or accounting.
	const std::uint64_t charged = store.allocatedBytes();
	REQUIRE_FALSE(store.growMemory(memory, maxPages));
	REQUIRE(memory->memorySize == 9 * kPage);
	REQUIRE(store.allocatedBytes() == charged);
	REQUIRE(store.memory(id) == view);
}

TEST_CASE("a reserved memory needs a declared maximum", "[mem-provider]")
{
	WASM::Store store;
	const std::uint64_t charged = store.allocatedBytes();
	try {
		store.createLinearMemory(1, UINT64_MAX, false, false, kPage,
				WASM::MemoryAllocationPolicy::Reserved);
		FAIL("a reservation must refuse a memory that declares no maximum");
	} catch (const std::invalid_argument& error) {
		INFO(error.what());
		REQUIRE(std::string(error.what()).find("declared maximum") != std::string::npos);
	}
	// The failed creation credited its initial charge back.
	REQUIRE(store.allocatedBytes() == charged);
}

TEST_CASE("a shared memory ignores the policy, because it already reserves", "[mem-provider]")
{
	WASM::Store store;
	// A shared memory takes the shared backing whatever the policy says, and a
	// base that never moves is the observable consequence.
	const WASM::Store::MemoryId id = store.createLinearMemory(1, 4, true, false, kPage,
			WASM::MemoryAllocationPolicy::Reserved);
	WASM::LinearMemory* memory = store.memory(id);
	REQUIRE(memory != nullptr);
	REQUIRE(memory->isShared);
	const std::uint8_t* base = memory->memoryBase;
	REQUIRE(store.growMemory(memory, 1));
	REQUIRE(memory->memoryBase == base);
	REQUIRE(memory->memorySize == 2 * kPage);
}

TEST_CASE("the store's policy reaches the memories a module declares", "[mem-provider]")
{
	// The module cannot tell the difference; the host can. Both runs below use
	// the same fixture and the same calls, and differ only in the policy.
	for (const bool reserved : {false, true}) {
		INFO("reserved policy: " << reserved);
		WASM::Store store;
		if (reserved)
			store.setMemoryAllocationPolicy(WASM::MemoryAllocationPolicy::Reserved);
		REQUIRE((store.memoryAllocationPolicy() == WASM::MemoryAllocationPolicy::Reserved)
				== reserved);

		WASM::Module module = loadProviderFixture(store);
		std::unique_ptr<TestBackend::Session> backend = TestBackend::create();
		REQUIRE(backend != nullptr);
		WASM::RegistryImportResolver imports;
		std::shared_ptr<WASM::ModuleInstance> instance =
				backend->instantiator().instantiate(module, imports);
		REQUIRE(instance != nullptr);

		WASM::VMContext* context = instance->context();
		REQUIRE(context != nullptr);
		REQUIRE(context->memoryCount == 1);
		WASM::LinearMemory* memory = context->memories[0];
		REQUIRE(memory != nullptr);
		REQUIRE(memory->memorySize == kPage);
		REQUIRE(memory->memoryMax == 4 * kPage);

		const std::uint8_t* base = memory->memoryBase;
		std::optional<WASM::Callable> grow = instance->exportedFunction("grow");
		REQUIRE(grow.has_value());
		std::optional<WASM::Callable> load = instance->exportedFunction("load");
		REQUIRE(load.has_value());
		for (std::int32_t i = 1; i <= 3; ++i) {
			REQUIRE(WASM::callCallable<std::int32_t>(*grow, 1) == i);
			REQUIRE(memory->memorySize == static_cast<std::uint64_t>(i + 1) * kPage);
			// The newly grown page reads as zero, through the module itself.
			REQUIRE(WASM::callCallable<std::int32_t>(*load,
					static_cast<std::int32_t>(i * kPage)) == 0);
			if (reserved)
				REQUIRE(memory->memoryBase == base);
		}
		// At the declared maximum the grow fails as -1, in both policies.
		REQUIRE(WASM::callCallable<std::int32_t>(*grow, 1) == -1);
		REQUIRE(memory->memorySize == 4 * kPage);
	}
}

