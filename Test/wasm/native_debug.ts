@external("env", "debug_message")
declare function debugMessage(): void;

export function run(): void {
  debugMessage();
}
