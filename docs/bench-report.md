# LuaTier vs Lua 5.3.6 Performance Report

Generated: **2026-08-03 21:43:19 +08:00**

## Environment

| Item | Value |
|------|-------|
| Host | Microsoft Windows NT 10.0.26200.0 |
| CPU | 13th Gen Intel(R) Core(TM) i5-13600KF |
| Reference | `D:\workspace\zlua\SrcRepo\lua-5.3.6\lua53.exe` |
| Reference version | Lua 5.3.6  Copyright (C) 1994-2020 Lua.org, PUC-Rio |
| LuaTier | `D:\workspace\zlua\luajit3\build\Release\luatier.exe` |
| LuaTier version | LuaTier 0.1.0 (Lua 5.3.4) |
| Build | Release `luatier.exe` |
| Methodology | in-script `os.clock()`; 1 warmup + 3 timed runs; report **best** |
| Suite | `bench/micro` (13 microbenchmarks) |

## Summary

- Geometric mean of (LuaTier / Lua 5.3.6) best-time ratios: **8.62x**
- Ratio > 1 means LuaTier is slower; < 1 means faster.
- Counts: faster=0, slower=13, tied=0; result mismatches=0
- `Match` compares the `ok ...` payload with the `time=` field stripped.

### Hotspots (largest slowdowns)

| Area | Bench | Ratio | Likely pressure |
|------|-------|------:|-----------------|
| Hash table + string keys | `table_hash.lua` | 51x | string intern / hash lookup / rehash |
| Alloc + GC | `binary_trees.lua` | 43x | allocation + incremental GC |
| Metamethods | `metatable.lua` | 17x | metamethod dispatch |
| Integer tight loop | `loop_arith.lua` | 11x | interpreter dispatch |
| Closest to PUC | `pattern.lua` | 1.8x | mostly C-side pattern engine |

## Results

| Benchmark | Lua 5.3.6 best (s) | LuaTier best (s) | Ratio (LT/PUC) | Match |
|-----------|-------------------:|-----------------:|---------------:|:-----:|
| `loop_arith.lua` | 0.604 | 6.920 | 11.46x | yes |
| `loop_float.lua` | 0.323 | 3.148 | 9.75x | yes |
| `table_array.lua` | 0.038 | 0.400 | 10.52x | yes |
| `table_hash.lua` | 0.382 | 19.486 | 51.01x | yes |
| `funcall.lua` | 0.298 | 2.625 | 8.81x | yes |
| `closure.lua` | 0.083 | 0.402 | 4.84x | yes |
| `string_ops.lua` | 0.029 | 0.177 | 6.12x | yes |
| `pattern.lua` | 0.165 | 0.294 | 1.78x | yes |
| `coroutine_switch.lua` | 0.361 | 0.838 | 2.32x | yes |
| `binary_trees.lua` | 1.223 | 52.589 | 43x | yes |
| `fib_rec.lua` | 0.490 | 5.213 | 10.64x | yes |
| `table_sort.lua` | 0.049 | 0.144 | 2.94x | yes |
| `metatable.lua` | 0.240 | 4.012 | 16.72x | yes |

## Per-benchmark detail

### `loop_arith.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.604000 | 0.612333 | `ok 4999202955077760` |
| LuaTier | 6.920056 | 7.090208 | `ok 4999202955077760` |

### `loop_float.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.323000 | 0.326000 | `ok 34816890.23` |
| LuaTier | 3.148024 | 3.153610 | `ok 34816890.23` |

### `table_array.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.038000 | 0.039667 | `ok 4000002000000` |
| LuaTier | 0.399640 | 0.402093 | `ok 4000002000000` |

### `table_hash.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.382000 | 0.386667 | `ok 80000200000` |
| LuaTier | 19.486102 | 20.073991 | `ok 80000200000` |

### `funcall.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.298000 | 0.299667 | `ok 200000010000000` |
| LuaTier | 2.624543 | 2.958130 | `ok 200000010000000` |

### `closure.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.083000 | 0.088000 | `ok 500001500000` |
| LuaTier | 0.401959 | 0.432797 | `ok 500001500000` |

### `string_ops.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.029000 | 0.030000 | `ok 50 120` |
| LuaTier | 0.177461 | 0.178418 | `ok 50 120` |

### `pattern.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.165000 | 0.280000 | `ok 11200000` |
| LuaTier | 0.294056 | 0.295098 | `ok 11200000` |

### `coroutine_switch.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.361000 | 0.366000 | `ok 90001200000` |
| LuaTier | 0.837968 | 0.853529 | `ok 90001200000` |

### `binary_trees.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 1.223000 | 2.310333 | `ok 65535 15` |
| LuaTier | 52.588833 | 57.283495 | `ok 65535 15` |

### `fib_rec.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.490000 | 0.491333 | `ok 9227465` |
| LuaTier | 5.213062 | 5.243439 | `ok 9227465` |

### `table_sort.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.049000 | 0.049667 | `ok 2937 2147447397` |
| LuaTier | 0.144127 | 0.147910 | `ok 2937 2147447397` |

### `metatable.lua`

| Runtime | Best (s) | Avg (s) | Output payload |
|---------|---------:|--------:|----------------|
| Lua 5.3.6 | 0.240000 | 0.246000 | `ok 1000002000000` |
| LuaTier | 4.012373 | 4.043234 | `ok 1000002000000` |

## Notes

- LuaTier is currently interpreter-only (no AOT/JIT tier active in these runs).
- Prefer a **Release** `luatier.exe`; Debug builds are not comparable.
- Primary metric excludes process startup; wall-clock still includes parse/load.
- Re-run: `.\scripts\run_bench.ps1`

