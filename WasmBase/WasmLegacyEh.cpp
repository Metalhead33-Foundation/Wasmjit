#include "WasmLegacyEh.hpp"

#include "WasmModule.hpp"
#include "WasmOpcodeDispatcher.hpp"
#include "WasmStore.hpp"
#include "WasmType.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace WASM {
namespace {

bool isLegacyOpcode(Opcode opcode)
{
	switch (opcode) {
		case Opcode::TryLegacy:
		case Opcode::Catch:
		case Opcode::CatchAll:
		case Opcode::Rethrow:
		case Opcode::Delegate:
			return true;
		default:
			return false;
	}
}

[[noreturn]] void refuseRewrite(const char* construct)
{
	throw std::runtime_error(std::string("legacy exception handling: this module uses a construct "
			"the built-in rewrite does not translate (") + construct + "). Rebuild it with "
			"`-mllvm -wasm-use-legacy-eh=false` / `-sWASM_LEGACY_EXCEPTIONS=0`, or translate it "
			"with `wasm-opt --enable-exception-handling --translate-to-exnref`. See "
			"docs/proposals/LEGACY_EXCEPTIONS.md.");
}

//! One decoded instruction: its byte range in the body and its primary opcode.
struct InstructionSpan {
	std::size_t begin = 0;
	std::size_t end = 0;
	Opcode opcode = Opcode::Unreachable;
	//! Number of clauses a final-form `try_table` names; zero for everything
	//! else. Used to tell a catching frame from a `try_table` that catches
	//! nothing when a legacy `delegate` is checked for a skipped handler
	//! (`LE8`).
	std::uint32_t catchCount = 0;
};

//! Decodes a body into `InstructionSpan`s. It parses every immediate by
//! reusing the engine's own dispatcher (all of whose callbacks are no-ops
//! here), so the operand lengths can never drift from the decoder's. The five
//! superseded opcodes are parsed locally because the dispatcher refuses them.
class InstructionScanner : public OpcodeDispatcher
{
public:
	std::vector<InstructionSpan> spans;

	void scan(const std::vector<std::uint8_t>& code)
	{
		static const std::uint8_t kEmpty = 0;
		const std::uint8_t* base = code.empty() ? &kEmpty : code.data();
		std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(base), code.size());
		Euph::Io::ConstBufferDevice device(bytes);
		FastMemReader stream(device);
		while (!stream.device.eof()) {
			const std::size_t begin = static_cast<std::size_t>(stream.device.tell());
			const Opcode opcode = stream.read<Opcode>();
			if (isLegacyOpcode(opcode))
				consumeLegacy(stream, opcode);
			else
				dispatchOpcode(stream, opcode);
			spans.push_back(InstructionSpan{begin, static_cast<std::size_t>(stream.device.tell()),
					opcode, pendingCatchCount_});
			pendingCatchCount_ = 0;
		}
	}

protected:
	void onUnhandledOpcode(const char*) override {}

	//! Records how many clauses a `try_table` names, so `build()` can tell a
	//! catching frame from one that catches nothing without re-parsing the
	//! immediate (docs/proposals/LEGACY_EXCEPTIONS.md LE8/LE9).
	void dispatchTryTable(const BlockType&, std::vector<CatchClause>&& clauses) override
	{
		pendingCatchCount_ = static_cast<std::uint32_t>(clauses.size());
	}

private:
	void consumeLegacy(FastMemReader& stream, Opcode opcode)
	{
		switch (opcode) {
			case Opcode::TryLegacy: {
				BlockType bt;
				stream >> bt;
				break;
			}
			case Opcode::Catch: {
				auto tag = stream.readLEB128<TagIdx>();
				(void)tag;
				break;
			}
			case Opcode::Rethrow:
			case Opcode::Delegate: {
				auto depth = stream.readLEB128<LabelIdx>();
				(void)depth;
				break;
			}
			case Opcode::CatchAll:
				break;
			default:
				break;
		}
	}

	//! Clauses the last dispatched `try_table` named; zero for every other
	//! opcode, and reset after each span is recorded.
	std::uint32_t pendingCatchCount_ = 0;
};

//! An unsigned LEB128 read straight from the body bytes.
std::uint32_t readUnsignedLeb(const std::vector<std::uint8_t>& code, std::size_t& at, std::size_t end)
{
	std::uint32_t value = 0;
	unsigned shift = 0;
	while (at < end) {
		const std::uint8_t byte = code[at++];
		value |= static_cast<std::uint32_t>(byte & 0x7Fu) << shift;
		if ((byte & 0x80u) == 0)
			return value;
		shift += 7;
		if (shift > 28)
			break;
	}
	throw std::runtime_error("legacy exception handling: malformed LEB128 in a rewritten body");
}

//! One `catch`/`catch_all` arm: its tag, its span range, and the synthetic
//! `exnref` local that keeps the caught exception when the arm rethrows.
struct LegacyArm {
	bool isCatchAll = false;
	std::uint32_t tagIdx = 0;
	std::size_t bodyFirst = 0;
	std::size_t bodyLast = 0;
	bool needsExnRef = false;
	std::uint32_t exnLocal = 0;
};

//! One legacy `try`: its block type, its body, its arms, and how it ends.
struct LegacyTry {
	std::size_t tokenIndex = 0;
	std::size_t blockTypeBegin = 0;
	std::size_t blockTypeEnd = 0;
	std::size_t bodyFirst = 0;
	std::size_t bodyLast = 0;
	std::vector<LegacyArm> arms;
	std::size_t terminator = 0;
	bool isDelegate = false;
	std::uint32_t delegateDepth = 0;
};

} // namespace

namespace {

//! Rewrites one function body: superseded `try`/`catch`/`catch_all`/`rethrow`/
//! `delegate` into the final `try_table`/`throw_ref` encoding. The shape it
//! emits is the one docs/proposals/LEGACY_EXCEPTIONS.md §10.3 works out:
//!
//!     block <bt>                      ; A: the try's label, the merge point
//!       block <last arm payload …>    ; arms open in *reverse* order, so the
//!         …                           ; matching arm lands innermost-first
//!           block                     ; catch_all target (no payload)
//!             try_table <bt> (catch $t_i <arm_i> …) (catch_all <inner>)
//!               <body>
//!             end
//!             br A                    ; normal completion
//!           end
//!           <arm_last> br A
//!         end
//!         <arm_last-1> br A
//!       end
//!       <arm_0> br A
//!     end
class BodyRewriter
{
public:
	BodyRewriter(Module& module, FunctionBody& body, const std::vector<InstructionSpan>& spans,
			std::uint32_t paramCount)
		: module_(module), body_(body), spans_(spans), code_(body.code),
		  localCursor_(paramCount)
	{
	}

	std::vector<std::uint8_t> run();

private:
	static constexpr std::size_t kNoTry = static_cast<std::size_t>(-1);

	void build();
	void allocateExnLocals();
	void emitRange(std::size_t first, std::size_t last);
	void emitLegacyTry(const LegacyTry& legacyTry);
	void emitOne(std::size_t index);
	void emitCopiedBranch(std::size_t index);
	void emitRethrow(std::size_t index);
	void emitTryTable(std::size_t index);
	void copySpan(std::size_t index);

	std::uint32_t remapDepth(std::uint32_t depth) const;
	//! True when the frame opened by span `token` can catch an exception: a
	//! legacy try that declares arms, or a final-form `try_table` with at least
	//! one clause. A frame that catches nothing is transparent to `delegate`.
	bool          isCatchFrame(std::size_t token) const;
	int           emitBranchTo(int frameId);
	int           openFrame();
	void          closeFrame();

	void emitByte(std::uint8_t byte);
	void emitUnsignedLeb(std::uint64_t value);
	void emitSignedLeb(std::int64_t value);
	void emitOpcode(Opcode opcode);
	void emitBlockType(std::size_t begin, std::size_t end);
	void emitBlockTypeForResults(const FuncType& results);

	FuncType tagPayload(std::uint32_t tagIdx) const;

	Module&                                module_;
	FunctionBody&                          body_;
	const std::vector<InstructionSpan>&    spans_;
	const std::vector<std::uint8_t>&       code_;
	std::vector<std::uint8_t>              out_;
	std::vector<LegacyTry>                 tries_;
	std::vector<std::size_t>               tryByToken_;
	std::vector<std::uint32_t>             rethrowArmLocal_; // per span; 0 = not a rethrow
	std::vector<std::uint32_t>             rethrowArmTry_;   // per span
	std::vector<int>                       binStack_;
	std::vector<std::size_t>               binToken_;   // opener span index per binary frame
	std::vector<int>                       emitStack_;
	int                                    nextFrame_ = 0;
	std::uint32_t                          localCursor_ = 0;
};

//! The payload function type of a tag, resolved through the module's tag index
//! space (imported tags first).
FuncType BodyRewriter::tagPayload(std::uint32_t tagIdx) const
{
	const std::uint32_t typeIdx = tagIdx < module_.importTags.size()
			? module_.importTags[tagIdx].typeIdx
			: module_.tags[tagIdx - module_.importTags.size()].typeIdx;
	return module_.funcTypeAt(typeIdx);
}

std::vector<std::uint8_t> BodyRewriter::run()
{
	build();
	allocateExnLocals();
	emitRange(0, spans_.size());
	return std::move(out_);
}

//! Walks the spans once and records every legacy `try`, its arms, and every
//! `rethrow`'s target arm.
void BodyRewriter::build()
{
	tryByToken_.assign(spans_.size(), kNoTry);
	rethrowArmLocal_.assign(spans_.size(), 0);
	rethrowArmTry_.assign(spans_.size(), static_cast<std::uint32_t>(kNoTry));

	struct OpenFrame {
		bool        isLegacy = false;
		std::size_t tryIndex = 0;
	};
	// Active catch arms, innermost last. `rethrow n` counts these, not labels.
	struct ActiveArm {
		std::size_t tryIndex = 0;
		std::size_t armIndex = 0;
	};

	std::vector<OpenFrame> frames;
	std::vector<ActiveArm> arms;

	for (std::size_t i = 0; i < spans_.size(); ++i) {
		switch (spans_[i].opcode) {
			case Opcode::Block:
			case Opcode::Loop:
			case Opcode::If:
				frames.push_back(OpenFrame{});
				break;
			case Opcode::Else:
				break;
			case Opcode::TryLegacy: {
				LegacyTry legacyTry;
				legacyTry.tokenIndex = i;
				legacyTry.blockTypeBegin = spans_[i].begin + 1;
				legacyTry.blockTypeEnd = spans_[i].end;
				legacyTry.bodyFirst = i + 1;
				tries_.push_back(std::move(legacyTry));
				tryByToken_[i] = tries_.size() - 1;
				frames.push_back(OpenFrame{true, tries_.size() - 1});
				break;
			}
			case Opcode::Catch:
			case Opcode::CatchAll: {
				if (frames.empty() || !frames.back().isLegacy)
					refuseRewrite("catch outside a legacy try");
				LegacyTry& legacyTry = tries_[frames.back().tryIndex];
				if (legacyTry.arms.empty())
					legacyTry.bodyLast = i;
				else
					legacyTry.arms.back().bodyLast = i;
				if (!arms.empty() && arms.back().tryIndex == frames.back().tryIndex)
					arms.pop_back();
				LegacyArm arm;
				arm.isCatchAll = spans_[i].opcode == Opcode::CatchAll;
				arm.bodyFirst = i + 1;
				if (!arm.isCatchAll) {
					std::size_t at = spans_[i].begin + 1;
					arm.tagIdx = readUnsignedLeb(code_, at, spans_[i].end);
				}
				legacyTry.arms.push_back(arm);
				arms.push_back(ActiveArm{frames.back().tryIndex, legacyTry.arms.size() - 1});
				break;
			}
			case Opcode::End: {
				if (frames.empty())
					break; // the function's own terminal `end`
				const OpenFrame frame = frames.back();
				frames.pop_back();
				if (frame.isLegacy) {
					LegacyTry& legacyTry = tries_[frame.tryIndex];
					if (legacyTry.arms.empty())
						legacyTry.bodyLast = i;
					else
						legacyTry.arms.back().bodyLast = i;
					legacyTry.terminator = i;
					if (!arms.empty() && arms.back().tryIndex == frame.tryIndex)
						arms.pop_back();
				}
				break;
			}
			case Opcode::Delegate: {
				if (frames.empty() || !frames.back().isLegacy)
					refuseRewrite("delegate outside a legacy try");
				const OpenFrame frame = frames.back();
				frames.pop_back();
				LegacyTry& legacyTry = tries_[frame.tryIndex];
				legacyTry.bodyLast = i;
				legacyTry.isDelegate = true;
				legacyTry.terminator = i;
				std::size_t at = spans_[i].begin + 1;
				legacyTry.delegateDepth = readUnsignedLeb(code_, at, spans_[i].end);
				break;
			}
			case Opcode::Rethrow: {
				std::size_t at = spans_[i].begin + 1;
				const std::uint32_t depth = readUnsignedLeb(code_, at, spans_[i].end);
				if (depth >= arms.size())
					refuseRewrite("rethrow depth with no enclosing catch");
				const ActiveArm target = arms[arms.size() - 1 - depth];
				rethrowArmTry_[i] = static_cast<std::uint32_t>(target.tryIndex);
				rethrowArmLocal_[i] = static_cast<std::uint32_t>(target.armIndex);
				tries_[target.tryIndex].arms[target.armIndex].needsExnRef = true;
				break;
			}
			case Opcode::TryTable:
				// A final-form try is an ordinary frame as far as branch depths
				// are concerned: its own label opens after its clauses, exactly
				// like a block. Its clause labels are renumbered at emission
				// (docs/proposals/LEGACY_EXCEPTIONS.md LE-9).
				frames.push_back(OpenFrame{});
				break;
			default:
				break;
		}
	}
}

//! Appends one `exnref` local per rethrowing arm and records its index.
void BodyRewriter::allocateExnLocals()
{
	// Local indices start after the function's parameters, then follow the
	// declared locals in order; the synthetic ones are appended, so every
	// existing index keeps its meaning.
	for (const LocalEntry& entry : body_.locals)
		localCursor_ += entry.count;
	for (LegacyTry& legacyTry : tries_) {
		for (LegacyArm& arm : legacyTry.arms) {
			if (!arm.needsExnRef)
				continue;
			LocalEntry entry;
			entry.count = 1;
			entry.type.opcode = ValueTypeCode::ExnRef;
			entry.type.heapType = -1;
			body_.locals.push_back(entry);
			arm.exnLocal = localCursor_++;
		}
	}
}

// ---- emission primitives -------------------------------------------------

void BodyRewriter::emitByte(std::uint8_t byte)
{
	out_.push_back(byte);
}

void BodyRewriter::emitUnsignedLeb(std::uint64_t value)
{
	do {
		std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
		value >>= 7;
		if (value != 0)
			byte |= 0x80u;
		out_.push_back(byte);
	} while (value != 0);
}

void BodyRewriter::emitSignedLeb(std::int64_t value)
{
	for (;;) {
		const std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
		const bool sign = (byte & 0x40u) != 0;
		value >>= 7;
		if ((value == 0 && !sign) || (value == -1 && sign)) {
			out_.push_back(byte);
			return;
		}
		out_.push_back(static_cast<std::uint8_t>(byte | 0x80u));
	}
}

void BodyRewriter::emitOpcode(Opcode opcode)
{
	out_.push_back(static_cast<std::uint8_t>(opcode));
}

void BodyRewriter::emitBlockType(std::size_t begin, std::size_t end)
{
	out_.insert(out_.end(), code_.begin() + static_cast<std::ptrdiff_t>(begin),
			code_.begin() + static_cast<std::ptrdiff_t>(end));
}

//! Emits a block type whose *results* are `type.results`: the one-byte form
//! when it fits (void, one numeric/vector/abstract-reference result), and a
//! module type index otherwise. The index case appends a `(param) -> (results)`
//! function type to the module, which is the only way a multi-value block type
//! can be spelled.
void BodyRewriter::emitBlockTypeForResults(const FuncType& type)
{
	if (type.results.empty()) {
		emitByte(0x40); // empty block type
		return;
	}
	if (type.results.size() == 1 && !type.results[0].isPacked
			&& type.results[0].val.heapType < 0 && !type.results[0].val.isExact) {
		// A value type is a single signed-LEB byte whose payload is the low 7
		// bits of the (negative) code: i32 -> 0x7F, exnref -> 0x69.
		emitByte(static_cast<std::uint8_t>(
				static_cast<std::uint8_t>(type.results[0].val.opcode) & 0x7Fu));
		return;
	}
	Subtype subtype;
	subtype.isFinal = true;
	subtype.composite = FuncType{ {}, type.results };
	std::vector<Subtype> scratch(1, subtype);
	TypeGroup group;
	group.first = LocalTypeIdx{0};
	group.count = 1;
	const std::vector<TypeId> ids = module_.store().types().internGroup(scratch, group,
			module_.typeIds);
	const std::uint32_t index = static_cast<std::uint32_t>(module_.typeIds.size());
	module_.typeIds.push_back(ids.at(0));
	module_.typeGroups.push_back(group);
	emitSignedLeb(static_cast<std::int64_t>(index));
}

// ---- depth bookkeeping ---------------------------------------------------

int BodyRewriter::openFrame()
{
	const int id = nextFrame_++;
	emitStack_.push_back(id);
	return id;
}

void BodyRewriter::closeFrame()
{
	emitOpcode(Opcode::End);
	if (emitStack_.empty())
		refuseRewrite("unbalanced synthetic frame");
	emitStack_.pop_back();
}

//! The branch depth that reaches `frameId` from the current position.
int BodyRewriter::emitBranchTo(int frameId)
{
	for (std::size_t i = emitStack_.size(); i-- > 0; ) {
		if (emitStack_[i] == frameId)
			return static_cast<int>(emitStack_.size() - 1 - i);
	}
	refuseRewrite("branch target is not open");
	return 0;
}

//! Maps a branch depth written against the *binary* frame stack onto the stack
//! this rewrite emits, whose synthetic frames sit between the two.
std::uint32_t BodyRewriter::remapDepth(std::uint32_t depth) const
{
	if (depth >= binStack_.size())
		refuseRewrite("branch depth out of range");
	const int target = binStack_[binStack_.size() - 1 - depth];
	for (std::size_t i = emitStack_.size(); i-- > 0; ) {
		if (emitStack_[i] == target)
			return static_cast<std::uint32_t>(emitStack_.size() - 1 - i);
	}
	refuseRewrite("branch target is not open");
	return 0;
}

bool BodyRewriter::isCatchFrame(std::size_t token) const
{
	const std::size_t tryIndex = tryByToken_[token];
	if (tryIndex != kNoTry)
		return !tries_[tryIndex].arms.empty();
	return spans_[token].opcode == Opcode::TryTable && spans_[token].catchCount != 0;
}

// ---- the walk itself -----------------------------------------------------

void BodyRewriter::copySpan(std::size_t index)
{
	out_.insert(out_.end(), code_.begin() + static_cast<std::ptrdiff_t>(spans_[index].begin),
			code_.begin() + static_cast<std::ptrdiff_t>(spans_[index].end));
}

//! Copies a branch instruction, rewriting every label it carries.
void BodyRewriter::emitCopiedBranch(std::size_t index)
{
	const std::uint8_t primary = code_[spans_[index].begin];
	const std::size_t end = spans_[index].end;
	std::size_t at = spans_[index].begin + 1;

	if (primary == static_cast<std::uint8_t>(Opcode::PrefixGC)) {
		// br_on_cast / br_on_cast_fail / their desc-eq spellings carry a label
		// after the castflags byte; every other 0xFB instruction does not.
		const std::uint32_t sub = readUnsignedLeb(code_, at, end);
		const bool carriesLabel = sub == 24 || sub == 25 || sub == 37 || sub == 38;
		emitByte(primary);
		emitUnsignedLeb(sub);
		if (!carriesLabel) {
			out_.insert(out_.end(), code_.begin() + static_cast<std::ptrdiff_t>(at),
					code_.begin() + static_cast<std::ptrdiff_t>(end));
			return;
		}
		emitByte(code_[at++]); // castflags
		emitUnsignedLeb(remapDepth(readUnsignedLeb(code_, at, end)));
		out_.insert(out_.end(), code_.begin() + static_cast<std::ptrdiff_t>(at),
				code_.begin() + static_cast<std::ptrdiff_t>(end));
		return;
	}

	emitByte(primary);
	switch (spans_[index].opcode) {
		case Opcode::Br:
		case Opcode::BrIf:
		case Opcode::BrOnNull:
		case Opcode::BrOnNonNull:
			emitUnsignedLeb(remapDepth(readUnsignedLeb(code_, at, end)));
			break;
		case Opcode::BrTable: {
			const std::uint32_t count = readUnsignedLeb(code_, at, end);
			emitUnsignedLeb(count);
			for (std::uint32_t i = 0; i < count; ++i)
				emitUnsignedLeb(remapDepth(readUnsignedLeb(code_, at, end)));
			emitUnsignedLeb(remapDepth(readUnsignedLeb(code_, at, end)));
			break;
		}
		default:
			refuseRewrite("a branch instruction this rewrite does not remap");
	}
}

//! `rethrow` becomes `throw_ref` on the caught exception the arm kept.
void BodyRewriter::emitRethrow(std::size_t index)
{
	const std::size_t tryIndex = rethrowArmTry_[index];
	const std::size_t armIndex = rethrowArmLocal_[index];
	const std::uint32_t local = tries_[tryIndex].arms[armIndex].exnLocal;
	emitOpcode(Opcode::LocalGet);
	emitUnsignedLeb(local);
	emitOpcode(Opcode::ThrowRef);
}

//! Re-emits a final-form `try_table`, renumbering every clause label
//! (docs/proposals/LEGACY_EXCEPTIONS.md LE-9). A clause's label is resolved in
//! the *enclosing* context — the `try_table`'s own label opens only after its
//! clauses — so the remap happens before this frame is pushed, exactly as for a
//! `br`.
void BodyRewriter::emitTryTable(std::size_t index)
{
	const std::size_t begin = spans_[index].begin + 1;
	const std::size_t end = spans_[index].end;
	if (end <= begin)
		refuseRewrite("an empty try_table immediate");
	std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(code_.data() + begin),
			end - begin);
	Euph::Io::ConstBufferDevice device(bytes);
	FastMemReader stream(device);
	BlockType blockType;
	stream >> blockType;
	const std::size_t blockTypeEnd = begin + static_cast<std::size_t>(stream.device.tell());
	const std::uint32_t count = stream.readLEB128<std::uint32_t>();
	std::vector<CatchClause> clauses;
	clauses.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i) {
		CatchClause clause{};
		stream >> clause;
		clauses.push_back(clause);
	}
	if (stream.device.tell() != static_cast<std::ptrdiff_t>(end - begin))
		refuseRewrite("a try_table whose immediate this rewrite cannot measure");

	emitOpcode(Opcode::TryTable);
	emitBlockType(begin, blockTypeEnd);
	emitUnsignedLeb(count);
	for (const CatchClause& clause : clauses) {
		emitByte(static_cast<std::uint8_t>(clause.kind));
		if (clause.kind == CatchKind::Catch || clause.kind == CatchKind::CatchRef)
			emitUnsignedLeb(clause.tagIdx);
		emitUnsignedLeb(remapDepth(clause.labelIdx));
	}
	const int id = openFrame();
	binStack_.push_back(id);
	binToken_.push_back(index);
}

void BodyRewriter::emitOne(std::size_t index)
{
	switch (spans_[index].opcode) {
		case Opcode::Block:
		case Opcode::Loop:
		case Opcode::If: {
			copySpan(index);
			const int id = openFrame();
			binStack_.push_back(id);
			binToken_.push_back(index);
			break;
		}
		case Opcode::Else:
			copySpan(index);
			break;
		case Opcode::End: {
			copySpan(index);
			if (!binStack_.empty()) {
				binStack_.pop_back();
				binToken_.pop_back();
				if (emitStack_.empty())
					refuseRewrite("unbalanced end");
				emitStack_.pop_back();
			}
			break;
		}
		case Opcode::Br:
		case Opcode::BrIf:
		case Opcode::BrTable:
		case Opcode::BrOnNull:
		case Opcode::BrOnNonNull:
			emitCopiedBranch(index);
			break;
		case Opcode::Rethrow:
			emitRethrow(index);
			break;
		case Opcode::TryTable:
			emitTryTable(index);
			break;
		case Opcode::TryLegacy: {
			const std::size_t tryIndex = tryByToken_[index];
			if (tryIndex == kNoTry)
				refuseRewrite("a legacy try this rewrite did not record");
			emitLegacyTry(tries_[tryIndex]);
			break;
		}
		case Opcode::Catch:
		case Opcode::CatchAll:
		case Opcode::Delegate:
			// Consumed by the enclosing `emitLegacyTry`.
			refuseRewrite("an arm marker outside a legacy try");
		default:
			// A prefixed instruction with a label immediate needs the same
			// remap as `br`; the GC branch family is the only one that has one.
			if (spans_[index].opcode == Opcode::PrefixGC)
				emitCopiedBranch(index);
			else
				copySpan(index);
			break;
	}
}

void BodyRewriter::emitRange(std::size_t first, std::size_t last)
{
	for (std::size_t i = first; i < last; ++i) {
		const std::size_t tryIndex = tryByToken_[i];
		if (tryIndex != kNoTry) {
			emitLegacyTry(tries_[tryIndex]);
			i = tries_[tryIndex].terminator; // skip the arms and the terminator
			continue;
		}
		emitOne(i);
	}
}

//! Emits one legacy `try`: the merge block, the payload landing blocks, the
//! `try_table`, the body, then every arm in binary order.
void BodyRewriter::emitLegacyTry(const LegacyTry& legacyTry)
{
	// A: the try's own label, and the merge point every path joins.
	const int mergeFrame = openFrame();
	emitOpcode(Opcode::Block);
	emitBlockType(legacyTry.blockTypeBegin, legacyTry.blockTypeEnd);
	binStack_.push_back(mergeFrame);
	binToken_.push_back(legacyTry.tokenIndex);

	if (legacyTry.arms.empty()) {
		// A `delegate` (or a try that catches nothing): the body's exceptions
		// must reach the frame at `delegateDepth`. A plain block is correct
		// unless a legacy try with arms sits in between — then the target would
		// be skipped and the intervening arms would wrongly catch.
		const std::size_t depth = legacyTry.delegateDepth;
		if (depth >= binStack_.size())
			refuseRewrite("delegate depth out of range");
		for (std::size_t k = 1; k < depth; ++k) {
			const std::size_t token = binToken_[binStack_.size() - 1 - k];
			if (isCatchFrame(token))
				refuseRewrite("delegate that skips an intervening catch");
		}
		emitRange(legacyTry.bodyFirst, legacyTry.bodyLast);
		closeFrame();
		binStack_.pop_back();
		binToken_.pop_back();
		return;
	}

	// Payload landing blocks: the *last* arm's block is outermost, so the
	// first listed catch lands innermost and the arms leave in binary order.
	std::vector<int> payloadFrames(legacyTry.arms.size(), -1);
	for (std::size_t k = legacyTry.arms.size(); k-- > 0; ) {
		const LegacyArm& arm = legacyTry.arms[k];
		payloadFrames[k] = openFrame();
		emitOpcode(Opcode::Block);
		FuncType landing;
		if (!arm.isCatchAll)
			landing.results = tagPayload(arm.tagIdx).params;
		if (arm.needsExnRef)
			landing.results.push_back(StorageType{false, ValueType{ValueTypeCode::ExnRef, -1}});
		emitBlockTypeForResults(landing);
	}

	emitOpcode(Opcode::TryTable);
	emitBlockType(legacyTry.blockTypeBegin, legacyTry.blockTypeEnd);
	emitUnsignedLeb(legacyTry.arms.size());
	for (std::size_t k = 0; k < legacyTry.arms.size(); ++k) {
		const LegacyArm& arm = legacyTry.arms[k];
		std::uint8_t kind = arm.isCatchAll ? 0x02 : 0x00; // catch_all / catch
		if (arm.needsExnRef)
			kind = static_cast<std::uint8_t>(kind + 1);    // *_ref variants
		emitByte(kind);
		if (!arm.isCatchAll)
			emitUnsignedLeb(arm.tagIdx);
		emitUnsignedLeb(static_cast<std::uint32_t>(emitBranchTo(payloadFrames[k])));
	}
	openFrame(); // the try_table's own label, now that the clauses are placed

	emitRange(legacyTry.bodyFirst, legacyTry.bodyLast);
	closeFrame(); // try_table
	emitOpcode(Opcode::Br);
	emitUnsignedLeb(static_cast<std::uint32_t>(emitBranchTo(mergeFrame)));

	for (std::size_t k = 0; k < legacyTry.arms.size(); ++k) {
		closeFrame(); // payloadFrames[k]: the landing point of arm k
		const LegacyArm& arm = legacyTry.arms[k];
		if (arm.needsExnRef) {
			// The landing carries (payload…, exnref); keep the reference and
			// leave the payload where the legacy catch put it.
			emitOpcode(Opcode::LocalSet);
			emitUnsignedLeb(arm.exnLocal);
		}
		emitRange(arm.bodyFirst, arm.bodyLast);
		emitOpcode(Opcode::Br);
		emitUnsignedLeb(static_cast<std::uint32_t>(emitBranchTo(mergeFrame)));
	}
	closeFrame(); // mergeFrame
	binStack_.pop_back();
	binToken_.pop_back();
}

} // namespace

bool mayUseLegacyExceptionHandling(std::span<const std::uint8_t> code) noexcept
{
	for (const std::uint8_t byte : code) {
		switch (byte) {
		case 0x06: // try
		case 0x07: // catch
		case 0x09: // rethrow
		case 0x18: // delegate
		case 0x19: // catch_all
			return true;
		default:
			break;
		}
	}
	return false;
}

std::string legacyExceptionHandlingMessage(std::string_view construct)
{
	return std::string("legacy exception handling (the superseded Phase-3 proposal) is not "
			"supported: ") + std::string(construct) +
			". Fix it at the source with `-mllvm -wasm-use-legacy-eh=false` (clang/LLVM) or "
			"`-sWASM_LEGACY_EXCEPTIONS=0` (Emscripten), or translate the module once with "
			"`wasm-opt --enable-exception-handling --translate-to-exnref`; or set "
			"WASMJIT_LEGACY_EH=1 to load it through the built-in rewrite. See "
			"docs/proposals/LEGACY_EXCEPTIONS.md.";
}

std::size_t rewriteLegacyExceptionHandling(Module& module)
{
	std::size_t rewritten = 0;
	for (std::size_t bodyIndex = 0; bodyIndex < module.functionBodies.size(); ++bodyIndex) {
		FunctionBody& body = module.functionBodies[bodyIndex];
		InstructionScanner scanner;
		scanner.scan(body.code);
		bool usesLegacy = false;
		for (const InstructionSpan& span : scanner.spans) {
			if (isLegacyOpcode(span.opcode)) {
				usesLegacy = true;
				break;
			}
		}
		if (!usesLegacy)
			continue;
		std::vector<std::uint8_t> rewrittenCode;
		{
			const std::uint32_t paramCount = static_cast<std::uint32_t>(
					module.funcTypeAt(module.internalFunctionTypeIndices.at(bodyIndex)).params.size());
			BodyRewriter rewriter(module, body, scanner.spans, paramCount);
			rewrittenCode = rewriter.run();
		}
		body.code = std::move(rewrittenCode);
		// Branch-hint offsets describe the old byte stream
		// (docs/proposals/LEGACY_EXCEPTIONS.md LE5).
		body.branchHints.clear();
		++rewritten;
	}
	return rewritten;
}

} // namespace WASM
