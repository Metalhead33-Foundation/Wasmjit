// AssemblyScript interop stress test: managed typed arrays, a host-owned handle
// cached in module state, and the failure paths that cross AS-generated code.

@external("env", "host_sum_i32")
declare function hostSumI32(ptr: usize, len: i32): i32;

@external("env", "host_fill_i32")
declare function hostFillI32(ptr: usize, len: i32): i32;

@external("env", "acquire_handle")
declare function acquireHandle(): usize;

@external("env", "read_handle")
declare function readHandle(handle: usize): i32;

@external("env", "release_handle")
declare function releaseHandle(handle: usize): void;

@external("env", "fail_import")
declare function failImport(): void;

@external("env", "throw_import")
declare function throwImport(): void;

// A host-owned handle cached in module state, like a WASI fd or a JS object
// index. The token is an opaque index the host maps back to its own object; the
// bytes behind it never enter AS memory.
let cached: usize = 0;

// AS -> C++: the data pointer and length of a managed Int32Array.
export function sumCreatedArray(count: i32): i32 {
  const values = new Int32Array(count);
  for (let i = 0; i < count; i++) values[i] = i + 1;
  return hostSumI32(values.dataStart, values.length);
}

// C++ -> AS: the host fills the AS-allocated array, which AS then sums.
export function filledSum(count: i32): i32 {
  const values = new Int32Array(count);
  hostFillI32(values.dataStart, values.length);
  let sum = 0;
  for (let i = 0; i < count; i++) sum += values[i];
  return sum;
}

// Acquire once, then read the cached token on later calls.
export function openAndRead(): i32 {
  if (cached == 0) cached = acquireHandle();
  return readHandle(cached);
}

export function releaseCached(): void {
  if (cached != 0) {
    releaseHandle(cached);
    cached = 0;
  }
}

// A null token must be rejected by the host import, not dereferenced.
export function readCached(): i32 {
  return readHandle(cached);
}

// A token the host has already released.
export function readReleased(): i32 {
  return readHandle(1);
}

// AS `abort()` compiles to `unreachable` on this target: a Wasm trap, not a
// Wasm exception.
export function abortNow(): void {
  abort("assemblyscript abort");
}

// A native trap and a C++ exception raised inside an import, crossing
// AS-generated frames.
export function callFailingImport(): void {
  failImport();
}

export function callThrowingImport(): void {
  throwImport();
}
