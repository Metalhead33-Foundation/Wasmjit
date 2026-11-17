#ifndef WASMJIT_ASMJIT_EMITTER_HPP
#define WASMJIT_ASMJIT_EMITTER_HPP

#include <asmjit/ujit.h>

#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "../WasmBase/WasmByteOrder.hpp"

/*!
  \file AsmjitEmitter.hpp
  \brief The one place this backend spells a generated instruction.

  The backend emits through **AsmJit's Universal JIT** (`asmjit::ujit`) rather
  than through `asmjit::x86` directly (docs/proposals/BIG_ENDIAN.md `BE-7`).
  `asmjit::ujit::UniCompiler` wraps the same `asmjit::host::Compiler` this backend
  already used — `asmjit::ujit::Gp`, `asmjit::ujit::Vec` and `asmjit::ujit::Mem` *are* the host's
  register and memory types — so only the *instruction* vocabulary changes:
  `uc.add(dst, a, b)` instead of `cc.add(dst, b)`, `uc.j(label, condition)`
  instead of `cc.test`/`cc.jz`.

  Two rules keep the migration honest:

  - A site that has a target-independent instruction calls this façade and
    nothing else.
  - A site that does not yet have one emits through host() and is listed in
    `Asmjit.md` \"The host-specific set\", with the reason. The list only ever
    shrinks; when it is empty the backend's emitted code is target-independent
    and a big-endian host can be served (`BE-7`'s payoff).

  Note on error reporting: this version of `ujit`'s emit entry points return
  `void`, so a rejected emission is reported by `BackendCompiler::finalize()`
  rather than at the call site. The backend keeps checking `finalize()`; the
  per-site message is coarser than it was, which `Asmjit.md` records.
*/

#if defined(ASMJIT_NO_UJIT)
#error "the WasmJit AsmJit backend emits through asmjit::ujit, but this AsmJit was built with ASMJIT_NO_UJIT (or without a compiler); rebuild AsmJit with its compiler enabled (docs/proposals/BIG_ENDIAN.md BE-7)"
#endif

#if !defined(ASMJIT_UJIT_X86) && !defined(ASMJIT_UJIT_AARCH64)
#error "asmjit::ujit reports no universal backend for this host architecture, so the WasmJit AsmJit backend cannot emit (docs/proposals/BIG_ENDIAN.md BE-7)"
#endif

#if ASMJIT_LIBRARY_VERSION < ASMJIT_LIBRARY_MAKE_VERSION(1, 21, 0)
#error "the WasmJit AsmJit backend requires AsmJit 1.21 or newer: asmjit::ujit and the API shape this backend uses landed there (Asmjit.md \"Build dependency\")"
#endif

namespace AsmJitBackend {

/*!
 * \brief The emission seam: universal instructions first, the host compiler
 *        second.
 *
 * One instance lives inside a function body's emitter, where both compilers are
 * bound: the host `BackendCompiler` that owns the function and the
 * `UniCompiler` that is hooked to it.
 */
class Emitter
{
public:
	/*!
	 * \brief Binds the seam to one hooked function.
	 * \param backend Host compiler that owns the current function.
	 * \param universal Universal compiler hooked to \p backend.
	 * \param swapsGuestAccesses True when this module's multi-byte guest
	 *        accesses must be byte-reversed, which the instance's byte-order
	 *        decision already resolved (docs/proposals/BIG_ENDIAN.md `BE-3`).
	 *        False emits exactly the code a little-endian build always emitted.
	 */
	Emitter(asmjit::ujit::BackendCompiler& backend, asmjit::ujit::UniCompiler& universal,
			bool swapsGuestAccesses = false) noexcept
		: backend_(backend)
		, universal_(universal)
		, swapsGuestAccesses_(swapsGuestAccesses)
	{
	}

	Emitter(const Emitter&) = delete;
	Emitter& operator=(const Emitter&) = delete;

	//! \name Universal instruction vocabulary
	//! Each slice of `BE-7` adds the operations it converts; see Asmjit.md.
	//! \{

	/*!
	 * \brief Materialises an integer immediate into a general-purpose register.
	 * \tparam Value Integral type of the immediate.
	 * \param dst Destination register; its own width decides what fits.
	 * \param value Immediate value.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void mov(const asmjit::ujit::Gp& dst, Value value)
	{
		universal_.mov(dst, asmjit::Imm(static_cast<std::int64_t>(value)));
	}

	/*!
	 * \brief Materialises an immediate operand into a register.
	 * \param dst Destination register.
	 * \param value Immediate operand.
	 */
	void mov(const asmjit::ujit::Gp& dst, const asmjit::Imm& value)
	{
		universal_.mov(dst, value);
	}

	/*!
	 * \brief Stores an integer immediate into memory.
	 *
	 * A zeroing store has its own universal instruction; any other immediate is
	 * materialised in a scratch register and then stored at the access width.
	 * \tparam Value Integral type of the immediate.
	 * \param dst Destination memory operand.
	 * \param value Immediate value.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void mov(const asmjit::ujit::Mem& dst, Value value)
	{
		mov(dst, asmjit::Imm(static_cast<std::int64_t>(value)));
	}

	/*!
	 * \brief Stores an immediate operand into memory.
	 *
	 * A zeroing store has its own universal instruction; any other immediate is
	 * materialised in a scratch register and then stored at the access width.
	 * \param dst Destination memory operand.
	 * \param value Immediate operand.
	 */
	void mov(const asmjit::ujit::Mem& dst, const asmjit::Imm& value)
	{
		const std::uint32_t width = accessWidth(dst.size(), sizeof(std::int64_t));
		if (value.value() == 0) {
			switch (width) {
			case 1: universal_.store_zero_u8(dst); return;
			case 2: universal_.store_zero_u16(dst); return;
			case 4: universal_.store_zero_u32(dst); return;
			default: universal_.store_zero_u64(dst); return;
			}
		}
		asmjit::ujit::Gp scratch = width == 8
				? backend_.new_gp64("immediateStore") : backend_.new_gp32("immediateStore");
		universal_.mov(scratch, value);
		mov(dst, scratch);
	}

	/*!
	 * \brief Materialises an immediate into a general-purpose register.
	 * \param dst Destination register; its own width decides what fits.
	 * \param value Immediate value.
	 */
	void movImm(const asmjit::ujit::Gp& dst, std::int64_t value)
	{
		universal_.mov(dst, asmjit::Imm(value));
	}

	/*!
	 * \brief Copies one register into another, of any width.
	 *
	 * `ujit`'s register move is **same-width only** — `mov eax, rax` is rejected,
	 * as is `mov rax, eax` — and there is no register extension operation at all
	 * (`uniop.h` has `kLoadU32`/`kLoadI32` but nothing for a register source). The
	 * universal way to change a register's width is therefore through memory: the
	 * store truncates to the narrower of the two widths and the load zero-extends
	 * back out, which is exactly what x86's `mov eax, rax`/`mov rax, eax` idiom
	 * meant. One store-forwarded pair, in one place, so no call site has to know.
	 * \param dst Destination register.
	 * \param src Source register.
	 */
	void mov(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		if (dst.size() == src.size()) {
			universal_.mov(dst, src);
			return;
		}
		const std::uint32_t width = dst.size() < src.size() ? dst.size() : src.size();
		auto slot = backend_.new_stack(width, width, "moveSlot");
		switch (width) {
		case 1:
			universal_.store_u8(slot, src);
			universal_.load_u8(dst, slot);
			return;
		case 2:
			universal_.store_u16(slot, src);
			universal_.load_u16(dst, slot);
			return;
		case 4:
			universal_.store_u32(slot, src);
			universal_.load_u32(dst, slot);
			return;
		default:
			universal_.store_u64(slot, src);
			universal_.load_u64(dst, slot);
			return;
		}
	}

	/*!
	 * \brief Loads a register from memory, zero-extending a narrow access.
	 *
	 * The access width is the memory operand's own size, and the destination's
	 * size when the operand is unsized — exactly asmjit's own inference. A
	 * 4-byte load into a 64-bit register zero-extends, which is what the host's
	 * `mov r32, m32` does.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void mov(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		switch (accessWidth(src.size(), dst.size())) {
		case 1: universal_.load_u8(dst, src); return;
		case 2: universal_.load_u16(dst, src); return;
		case 4: universal_.load_u32(dst, src); return;
		default: universal_.load_u64(dst, src); return;
		}
	}

	/*!
	 * \brief Stores a register into memory, truncating to the access width.
	 *
	 * The access width is the memory operand's own size, and the source's size
	 * when the operand is unsized.
	 * \param dst Destination memory operand.
	 * \param src Source register.
	 */
	void mov(const asmjit::ujit::Mem& dst, const asmjit::ujit::Gp& src)
	{
		switch (accessWidth(dst.size(), src.size())) {
		case 1: universal_.store_u8(dst, src); return;
		case 2: universal_.store_u16(dst, src); return;
		case 4: universal_.store_u32(dst, src); return;
		default: universal_.store_u64(dst, src); return;
		}
	}

	/*!
	 * \brief Adds a register to another, in place.
	 * \param dst Augend and destination.
	 * \param src Addend.
	 */
	void add(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		universal_.add(dst, dst, src);
	}

	/*!
	 * \brief Adds an immediate to a register, in place.
	 * \param dst Augend and destination.
	 * \param src Addend.
	 */
	void add(const asmjit::ujit::Gp& dst, const asmjit::Imm& src)
	{
		universal_.add(dst, dst, src);
	}

	/*!
	 * \brief Subtracts a register from another, in place.
	 * \param dst Minuend and destination.
	 * \param src Subtrahend.
	 */
	void sub(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		universal_.sub(dst, dst, src);
	}

	/*!
	 * \brief Subtracts an immediate from a register, in place.
	 * \param dst Minuend and destination.
	 * \param src Subtrahend.
	 */
	void sub(const asmjit::ujit::Gp& dst, const asmjit::Imm& src)
	{
		universal_.sub(dst, dst, src);
	}

	/*!
	 * \brief Bitwise-and of two registers, in place.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	void and_(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		universal_.and_(dst, dst, src);
	}

	/*!
	 * \brief Bitwise-and with an immediate, in place.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	void and_(const asmjit::ujit::Gp& dst, const asmjit::Imm& src)
	{
		universal_.and_(dst, dst, src);
	}

	/*!
	 * \brief Bitwise-or of two registers, in place.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	void or_(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		universal_.or_(dst, dst, src);
	}

	/*!
	 * \brief Bitwise-or with an immediate, in place.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	void or_(const asmjit::ujit::Gp& dst, const asmjit::Imm& src)
	{
		universal_.or_(dst, dst, src);
	}

	/*!
	 * \brief Bitwise-xor of two registers, in place.
	 *
	 * With both operands the same register this is the canonical zeroing idiom,
	 * which the universal layer keeps as one instruction per target.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	void xor_(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		universal_.xor_(dst, dst, src);
	}

	/*!
	 * \brief Bitwise-xor with an immediate, in place.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	void xor_(const asmjit::ujit::Gp& dst, const asmjit::Imm& src)
	{
		universal_.xor_(dst, dst, src);
	}

	/*!
	 * \brief Multiplies two registers, keeping the low half.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	void imul(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		universal_.mul(dst, dst, src);
	}

	/*!
	 * \brief Decrements a register by one.
	 * \param dst Register to decrement.
	 */
	void dec(const asmjit::ujit::Gp& dst)
	{
		universal_.sub(dst, dst, asmjit::Imm(1));
	}

	/*!
	 * \brief Shifts a register left, in place.
	 * \param dst Value and destination.
	 * \param count Shift count.
	 */
	void shl(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& count)
	{
		universal_.shl(dst, dst, count);
	}

	/*!
	 * \brief Shifts a register left into a distinct destination.
	 *
	 * The count is an ordinary register operand: no fixed register (CL) and no
	 * memory round-trip is needed, which is what the host lowering required.
	 * \param dst Destination.
	 * \param src Value.
	 * \param count Shift count.
	 */
	void shl(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src,
			const asmjit::ujit::Gp& count)
	{
		universal_.shl(dst, src, count);
	}

	/*!
	 * \brief Shifts a register right (logical) into a distinct destination.
	 * \param dst Destination.
	 * \param src Value.
	 * \param count Shift count.
	 */
	void shr(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src,
			const asmjit::ujit::Gp& count)
	{
		universal_.shr(dst, src, count);
	}

	/*!
	 * \brief Shifts a register right (arithmetic) into a distinct destination.
	 * \param dst Destination.
	 * \param src Value.
	 * \param count Shift count.
	 */
	void sar(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src,
			const asmjit::ujit::Gp& count)
	{
		universal_.sar(dst, src, count);
	}

	/*!
	 * \brief Rotates a register left into a distinct destination.
	 * \param dst Destination.
	 * \param src Value.
	 * \param count Rotate count.
	 */
	void rol(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src,
			const asmjit::ujit::Gp& count)
	{
		universal_.rol(dst, src, count);
	}

	/*!
	 * \brief Rotates a register right into a distinct destination.
	 * \param dst Destination.
	 * \param src Value.
	 * \param count Rotate count.
	 */
	void ror(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src,
			const asmjit::ujit::Gp& count)
	{
		universal_.ror(dst, src, count);
	}

	/*!
	 * \brief Shifts a register left by an immediate, in place.
	 * \param dst Value and destination.
	 * \param count Shift count.
	 */
	void shl(const asmjit::ujit::Gp& dst, const asmjit::Imm& count)
	{
		universal_.shl(dst, dst, count);
	}

	/*!
	 * \brief Shifts a register right (arithmetic) by an immediate, in place.
	 * \param dst Value and destination.
	 * \param count Shift count.
	 */
	void sar(const asmjit::ujit::Gp& dst, const asmjit::Imm& count)
	{
		universal_.sar(dst, dst, count);
	}

	/*!
	 * \brief Shifts a register right (logical) by an immediate, in place.
	 * \param dst Value and destination.
	 * \param count Shift count.
	 */
	void shr(const asmjit::ujit::Gp& dst, const asmjit::Imm& count)
	{
		universal_.shr(dst, dst, count);
	}

	/*!
	 * \brief Counts leading zero bits.
	 *
	 * The universal operation is the *bit count*, not the host's bit scan: it is
	 * defined for zero as the register width, so a call site must **not** keep
	 * the scan-era `^ 31` / `^ 63` adjustment around it.
	 * \param dst Destination register.
	 * \param src Value to scan.
	 */
	void clz(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		universal_.clz(dst, src);
	}

	/*!
	 * \brief Counts trailing zero bits, defined for zero like clz().
	 * \param dst Destination register.
	 * \param src Value to scan.
	 */
	void ctz(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		universal_.ctz(dst, src);
	}

	/*!
	 * \brief Sign-extends the low byte of a source register.
	 * \param dst Destination register.
	 * \param src Source register, read at byte width.
	 */
	void movsx8(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		// No register-to-register sign extension exists in the universal
		// vocabulary, and a universal shift needs both operands as wide, so the
		// value is copied to the destination's width and shifted up and back down.
		const std::uint32_t shift = static_cast<std::uint32_t>(dst.size() * 8 - 8);
		mov(dst, src);
		universal_.shl(dst, dst, asmjit::Imm(shift));
		universal_.sar(dst, dst, asmjit::Imm(shift));
	}

	/*!
	 * \brief Sign-extends the low word of a source register.
	 * \param dst Destination register.
	 * \param src Source register, read at word width.
	 */
	void movsx16(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		const std::uint32_t shift = static_cast<std::uint32_t>(dst.size() * 8 - 16);
		mov(dst, src);
		universal_.shl(dst, dst, asmjit::Imm(shift));
		universal_.sar(dst, dst, asmjit::Imm(shift));
	}

	/*!
	 * \brief Zero-extends the low byte of a source register.
	 * \param dst Destination register.
	 * \param src Source register, read at byte width.
	 */
	void movzx8(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		const std::uint32_t shift = static_cast<std::uint32_t>(dst.size() * 8 - 8);
		mov(dst, src);
		universal_.shl(dst, dst, asmjit::Imm(shift));
		universal_.shr(dst, dst, asmjit::Imm(shift));
	}

	/*!
	 * \brief Zero-extends the low word of a source register.
	 * \param dst Destination register.
	 * \param src Source register, read at word width.
	 */
	void movzx16(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		const std::uint32_t shift = static_cast<std::uint32_t>(dst.size() * 8 - 16);
		mov(dst, src);
		universal_.shl(dst, dst, asmjit::Imm(shift));
		universal_.shr(dst, dst, asmjit::Imm(shift));
	}

	/*!
	 * \brief Zero-extends a 32-bit register into a 64-bit destination.
	 *
	 * A zero-extending copy: `mov` covers every width combination (a same-width
	 * copy is one instruction, a width change goes through the slot), and the
	 * load side zero-extends.
	 * \param dst Destination register.
	 * \param src Source register.
	 */
	void movzx32(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		// A width change zero-extends through the slot; see mov().
		mov(dst, src);
	}

	/*!
	 * \brief Loads a byte from memory and sign-extends it.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void loadS8(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_i8(dst, src);
	}

	/*!
	 * \brief Sign-extends a 32-bit register into a 64-bit destination.
	 *
	 * The extension is a widening load from a slot; at 32 bits there is nothing
	 * to extend, so it is a plain move.
	 * \param dst Destination register.
	 * \param src Source register.
	 */
	void movsx32(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& src)
	{
		mov(dst, src);
		if (dst.size() == 4)
			return;
		// Sign-extend bit 31 of the copy with a shift pair at the destination's own
		// width. This is arithmetic rather than a sign-extending load because a
		// four-byte slot used by one store and one load is promoted by the register
		// allocator, and the promoted copy would drop the extension's sign.
		const std::uint32_t shift = static_cast<std::uint32_t>(dst.size() * 8 - 32);
		universal_.shl(dst, dst, asmjit::Imm(shift));
		universal_.sar(dst, dst, asmjit::Imm(shift));
	}

	/*!
	 * \brief Loads a doubleword from memory and sign-extends it.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void loadS32(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_i32(dst, src);
	}

	/*!
	 * \brief Loads a word from memory and sign-extends it.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void loadS16(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_i16(dst, src);
	}

	/*!
	 * \brief Loads a byte from memory and zero-extends it.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void loadZ8(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_u8(dst, src);
	}

	/*!
	 * \brief Loads a word from memory and zero-extends it.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void loadZ16(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_u16(dst, src);
	}

	/*!
	 * \brief Loads a doubleword from memory and zero-extends it.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void loadZ32(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_u32(dst, src);
	}

	//! \}

	//! \name Immediate forms
	//! The call sites pass plain integers (`reinterpret_cast`d offsets, constant
	//! masks, `u32`/`u64` variables) as often as they pass `asmjit::Imm`, so each
	//! in-place operation takes an integral value directly.
	//! \{

	/*!
	 * \brief Adds an integer immediate to a register, in place.
	 * \tparam Value Integral type of the immediate.
	 * \param dst Augend and destination.
	 * \param src Addend.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void add(const asmjit::ujit::Gp& dst, Value src)
	{
		universal_.add(dst, dst, asmjit::Imm(static_cast<std::int64_t>(src)));
	}

	/*!
	 * \brief Subtracts an integer immediate from a register, in place.
	 * \tparam Value Integral type of the immediate.
	 * \param dst Minuend and destination.
	 * \param src Subtrahend.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void sub(const asmjit::ujit::Gp& dst, Value src)
	{
		universal_.sub(dst, dst, asmjit::Imm(static_cast<std::int64_t>(src)));
	}

	/*!
	 * \brief Bitwise-and with an integer immediate, in place.
	 * \tparam Value Integral type of the immediate.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void and_(const asmjit::ujit::Gp& dst, Value src)
	{
		universal_.and_(dst, dst, asmjit::Imm(static_cast<std::int64_t>(src)));
	}

	/*!
	 * \brief Bitwise-or with an integer immediate, in place.
	 * \tparam Value Integral type of the immediate.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void or_(const asmjit::ujit::Gp& dst, Value src)
	{
		universal_.or_(dst, dst, asmjit::Imm(static_cast<std::int64_t>(src)));
	}

	/*!
	 * \brief Bitwise-xor with an integer immediate, in place.
	 * \tparam Value Integral type of the immediate.
	 * \param dst Destination and first operand.
	 * \param src Second operand.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void xor_(const asmjit::ujit::Gp& dst, Value src)
	{
		universal_.xor_(dst, dst, asmjit::Imm(static_cast<std::int64_t>(src)));
	}

	/*!
	 * \brief Shifts a register left by an integer immediate, in place.
	 * \tparam Value Integral type of the count.
	 * \param dst Value and destination.
	 * \param count Shift count.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void shl(const asmjit::ujit::Gp& dst, Value count)
	{
		universal_.shl(dst, dst, asmjit::Imm(static_cast<std::int64_t>(count)));
	}

	/*!
	 * \brief Shifts a register right (logical) by an integer immediate, in place.
	 * \tparam Value Integral type of the count.
	 * \param dst Value and destination.
	 * \param count Shift count.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void shr(const asmjit::ujit::Gp& dst, Value count)
	{
		universal_.shr(dst, dst, asmjit::Imm(static_cast<std::int64_t>(count)));
	}

	/*!
	 * \brief Shifts a register right (arithmetic) by an integer immediate, in place.
	 * \tparam Value Integral type of the count.
	 * \param dst Value and destination.
	 * \param count Shift count.
	 */
	template <typename Value,
			std::enable_if_t<std::is_integral<Value>::value, int> = 0>
	void sar(const asmjit::ujit::Gp& dst, Value count)
	{
		universal_.sar(dst, dst, asmjit::Imm(static_cast<std::int64_t>(count)));
	}

	//! \}

	//! \name Vector vocabulary
	//! The vector instructions are spelled by their universal operation, which
	//! is what keeps the lane width and signedness explicit at the call site:
	//! `vector3(UniOpVVV::kCmpLtI8, dst, a, b)` cannot be mistaken for the
	//! unsigned form, and there is no operand-swapping idiom to get wrong.
	//! \{

	/*!
	 * \brief Applies a two-operand universal vector operation.
	 * \param op The operation, naming its lane width and signedness.
	 * \param dst Destination register.
	 * \param src Source register.
	 */
	void vector2(asmjit::ujit::UniOpVV op, const asmjit::ujit::Vec& dst,
			const asmjit::ujit::Vec& src)
	{
		universal_.emit_2v(op, dst, src);
	}

	/*!
	 * \brief Applies a three-operand universal vector operation.
	 * \param op The operation, naming its lane width and signedness.
	 * \param dst Destination register.
	 * \param src1 First source register.
	 * \param src2 Second source register.
	 */
	void vector3(asmjit::ujit::UniOpVVV op, const asmjit::ujit::Vec& dst,
			const asmjit::ujit::Vec& src1, const asmjit::ujit::Vec& src2)
	{
		universal_.emit_3v(op, dst, src1, src2);
	}

	/*!
	 * \brief Loads 128 bits from memory without an alignment requirement.
	 * \param dst Destination vector register.
	 * \param src Source memory operand.
	 */
	void vectorLoad128(const asmjit::ujit::Vec& dst, const asmjit::ujit::Mem& src)
	{
		universal_.v_loadu128(dst, src);
	}

	/*!
	 * \brief Stores 128 bits to memory without an alignment requirement.
	 * \param dst Destination memory operand.
	 * \param src Source vector register.
	 */
	void vectorStore128(const asmjit::ujit::Mem& dst, const asmjit::ujit::Vec& src)
	{
		universal_.v_storeu128(dst, src);
	}

	/*!
	 * \brief Zeroes a vector register.
	 * \param dst Register to zero.
	 */
	void vectorZero(const asmjit::ujit::Vec& dst)
	{
		universal_.emit_3v(asmjit::ujit::UniOpVVV::kXorU32, dst, dst, dst);
	}

	/*!
	 * \brief Copies one vector register into another.
	 * \param dst Destination register.
	 * \param src Source register.
	 */
	void vectorMov(const asmjit::ujit::Vec& dst, const asmjit::ujit::Vec& src)
	{
		universal_.v_mov(dst, src);
	}

	/*!
	 * \brief Inverts every bit of a vector register.
	 * \param dst Register to invert in place.
	 */
	void vectorNot(const asmjit::ujit::Vec& dst)
	{
		universal_.v_not_u32(dst, dst);
	}

	/*!
	 * \brief Moves the low 32 bits of a vector register into a general register.
	 * \param dst Destination general-purpose register.
	 * \param src Source vector register.
	 */
	void vectorToGp32(const asmjit::ujit::Gp& dst, const asmjit::ujit::Vec& src)
	{
		universal_.s_mov_u32(dst, src);
	}

	/*!
	 * \brief Moves the low 64 bits of a vector register into a general register.
	 * \param dst Destination general-purpose register.
	 * \param src Source vector register.
	 */
	void vectorToGp64(const asmjit::ujit::Gp& dst, const asmjit::ujit::Vec& src)
	{
		universal_.s_mov_u64(dst, src);
	}

	/*!
	 * \brief Moves a general register into the low 32 bits of a vector register.
	 * \param dst Destination vector register.
	 * \param src Source general-purpose register.
	 */
	void gpToVector32(const asmjit::ujit::Vec& dst, const asmjit::ujit::Gp& src)
	{
		universal_.s_mov_u32(dst, src);
	}

	/*!
	 * \brief Moves a general register into the low 64 bits of a vector register.
	 * \param dst Destination vector register.
	 * \param src Source general-purpose register.
	 */
	void gpToVector64(const asmjit::ujit::Vec& dst, const asmjit::ujit::Gp& src)
	{
		universal_.s_mov_u64(dst, src);
	}

	//! \}

	//! \name Guest accesses
	//! Every read or write of guest-visible storage goes through here, because
	//! the byte reversal is decided **once, while the code is generated** — a
	//! little-endian build emits nothing extra and its code stays byte-identical
	//! to what it always was (docs/proposals/BIG_ENDIAN.md `BE-3`, `BED5`).
	//! One helper covers both directions, so a converted load and a converted
	//! store still round-trip.
	//! \{

	/*!
	 * \brief Loads one byte and zero-extends it; a single byte has no order.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void guestLoad8Z(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_u8(dst, src);
	}

	/*!
	 * \brief Loads one byte and sign-extends it; a single byte has no order.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void guestLoad8S(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_i8(dst, src);
	}

	/*!
	 * \brief Loads a halfword and zero-extends it, reversing it when required.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void guestLoad16Z(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_u16(dst, src);
		if (swapsGuestAccesses_)
			reverse16(dst);
	}

	/*!
	 * \brief Loads a halfword and sign-extends it.
	 *
	 * The reversal must happen on the raw halfword and *before* the extension,
	 * or a negative value would carry the wrong sign.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void guestLoad16S(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		if (!swapsGuestAccesses_) {
			universal_.load_i16(dst, src);
			return;
		}
		universal_.load_u16(dst, src);
		reverse16(dst);
		extendS16(dst);
	}

	/*!
	 * \brief Loads a doubleword and zero-extends it, reversing it when required.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void guestLoad32Z(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_u32(dst, src);
		if (swapsGuestAccesses_)
			reverse32(dst);
	}

	/*!
	 * \brief Loads a doubleword and sign-extends it.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void guestLoad32S(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		if (!swapsGuestAccesses_) {
			universal_.load_i32(dst, src);
			return;
		}
		universal_.load_u32(dst, src);
		reverse32(dst);
		extendS32(dst);
	}

	/*!
	 * \brief Loads a doubleword into a 64-bit register without extension.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void guestLoad64(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_u64(dst, src);
		if (swapsGuestAccesses_)
			reverse64(dst);
	}

	/*!
	 * \brief Stores one byte; a single byte has no order.
	 * \param dst Destination memory operand.
	 * \param src Source register.
	 */
	void guestStore8(const asmjit::ujit::Mem& dst, const asmjit::ujit::Gp& src)
	{
		universal_.store_u8(dst, src);
	}

	/*!
	 * \brief Stores a halfword, reversing a copy of the value when required.
	 *
	 * The source register is a live stack value, so the reversal happens on a
	 * scratch copy rather than in place.
	 * \param dst Destination memory operand.
	 * \param src Source register.
	 */
	void guestStore16(const asmjit::ujit::Mem& dst, const asmjit::ujit::Gp& src)
	{
		if (!swapsGuestAccesses_) {
			universal_.store_u16(dst, src);
			return;
		}
		asmjit::ujit::Gp value = backend_.new_gp32("guestStoreValue");
		// A narrowing move is legal and needs no view; the store truncates.
		universal_.mov(value, src);
		reverse16(value);
		universal_.store_u16(dst, value);
	}

	/*!
	 * \brief Stores a doubleword, reversing a copy of the value when required.
	 * \param dst Destination memory operand.
	 * \param src Source register.
	 */
	void guestStore32(const asmjit::ujit::Mem& dst, const asmjit::ujit::Gp& src)
	{
		if (!swapsGuestAccesses_) {
			universal_.store_u32(dst, src);
			return;
		}
		asmjit::ujit::Gp value = backend_.new_gp32("guestStoreValue");
		// A narrowing move is legal and needs no view; the store truncates.
		universal_.mov(value, src);
		reverse32(value);
		universal_.store_u32(dst, value);
	}

	/*!
	 * \brief Stores a quadword, reversing a copy of the value when required.
	 * \param dst Destination memory operand.
	 * \param src Source register.
	 */
	void guestStore64(const asmjit::ujit::Mem& dst, const asmjit::ujit::Gp& src)
	{
		if (!swapsGuestAccesses_) {
			universal_.store_u64(dst, src);
			return;
		}
		asmjit::ujit::Gp value = backend_.new_gp64("guestStoreValue");
		universal_.mov(value, src);
		reverse64(value);
		universal_.store_u64(dst, value);
	}

	/*!
	 * \brief Loads one GC header word.
	 *
	 * The GC object header is little-endian on **every** host
	 * (docs/proposals/BIG_ENDIAN.md `BED3`): it is our layout, not a quotation
	 * of the proposal. This is therefore the one access that converts by
	 * *target* rather than by policy, so it is decided at compile time and
	 * emits nothing at all on a little-endian build.
	 * \param dst Destination register.
	 * \param src Source memory operand.
	 */
	void headerLoad32(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		universal_.load_u32(dst, src);
		if constexpr (!WASM::hostMatchesGuestByteOrder())
			reverse32(dst);
	}

	//! \}

	//! \name Host-specific vocabulary
	//! Each entry is a *decision* that the universal layer has no spelling for,
	//! and `Asmjit.md` "The host-specific set" records the reason.
	//! \{

	/*!
	 * \brief Forms the address of a memory operand in a register.
	 *
	 * Host-specific on purpose: the operand may be frame-relative, and the
	 * compiler rewrites such an operand's displacement during frame
	 * finalisation, so extracting base and displacement here would read the
	 * pre-layout value. The universal layer has no address-formation op.
	 * \param dst Destination register.
	 * \param src Address operand.
	 */
	void lea(const asmjit::ujit::Gp& dst, const asmjit::ujit::Mem& src)
	{
		backend_.lea(dst, src);
	}

	//! \}

	//! \name Conditions
	//! A condition carries its own operation (`test`, `compare`, `add`, `shr`,
	//! bit test), so one object serves a branch, a `select` and a `cmov`.
	//! \{

	/*!
	 * \brief Jumps to \p target unconditionally.
	 * \param target Branch target.
	 */
	void jmp(const asmjit::Label& target)
	{
		universal_.j(target);
	}

	/*!
	 * \brief Branches to \p target when \p condition holds.
	 * \param target Branch target.
	 * \param condition Condition to test.
	 */
	void j(const asmjit::Label& target, const asmjit::ujit::UniCondition& condition)
	{
		universal_.j(target, condition);
	}

	/*!
	 * \brief Materialises one of two immediates from a condition.
	 * \param dst Destination register.
	 * \param whenTrue Value when the condition holds.
	 * \param whenFalse Value when it does not.
	 * \param condition Condition to test.
	 */
	void select(const asmjit::ujit::Gp& dst, const asmjit::Imm& whenTrue,
			const asmjit::Imm& whenFalse, const asmjit::ujit::UniCondition& condition)
	{
		universal_.select(dst, whenTrue, whenFalse, condition);
	}

	/*!
	 * \brief Materialises one of two registers from a condition.
	 * \param dst Destination register.
	 * \param whenTrue Register holding the value when the condition holds.
	 * \param whenFalse Register holding the value when it does not.
	 * \param condition Condition to test.
	 */
	void select(const asmjit::ujit::Gp& dst, const asmjit::ujit::Gp& whenTrue,
			const asmjit::ujit::Gp& whenFalse,
			const asmjit::ujit::UniCondition& condition)
	{
		universal_.select(dst, whenTrue, whenFalse, condition);
	}

	/*!
	 * \brief Materialises a 0/1 flag from a condition.
	 *
	 * The condition already carries the width of what it tested, so the
	 * destination does **not** have to match it: a 64-bit result over a 32-bit
	 * subject selects directly (`mov`+`test`+`cmov` at the destination's width),
	 * which is one condition, no branching, and no temp register. The subject is
	 * therefore not a parameter — the call site has already folded it into
	 * \p condition.
	 * \param dst Destination register, which receives 1 or 0.
	 * \param condition Condition to test.
	 */
	void selectFlag(const asmjit::ujit::Gp& dst,
			const asmjit::ujit::UniCondition& condition)
	{
		universal_.select(dst, asmjit::Imm(1), asmjit::Imm(0), condition);
	}

	//! \}

	/*!
	 * \brief The access width of one memory operand.
	 * \param memorySize Size declared by the operand, or zero when unsized.
	 * \param registerSize Size of the register on the other side.
	 * \return The width to emit, in bytes.
	 */
	static std::uint32_t accessWidth(std::uint32_t memorySize,
			std::uint32_t registerSize) noexcept
	{
		return memorySize != 0 ? memorySize : registerSize;
	}

	//! \name The escape hatch
	//! \{

	/*!
	 * \brief The host compiler, for an instruction this layer has no universal
	 *        spelling for yet.
	 *
	 * Every use must appear in `Asmjit.md` "The host-specific set", because the
	 * point of the layer is that the list converges on empty. Returning the
	 * compiler rather than exposing raw `emit` keeps the call sites readable and
	 * the grep honest.
	 * \return The host compiler bound to this seam.
	 */
	asmjit::ujit::BackendCompiler& host() noexcept { return backend_; }

	//! \}

	/*!
	 * \brief The universal compiler bound to this seam.
	 * \return The hooked `UniCompiler`.
	 */
	asmjit::ujit::UniCompiler& universal() noexcept { return universal_; }

private:
	//! \name Byte-order helpers
	//! The four reversals one guest access can need, and the two in-place
	//! extensions a reversed signed load finishes with. All of them work on the
	//! value already in a register, so the same helper serves a load and a store.
	//! \{

	/*!
	 * \brief Reverses the low halfword of a zero-extended value, in place.
	 *
	 * A halfword rotate is not expressible through this vocabulary — a 16-bit
	 * operand is promoted to its 32-bit register — so the reversal is a byte swap
	 * followed by a shift down. The swap runs at the register's own width and the
	 * shift drops the halfword it does not want: after `bswap` the reversed
	 * halfword sits in the top 16 bits, so the count is the register width minus
	 * 16.
	 * \param value Register holding the value.
	 */
	void reverse16(const asmjit::ujit::Gp& value)
	{
		universal_.bswap(value, value);
		universal_.shr(value, value,
				asmjit::Imm(static_cast<std::uint32_t>(value.size() * 8 - 16)));
	}

	/*!
	 * \brief Reverses the low doubleword of a value, in place.
	 *
	 * At 32 bits the byte swap is the reversal; at 64 bits it reverses eight
	 * bytes, so the result is shifted down to keep only the reversed doubleword.
	 * \param value Register holding the value.
	 */
	void reverse32(const asmjit::ujit::Gp& value)
	{
		universal_.bswap(value, value);
		if (value.size() == 8)
			universal_.shr(value, value, asmjit::Imm(32));
	}

	/*!
	 * \brief Reverses a quadword, in place.
	 * \param value Register holding the value.
	 */
	void reverse64(const asmjit::ujit::Gp& value)
	{
		universal_.bswap(value, value);
	}

	/*!
	 * \brief Sign-extends the low halfword of a value, in place.
	 * \param value Register holding the zero-extended value.
	 */
	void extendS16(const asmjit::ujit::Gp& value)
	{
		const std::uint32_t shift = static_cast<std::uint32_t>(value.size() * 8 - 16);
		universal_.shl(value, value, asmjit::Imm(shift));
		universal_.sar(value, value, asmjit::Imm(shift));
	}

	/*!
	 * \brief Sign-extends the low doubleword of a value, in place.
	 * \param value Register holding the zero-extended value.
	 */
	void extendS32(const asmjit::ujit::Gp& value)
	{
		const std::uint32_t shift = static_cast<std::uint32_t>(value.size() * 8 - 32);
		universal_.shl(value, value, asmjit::Imm(shift));
		universal_.sar(value, value, asmjit::Imm(shift));
	}

	//! \}

	asmjit::ujit::BackendCompiler& backend_; //!< Owns the function; also the escape hatch.
	asmjit::ujit::UniCompiler& universal_; //!< Emits the target-independent instructions.
	//! True when this module's multi-byte guest accesses convert (`BE-3`).
	bool swapsGuestAccesses_ = false;
};

} // namespace AsmJitBackend

#endif // WASMJIT_ASMJIT_EMITTER_HPP
