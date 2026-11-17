#include <catch2/catch_all.hpp>

#include "TestBackend.hpp"
#include "TestStore.hpp"

#include "WasmBase/WasmArtifact.hpp"
#include "WasmBase/WasmByteOrder.hpp"
#include "WasmBase/WasmModule.hpp"

#include <Euphemy/Io/EuphFile.hpp>

#include <cstdint>
#include <memory>
#include <string>

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

/*!
  \file ByteOrderTests.cpp
  \brief The byte-order policy, its decision table, and the evidence it reads.

  Guest memory is little-endian by specification, so on a little-endian host the
  policy must be invisible; the conversion itself cannot be exercised here at all.
  Two things make it testable anyway, and both are deliberate:

  - `decideByteOrderFor` takes the host byte order as a parameter, so the whole
    big-endian decision matrix runs on this host;
  - the swap primitives are value-level functions, so their involution and their
    known-answer vectors are checkable without a big-endian target.

  What stays untestable here is the one thing that matters most: that a backend
  actually *emits* the conversion around the accesses it compiles
  (docs/proposals/BIG_ENDIAN.md §8, tier 3).
*/

namespace {

std::unique_ptr<WASM::Artifact> loadFixture(const char* name)
{
	const std::string path = std::string(WASM_TEST_DIR) + name + ".wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	return WASM::Artifact::parse(file, testStore());
}

WASM::ByteOrderEvidence evidenceOf(const char* fixture)
{
	auto artifact = loadFixture(fixture);
	REQUIRE(artifact != nullptr);
	auto* module = artifact->asCoreModule();
	REQUIRE(module != nullptr);
	return WASM::byteOrderEvidence(*module);
}

} // namespace

TEST_CASE("guest memory is little-endian whatever the host is", "[byteorder]")
{
	// The premise of the whole policy: the guest's order is not a host property.
	CHECK(WASM::guestByteOrder() == WASM::ByteOrder::Little);

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) \
		&& (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	CHECK(WASM::nativeByteOrder() == WASM::ByteOrder::Big);
	CHECK_FALSE(WASM::hostMatchesGuestByteOrder());
#else
	CHECK(WASM::nativeByteOrder() == WASM::ByteOrder::Little);
	CHECK(WASM::hostMatchesGuestByteOrder());
#endif
}

TEST_CASE("the swap primitives reverse bytes and are their own inverse", "[byteorder]")
{
	CHECK(WASM::byteSwap(std::uint16_t(0x1234)) == 0x3412);
	CHECK(WASM::byteSwap(std::uint16_t(0x00FF)) == 0xFF00);
	CHECK(WASM::byteSwap(std::uint32_t(0x11223344)) == 0x44332211);
	CHECK(WASM::byteSwap(std::uint32_t(0x00000001)) == 0x01000000);
	CHECK(WASM::byteSwap(std::uint64_t(0x0102030405060708ull))
			== 0x0807060504030201ull);
	CHECK(WASM::byteSwap(std::uint64_t(0xFFFFFFFF00000001ull))
			== 0x01000000FFFFFFFFull);

	CHECK(WASM::byteSwap(WASM::byteSwap(std::uint16_t(0xA1B2))) == 0xA1B2);
	CHECK(WASM::byteSwap(WASM::byteSwap(std::uint32_t(0xDEADBEEF))) == 0xDEADBEEF);
	CHECK(WASM::byteSwap(WASM::byteSwap(std::uint64_t(0x0123456789ABCDEFull)))
			== 0x0123456789ABCDEFull);

	// `byteSwapIf` is the shape a backend uses: one branch, no cost when false.
	CHECK(WASM::byteSwapIf(false, std::uint32_t(0x11223344)) == 0x11223344);
	CHECK(WASM::byteSwapIf(true, std::uint32_t(0x11223344)) == 0x44332211);
}

TEST_CASE("the policy names parse case-insensitively and stay strict by default",
		"[byteorder]")
{
	WASM::ByteOrderPolicy policy = WASM::ByteOrderPolicy::Nonconformant;
	CHECK(WASM::parseByteOrderPolicy("strict", policy));
	CHECK(policy == WASM::ByteOrderPolicy::Strict);
	CHECK(WASM::parseByteOrderPolicy("  ADAPTIVE\t", policy));
	CHECK(policy == WASM::ByteOrderPolicy::Adaptive);
	CHECK(WASM::parseByteOrderPolicy("Nonconformant", policy));
	CHECK(policy == WASM::ByteOrderPolicy::Nonconformant);

	// A name the engine does not know leaves the caller's value alone, and the
	// environment reader then keeps `Strict`: a typo in an opt-in must never widen
	// the deviation from the spec.
	const WASM::ByteOrderPolicy previous = policy;
	CHECK_FALSE(WASM::parseByteOrderPolicy("host-order", policy));
	CHECK(policy == previous);
	CHECK_FALSE(WASM::parseByteOrderPolicy("", policy));
	CHECK(WASM::parseByteOrderPolicy("nonconformant ", policy));
	CHECK(policy == WASM::ByteOrderPolicy::Nonconformant);

	CHECK(std::string(WASM::byteOrderPolicyName(WASM::ByteOrderPolicy::Strict))
			== "strict");
	CHECK(std::string(WASM::byteOrderPolicyName(WASM::ByteOrderPolicy::Adaptive))
			== "adaptive");
	CHECK(std::string(
			WASM::byteOrderPolicyName(WASM::ByteOrderPolicy::Nonconformant))
			== "nonconformant");
}

TEST_CASE("on a little-endian host every policy behaves the same", "[byteorder]")
{
	// The property an embedder relies on when it flips this switch: on the hosts
	// this tree actually runs on, the switch changes nothing at all.
	WASM::ByteOrderEvidence openEvidence;
	WASM::ByteOrderEvidence closedEvidence;
	closedEvidence.hasMemory = true;

	const WASM::ByteOrderPolicy policies[3] = { WASM::ByteOrderPolicy::Strict,
		WASM::ByteOrderPolicy::Adaptive,
		WASM::ByteOrderPolicy::Nonconformant };

	for (const WASM::ByteOrderEvidence& sample : { openEvidence, closedEvidence }) {
		for (WASM::ByteOrderPolicy policy : policies) {
			const WASM::ByteOrderDecision decision =
					WASM::decideByteOrderFor(policy, sample, WASM::ByteOrder::Little);
			CHECK(decision.requested == policy);
			CHECK_FALSE(decision.swapsAccesses);
			CHECK((decision.conformant
					|| policy == WASM::ByteOrderPolicy::Nonconformant));
			CHECK(std::string(decision.reason).empty() == false);
		}
	}
}

TEST_CASE("the big-endian decision matrix, exercised on a little-endian host",
		"[byteorder]")
{
	// `decideByteOrderFor` takes the host order as an argument for exactly this
	// reason: the table a big-endian port would run is checked on every build.
	const WASM::ByteOrder big = WASM::ByteOrder::Big;

	WASM::ByteOrderEvidence openEvidence; // nothing known about the memory
	WASM::ByteOrderEvidence closedEvidence;
	closedEvidence.hasMemory = true; // private, unshared, no data segment

	const WASM::ByteOrderDecision strictOpen = WASM::decideByteOrderFor(
			WASM::ByteOrderPolicy::Strict, openEvidence, big);
	CHECK(strictOpen.swapsAccesses);
	CHECK(strictOpen.conformant);
	CHECK(strictOpen.applied == WASM::ByteOrderPolicy::Strict);

	// Strict does not consult the evidence: it converts even where it could skip.
	const WASM::ByteOrderDecision strictClosed = WASM::decideByteOrderFor(
			WASM::ByteOrderPolicy::Strict, closedEvidence, big);
	CHECK(strictClosed.swapsAccesses);

	const WASM::ByteOrderDecision nonconformant = WASM::decideByteOrderFor(
			WASM::ByteOrderPolicy::Nonconformant, closedEvidence, big);
	CHECK_FALSE(nonconformant.swapsAccesses);
	CHECK_FALSE(nonconformant.conformant);

	// Adaptive is the only policy whose verdict depends on the module.
	const WASM::ByteOrderDecision adaptiveOpen = WASM::decideByteOrderFor(
			WASM::ByteOrderPolicy::Adaptive, openEvidence, big);
	CHECK(adaptiveOpen.swapsAccesses);
	CHECK(adaptiveOpen.conformant);
	CHECK(adaptiveOpen.applied == WASM::ByteOrderPolicy::Strict);

	const WASM::ByteOrderDecision adaptiveClosed = WASM::decideByteOrderFor(
			WASM::ByteOrderPolicy::Adaptive, closedEvidence, big);
	CHECK_FALSE(adaptiveClosed.swapsAccesses);
	CHECK_FALSE(adaptiveClosed.conformant);
	CHECK(adaptiveClosed.applied == WASM::ByteOrderPolicy::Nonconformant);

	// One piece of evidence at a time is enough to keep Adaptive conforming.
	struct Case {
		const char* name;
		void (*make)(WASM::ByteOrderEvidence&);
	};
	const Case cases[] = {
		{ "an active data segment",
				[](WASM::ByteOrderEvidence& e) { e.hasActiveDataSegment = true; } },
		{ "a passive data segment",
				[](WASM::ByteOrderEvidence& e) { e.hasPassiveDataSegment = true; } },
		{ "an imported memory",
				[](WASM::ByteOrderEvidence& e) { e.importsMemory = true; } },
		{ "an exported memory",
				[](WASM::ByteOrderEvidence& e) { e.exportsMemory = true; } },
		{ "a shared memory",
				[](WASM::ByteOrderEvidence& e) { e.hasSharedMemory = true; } },
	};
	for (const Case& sample : cases) {
		WASM::ByteOrderEvidence evidence;
		evidence.hasMemory = true;
		sample.make(evidence);
		CHECK_FALSE(evidence.closed());
		const WASM::ByteOrderDecision decision = WASM::decideByteOrderFor(
				WASM::ByteOrderPolicy::Adaptive, evidence, big);
		INFO(sample.name);
		CHECK(decision.swapsAccesses);
		CHECK(decision.conformant);
	}
}

TEST_CASE("the evidence a module shows about its own bytes", "[byteorder]")
{
	// What `Adaptive` is allowed to look at. Each fixture is the smallest module
	// that shows (or fails to show) one piece of it.
	const WASM::ByteOrderEvidence none = evidenceOf("byte_order_evidence_none");
	CHECK_FALSE(none.hasMemory);
	CHECK(none.showsNoModuleBytes());
	CHECK_FALSE(none.closed());

	const WASM::ByteOrderEvidence closed = evidenceOf("byte_order_evidence_closed");
	CHECK(closed.hasMemory);
	CHECK_FALSE(closed.importsMemory);
	CHECK_FALSE(closed.exportsMemory);
	CHECK(closed.showsNoModuleBytes());
	CHECK(closed.closed());

	const WASM::ByteOrderEvidence withData = evidenceOf("byte_order_evidence_data");
	CHECK(withData.hasMemory);
	CHECK(withData.hasActiveDataSegment);
	CHECK_FALSE(withData.hasPassiveDataSegment);
	CHECK_FALSE(withData.showsNoModuleBytes());
	CHECK_FALSE(withData.closed());

	const WASM::ByteOrderEvidence exported =
			evidenceOf("byte_order_evidence_exported");
	CHECK(exported.exportsMemory);
	CHECK_FALSE(exported.closed());

	// And the evidence decides the Adaptive verdict, not the other way round.
	const WASM::ByteOrderDecision closedDecision = WASM::decideByteOrderFor(
			WASM::ByteOrderPolicy::Adaptive, closed, WASM::ByteOrder::Big);
	CHECK_FALSE(closedDecision.swapsAccesses);
	const WASM::ByteOrderDecision exportedDecision = WASM::decideByteOrderFor(
			WASM::ByteOrderPolicy::Adaptive, exported, WASM::ByteOrder::Big);
	CHECK(exportedDecision.swapsAccesses);
}

