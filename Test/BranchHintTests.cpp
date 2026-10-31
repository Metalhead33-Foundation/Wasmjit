#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "TestBackend.hpp"
#include "helper.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmOpcodeDispatcher.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmType.hpp"
#include "WasmBase/WasmValue.hpp"

/*!
  \file BranchHintTests.cpp
  \brief The branch-hinting seam a backend consults.

  `metadata.code.branch_hint` is a *pure* optimisation hint
  (docs/proposals/SMALL_PROPOSALS.md §4): an engine may act on it, ignore it, or
  consult it only where its own code generator can use it. This suite covers the
  backend-neutral accessor (`OpcodeDispatcher::setBranchHints`/
  `branchHintAt`, added so a backend with block layout does not have to re-parse
  the custom section) and re-checks that acting on a hint still leaves results
  unchanged.
*/

namespace {

//! Exposes the protected seam, which is what a backend inherits.
class HintProbe : public WASM::OpcodeDispatcher
{
public:
	using WASM::OpcodeDispatcher::branchHintAt;
	using WASM::OpcodeDispatcher::setBranchHints;
};

} // namespace

TEST_CASE("a backend can look a branch hint up by body offset", "[branch-hint]")
{
	HintProbe probe;
	// No hints installed: every lookup is `None`, including out-of-range ones.
	REQUIRE(probe.branchHintAt(0) == WASM::BranchHintKind::None);
	REQUIRE(probe.branchHintAt(9) == WASM::BranchHintKind::None);

	// The list the module produces is ordered by offset; the seam says so.
	const std::vector<WASM::BranchHint> hints = {
		{ 9, 1 },   // likely true
		{ 20, 0 },  // likely false
		{ 42, 7 },  // a byte the proposal does not define
	};
	probe.setBranchHints(hints.data(), hints.size());

	REQUIRE(probe.branchHintAt(0) == WASM::BranchHintKind::None);
	REQUIRE(probe.branchHintAt(9) == WASM::BranchHintKind::Likely);
	REQUIRE(probe.branchHintAt(10) == WASM::BranchHintKind::None);
	REQUIRE(probe.branchHintAt(20) == WASM::BranchHintKind::Unlikely);
	REQUIRE(probe.branchHintAt(41) == WASM::BranchHintKind::None);
	// An undefined wire byte is "no hint", never a guess.
	REQUIRE(probe.branchHintAt(42) == WASM::BranchHintKind::None);
	REQUIRE(probe.branchHintAt(1000) == WASM::BranchHintKind::None);

	// Clearing the list restores the hint-less behaviour.
	probe.setBranchHints(nullptr, 0);
	REQUIRE(probe.branchHintAt(9) == WASM::BranchHintKind::None);
}

TEST_CASE("a hinted module still computes the same results", "[branch-hint]")
{
	// The LibJIT backend already acts on a forward `br_if` hint (it emits the
	// inverted form so the likely path is the fall-through). The seam must not
	// change that, and a hint must never change a result.
	WASM::Module module = loadTestModule("branch_hint_loop");
	REQUIRE(module.functionBodies.size() == 1);
	REQUIRE_FALSE(module.functionBodies[0].branchHints.empty());

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	auto sumUpto = instance->exportedFunction("sum_upto");
	REQUIRE(sumUpto.has_value());
	REQUIRE(WASM::callCallable<std::int32_t>(*sumUpto, std::int32_t(0)) == 0);
	REQUIRE(WASM::callCallable<std::int32_t>(*sumUpto, std::int32_t(10)) == 55);
	REQUIRE(WASM::callCallable<std::int32_t>(*sumUpto, std::int32_t(100)) == 5050);
}
