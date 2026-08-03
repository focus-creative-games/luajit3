# PUC-Rio Lua 5.3.4 Official Suite Status

**Suite:** `tests/lua-5.3.4-tests/`  
**Tarball sha256:** `b80771238271c72565e5a1183292ef31bd7166414cd0c43a8eb79845fa7f599f`  
**Hard gate:** `luatier -e "_U=true" all.lua` → `final OK` — **PASSED** (2026-08-03)

| File | Basic `_U` | Notes |
|------|------------|-------|
| all.lua (harness) | OK | prints `final OK !!!` |
| main.lua | OK | under `_U` soft path |
| gc.lua | OK | weak tables, `__gc`, ephemerons |
| db.lua | OK | via dump/undump dofile |
| calls.lua | OK | |
| strings.lua | OK | via olddofile |
| literals.lua | OK | via olddofile |
| tpack.lua | OK | |
| attrib.lua | OK | |
| locals.lua | OK | |
| constructs.lua | OK | |
| code.lua | OK | |
| nextvar.lua | OK | |
| pm.lua | OK | |
| utf8.lua | OK | |
| api.lua | OK | |
| events.lua | OK | |
| vararg.lua | OK | |
| closure.lua | OK | |
| coroutine.lua | OK | |
| goto.lua | OK | |
| errors.lua | OK | |
| math.lua | OK | |
| sort.lua | OK | unpack too-many / in-place sort / move |
| bitwise.lua | OK | hex strings with `e` digit |
| verybig.lua | OK | early return under `_soft`; SETLIST flush |
| files.lua | OK | io.lines/read/write/date |
| big.lua | skip under `_soft` | |

**Unit tests:** `luatier_tests` green.

**Complete suite (no `_U`):** not started; needs working `package.loadlib` + suite `libs/`, and non-`_soft`/`_port` paths (big.lua, popen, …).

## Recent fixes (2026-08-03)

### Table / sort
- `table.unpack` rejects ranges with ≥ `INT_MAX` results (`too many results to unpack`).
- `table.move` overflow / wrap-around checks; PUC-style in-place quicksort with invalid-order detection.
- Table constructors flush `SETLIST` every 50 fields (LFIELDS_PER_FLUSH).

### Numbers / bitwise
- Hex integer parse: `e`/`E` are digits in `0x…` (not decimal exponents) — fixes `0xfffffffffffffffe`.

### IO / OS
- `io.close` / `io.type` / `io.lines` / `io.flush` / `io.tmpfile` / `file:setvbuf`.
- `io.write`/`io.read` use default files; multi-format read (`l`/`L`/`n`/`a`/count).
- `os.date`/`os.time` PUC-style (UTC `!`, `*t`, conversion-specifier validation).
- `os.tmpname` generates unique non-existent names.

### Errors / debug
- `arg_type_error` resolves `io.write` via globals; TAILCALL→C preserves invoked name (`'sin'`).

### Tooling
- MSBuild may leave a stale `luatier.exe`; touch `src/main.cpp` or `--clean-first` after lib changes.

Update this table as the complete (non-`_U`) suite progresses.
