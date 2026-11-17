#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Sljit/SljitModuleCompiler.hpp"
#include "TestBackend.hpp"
#include "TestStore.hpp"

#include "WasmBase/WasmArtifact.hpp"
#include "WasmBase/WasmBackendRegistry.hpp"
#include "WasmBase/WasmByteOrder.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"

#include <Euphemy/Config/EuphConfiguration.hpp>
#include <Euphemy/Io/EuphFile.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

/*!
  \file ByteOrderTests.cpp
  \brief The byte-order policy, its decision table, and the evidence it reads.

  Guest memory is little-endian by specification, so on a little-endian host the
  policy must be invisible. Three things make the big-endian path testable
  anyway, and all three are deliberate:

  - `decideByteOrderFor` takes the host byte order as a parameter, so the whole
    big-endian decision matrix runs on this host;
  - the swap primitives are value-level functions, so their involution and their
    known-answer vectors are checkable without a big-endian target;
  - `WASMJIT_BYTE_ORDER_HOST` claims another host order for a compilation, so the
    conversion this backend emits can be *executed* and observed here. It is a
    diagnostic, never a support claim (docs/proposals/BIG_ENDIAN.md §8).

  What still needs a real big-endian runner is the arithmetic itself: the atomic
  helper conversion is a compile-time property of the target and is evidenced by
  the primitive vectors below rather than by execution.
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

namespace {

//! Claims a host byte order for the duration of one case.
//!
//! `WASMJIT_BYTE_ORDER_HOST` is the diagnostic that lets the big-endian emission
//! path run on a little-endian host (docs/proposals/BIG_ENDIAN.md §8). It is
//! restored on destruction, so one case cannot leak a claimed order into the
//! next.
struct HostOrderClaim {
explicit HostOrderClaim(const char* order)
{
setenv("WASMJIT_BYTE_ORDER_HOST", order, 1);
}
~HostOrderClaim()
{
unsetenv("WASMJIT_BYTE_ORDER_HOST");
}
};

std::string fixturePath(const char* name)
{
return std::string(WASM_TEST_DIR) + name + ".wasm";
}

//! Instantiates a fixture on the SLJIT backend and hands back the instance.
struct InstalledFixture {
WASM::RuntimeConfig config;
std::unique_ptr<WASM::Runtime> runtime;
std::shared_ptr<const WASM::Module> module;
WASM::RegistryImportResolver imports;
WASM::ModuleInstance* instance = nullptr;

explicit InstalledFixture(const char* fixture, WASM::ByteOrderPolicy policy)
{
config.byteOrder = policy;
runtime = WASM::Runtime::create("sljit", config);
module = runtime->loadFile(fixturePath(fixture));
instance = &runtime->instantiate(*module, imports);
}
};

//! One direct compilation of a fixture through a fresh SLJIT backend instance.
//!
//! The backend object is kept alive because the code it generated lives in it:
//! two of these exist at once, which is what makes a byte-for-byte comparison of
//! their bodies meaningful (`BED5`).
struct CompiledBody {
WASM::Store store;
std::unique_ptr<WASM::Module> module;
WASM::BackendInstance backend;
std::unique_ptr<WASM::ModuleInstance> instance;
WASM::RegistryImportResolver imports;
const void* entry = nullptr;
std::size_t bytes = 0;

explicit CompiledBody(const char* fixture, WASM::ByteOrderPolicy policy)
: backend(WASM::createBackendInstance("sljit"))
{
Euph::Io::File file(fixturePath(fixture).c_str(), Elv::Io::Mode::READ);
module = std::make_unique<WASM::Module>();
module->fromFile(file, store);
auto* compiler = dynamic_cast<Sljit::ModuleCompiler*>(backend.instantiator.get());
REQUIRE(compiler != nullptr);
compiler->setByteOrderPolicy(policy);
instance = backend.instantiator->instantiate(*module, imports);
REQUIRE(instance != nullptr);
entry = compiler->firstCodeEntry();
bytes = compiler->firstCodeBytes();
}
};

} // namespace

TEST_CASE("the header words and value conversions keep one order on every host",
"[byteorder]")
{
// `BED3` and `BE-4` in their smallest form: the two primitives every
// conversion in the engine is built from. Reversing is its own inverse, a
// single byte has nothing to reverse and a 16-byte vector image keeps its byte
// order, which is why width 16 is left alone.
CHECK(WASM::headerWordFromGuest(false, 0x11223344u) == 0x11223344u);
CHECK(WASM::headerWordFromGuest(true, 0x11223344u) == 0x44332211u);
CHECK(WASM::headerWordFromGuest(true, WASM::headerWordFromGuest(true, 0x11223344u))
== 0x11223344u);

CHECK(WASM::swapValue(true, 1, 0x11u) == 0x11u);
CHECK(WASM::swapValue(true, 2, 0x1122u) == 0x2211u);
CHECK(WASM::swapValue(true, 4, 0x11223344u) == 0x44332211u);
CHECK(WASM::swapValue(true, 8, 0x1122334455667788ull) == 0x8877665544332211ull);
CHECK(WASM::swapValue(true, 16, 0x1122334455667788ull) == 0x1122334455667788ull);
CHECK(WASM::swapValue(false, 8, 0x1122334455667788ull) == 0x1122334455667788ull);
}

TEST_CASE("the byte-order policy is a runtime setting with a conforming default",
"[byteorder]")
{
// `BE-2`: the switch an embedder sees. The environment seeds it, and an
// unnamed value stays `Strict` — the only policy that can never be wrong on a
// little-endian host.
unsetenv("WASMJIT_BYTE_ORDER");
CHECK(WASM::RuntimeConfig{}.byteOrder == WASM::ByteOrderPolicy::Strict);

setenv("WASMJIT_BYTE_ORDER", "adaptive", 1);
CHECK(WASM::RuntimeConfig{}.byteOrder == WASM::ByteOrderPolicy::Adaptive);
setenv("WASMJIT_BYTE_ORDER", "FULL-ORDER", 1);
CHECK(WASM::RuntimeConfig{}.byteOrder == WASM::ByteOrderPolicy::Strict);
unsetenv("WASMJIT_BYTE_ORDER");
}

TEST_CASE("a configuration file names the policy and refuses a typo",
"[byteorder][config]")
{
const auto read = [](const std::string& text) {
return WASM::RuntimeConfig::fromConfiguration(Euph::Conf::Configuration(text));
};
CHECK(read("[WasmJit]\nsByteOrder = nonconformant\n").byteOrder
== WASM::ByteOrderPolicy::Nonconformant);
CHECK(read("[WasmJit]\nsByteOrder = ADAPTIVE\n").byteOrder
== WASM::ByteOrderPolicy::Adaptive);
CHECK(read("[WasmJit]\nsProfile = scalar\n").byteOrder
== WASM::ByteOrderPolicy::Strict);
// A spelling that names no policy is refused where it is set, like every other
// named key (docs/CONFIGURING.md §2.1 rule 3).
REQUIRE_THROWS_AS(read("[WasmJit]\nsByteOrder = sometimes\n"), std::invalid_argument);
}

TEST_CASE("the decision reaches the instance the runtime compiled", "[byteorder]")
{
// `BE-2`'s done-when: ask for a policy, read back what was applied. The
	// closed fixture has a private memory and no data segment, so `Adaptive` is
	// the policy whose verdict could differ from the request.
	InstalledFixture installed("byte_order_evidence_closed",
WASM::ByteOrderPolicy::Adaptive);
	const WASM::ByteOrderDecision& decision = installed.instance->byteOrderDecision();
	CHECK(decision.requested == WASM::ByteOrderPolicy::Adaptive);
	CHECK(std::strlen(decision.reason) > 0);

	if (WASM::hostMatchesGuestByteOrder()
			&& std::getenv("WASMJIT_BYTE_ORDER_HOST") == nullptr) {
		// On a host that already speaks the guest's order the whole layer is
// inert: no conversion, and the decision still conforms. `Adaptive`
// collapses to `Strict` because there is nothing to relax.
CHECK_FALSE(decision.swapsAccesses);
CHECK(decision.conformant);
CHECK(decision.applied == WASM::ByteOrderPolicy::Strict);
}
}

TEST_CASE("a little-endian build emits the same body for every policy", "[byteorder]")
{
// `BED5`, measured rather than argued. Two bodies at different addresses
// cannot be compared byte for byte — x86-64 spells a helper call as a
// RIP-relative displacement, so the same instruction encodes differently in
// two code blocks — which makes the *size* the address-independent measure.
// Equal sizes for all three policies is the claim; the next case shows that a
// conversion, when it is emitted, does change the size.
if (!WASM::hostMatchesGuestByteOrder()
|| std::getenv("WASMJIT_BYTE_ORDER_HOST") != nullptr) {
SUCCEED("this build's own host order is big-endian: the property is about LE");
return;
}

const CompiledBody strict("byte_order_roundtrip", WASM::ByteOrderPolicy::Strict);
const CompiledBody adaptive("byte_order_roundtrip", WASM::ByteOrderPolicy::Adaptive);
const CompiledBody lax("byte_order_roundtrip", WASM::ByteOrderPolicy::Nonconformant);

REQUIRE(strict.bytes > 0);
CHECK(adaptive.bytes == strict.bytes);
CHECK(lax.bytes == strict.bytes);

// And the reason the sizes agree: no policy asks for a conversion here.
for (const WASM::ByteOrderPolicy policy : { WASM::ByteOrderPolicy::Strict,
WASM::ByteOrderPolicy::Adaptive, WASM::ByteOrderPolicy::Nonconformant }) {
InstalledFixture installed("byte_order_roundtrip", policy);
CHECK_FALSE(installed.instance->byteOrderDecision().swapsAccesses);
CHECK(installed.instance->byteOrderDecision().conformant);
}
}

TEST_CASE("a claimed big-endian host emits the conversion the policy asks for",
"[byteorder]")
{
// What this diagnostic can prove, and what it cannot. Adding a byte reversal
// to a little-endian CPU's loads and stores does **not** reproduce big-endian
// memory: the storage order stays little-endian, so a `store8`/`load` pair
// observed byte-wise would disagree with a real port. What it does prove is
// that the backend *emits* the conversion, and that the policy decides it —
// which is the emitted half of `BE-3` (docs/proposals/BIG_ENDIAN.md §8). The
// end-to-end semantics still need a big-endian runner (`BE-5`).
if (!WASM::hostMatchesGuestByteOrder()) {
SUCCEED("this build is big-endian already: the conversion is not simulated");
return;
}

const CompiledBody native("byte_order_roundtrip", WASM::ByteOrderPolicy::Strict);
REQUIRE(native.bytes > 0);

HostOrderClaim claim("big");
CHECK(WASM::compilingHostByteOrder() == WASM::ByteOrder::Big);
CHECK(WASM::compilingHostSwapsAccesses());

const CompiledBody converted("byte_order_roundtrip", WASM::ByteOrderPolicy::Strict);
const CompiledBody refused("byte_order_roundtrip", WASM::ByteOrderPolicy::Nonconformant);
// `Strict` converts, so the body carries the reversal instructions; the
// relaxed policy does not, so its body is exactly the native one.
CHECK(converted.bytes > native.bytes);
CHECK(refused.bytes == native.bytes);

// A converted load followed by a converted store still round-trips: the
// reversal is symmetric, which is why a value-level fixture cannot tell a big
// endian host from a little one and only the byte-image fixtures in the spec
// suite can.
InstalledFixture strict("byte_order_evidence_closed", WASM::ByteOrderPolicy::Strict);
CHECK(strict.instance->byteOrderDecision().swapsAccesses);
CHECK(strict.instance->byteOrderDecision().conformant);
CHECK(strict.runtime->callExport<std::int32_t>(*strict.instance, "scratch")
== 0x11223344);

// `Nonconformant` is the documented non-conformance: no conversion at all.
InstalledFixture lax("byte_order_evidence_closed", WASM::ByteOrderPolicy::Nonconformant);
CHECK_FALSE(lax.instance->byteOrderDecision().swapsAccesses);
CHECK_FALSE(lax.instance->byteOrderDecision().conformant);

// `Adaptive` skips only where the evidence is closed, and reports the policy
// it actually applied (`BED2`).
InstalledFixture adaptive("byte_order_evidence_closed",
WASM::ByteOrderPolicy::Adaptive);
CHECK_FALSE(adaptive.instance->byteOrderDecision().swapsAccesses);
CHECK(adaptive.instance->byteOrderDecision().applied
== WASM::ByteOrderPolicy::Nonconformant);
}

TEST_CASE("a backend that cannot convert refuses instead of degrading", "[byteorder]")
{
// `BED4`. On a host whose order matches the guest's, every backend is correct
// and nothing is refused; with the host order claimed as big-endian, the two
// backends without a byte reversal refuse the module and the one that converts
// compiles it.
if (!WASM::hostMatchesGuestByteOrder()) {
SUCCEED("this build is big-endian: the refusal is the normal path");
return;
}
HostOrderClaim claim("big");

for (const char* backend : { "libjit", "asmjit" }) {
INFO("backend: " << backend);
auto runtime = WASM::Runtime::create(backend);
auto module = runtime->loadFile(fixturePath("byte_order_roundtrip"));
WASM::RegistryImportResolver imports;
REQUIRE_THROWS_WITH(runtime->instantiate(*module, imports),
Catch::Matchers::ContainsSubstring("big-endian"));
}

auto runtime = WASM::Runtime::create("sljit");
auto module = runtime->loadFile(fixturePath("byte_order_roundtrip"));
WASM::RegistryImportResolver imports;
REQUIRE_NOTHROW(runtime->instantiate(*module, imports));
}
