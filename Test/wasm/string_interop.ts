// AssemblyScript string interop stress test.
//
// An AS `string` is a UTF-16LE managed object; a native import sees bytes in
// linear memory. These exports exercise both encodings in both directions:
// AS-encoded bytes handed to C++, and a C++-written buffer turned back into an
// AS string (and then observed again).

@external("env", "host_observe_utf8")
declare function hostObserveUtf8(ptr: usize, len: i32): i32;

@external("env", "host_observe_utf16")
declare function hostObserveUtf16(ptr: usize, len: i32): i32;

@external("env", "host_fill_greeting")
declare function hostFillGreeting(ptr: usize, capacity: i32): i32;

// AS -> C++ using UTF-8.
export function observe(s: string): i32 {
  const bytes = String.UTF8.encode(s);
  return hostObserveUtf8(changetype<usize>(bytes), bytes.byteLength);
}

// AS -> C++ using the UTF-16 code units the AS string actually stores. The host
// reports the code-unit count.
export function observeUtf16(s: string): i32 {
  const units = String.UTF16.encode(s);
  return hostObserveUtf16(changetype<usize>(units), units.byteLength);
}

// C++ -> AS: the host writes UTF-8 into an AS-allocated buffer and returns the
// byte count; the AS side decodes those bytes into a new managed string.
export function greeting(): string {
  const buffer = new ArrayBuffer(32);
  const written = hostFillGreeting(changetype<usize>(buffer), buffer.byteLength);
  return String.UTF8.decodeUnsafe(changetype<usize>(buffer), written);
}

// A full round-trip the host can grade without AS's runtime helpers: C++ writes
// the bytes, AS decodes them, then the same native import observes the
// re-encoded string.
export function greetingObserved(): i32 {
  return observe(greeting());
}

export function greetingLength(): i32 {
  return greeting().length;
}

// The host cannot construct an AS managed string, so the module supplies the
// literal it wants the native import to observe. These wrappers are the
// host-callable entry points for the two encodings.
export function observeLiteral(): i32 {
  return observe("h\u00e9llo");
}

export function observeUtf16Literal(): i32 {
  return observeUtf16("h\u00e9llo");
}
