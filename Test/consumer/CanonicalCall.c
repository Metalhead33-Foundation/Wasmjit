/* GAP-7: a literal **C** caller for the compiled-function ABI.
 *
 * docs/ABI.md "The C ABI Contract" (D-ABI-1) claims that a compiled Wasm
 * function is an ordinary C function with C linkage, callable from a C
 * embedder that knows nothing about Wasm. Every other consumer in this tree is
 * C++, so this translation unit is the proof: it is compiled as C (no C++
 * header, no template, no alternative name mangling) and calls the entry pointer
 * with the canonical signature only.
 *
 * The signatures below are the documented ones:
 *   - a no-argument `i32` function: `int32_t fn(void* ctx)`;
 *   - `f64` by value:                `double  fn(void* ctx, double)`;
 *   - a two-result tuple:            `void    fn(void* ctx, int32_t, int32_t,
 *                                               void* results)`.
 * A `v128` is deliberately absent: its canonical spelling is two `uint64_t`
 * arguments and the trailing result buffer, and LibJIT's aggregate deviation
 * (D-L1) is exercised from C++ by the same consumer.
 *
 * The `void*` context is the opaque `WASM::VMContext*` every compiled function
 * takes as its first argument; it is not interpreted here.
 */

#include <stdint.h>

typedef int32_t (*wasmjit_i32_fn)(void* context);
typedef double (*wasmjit_f64_fn)(void* context, double value);
typedef void (*wasmjit_pair_fn)(void* context, int32_t first, int32_t second,
		void* results);

int32_t wasmjit_c_call_i32(void* entry, void* context)
{
	return ((wasmjit_i32_fn)entry)(context);
}

double wasmjit_c_call_f64(void* entry, void* context, double value)
{
	return ((wasmjit_f64_fn)entry)(context, value);
}

void wasmjit_c_call_pair(void* entry, void* context, int32_t first,
		int32_t second, void* results)
{
	((wasmjit_pair_fn)entry)(context, first, second, results);
}
