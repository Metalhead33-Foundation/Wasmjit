export function sumUpto(n: i32): i32 {
  let total: i32 = 0;
  for (let i: i32 = 1; i <= n; i++) {
    total += i;
  }
  return total;
}

export function factorial(n: i32): i32 {
  let result: i32 = 1;
  for (let i: i32 = 2; i <= n; i++) {
    result *= i;
  }
  return result;
}
