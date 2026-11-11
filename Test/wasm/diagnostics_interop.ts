// The host diagnostics module's AssemblyScript shape: `log_as(kind, message)`
// receives the AS `String` object pointer (`usize`, i.e. i32 on wasm32) and the
// host reads the managed object's UTF-16LE payload (docs/DIAGNOSTICS.md §5.4).

@external("env", "log_as")
declare function logAs(kind: i32, message: string): void;

export function logInfoLiteral(): void {
  logAs(0, "h\u00e9llo from AS");
}

export function logWarnLiteral(): void {
  logAs(1, "warn from AS");
}

export function logPanicLiteral(): void {
  logAs(3, "panic from AS");
}
