#include "WasmByteOrder.hpp"

#include "WasmModule.hpp"
#include "WasmType.hpp"

#include <cstdlib>
#include <cstring>

/*!
  \file WasmByteOrder.cpp
  \brief The byte-order policy: names, parsing, evidence and the decision table.

  Nothing here emits code. The conversion a backend performs is
  `byteSwapIf(decision.swapsAccesses, value)` around its own loads and stores
  (docs/proposals/BIG_ENDIAN.md §7 `BE-3`); this file only decides *whether* it
  must, and says why in the decision's `reason`.
*/

WASMJIT_VISIBLE_BEGIN

namespace WASM {

const char* byteOrderPolicyName(ByteOrderPolicy policy) noexcept
{
	switch (policy) {
	case ByteOrderPolicy::Strict:
		return "strict";
	case ByteOrderPolicy::Adaptive:
		return "adaptive";
	case ByteOrderPolicy::Nonconformant:
		return "nonconformant";
	}
	return "strict";
}

namespace {

//! Case-folds and trims ASCII whitespace, so `" Adaptive "` and `"ADAPTIVE"` both
//! name the same policy. A configuration file and an environment variable should
//! not disagree about spelling.
std::string_view normalizePolicyText(std::string_view text) noexcept
{
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
		text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
		text.remove_suffix(1);
	return text;
}

bool equalsIgnoreCase(std::string_view left, std::string_view right) noexcept
{
	if (left.size() != right.size())
		return false;
	for (std::size_t i = 0; i < left.size(); ++i) {
		char a = left[i];
		char b = right[i];
		if (a >= 'A' && a <= 'Z')
			a = char(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = char(b - 'A' + 'a');
		if (a != b)
			return false;
	}
	return true;
}

} // namespace

bool parseByteOrderPolicy(std::string_view text, ByteOrderPolicy& out) noexcept
{
	const std::string_view name = normalizePolicyText(text);
	if (equalsIgnoreCase(name, "strict")) {
		out = ByteOrderPolicy::Strict;
		return true;
	}
	if (equalsIgnoreCase(name, "adaptive")) {
		out = ByteOrderPolicy::Adaptive;
		return true;
	}
	if (equalsIgnoreCase(name, "nonconformant")) {
		out = ByteOrderPolicy::Nonconformant;
		return true;
	}
	return false;
}

ByteOrderPolicy byteOrderPolicyFromEnvironment() noexcept
{
	ByteOrderPolicy policy = ByteOrderPolicy::Strict;
	const char* value = std::getenv("WASMJIT_BYTE_ORDER");
	// An unset or unnamed value stays `Strict`: that is the only policy whose
	// behaviour cannot be wrong, and a typo in an opt-in must never widen the
	// engine's deviation from the spec (docs/SAFETY.md §2.1).
	if (value != nullptr)
		(void)parseByteOrderPolicy(std::string_view(value), policy);
	return policy;
}

ByteOrder compilingHostByteOrder() noexcept
{
	// The override is a diagnostic for running the big-endian emission path on
	// the only kind of host this tree has ever run on (docs/proposals/
	// BIG_ENDIAN.md §8). It is read per call rather than cached so a test can
	// switch it between two instantiations; `decideByteOrder` calls it once per
	// instantiation, which is cold next to compilation.
	const char* value = std::getenv("WASMJIT_BYTE_ORDER_HOST");
	if (value != nullptr) {
		const std::string_view name = normalizePolicyText(std::string_view(value));
		// An unparsable value is ignored rather than refused: this is a
		// diagnostic, and a typo must not change what the engine compiles.
		if (equalsIgnoreCase(name, "big"))
			return ByteOrder::Big;
		if (equalsIgnoreCase(name, "little"))
			return ByteOrder::Little;
	}
	return nativeByteOrder();
}

bool compilingHostSwapsAccesses() noexcept
{
	return compilingHostByteOrder() != guestByteOrder();
}

ByteOrderEvidence byteOrderEvidence(const Module& module) noexcept
{
	ByteOrderEvidence evidence;
	evidence.hasMemory = !module.memories.empty();
	evidence.importsMemory = !module.importMemories.empty();
	for (const MemoryType& memory : module.memories) {
		// Limits flag bit 1 is `shared`: the bytes are visible to another thread
		// through the same storage, so no policy may reinterpret them locally.
		if ((memory.limits.flags & 0x02u) != 0u)
			evidence.hasSharedMemory = true;
	}
	for (const DataSegment& segment : module.dataSegments) {
		// Bit 0 of the segment flags is the passive mode; an active segment copies
		// its bytes in at instantiation, a passive one only under `memory.init`.
		if ((segment.mode & 0x01u) != 0u)
			evidence.hasPassiveDataSegment = true;
		else
			evidence.hasActiveDataSegment = true;
	}
	for (const Export& entry : module.exports) {
		if (entry.kind == ExternalKind::Memory)
			evidence.exportsMemory = true;
	}
	return evidence;
}

ByteOrderDecision decideByteOrderFor(ByteOrderPolicy policy,
		const ByteOrderEvidence& evidence, ByteOrder native) noexcept
{
	ByteOrderDecision decision;
	decision.requested = policy;
	if (native == guestByteOrder()) {
		// The host already speaks the guest's order, so every policy behaves the
		// same and conforms to the spec. This is the property the tests pin: a
		// little-endian build cannot be affected by the policy at all.
		decision.applied = policy == ByteOrderPolicy::Adaptive
				? ByteOrderPolicy::Strict
				: policy;
		decision.swapsAccesses = false;
		decision.conformant = true;
		decision.reason = "host byte order matches the guest's";
		return decision;
	}

	switch (policy) {
	case ByteOrderPolicy::Strict:
		decision.applied = ByteOrderPolicy::Strict;
		decision.swapsAccesses = true;
		decision.conformant = true;
		decision.reason = "strict: every multi-byte access converts";
		return decision;
	case ByteOrderPolicy::Nonconformant:
		decision.applied = ByteOrderPolicy::Nonconformant;
		decision.swapsAccesses = false;
		decision.conformant = false;
		decision.reason = "nonconformant: the guest sees the host's byte order";
		return decision;
	case ByteOrderPolicy::Adaptive:
		break;
	}

	// Adaptive: skip the conversion only when the module carries no bytes of its
	// own and nobody else can observe its memory. The relaxation is reported,
	// because the evidence is a heuristic (BED2).
	if (evidence.closed()) {
		decision.applied = ByteOrderPolicy::Nonconformant;
		decision.swapsAccesses = false;
		decision.conformant = false;
		decision.reason = "adaptive: private memory, no module-supplied bytes";
		return decision;
	}
	decision.applied = ByteOrderPolicy::Strict;
	decision.swapsAccesses = true;
	decision.conformant = true;
	decision.reason = "adaptive: evidence the module's bytes are shared or come from the binary";
	return decision;
}

ByteOrderDecision decideByteOrder(ByteOrderPolicy policy,
		const ByteOrderEvidence& evidence) noexcept
{
	// The host order is a parameter of `decideByteOrderFor`, so a diagnostic
	// that claims a big-endian host runs the whole table on a little-endian
	// build (docs/proposals/BIG_ENDIAN.md §8).
	return decideByteOrderFor(policy, evidence, compilingHostByteOrder());
}

} // namespace WASM

WASMJIT_VISIBLE_END
