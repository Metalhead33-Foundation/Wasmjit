export function writeAndSum(index: i32, value: i32): i32 {
  let offset = index << 2;
  store<i32>(offset, value);
  return load<i32>(offset) + value;
}

export function readValue(index: i32): i32 {
  return load<i32>(index << 2);
}

export function fillAndSum(startIndex: i32, count: i32): i32 {
  let sum: i32 = 0;
  for (let i: i32 = 0; i < count; i++) {
    let value = i + 1;
    store<i32>((startIndex + i) << 2, value);
    sum += load<i32>((startIndex + i) << 2);
  }
  return sum;
}
