# LuaTier microbenchmarks

Compare LuaTier against a reference PUC-Rio Lua (default: Lua 5.3.6).

## Layout

| Script | Focus |
|--------|--------|
| `loop_arith.lua` | integer arithmetic / bitwise loop |
| `loop_float.lua` | floating-point loop |
| `table_array.lua` | dense array write/read |
| `table_hash.lua` | string-key hash table |
| `funcall.lua` | Lua→Lua calls |
| `closure.lua` | closure allocate + upvalue |
| `string_ops.lua` | concat / sub / length |
| `pattern.lua` | `string.find` / `gsub` |
| `coroutine_switch.lua` | create / resume / yield |
| `binary_trees.lua` | alloc + GC (binary trees) |
| `fib_rec.lua` | recursive calls |
| `table_sort.lua` | `table.sort` |
| `metatable.lua` | metamethods |

## Run

```powershell
# defaults: Lua 5.3.6 at ../SrcRepo/lua-5.3.6/lua53.exe, Release luatier
.\scripts\run_bench.ps1

# custom paths / repeats
.\scripts\run_bench.ps1 `
  -RefLua "D:\workspace\zlua\SrcRepo\lua-5.3.6\lua53.exe" `
  -Luatier ".\build\Release\luatier.exe" `
  -Runs 3 `
  -Report "docs\bench-report.md"
```

Environment overrides: `LUATIER_REF_LUA`, `LUATIER_BIN`.
