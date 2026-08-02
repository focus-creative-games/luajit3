# PUC-Rio Lua 5.3.4 Official Suite Status

**Suite:** `tests/lua-5.3.4-tests/`  
**Tarball sha256:** `b80771238271c72565e5a1183292ef31bd7166414cd0c43a8eb79845fa7f599f`  
**Hard gate:** `luajit3 -e "_U=true" all.lua` → `final OK`

| File | Basic `_U` | Notes |
|------|------------|-------|
| all.lua (harness) | WIP | reaches `db.lua`; PowerShell: `'-e' '_U=true' 'all.lua'` |
| main.lua | OK | under `_U` soft path |
| gc.lua | OK | weak tables, `__gc`, ephemerons, finalizer errors |
| db.lua | WIP | `getinfo`/`short_src`/`linedefined` in; namewhat/hooks/getlocal next |
| calls.lua | WIP | not reached yet |
| strings.lua | WIP | via olddofile |
| literals.lua | WIP | via olddofile |
| tpack.lua | WIP | string.pack/unpack |
| attrib.lua | WIP | package |
| locals.lua | WIP | |
| constructs.lua | WIP | |
| code.lua | WIP | dump strip |
| nextvar.lua | WIP | |
| pm.lua | WIP | patterns |
| utf8.lua | WIP | |
| api.lua | WIP | C API / userdata |
| events.lua | WIP | |
| vararg.lua | WIP | |
| closure.lua | WIP | |
| coroutine.lua | WIP | |
| goto.lua | WIP | |
| errors.lua | WIP | |
| math.lua | WIP | |
| sort.lua | WIP | |
| bitwise.lua | WIP | |
| verybig.lua | skip under `_soft` | |
| files.lua | WIP | io |
| big.lua | skip under `_soft` | |

**Complete suite (no `_U`):** not started; needs working `package.loadlib` + suite `libs/`.

## Recent fixes (2026-08-02)

### GC / tables
- Weak-key clear uses tombstones (keep `used`) so open-addressing probe chains stay intact.
- Atomic order: clear weak values → resurrect finalizables → clear weak values/keys again.
- Mark stack: strict `top`-only for current C call (weak `__gc`); full Lua windows for suspended threads.
- `_ENV` name resolves to the chunk upvalue (not `_G["_ENV"]`).
- `__gc` errors clear `collecting_`; finalizers push above `thread_live_top`.

### Debug / load
- `debug.getinfo` with `S`/`L`/`f`/`l`/`u`, `linedefined`/`lastlinedefined`, `activelines`, `chunkid`.
- `load` default chunkname is the source string (Lua 5.3).
- Parser tracks function `lastline` for `lastlinedefined`.

### Tooling
- Vendor suite under `tests/lua-5.3.4-tests/`; `scripts/run_puc_tests.{ps1,sh}`.
- MSBuild may leave a stale `luajit3.exe`; delete and rebuild `luajit3_cli` if behavior looks old.

## Known issues

- `db.lua` still fails after short_src tests (`name`/`namewhat`, line hooks, getlocal, …).
- Complete suite / `loadlib` not started.

Update this table as files go green.
