# elf/ — The Dynamic Linker (ld.so)

This directory contains the core of glibc's ELF dynamic linking subsystem: the
runtime dynamic linker/loader (`ld.so`), supporting utilities, and an extensive
test suite.

## Execution Flow

### Program Startup

When you run a dynamically-linked program, execution flows through the
following path:

```
kernel loads ELF, reads .interp → executes ld.so
    │
    ▼
rtld.c: _dl_start()
    │   ld.so relocates its own symbols (bootstrap)
    │   before any other code can run
    │
    ▼
rtld.c: dl_main()
    │   process environment variables (LD_LIBRARY_PATH, LD_PRELOAD, etc.)
    │   parse GLIBC_TUNABLES
    │   set up secure mode if AT_SECURE=1
    │
    ▼
dl-load.c: _dl_map_object() / _dl_map_object_from_fd()
    │   map the executable's ELF segments into memory
    │   parse ELF headers, program headers
    │   validate the binary
    │
    ▼
dl-deps.c: _dl_map_object_deps()
    │   walk DT_NEEDED entries recursively
    │   load all shared library dependencies
    │   handle auxiliary and filter objects
    │
    ▼
dl-setup_hash.c
    │   prepare symbol hash tables for each loaded object
    │
    ▼
dl-reloc.c: _dl_relocate_object()
    │   process REL/RELA relocation records
    │   patch up GOT/PLT entries with resolved addresses
    │   allocate static TLS blocks
    │
    ▼
dl-init.c: _dl_init()
    │   call .init and .init_array constructors
    │   in dependency order (leaves first)
    │
    ▼
transfer control → program's entry point (_start → __libc_start_main → main)
```

### Lazy Symbol Resolution (Runtime)

When a program calls a function through the PLT for the first time:

```
application calls foo() → PLT stub
    │
    ▼
dl-trampoline (arch-specific assembly)
    │   saves registers, calls _dl_fixup()
    │
    ▼
dl-runtime.c: _dl_fixup()
    │
    ▼
dl-lookup.c: _dl_lookup_symbol_x()
    │   search hash tables across all loaded objects
    │   check symbol versioning (GLIBC_2.x)
    │   respect visibility (global, protected, hidden)
    │   walk search scopes
    │
    ▼
GOT entry updated with resolved address
    │
    ▼
jump to resolved function (subsequent calls go direct)
```

### dlopen() at Runtime

```
application calls dlopen("libfoo.so", flags)
    │
    ▼
dl-open.c: _dl_open()
    │
    ├─→ dl-load.c: map the new shared object
    ├─→ dl-deps.c: load its dependencies
    ├─→ dl-reloc.c: relocate everything
    ├─→ dl-scope.c: update search scopes
    ├─→ dl-tls.c: allocate TLS if needed
    └─→ dl-init.c: run constructors
    │
    ▼
return handle to application
    │
    ▼
application calls dlsym(handle, "symbol")
    │
    ▼
dl-sym.c → dl-lookup.c: resolve symbol → return address
```

### Program Exit

```
exit() or return from main()
    │
    ▼
dl-fini.c: _dl_fini()
    │   topologically sort loaded objects (dl-sort-maps.c)
    │   call .fini_array and .fini destructors
    │   in reverse dependency order (roots first)
    │
    ▼
dl-close.c (for dlopen'd objects)
    │   decrement reference counts
    │   unmap segments
    │   free TLS blocks
    │   clean up link_map structures
```

## Key Source Files

### Startup and Bootstrap

| File | Purpose |
|------|---------|
| `rtld.c` | Main entry point for ld.so; self-relocation bootstrap, environment variable processing |
| `rtld_static_init.c` | Static initialization for rtld |

### Loading and Mapping

| File | Purpose |
|------|---------|
| `dl-load.c` | Maps ELF segments into memory, parses headers, validates binaries |
| `dl-object.c` | Creates and initializes `link_map` structures for loaded objects |
| `dl-deps.c` | Processes `DT_NEEDED` entries, loads auxiliary/filter objects |
| `dl-map-segments.h` | Architecture-specific segment mapping |

### Symbol Resolution

| File | Purpose |
|------|---------|
| `dl-lookup.c` | Core symbol lookup engine with versioning and visibility |
| `dl-lookup-direct.c` | Optimized direct symbol lookup (common fast path) |
| `dl-sym.c` | `dlsym()` and `dlmopen()` public API |
| `dl-setup_hash.c` | Prepares hash tables for symbol lookup |

### Relocation

| File | Purpose |
|------|---------|
| `dl-reloc.c` | Processes REL/RELA relocation records, handles static TLS allocation |
| `dl-runtime.c` | Lazy PLT binding — called via trampoline on first function call |
| `dynamic-link.h` | Core inline relocation functions (architecture-dependent) |
| `do-rel.h` | Relocation processing templates |

### Runtime Dynamic Loading

| File | Purpose |
|------|---------|
| `dl-open.c` | `dlopen()` implementation |
| `dl-close.c` | `dlclose()` with dependency tracking and cleanup |

### Initialization and Finalization

| File | Purpose |
|------|---------|
| `dl-init.c` | Runs `.init`/`.init_array` constructors in dependency order |
| `dl-fini.c` | Runs destructors at program exit in reverse dependency order |
| `dl-call_fini.c` | Helper for calling finalizers |

### Thread-Local Storage (TLS)

| File | Purpose |
|------|---------|
| `dl-tls.c` | TLS block management and Dynamic Thread Vector (DTV) handling |
| `dl-tls_init_tp.c` | Thread pointer and TLS initialization |
| `dl-static-tls.h` | Static TLS allocation |

### Supporting Infrastructure

| File | Purpose |
|------|---------|
| `dl-version.c` | Symbol versioning (e.g., `GLIBC_2.34`) |
| `dl-scope.c` | Search scope management (what symbols each object can see) |
| `dl-audit.c` | `LD_AUDIT` hook interface for runtime introspection |
| `dl-hwcaps.c` | CPU feature detection for selecting optimized libraries |
| `dl-cache.c` | Reads `ld.so.cache` for fast library lookups |
| `dl-tunables.c` | `GLIBC_TUNABLES` runtime configuration system |
| `dl-sort-maps.c` | Topological sort of objects by dependency |
| `dl-find_object.c` | Efficient object lookup by address |
| `dl-origin.c` | `$ORIGIN` and `$LIB` path expansion |
| `dl-minimal-malloc.c` | Minimal allocator for early startup (before libc is usable) |
| `dl-printf.c` | Minimal printf for early error messages |
| `dl-environ.c` | Environment variable iteration |
| `dl-profile.c` | Shared library profiling support |
| `dl-mutex.c` | Locking primitives for thread-safe dynamic linking |
| `dl-secure.c` | Policy-based security enforcement: path rules, SHA-256 hash whitelisting, HMAC-SHA256 signature verification |
| `dl-secure.h` | Data structures and API for the secure loader module |
| `dl-sha256.h` | Self-contained SHA-256 implementation (header-only, no libc deps) |

### Standalone Utilities

| File | Purpose |
|------|---------|
| `ldconfig.c` | Builds `/etc/ld.so.cache` |
| `pldd.c` | Shows loaded objects for a running process |
| `sprof.c` | Shared library profiler |
| `sln.c` | Statically-linked `ln` (for bootstrapping ld.so installation) |

## Environment Variables

All `LD_*` variables are processed in `rtld.c`. Every one of them is **disabled
for setuid/setgid programs** (see [Security](#security-setuidsetgid-programs)
below).

### Core Variables

| Variable | Purpose |
|----------|---------|
| `LD_LIBRARY_PATH` | Colon-separated extra search directories for shared libraries, checked before system paths |
| `LD_PRELOAD` | Space/colon-separated list of libraries to load before all others |
| `LD_AUDIT` | Colon-separated audit modules that hook into loading and binding events (the `la_*` interface) |
| `LD_BIND_NOW` | Force eager symbol resolution — disable lazy PLT binding |
| `LD_BIND_NOT` | Resolve symbols but don't update the GOT/PLT (for debugging) |
| `LD_TRACE_LOADED_OBJECTS` | Print dependencies and exit without running the program (this is how `ldd` works) |
| `LD_WARN` | Show warnings about unresolved symbols |
| `LD_VERBOSE` | Print version info about loaded libraries |
| `LD_DYNAMIC_WEAK` | Allow weak symbols to be overridden dynamically |
| `LD_ORIGIN_PATH` | Override `$ORIGIN` for RPATH/RUNPATH substitution |
| `LD_SHOW_AUXV` | Dump the kernel auxiliary vector and exit |
| `LD_HWCAP_MASK` | Bitmask filtering which hwcap subdirectories to search |

### Debugging Variables

| Variable | Purpose |
|----------|---------|
| `LD_DEBUG` | Enable debug output (see categories below) |
| `LD_DEBUG_OUTPUT` | Redirect debug output to `<filename>.<pid>` instead of stderr |

`LD_DEBUG` accepts comma-separated categories:

| Category | Output |
|----------|--------|
| `libs` | Library search paths |
| `reloc` | Relocation processing |
| `files` | Input file progress |
| `symbols` | Symbol table processing |
| `bindings` | Which symbol binds to which object |
| `versions` | Version dependency activity |
| `scopes` | Search scope information |
| `all` | All of the above |
| `statistics` | Relocation statistics |
| `unused` | Detect unused DSOs |
| `help` | Print this list and exit |

### Profiling Variables

| Variable | Purpose |
|----------|---------|
| `LD_PROFILE` | Name of a shared library to generate gmon-style profiling data for |
| `LD_PROFILE_OUTPUT` | Directory for profile output (default: `/var/tmp`) |

## GLIBC_TUNABLES

The tunables system (`dl-tunables.c`, `dl-tunables.list`) provides structured
runtime configuration. Format:

```
GLIBC_TUNABLES=namespace.key=value[:namespace.key=value ...]
```

### glibc.rtld (dynamic linker)

| Tunable | Default | Range | Purpose |
|---------|---------|-------|---------|
| `nns` | 4 | 1–16 | Maximum number of link-map namespaces |
| `optional_static_tls` | 512 | 0+ | Extra static TLS space reserved for dlopen'd objects |
| `dynamic_sort` | 2 | 1–2 | Dependency sort algorithm |

### glibc.malloc (allocator tuning)

Many of these have legacy `MALLOC_*` environment variable aliases.

| Tunable | Alias | Purpose |
|---------|-------|---------|
| `check` | `MALLOC_CHECK_` | Consistency checking level (0–3) |
| `top_pad` | `MALLOC_TOP_PAD_` | Extra padding when growing the heap (default 131072) |
| `perturb` | `MALLOC_PERTURB_` | Fill byte for allocated/freed memory (0–255) |
| `mmap_threshold` | `MALLOC_MMAP_THRESHOLD_` | Size above which allocations use mmap |
| `trim_threshold` | `MALLOC_TRIM_THRESHOLD_` | Threshold for releasing memory back to the OS |
| `mmap_max` | `MALLOC_MMAP_MAX_` | Maximum number of mmap'd regions |
| `arena_max` | `MALLOC_ARENA_MAX` | Maximum number of arenas |
| `arena_test` | `MALLOC_ARENA_TEST` | Number of arenas before testing for reuse |
| `tcache_max` | — | Maximum size of per-thread cached chunks |
| `tcache_count` | — | Maximum number of chunks in each tcache bin |
| `tcache_unsorted_limit` | — | Unsorted bin limit for tcache fill |
| `mxfast` | — | Maximum fast bin size |
| `hugetlb` | — | Huge page usage for malloc arenas |

### glibc.elision (transactional lock elision)

| Tunable | Default | Purpose |
|---------|---------|---------|
| `enable` | 0 | Enable TSX-based lock elision (0–1) |
| `skip_lock_busy` | 3 | Retries to skip on busy lock |
| `skip_lock_internal_abort` | 3 | Retries to skip on internal abort |
| `skip_lock_after_retries` | 3 | Retries before falling back |
| `tries` | 3 | Transaction retry attempts |
| `skip_trylock_internal_abort` | 3 | Retries to skip on trylock internal abort |

### glibc.mem

| Tunable | Purpose |
|---------|---------|
| `tagging` | ARM MTE memory tagging mode (0–255) |
| `decorate_maps` | Annotate memory maps with descriptive names (0–1) |

### glibc.gmon

| Tunable | Default | Purpose |
|---------|---------|---------|
| `minarcs` | 50 | Minimum number of profiling arcs |
| `maxarcs` | 1048576 | Maximum number of profiling arcs |

### glibc.cpu

| Tunable | Purpose |
|---------|---------|
| `hwcap_mask` | Bitmask for hardware capability directory filtering (alias: `LD_HWCAP_MASK`) |

## Security: Setuid/Setgid Programs

When the kernel sets `AT_SECURE=1` (for setuid/setgid binaries), the dynamic
linker activates secure mode (`__libc_enable_secure`). In this mode:

- **All `LD_*` variables, `GLIBC_TUNABLES`, and `MALLOC_*` aliases are stripped
  from the environment.** The full list lives in `sysdeps/generic/unsecvars.h`
  and also includes `TMPDIR`, `LOCPATH`, `HOSTALIASES`, `RES_OPTIONS`, and
  others.

- **`LD_PRELOAD` and `LD_AUDIT`** are partially honored: only simple filenames
  (no `/` path separators) are accepted, and the library must be found in
  trusted system directories.

- **`GLIBC_TUNABLES`** is completely ignored — `__tunables_init()` returns
  immediately.

The secure environment processing is handled by `process_envvars_secure()` in
`rtld.c`.

## Secure Loader (dl-secure)

The `dl-secure` module adds policy-based security enforcement to ld.so,
controlling which executables and shared libraries may be loaded.  When a
policy file exists at `/etc/ld.so.secure`, every ELF binary is evaluated
against the configured rules before loading proceeds.

Three enforcement modes are available:

| Mode      | Behavior                                |
|-----------|-----------------------------------------|
| `off`     | No enforcement (default when no config) |
| `audit`   | Log violations but allow loading        |
| `enforce` | Block violations (`EACCES`)             |

Verification mechanisms (evaluated in order):

1. **Path-based allow/deny rules** — glob patterns with `*` wildcards; deny
   rules always take precedence.
2. **SHA-256 hash whitelist** — hash computed over concatenated `PT_LOAD`
   segments (stable across `strip`).
3. **HMAC-SHA256 signature** — embedded in a `.note.dl-secure` ELF section,
   verified with a shared key from the policy file using constant-time
   comparison.

Integration points in ld.so:

| Function                  | Called from      | Purpose                              |
|---------------------------|------------------|--------------------------------------|
| `_dl_secure_init()`       | `dl_main()`      | Parse `/etc/ld.so.secure` at startup |
| `_dl_secure_check_file()` | `open_verify()`  | Gate every shared library load       |
| `_dl_secure_check_main()` | `dl_main()`      | Verify the main executable           |

See [README-dl-secure.md](README-dl-secure.md) for full configuration
reference, signing tool usage, deployment workflow, and testing instructions.

## Code Review Notes: `dl-load.c`

`dl-load.c` (~2376 lines) is the core library loading engine. It contains three
major entry points:

- **`_dl_map_object()`** (line 1964) — top-level "find and load this library"
- **`_dl_map_object_from_fd()`** (line 939) — given an open fd, map the ELF
  into memory
- **`open_verify()`** (line 1571) — open a file and verify it's a valid ELF

The library search order implemented by `_dl_map_object()` is:
RPATH → LD_LIBRARY_PATH → RUNPATH → ld.so.cache → default system paths.

### Potential Issues

#### 1. `alloca` with attacker-influenced sizes

Several `alloca` calls use sizes derived from ELF file contents with no bounds
check:

- **`open_path()` line 1824**: `alloca(max_dirnamelen + max_capstrlen + namelen)`
  — `namelen` comes from the library name in a `DT_NEEDED` string table entry.
  A crafted ELF with an extremely long `DT_NEEDED` string could blow the stack.

- **`_dl_map_object_from_fd()` line 1074** and **`open_verify()` line 1777**:
  `alloca(header->e_phnum * sizeof(ElfW(Phdr)))` — `e_phnum` is a `uint16_t`
  from the ELF header. Max value 65535 × 56 bytes = ~3.6MB, well beyond typical
  stack limits.

- **`is_trusted_path_normalize()` line 133**: `alloca(len + 2)` where `len`
  comes from an RPATH string in the ELF file.

#### 2. Pervasive `(void *) -1` sentinel pointer

Lines 610, 624, 664, 682, 802, 806, 822, 850, 856, 956, 2189 all use
`(void *) -1` (or a cast variant) to mean "no path / already checked /
disabled." There is no named constant — every site uses a raw cast. A
`#define` like `NO_SEARCH_PATH ((void *) -1)` would improve readability and
reduce the risk of a missed or mistyped comparison.

#### 3. Fragile path normalization in `is_trusted_path_normalize`

Lines 127–181 normalize a path and check if it falls under a trusted system
directory. The `..` handling walks backward to find `/` with a
`wnp > npath` guard that correctly prevents walking past the root, but this
is subtle and undocumented. The trust check itself is prefix-based (`memcmp`
with `system_dirs_len`), which is correct but worth noting: any path whose
normalized form starts with a trusted prefix is accepted.

#### 4. Error cleanup goto structure in `_dl_map_object_from_fd`

Lines 968–986 define `lose:` / `lose_errno:` labels with a cascade of five
conditionals on partially-initialized `l` fields:

```c
if (l != NULL && l->l_map_start != 0) _dl_unmap_segments(l);
if (l != NULL && l->l_origin != (char *) -1l) free((char *) l->l_origin);
if (l != NULL && !l->l_libname->dont_free) free(l->l_libname);
if (l != NULL && l->l_phdr_allocated) free((void *) l->l_phdr);
free(l);
free(realname);
```

If a new code path leaves a field partially initialized before jumping to
`lose`, it could miss a free or double-free. The comment at line 1388
acknowledges the fragility: "Failures before this point are handled locally
via lose. There are no more failures in this function until return."

#### 5. Silent OOM in `open_path`

Line 1918: when `malloc` fails for `*realname`, the fd is closed and -1 is
returned with no error signal. The caller interprets this as "file not found"
and keeps searching or eventually produces "cannot open shared object file"
rather than an OOM error. Memory exhaustion is silently swallowed.

#### 6. Stale comment on `add_name_to_object`

Lines 416–417: the comment says "Returns false if the object already had this
name" but the function signature is `static void`. The comment is stale from a
previous version that returned `bool`.

#### 7. Security-critical `$ORIGIN` SUID guard is hard to read

Lines 317–320 in `_dl_dst_substitute()`:

```c
if (__glibc_unlikely (__libc_enable_secure)
    && !(input == start + 1
         && (input[len] == '\0' || input[len] == '/')))
    repl = (const char *) -1;
```

For SUID programs, `$ORIGIN` is only allowed as the very first path element,
immediately followed by `/` or end-of-string. The double-negative condition
and `(const char *) -1` sentinel make this security-critical logic difficult
to audit.

#### 8. Redundant `elf_machine_matches_host` in `open_verify`

`elf_machine_matches_host(ehdr)` is checked at line 1714 (inside the invalid
header branch, as a non-fatal fallthrough) and again unconditionally at
line 1754. On the common success path the function is called twice.

#### 9. `__RTLD_SECURE` SUID bit check

Lines 1893–1913: in secure mode, preloaded libraries from trusted directories
must have the SUID bit set (`S_ISUID`). This means an administrator must
explicitly `chmod u+s` any library allowed to be preloaded into setuid
binaries. The comment explains *what* but not *why* this unusual requirement
exists.

#### 10. Unresolved XXX markers shipped in production

- Line 2139: `// XXX Correct to unconditionally default to namespace 0?`
- Line 2329: `#define add_path(p, sps, flags) add_path(p, sps, 0) /* XXX */`
  — the `flags` parameter is silently discarded by this macro.
- Line 2363: `/* XXX Here is where ld.so.cache gets checked, but we don't
  have a way to indicate that in the results for Dl_serinfo. */`

### Strengths

- **Search order**: `_dl_map_object()` implements the ELF search order
  (RPATH → LD_LIBRARY_PATH → RUNPATH → cache → defaults) cleanly, with each
  step clearly commented.
- **Deduplication**: Files are matched by device+inode (`r_file_id`), not just
  name, preventing the same library from being loaded twice via different paths
  (line 989–1002).
- **Concurrency safety**: `add_name_to_object()` uses `atomic_store_release`
  (line 459) with detailed concurrency notes explaining the synchronization
  with lock-free reads in `_dl_name_match_p`.
- **Debug infrastructure**: `DL_DEBUG_LIBS` / `DL_DEBUG_FILES` output
  throughout makes the code highly debuggable with `LD_DEBUG=libs,files`.
- **DST substitution**: `$ORIGIN`/`$PLATFORM`/`$LIB` handling is thorough with
  proper security restrictions for SUID binaries.

## Code Review Notes: `dl-lookup.c`

`dl-lookup.c` (892 lines) is the symbol resolution engine — the hottest code
path in the dynamic linker. Every function call through a PLT eventually lands
here. The file contains three main layers:

- **`_dl_lookup_symbol_x()`** (line 766) — public entry point, iterates scopes
- **`do_lookup_x()`** (line 340) — inner loop, searches one scope across all
  loaded objects using GNU hash bloom filter or SysV hash fallback
- **`check_match()`** (line 58) — validates a candidate symbol (type, version,
  visibility)

Plus **`do_lookup_unique()`** (line 210) for `STB_GNU_UNIQUE` symbols and
**`add_dependency()`** (line 526) for runtime dependency tracking.

A companion file, `dl-lookup-direct.c`, provides a simplified single-object
lookup variant that requires symbol versioning.

### Potential Issues

#### 1. Compiler barrier instead of memory barrier — acknowledged shortcut

Line 349–353 in `do_lookup_x()`:

```c
size_t n = scope->r_nlist;
/* ...A read barrier here might be to expensive.  */
__asm volatile ("" : "+r" (n), "+m" (scope->r_list));
struct link_map **list = scope->r_list;
```

This is a compiler barrier (prevents reordering by the compiler) but not a
hardware memory barrier. On x86 this is safe due to strong memory ordering,
but on weakly-ordered architectures (ARM, POWER), another thread resizing
`r_list` in `dl-open.c` could cause `r_nlist` and `r_list` to be observed
inconsistently. The `GSCOPE` mechanism provides some protection, but the
comment itself flags this as a known shortcut.

#### 2. Unbounded recursion in `_dl_lookup_symbol_x`

Lines 864–867: when `add_dependency()` returns -1 (the target object was
unloaded between finding the symbol and recording the dependency), the function
recursively calls itself to retry:

```c
return _dl_lookup_symbol_x (undef_name, undef_map, ref, ...);
```

If an object is repeatedly unloaded by another thread during the retry window,
this could recurse indefinitely. In practice `dl_load_lock` serialization
limits this, but there is no recursion depth guard.

#### 3. Unbounded `skip_map` scan

Lines 785–788:

```c
if (__glibc_unlikely (skip_map != NULL))
    while ((*scope)->r_list[i] != skip_map)
        ++i;
```

This scans `r_list` with no bounds check against `r_nlist`. If `skip_map` is
not in the current scope (a precondition violation by calling code), this walks
off the end of the array.

#### 4. Magic numbers in version table handling

In `check_match()`, lines 115–157:

- `0x7fff` — masks off the hidden bit to get the version index
- `0x8000` — the ELF "hidden version" bit
- `2` — first user-defined version index
- `3` — first non-base version index

These are all ELF versioning protocol values that appear as raw magic numbers.
Named constants (e.g., `VERSYM_HIDDEN`, `VERSYM_VERSION`) exist in some ELF
implementations but are not used here.

#### 5. Unique symbol hash table — high load factor

Line 262: `if (size * 3 <= tab->n_elements * 4)` — the table is expanded at
75% occupancy. For open addressing with double hashing, lookup performance
degrades significantly above ~70%. The initial size of 31 (line 299) is also
small; programs with many shared libraries defining unique symbols will trigger
frequent early rehashes.

#### 6. Unique symbol table — fatal abort on OOM

Lines 272–277: when the unique symbol table can't be expanded, the process is
unconditionally killed with `_dl_fatal_printf("out of memory\n")`. Compare
with `add_dependency()` (lines 692–707) which gracefully degrades by marking
the object as NODELETE. There is no equivalent fallback for unique symbol OOM.

#### 7. Subtle TOCTOU in `add_dependency` with `l_serial`

Lines 576, 647: the map's serial number is saved before acquiring
`dl_load_lock`, then checked after. This detects the ABA problem (a
`link_map` freed and a new one allocated at the same address). However:

- Between saving `serial` (line 576) and acquiring the lock (line 584/630),
  the map pointer could be freed and the memory reused. The comment at
  lines 588–589 acknowledges this and uses `atomic_forced_read(map)` as an
  optimizer fence.
- If memory is reused for a different `link_map` with the same `l_serial`
  (an `unsigned long long`), the check passes incorrectly. Collisions are
  extremely unlikely but not impossible.

#### 8. Protected symbol re-lookup uses stale `i`

Lines 838–842 in `_dl_lookup_symbol_x()`:

```c
for (scope = symbol_scope; *scope != NULL; i = 0, ++scope)
    if (do_lookup_x (..., *scope, i, ...))
        break;
```

The variable `i` is not reset to 0 before the first iteration. The `i = 0`
reset is in the loop increment expression, which does not execute before the
first call to `do_lookup_x`. If `skip_map` was used (lines 785–788), `i` may
be non-zero, causing the protected-symbol re-lookup to skip the first `i`
objects in the scope. This appears to be a bug — the first scope search would
start at the wrong index.

#### 9. Inconsistent `const` cast-away

Throughout the file, `map` is received as `const struct link_map *` (from the
scope list) but stored into `result->m` via explicit cast:

```c
result->m = (struct link_map *) map;
```

This appears at lines 334, 492, 500, 831, and 847. The `struct sym_val`
stores a non-const `map`, so the const qualifier is silently discarded. This
makes the const contract misleading to callers.

#### 10. `dl-lookup-direct.c` duplicates `check_match` logic

`dl-lookup-direct.c` defines its own `check_match()` with an identical
`ALLOWED_STT` bitmask and similar symbol-type filtering. If the allowed symbol
types ever change, both files must be updated in sync — there is no shared
definition.

#### 11. Typos and stale markers

- Line 127: "can got here" → "can get here"
- Line 197: `FLAGS>` → `FLAGS.` (malformed comment)
- Line 352: "to expensive" → "too expensive"
- Line 808: `/* XXX We cannot translate the message. */`

### Strengths

- **GNU hash bloom filter**: The bloom filter check (lines 409–441) efficiently
  rejects non-matching symbols before touching the hash chain. The two-bit
  check (`bitmask_word >> hashbit1 & bitmask_word >> hashbit2 & 1`) gives
  excellent cache behavior on the hot path.
- **SysV hash fallback**: Lines 446–463 cleanly fall back to `DT_HASH` when
  `DT_GNU_HASH` is absent, maintaining compatibility with older ELF objects.
- **Lazy old-hash computation**: `old_hash` starts as `0xffffffff` and is only
  computed if actually needed (line 447–448), avoiding the SysV hash entirely
  for the common case of GNU hash tables.
- **Weak symbol semantics**: The `LD_DYNAMIC_WEAK` flag (lines 487–495)
  correctly implements both traditional ("keep searching") and modern ("first
  match wins") weak-symbol behavior with a clean fallthrough.
- **`add_dependency` graceful degradation**: OOM during dependency tracking
  marks the target as NODELETE rather than crashing (lines 692–707) — a
  pragmatic choice that maintains correctness.
- **Concurrency documentation**: The `GSCOPE` / `dl_load_lock` interactions in
  `add_dependency()` (lines 578–628) are extensively commented, explaining the
  ABBA deadlock avoidance and the re-check strategy.
- **`STB_GNU_UNIQUE` correctness**: `do_lookup_unique()` properly maintains a
  per-namespace hash table under a lock, with NODELETE marking to prevent the
  defining object from being unloaded.

## Code Review Notes: `dl-reloc.c`

`dl-reloc.c` (405 lines) applies relocations to a loaded shared object and
manages static TLS allocation for dynamically loaded libraries. It bridges
loading (`dl-load.c`) and symbol resolution (`dl-lookup.c`). The actual
per-relocation work is delegated to architecture-specific code via
`dynamic-link.h` and `do-rel.h`.

Entry points:

- **`_dl_relocate_object()`** (line 204) — relocate an object's GOT, PLT, and
  data references
- **`_dl_try_allocate_static_tls()`** (line 51) — carve out static TLS space
  for a dlopen'd library
- **`_dl_allocate_static_tls()`** (line 133) — wrapper that signals error on
  failure
- **`_dl_protect_relro()`** (line 353) — apply RELRO protection after
  relocation

The heavy lifting is done by macros in `dynamic-link.h`:

- **`ELF_DYNAMIC_RELOCATE`** — orchestrates RELR, REL, and RELA processing
- **`_ELF_DYNAMIC_DO_RELOC`** — handles DT_REL/DT_JMPREL overlap cases
- **`ELF_DYNAMIC_DO_RELR`** — processes compact RELR relative relocations

These expand to `elf_dynamic_do_Rel` / `elf_dynamic_do_Rela` in `do-rel.h`
(included twice — once for REL, once for RELA — using `#define` renaming).

### Potential Issues

#### 1. `_dl_reloc_bad_type` signals error with stack-local buffer

Lines 374–404: `msgbuf` is stack-allocated, then passed to
`_dl_signal_error()` which `longjmp`s out of the frame. If the error handler
accesses the string after the unwind, the pointer is dangling. This works
because `_dl_signal_error` copies the string into a thread-local exception
buffer, but the pattern is fragile — any future change that defers string
usage would hit a use-after-free.

#### 2. `alloca` in text-relocation loop

Lines 267–295: each read-only PT_LOAD segment causes an `alloca`:

```c
for (ph = l->l_phdr; ph < &l->l_phdr[l->l_phnum]; ++ph)
    if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W) == 0)
        newp = (struct textrels *) alloca (sizeof (*newp));
```

`l_phnum` comes from the ELF header (`e_phnum`, `uint16_t`). While
`sizeof(struct textrels)` is small (~32 bytes), a malicious ELF with many
PT_LOAD segments (up to 65535) could allocate ~2MB on the stack. Normal
binaries have 1–2 read-only PT_LOAD segments.

#### 3. `resolve_map` single-entry lookup cache — stale scope risk

Lines 166–197: the per-link-map cache stores exactly one resolved symbol:

- The cache is not invalidated if the link map's scope changes (e.g., during
  a `dlopen` called from a constructor). A stale `l_lookup_cache.value` could
  point to the wrong object.
- The function mutates `*ref` on a cache hit (redirecting the caller's symbol
  pointer), which is a subtle side effect.
- The cache persists on the `link_map` struct and could theoretically be
  observed by later code paths, though relocation of a single object is not
  concurrent.

#### 4. Fatal abort for profiling/audit OOM

Lines 317–322: when `l_reloc_result` (the per-PLT-entry audit array) can't be
allocated, the process is killed with `_dl_fatal_printf`. This only affects
profiling/audit mode, not normal execution, but a graceful fallback (disabling
profiling for this object) would be more resilient.

#### 5. Text relocation security window

Lines 262–296, 330–344: when `DT_TEXTREL` is set, read-only segments are
made writable (`mprotect(..., PROT_WRITE)`) for relocation, then restored.
During this window, code segments are writable. In a multi-threaded process,
another thread could modify executable code. The comment at line 264
("Bletch.") acknowledges this is undesirable.

This is inherent to text relocations, not a code bug. Modern toolchains avoid
`DT_TEXTREL` entirely, but the code doesn't warn or log when text relocations
are encountered.

#### 6. `dynamic-link.h` — macro complexity and double inclusion

`dynamic-link.h` is included twice in `dl-reloc.c`:

- Line 30: before `RESOLVE_MAP` is defined — gets only forward declarations
- Line 202: after `RESOLVE_MAP` is defined — gets the full macro
  implementations

The `_ELF_DYNAMIC_DO_RELOC` macro is 47 lines that compute range splits
between DT_REL and DT_JMPREL sections. The comment at line 184 of
`dynamic-link.h` is candid: "This can't just be an inline function because
GCC is too dumb to inline functions containing inlines themselves."

#### 7. `do-rel.h` — textual code duplication via double inclusion

`do-rel.h` is included once for REL and once for RELA (after
`#define DO_RELA`), with all names `#define`'d to Rela variants. This:

- Produces two nearly identical copies of ~220 lines of relocation logic
- Has highly nested `#ifdef` chains (RTLD_BOOTSTRAP, ELF_MACHINE_IRELATIVE,
  DO_RELA, ELF_MACHINE_PLT_REL, SHARED) creating many compilation paths
- The IRELATIVE two-pass handling (`r2`/`end2` markers) is duplicated three
  times within the file (versioned, unversioned, and lazy paths)

#### 8. `_dl_try_allocate_static_tls` — locking is a caller precondition

Lines 51–128: the function modifies global state (`GL(dl_tls_static_used)`,
`GL(dl_tls_static_optional)`) without internal locking. The comment at
line 116 says "GL(dl_load_tls_lock) is held here" — this is a precondition
on the caller, not enforced by the function. If any caller fails to hold
the lock, static TLS globals could be corrupted.

#### 9. `_dl_protect_relro` — sub-page RELRO silently skipped

Lines 353–370: if `l_relro_size` is non-zero but the page-aligned `start`
equals `end`, the function silently does nothing. This occurs when the RELRO
region is smaller than a page. The protection doesn't apply, leaving the
region writable. Technically correct (nothing to protect at page granularity)
but could surprise security auditors.

#### 10. XXX marker and inconsistent branch hints

- Line 250: `// XXX Correct for auditing?` — unresolved question about
  `DT_BIND_NOW` interaction with audit mode.
- The file mixes `__builtin_expect(..., 0)` (lines 252, 331) with
  `__glibc_unlikely()` (lines 255, 262). These are functionally identical
  (`__glibc_unlikely` wraps `__builtin_expect`) but the inconsistency
  suggests different authors or eras.

### Strengths

- **RELR support**: `ELF_DYNAMIC_DO_RELR` in `dynamic-link.h` (lines 153–178)
  implements the compact relative relocation format cleanly. The bitmap
  encoding (odd = bitmap, even = address) is decoded in a tight, verifiable
  loop.
- **Static TLS surplus management**: `_dl_try_allocate_static_tls()` carefully
  partitions surplus into "optional" (for TLSDESC/powerpc optimizations) and
  "required" pools, with clean fallback. Both `TLS_TCB_AT_TP` and
  `TLS_DTV_AT_TP` layouts are handled with clear separation.
- **RELRO protection**: `_dl_protect_relro()` is simple and correct — align to
  page boundaries, `mprotect` to read-only. Applied immediately after
  relocation (line 349), minimizing the writable-GOT window.
- **Lookup cache**: The single-entry `resolve_map` cache (lines 166–197) is a
  pragmatic optimization for consecutive relocations referencing the same
  symbol. The statistics counter enables measuring effectiveness.
- **Clean delegation**: The file is only 405 lines because per-relocation work
  is delegated to architecture-specific `elf_machine_rel`/`elf_machine_rela`
  via the `RESOLVE_MAP` callback. Core logic stays architecture-independent.

## Code Review Notes: `rtld.c`

`rtld.c` (~2842 lines) is the **main entry point and orchestrator** of the
dynamic linker (`ld.so`) — the largest and most critical file in the ELF loader.
It handles:

- **Bootstrap** (`_dl_start`, line 516): Self-relocation of ld.so before the GOT
  is usable
- **Startup orchestration** (`dl_main`, line 1342, ~1065 lines): The entire
  dynamic linking pipeline — environment processing, executable loading,
  dependency resolution, TLS setup, relocation, audit module loading, and
  handoff to the application
- **Environment variable processing** (`process_envvars`, line 2747;
  `process_envvars_default`, line 2596; `process_envvars_secure`, line 2536):
  Parsing `LD_*` variables with security-aware filtering
- **Security initialization** (`security_init`, line 834): Stack canary and
  pointer guard setup
- **TLS initialization** (`init_tls`, line 733): Static TLS block and DTV setup
  for the initial thread
- **Audit module loading** (`load_audit_module`, line 930;
  `load_audit_modules`, line 1048): LD_AUDIT interface
- **Statistics and diagnostics** (`print_statistics`, line 2787;
  `process_dl_debug`, line 2432)

### Potential Issues

#### 1. Monolithic `dl_main()` — 1065 lines (lines 1342–2406)

`dl_main()` is a single function spanning over 1000 lines with deeply nested
control flow. It handles argument parsing, executable loading, dependency
resolution, preloading, relocation, TLS setup, audit initialization, and debug
output all in one function. This makes it extremely difficult to reason about,
test in isolation, or review for correctness. The function has multiple
`if (rtld_is_main)` branches that interleave two fundamentally different code
paths (invoked-as-program vs invoked-as-interpreter).

#### 2. `LD_BIND_NOW` logic is inverted/confusing (line 2665)

```c
if (memcmp (envline, "BIND_NOW", 8) == 0)
  {
    GLRO(dl_lazy) = envline[9] == '\0';
```

This sets `dl_lazy = 1` (enable lazy binding) when `LD_BIND_NOW` is *empty*
(`"LD_BIND_NOW="`), and `dl_lazy = 0` (disable lazy) when it has any value.
While technically correct (empty value means "don't bind now"), the
double-negative logic is a readability trap. Every other boolean env var in
this function uses `!= '\0'` to mean "enabled" — this one flips it because
the semantics are inverted (`BIND_NOW` disables `lazy`). A comment explaining
this would prevent future misreading.

#### 3. `init_tls()` NULL dereference after calloc (lines 749–755)

```c
GL(dl_tls_dtv_slotinfo_list) = (struct dtv_slotinfo_list *)
  calloc (sizeof (struct dtv_slotinfo_list)
      + nelem * sizeof (struct dtv_slotinfo), 1);
/* No need to check the return value.  If memory allocation failed
   the program would have been terminated.  */

struct dtv_slotinfo *slotinfo = GL(dl_tls_dtv_slotinfo_list)->slotinfo;
```

The comment claims calloc failure terminates the program, but this depends on
the malloc implementation in use at this point in startup. If using the minimal
bootstrap allocator, this may be true — but it's an undocumented, fragile
assumption. The very next `_dl_allocate_tls_storage()` call at line 787 *does*
check for NULL and calls `_dl_fatal_printf`, showing inconsistency.

#### 4. `audit_list_add_string()` — fatal abort on overflow (line 210)

```c
if (list->length == array_length (list->audit_strings))
  _dl_fatal_printf ("Fatal glibc error: Too many audit modules requested\n");
```

A user supplying too many colon-separated entries in `LD_AUDIT` causes an
immediate process termination. This is a denial-of-service vector (though mostly
self-inflicted). The fixed-size `audit_strings` array silently imposes a limit
that isn't documented in any user-facing documentation.

#### 5. Typos (lines 1252, 2281)

- Line 2281: `"reloaction"` → should be `"relocation"`
- Line 1252: `"PT_GNU_PROPERTY exits"` → should be `"exists"`

#### 6. `audit_list_next()` — redundant/misleading secure check (line 268)

```c
if (! __glibc_unlikely (__libc_enable_secure)
    && dso_name_valid_for_suid (list->fname))
  return list->fname;
```

This checks `__libc_enable_secure` twice: once explicitly here, and again
inside `dso_name_valid_for_suid()` (line 183). When `__libc_enable_secure` is
true, this line *always* skips the entry regardless of the name. But
`dso_name_valid_for_suid()` would have allowed names without slashes and under
the length limit. The outer check makes audit modules **entirely unavailable**
in secure mode via this path, which is the correct security decision — but the
double guard is confusing. The function's comment (line 148) says "Only modules
for which dso_name_valid_for_suid is true are returned" — yet that's not what
happens when `__libc_enable_secure` is set.

#### 7. `_dl_start_args_adjust()` — acknowledged aliasing violation (line 1323)

```c
GLRO(dl_auxv) = (ElfW(auxv_t) *) auxv; /* Aliasing violation.  */
```

The code casts `void **` to `ElfW(auxv_t) *` and the comment openly admits it's
an aliasing violation. While this works in practice (ld.so is compiled with
specific flags), it's technically undefined behavior and could break with
aggressive compiler optimizations.

#### 8. `strndupa` in ld.so.preload parsing (line 1927)

```c
char *p = strndupa (problem, file_size - (problem - file));
```

`strndupa` uses `alloca` internally. The size `file_size - (problem - file)` is
bounded by the file size of `/etc/ld.so.preload`, which is typically small.
However, a maliciously large preload file could blow the stack. Since only root
can modify `/etc/ld.so.preload`, this is low risk but worth noting.

#### 9. `print_statistics_item` switch fall-through without default (lines 2768–2779)

```c
switch (relative + sizeof (relative) - cp)
  {
  case 3:
    *wp++ = *cp++;
    /* Fall through.  */
  case 2:
    *wp++ = *cp++;
    /* Fall through.  */
  case 1:
    *wp++ = '.';
    *wp++ = *cp++;
  }
```

No `default` case. If the value is 0 or >3, the `relative` string is left
uninitialized/empty but `*wp = '\0'` at line 2780 saves it from being truly
undefined. Still, a value of 0 would produce a misleading "0.0%" output. More
importantly, this is a statistics-only code path, so the impact is cosmetic.

#### 10. Command-line parsing — inconsistent `strcmp` chain (lines 1399–1509)

A linear chain of 14 `strcmp` calls for argument parsing. While the number of
options is small enough that this isn't a performance concern, the inconsistent
use of `strcmp` vs `!strcmp` (some use `! strcmp(...)`, others use
`strcmp(...) == 0`) hurts readability. The indentation is also inconsistent
within the chain (e.g., line 1404 has a misaligned `state.mode` assignment).

#### 11. `process_envvars_secure()` — exits with undocumented magic code 5 (line 2592)

```c
if (GLRO(dl_debug_mask) != 0
    || GLRO(dl_verbose) != 0
    || GLRO(dl_lazy) != 1
    || GLRO(dl_bind_not) != 0
    || state->mode != rtld_mode_normal
    || state->version_info)
  _exit (5);
```

After unsetting dangerous environment variables, this checks if any restricted
option somehow got set and terminates with exit code 5. The exit code is
undocumented and not defined as a constant. This is a defense-in-depth check,
but the `_exit(5)` makes debugging harder — there's no error message explaining
what happened.

#### 12. `process_envvars_default()` — hardcoded offset magic numbers (lines 2615–2721)

The environment variable parser uses a `switch (len)` pattern where each case
matches by string length, then uses `memcmp` to verify. While clever and fast,
it's error-prone: adding a new variable requires finding the right length case
and ensuring the hardcoded `&envline[N]` offset matches `len + 1`. The offsets
(e.g., `&envline[6]` for a 5-char name) are derived from the variable name
length + 1 for the `=` sign, but are never computed — just hardcoded.

#### 13. `load_audit_module()` — fatal OOM for audit ifaces struct (line 995)

```c
void *newp = malloc (sizeof (*newp));
if (newp == NULL)
  _dl_fatal_printf ("Out of memory while loading audit modules\n");
```

This is a small allocation (~64 bytes for 8 function pointers + the ifaces
struct), so OOM here indicates a severely broken system. But `_dl_fatal_printf`
terminates the entire process — there's no graceful degradation (e.g., skipping
the audit module).

#### 14. VLA in `process_envvars_default()` (line 2731)

```c
size_t name_len = strlen (debug_output);
char buf[name_len + 12];
```

`debug_output` comes from the `LD_DEBUG_OUTPUT` environment variable. A user
can set this to an extremely long string, causing a stack overflow via VLA.
Since this code path is only reached in non-secure mode
(`process_envvars_default`), the attacker would need to control the environment
of their own process — limiting exploitability. But it's still a potential crash
vector.

#### 15. `RLTD` vs `RTLD` typo in macro names (lines 76, 104, 392)

The timing declaration macro is named `RLTD_TIMING_DECLARE` (note the
transposed `L` and `T`) while the usage macros are correctly named
`RTLD_TIMING_VAR`, `RTLD_TIMING_SET`, etc. This inconsistency has persisted
because the "wrong" name is only used in internal declarations and the code
compiles fine, but it's confusing when reading the source.

### Strengths

- **Bootstrap discipline**: The `DONT_USE_BOOTSTRAP_MAP` / `bootstrap_map`
  pattern carefully avoids GOT references before self-relocation is complete.
  The split between `_dl_start()` (pre-relocation) and `_dl_start_final()`
  (post-relocation) is well-designed.
- **Security-first environment processing**: The `process_envvars` dispatcher
  cleanly separates secure vs. default paths. The secure path strips dangerous
  variables via `UNSECURE_ENVVARS` and has a defense-in-depth exit check.
- **Audit module lifecycle**: The audit loading code is well-structured with
  proper error handling — failed modules are unloaded, TLS state is rolled
  back (`original_tls_idx`), and errors are reported without aborting.
- **`_dl_start_args_adjust()`**: The stack argument shuffling (lines 1290–1339)
  is carefully implemented with assertions verifying the invariants at each
  step, including the tricky auxv copy using `memcpy` to avoid alignment issues.
- **Timing infrastructure**: The `RTLD_TIMING_*` macros compile to nothing on
  architectures without inline hp-timing, avoiding any overhead. The timing
  measurement points are well-placed to capture load vs. relocation time.
- **`rtld_chain_load()`**: The detection and direct `execve` of static
  executables (lines 1071–1106) is a clean addition that avoids the overhead
  of full dynamic linking for programs that don't need it.
- **Debug infrastructure**: The `process_dl_debug()` function (lines 2431–2533)
  with its structured `debopts` table and help output is cleanly designed and
  easy to extend.

## Code Review Notes: `dl-deps.c`

`dl-deps.c` (575 lines) implements **dependency graph resolution** — the BFS
walk that discovers and loads all transitive `DT_NEEDED`, `DT_AUXILIARY`, and
`DT_FILTER` objects for a loaded binary. It produces the flat, ordered search
list (`l_searchlist`) and initialization order list (`l_initfini`) stored on
each `link_map`.

Entry point:

- **`_dl_map_object_deps()`** (line 140) — given a loaded object and its
  preloads, recursively load all dependencies and build the searchlist/initfini
  arrays

Supporting pieces:
- **`openaux()`** (line 60) — callback wrapper for `_dl_map_object`, used with
  `_dl_catch_exception`
- **`preload()`** (line 126) — inserts a preloaded map into the BFS work list
- **`expand_dst`** macro (line 84) — DST (`$ORIGIN`, `$LIB`, etc.) expansion
  using `alloca`

### Potential Issues

#### 1. `expand_dst` macro contains a `continue` statement (line 118)

```c
#define expand_dst(l, str, fatal) \
  ({                                    \
    ...                                 \
    if (*__result == '\0')              \
      {                                 \
        if (fatal)                      \
          _dl_signal_error (...);       \
        else                            \
          {                             \
            /* This is for DT_AUXILIARY.  */ \
            ...                         \
            continue;                   \
          }                             \
      }                                 \
    __result; })
```

A macro that silently controls flow in the caller's loop is dangerous. The
`continue` at line 118 jumps to the next iteration of the enclosing
`for (d = l->l_ld; ...)` loop. If this macro were ever used in a different loop
context, the `continue` would have completely different semantics. The comment
at line 83 acknowledges this is a macro "since we use `alloca`" but doesn't
warn about the embedded control flow.

#### 2. `fatal` parameter appears inverted for DT_AUXILIARY vs DT_FILTER (line 272)

```c
name = expand_dst (l, strtab + d->d_un.d_val,
                   d->d_tag == DT_AUXILIARY);
```

This passes `fatal=true` for `DT_AUXILIARY` (optional) and `fatal=false` for
`DT_FILTER` (required). The semantics:

- **DT_AUXILIARY** (optional dependency): empty DST → `_dl_signal_error`
  (hard crash)
- **DT_FILTER** (required dependency): empty DST → silent `continue` (skip)

This is backwards. The comment inside the non-fatal branch even says
`/* This is for DT_AUXILIARY. */` (line 113), suggesting the original intent
was for auxiliary objects to take the non-fatal path. Compare with the loading
failure handling at lines 290–304 which correctly makes auxiliary non-fatal and
filter fatal.

#### 3. Unbounded `alloca` accumulation in BFS loop (lines 249, 311)

Each newly discovered dependency or auxiliary object causes
`alloca(sizeof(struct list))` (~24 bytes). The comment at lines 179–181
explains:

```
/* The whole process is complicated by the fact that we better
   should use alloca for the temporary list elements.  But using
   alloca means we cannot use recursive function calls.  */
```

For programs with hundreds of shared libraries (large applications, container
environments), this accumulates significant stack usage. Unlike a single large
`alloca`, these happen one-per-iteration across potentially many loop
iterations, making the total hard to predict or bound.

#### 4. `alloca` inside the `expand_dst` macro, called inside a loop (line 99)

```c
__newp = (char *) alloca (DL_DST_REQUIRED (l, __str, strlen (__str),
                                           __dst_cnt));
```

This is inside the `for (d = l->l_ld; d->d_tag != DT_NULL; ++d)` loop. Each
`DT_NEEDED` or `DT_AUXILIARY`/`DT_FILTER` entry containing a DST causes a
separate `alloca`. The size depends on the string length from the ELF string
table, so a crafted ELF with many DST-containing entries could accumulate
substantial stack allocations.

#### 5. Duplicated and incorrect comment for FILTERTAG (lines 38–41)

```c
/* Whether an shared object references one or more auxiliary objects
   is signaled by the AUXTAG entry in l_info.  */
#define FILTERTAG ...
```

The comment is copy-pasted from the `AUXTAG` definition above and still says
"auxiliary objects" and "AUXTAG" — it should say "filter objects" and
"FILTERTAG". Also, both comments say "an shared" which should be "a shared".

#### 6. `l_reserved` field used as temporary mark bit — global side effect (lines 136, 259, 389, 490, 498, 527)

The function repurposes `l_reserved` on every `link_map` as a "visited" marker
during BFS traversal. It's set when objects enter the work list and cleared in
two sweeps at lines 490 and 526–527. This works but:

- It's a global side effect on a field in a shared data structure
- If any error path failed to reach the cleanup loops, subsequent calls to
  `_dl_map_object_deps` would see stale marks and skip objects
- The same field is reused with different semantics at lines 497–501 (marking
  search list members to identify removable relocation dependencies) — the
  comment "Avoid removing relocation dependencies of the main binary" at
  line 500 clears `map->l_reserved` specifically to exclude it from the
  second pass

#### 7. Auxiliary object insertion — opaque `memcpy` swap trick (lines 314–317)

```c
/* We want to insert the new map before the current one,
   but we have no back links.  So we copy the contents of
   the current entry over.  Note that ORIG and NEWP now
   have switched their meanings.  */
memcpy (newp, orig, sizeof (*newp));
```

To insert before the current element in a singly-linked list, the code copies
`orig`'s content to `newp`, then repurposes `orig` for the auxiliary. After
this, `orig` is the new entry and `newp` holds the old entry's data. The
comment "ORIG and NEWP now have switched their meanings" is accurate but this
pattern is extremely confusing — variable names no longer match their content.
The subsequent link surgery (lines 346–404) operates on these swapped pointers,
making the code very hard to follow.

#### 8. Duplicated link-map chain surgery (lines 364–404)

When an auxiliary object needs to be moved earlier in the search order, the code
performs manual doubly-linked list surgery on the global `l_prev`/`l_next`
chain. This surgery is duplicated nearly identically for the "already in list,
move earlier" case (lines 364–373) and the "new object" case (lines 395–404).
A helper function would reduce duplication and the risk of the two copies
diverging.

#### 9. `l_initfini` dual-purpose allocation layout (lines 421–435, 462–470)

The allocation `(2 * nlist + 1) * sizeof(struct link_map *)` serves double
duty: the first `nlist` entries store the init/fini order, and
`&l_initfini[nlist + 1]` is assigned as `l_searchlist.r_list`. This is a clever
space optimization — one malloc serves two arrays — but it means the
`l_initfini` pointer and `l_searchlist.r_list` pointer are into the same
allocation. Freeing one frees the other. The `l_free_initfini` flag tracks
ownership, but the coupling is non-obvious and easy to get wrong.

#### 10. `DT_NEEDED` with empty DST is silently skipped (line 228)

```c
name = expand_dst (l, strtab + d->d_un.d_val, 0);
```

For `DT_NEEDED` (a required dependency), `fatal=0` means an empty DST result
triggers `continue` — the needed dependency is silently skipped. A required
library whose name involves an unresolvable DST (e.g., `$ORIGIN/libfoo.so`
where `$ORIGIN` can't be determined) is dropped from the dependency list
without warning. Symbol resolution failures would occur later without any
indication that a dependency was skipped.

#### 11. Grammar error in macro comment (line 107)

```c
/* The replacement for the DST is not known.  We can't
   processed.  */
```

Should be "We can't proceed" or "It can't be processed."

#### 12. `errno` save/clear/restore pattern (lines 182–184, 448–449)

```c
errno_saved = errno;
errno_reason = 0;
errno = 0;
...
out:
  if (errno == 0 && errno_saved != 0)
    __set_errno (errno_saved);
```

The function manipulates `errno` directly because the internal loading calls
use it. The restore at line 448 only happens if `errno` is still 0 — if any
internal code set errno to a non-zero value, the original errno is lost. This
is technically correct (the new errno is the relevant one) but the pattern is
subtle and fragile.

### Strengths

- **BFS-based dependency resolution**: The iterative BFS (rather than recursive
  DFS) is the right choice — it naturally produces breadth-first ordering and
  avoids stack overflow from deep dependency trees. The `struct list`
  work-queue with `alloca` nodes is efficient for the common case.
- **`scratch_buffer` for per-object dependency arrays**: The `needed_space`
  scratch buffer (line 169) is reused across iterations, avoiding repeated
  malloc/free for temporary per-object dependency lists.
- **Auxiliary/filter loading distinction**: The distinction between
  `DT_AUXILIARY` (optional, failure ignored at lines 290–295) and `DT_FILTER`
  (required, failure propagated at lines 297–304) is correctly implemented for
  loading failures.
- **Relocation dependency pruning**: Lines 493–527 detect relocation
  dependencies that are now redundant (because they're in the search list) and
  remove them, preventing duplicate entries from inflating `l_reldeps`.
- **Atomic write barriers**: The `atomic_write_barrier()` calls at lines 433,
  558, 563 ensure that the `l_initfini` and `l_reldeps` pointers are published
  only after the arrays they point to are fully initialized — protecting
  lock-free readers in other threads.
- **Deferred exception propagation**: The `errno_reason` / `exception` pattern
  (lines 236–239, 571–573) allows cleanup to complete before propagating the
  error via `_dl_signal_exception`, ensuring `l_reserved` marks are cleared and
  scratch buffers freed.
- **`_dl_sort_maps` integration**: The topological sort at line 552 takes a
  parameter controlling whether `libc.so.6` participates in sorting, ensuring
  correct relocation order when libc is the main map.

## Code Review Notes: `dl-open.c`

`dl-open.c` (985 lines) implements the **`dlopen()` / `dlmopen()` runtime** —
loading a shared object into an already-running process, relocating it, and
running its constructors. This is significantly more complex than startup
loading because it must be atomic: if anything fails, the partially loaded state
must be rolled back without corrupting the process.

The file's central design pattern is a **two-phase commit**:
1. **Phase 1 (can fail)**: Pre-allocate all memory — `resize_scopes`,
   `resize_tls_slotinfo`, `add_to_global_resize`
2. **Demarcation point** (line 739): "After this, no recoverable errors are
   allowed"
3. **Phase 2 (cannot fail)**: Commit changes — `activate_nodelete`,
   `update_scopes`, `update_tls_slotinfo`, `add_to_global_update`

Entry points:

| Function | Line | Role |
|---|---|---|
| `_dl_open()` | 842 | Public API: validates args, acquires `dl_load_lock`, error handling |
| `dl_open_worker()` | 792 | Wraps `dl_open_worker_begin` with TLS lock, runs constructors |
| `dl_open_worker_begin()` | 533 | Main work: load, deps, relocate, two-phase scope/TLS update |
| `add_to_global_resize()` / `_update()` | 92 / 173 | Two-phase global scope extension |
| `resize_scopes()` / `update_scopes()` | 256 / 323 | Two-phase scope update for dependents |
| `resize_tls_slotinfo()` / `update_tls_slotinfo()` | 362 / 384 | Two-phase TLS slot allocation |
| `activate_nodelete()` | 447 | Commit pending NODELETE flags |
| `_dl_open_relocate_one_object()` | 474 | Relocate a single object with profiling support |
| `_dl_find_dso_for_object()` | 213 | Linear search for DSO containing an address |

### Potential Issues

#### 1. `scope_has_map` returns `size_t` instead of `bool` (line 232)

```c
static size_t
scope_has_map (struct link_map *map, struct link_map *new)
{
  size_t cnt;
  for (cnt = 0; map->l_scope[cnt] != NULL; ++cnt)
    if (map->l_scope[cnt] == &new->l_searchlist)
      return true;
  return false;
}
```

The function returns `true`/`false` but has return type `size_t`. This is
misleading — someone reading the signature might expect it to return a count
or index. Should be `bool`.

#### 2. `_dl_find_object_update` failure AFTER the demarcation point (lines 756–758)

```c
if (!_dl_find_object_update (new))
  _dl_signal_error (ENOMEM, new->l_libname->name, NULL,
                    N_ ("cannot allocate address lookup data"));
```

This is after line 739's "Demarcation point: After this, no recoverable errors
are allowed." But `_dl_find_object_update` can fail due to memory allocation,
and `_dl_signal_error` raises an exception. At this point, `activate_nodelete`
and `update_scopes` have already committed irreversible changes. The exception
handler in `_dl_open` (line 937) calls `_dl_close_worker` to clean up, but the
scope updates cannot be undone, potentially leaving dangling scope references.

#### 3. FIXME: `_dl_update_slotinfo` can abort the process (lines 428–434, 769–770)

```c
/* FIXME: This calls _dl_update_slotinfo, which aborts the process
   on memory allocation failure.  See bug 16134.  */
update_tls_slotinfo (new);
```

Also after the demarcation point. The `_dl_update_slotinfo` function (called
from `update_tls_slotinfo` at line 434) can call `_dl_fatal_printf` on OOM,
terminating the process. This is a known bug referenced twice in the file
(lines 428–433 and 769–770) as bug 16134. The comment suggests the fix would
involve splitting `_dl_update_slotinfo` into a resize+update pattern like the
other operations.

#### 4. FIXME: Objects visible before TLS is initialized (lines 760–764)

```c
/* FIXME: It is unclear whether the order here is correct.
   Shouldn't new objects be made available for binding (and thus
   execution) only after there TLS data has been set up fully?  */
```

`update_scopes` (line 754) makes objects visible for symbol resolution before
`update_tls_slotinfo` (line 771) initializes their TLS. A lazy-binding
resolution in another thread could call a function that accesses `__thread`
variables before the TLS block is ready. Also contains a typo: "there" should
be "their".

#### 5. Profiling overrides `RTLD_NOW` with `RTLD_LAZY` (line 498)

```c
_dl_relocate_object (l, l->l_scope, reloc_mode | RTLD_LAZY, 1);
```

When `LD_PROFILE` is set, `RTLD_LAZY` is unconditionally OR'd into the
relocation mode. This means `dlopen("libfoo.so", RTLD_NOW)` still gets lazy
binding when profiling is active, silently changing the semantics the user
requested. There is no warning or documentation about this override.

#### 6. `_dl_find_dso_for_object` — O(n) linear scan (lines 218–227)

```c
for (Lmid_t ns = 0; ns < GL(dl_nns); ++ns)
  for (l = GL(dl_ns)[ns]._ns_loaded; l != NULL; l = l->l_next)
    if (addr >= l->l_map_start && addr < l->l_map_end
        && (l->l_contiguous
            || _dl_addr_inside_object (l, (ElfW(Addr)) addr)))
```

Every `dlopen` call uses this to find the caller's DSO. It walks every loaded
object in every namespace. For programs with hundreds of shared libraries, this
is non-trivial. The fast `_dl_find_object` lookup facility exists (line 756
uses `_dl_find_object_update`) but isn't used for this caller-lookup path.

#### 7. `add_to_global_update` deferred to after constructors (line 834)

When `RTLD_GLOBAL` is requested, the new objects' symbols aren't added to the
global scope until AFTER constructors run (line 834 in `dl_open_worker`). This
means constructor code in the newly loaded library cannot rely on its own
symbols being in the global scope for other libraries to find. The
`add_to_global_resize` happened at line 737 (inside `dl_open_worker_begin`),
but the actual update is at line 834 — separated by ~100 lines and a function
boundary.

#### 8. Incomplete `mode` flag validation (lines 846–848)

```c
if ((mode & RTLD_BINDING_MASK) == 0)
  _dl_signal_error (EINVAL, file, NULL, N_("invalid mode for dlopen()"));
```

Only the binding mask (`RTLD_LAZY` / `RTLD_NOW`) is validated. Passing both
`RTLD_LAZY | RTLD_NOW` simultaneously is not detected (the standard says the
behavior is implementation-defined). Other potentially invalid flag combinations
are silently accepted.

#### 9. TLS generation counter wrap — fatal on 32-bit (lines 403–406)

```c
size_t newgen = GL(dl_tls_generation) + 1;
if (__glibc_unlikely (newgen == 0))
  _dl_fatal_printf (N_("\
TLS generation counter wrapped!  Please report this."));
```

On 64-bit systems, wrapping requires ~2^64 `dlopen` calls (impossible). On
32-bit systems, ~4 billion calls is conceivable in a very long-running server
doing plugin reloading. The `_dl_fatal_printf` terminates with no recovery
path.

#### 10. Comment typos

- Line 762: `"there TLS data"` → should be `"their TLS data"`
- Line 878: `"Such direct placements is"` → should be `"Such direct placement
  is"` or `"Such direct placements are"`
- Line 113: `"in an realloc()"` → should be `"in a realloc()"`

### Strengths

- **Two-phase commit architecture**: The central design insight — separate
  allocation (can fail) from mutation (cannot fail) — is excellent. The
  `resize_*` / `update_*` pairs for scopes, TLS, and global scope ensure that
  dlopen failure can be rolled back cleanly. The demarcation point at line 739
  makes the contract explicit.
- **Overflow-safe arithmetic**: `add_to_global_resize` (lines 116–151) uses
  `__builtin_add_overflow` and `__builtin_mul_overflow` for every size
  computation, preventing integer overflow in allocation size calculations.
- **RCU-like scope update**: `update_scopes` (lines 341–346) writes the NULL
  terminator before the new entry, with an `atomic_write_barrier` between them.
  This ensures concurrent readers (lazy binding in other threads) always see
  either the old scope or the complete new scope, never a partially updated one.
  The `THREAD_GSCOPE_WAIT` in `add_to_global_resize` (line 164) similarly
  ensures old memory isn't freed while threads might be reading it.
- **NODELETE activation timing**: `activate_nodelete` (line 749) is placed
  after the demarcation point and before `update_scopes`. This means if dlopen
  fails, NODELETE flags are not yet committed and `_dl_close_worker` can clean
  up normally.
- **Error cleanup in `_dl_open`**: The error path (lines 926–948) correctly
  resets `libc_map` if libc was freshly loaded, calls `_dl_close_worker` to
  unmap the partially loaded object, and releases `dl_load_lock` before
  re-raising the exception.
- **Exponential global scope growth**: `add_to_global_resize` uses a doubling
  strategy (line 138: `required_new_size * 2`) after the initial allocation,
  reducing the number of reallocations for programs that load many libraries.
- **Recursive dlopen safety**: The `_ns_global_scope_pending_adds` counter
  (lines 58–61, 116–117, 199–203) correctly handles recursive dlopen (dlopen
  from a constructor). The save/restore pattern ensures inner and outer dlopen
  calls can both track their contributions to the global scope.

## Code Review Notes: `dl-close.c`

`dl-close.c` (797 lines) implements **`dlclose()` — the counterpart to
`dl-open.c`**. Unloading a shared object is arguably harder than loading one:
it must identify which objects are still needed (transitive reachability
analysis), run destructors, update scopes, reclaim TLS space, unlink from the
global chain, and unmap memory — all while handling recursive dlclose from
destructors and concurrent access from other threads.

Entry points:

| Function | Line | Role |
|---|---|---|
| `_dl_close()` | 756 | Public API: acquires `dl_load_lock`, validates map, dispatches |
| `_dl_close_worker()` | 109 | Main work: reachability, destructors, scope cleanup, unmap, free |
| `remove_slotinfo()` | 44 | Recursive TLS slotinfo entry removal |

The core algorithm in `_dl_close_worker`:
1. Build `maps[]` array of all objects in the namespace
2. Mark reachable objects via `l_initfini` and `l_reldeps` dependency chains
3. Sort by dependency order for destructor execution
4. Run destructors for unreachable objects
5. Clean up scopes of surviving objects that referenced removed ones
6. Remove from global scope, unmap segments, free metadata

### Potential Issues

#### 1. Known use-after-free race — bug 20990 (lines 777–791)

```c
/* At present this is an unreliable check... In a non-recursive dlclose
   the map itself might have been freed and this access is potentially a
   data race... This is bug 20990. */
if (__builtin_expect (map->l_direct_opencount, 1) == 0)
```

The comment explicitly acknowledges this: after a concurrent `dlclose` frees
the map but before this thread acquires `dl_load_lock`, reading
`map->l_direct_opencount` is a use-after-free. The memory may have been
returned to the allocator and reused.

#### 2. VLA `maps[nloaded]` on stack (line 141)

```c
const unsigned int nloaded = ns->_ns_nloaded;
struct link_map *maps[nloaded];
```

`nloaded` is the total number of objects in the namespace. For programs with
hundreds of shared libraries (common in large applications, Java/Python with
many native modules), this VLA could be substantial (8 bytes × nloaded). A
namespace with 500 objects uses 4KB of stack just for this array.

#### 3. `malloc` failure during scope cleanup raises exception after destructors (lines 354–357)

```c
newp = (struct r_scope_elem **)
  malloc (new_size * sizeof (struct r_scope_elem *));
if (newp == NULL)
  _dl_signal_error (ENOMEM, "dlclose", NULL,
                    N_("cannot create scope list"));
```

This is the only allocation in the unload path. It happens after destructors
have already run (`_dl_call_fini` at line 264), so some objects are partially
torn down. Raising an exception here leaves the process in an inconsistent
state — destructors ran but objects weren't actually unloaded. However,
without this allocation, surviving objects would have dangling scope pointers,
which is worse.

#### 4. Static local for recursive dlclose detection (line 117)

```c
static enum { not_pending, pending, rerun } dl_close_state;
```

This function-scoped static variable implements a state machine for handling
recursive `dlclose` (a destructor calling `dlclose`). The inner call sets
`dl_close_state = rerun`, which causes the outer call to `goto retry` at
line 749 with `map = NULL`. While `dl_load_lock` serializes access, the static
state means this function is inherently non-reentrant across threads — correct
only because the lock is held, but a maintenance hazard if the locking strategy
ever changes.

#### 5. TLS static space reclamation — only contiguous tail chunks freed (lines 517–597)

The TLS reclamation algorithm only frees space from the *end* of the used area.
Non-contiguous gaps between still-loaded modules create permanent leaks. The
comment at lines 538–540 admits: "This isn't contiguous with the last chunk
freed. One of them will be leaked unless we can free one block right away."
Programs that repeatedly load/unload TLS-using libraries in different orders
will gradually exhaust static TLS surplus.

#### 6. `offsetof` cast to recover `link_map` from scope pointer (lines 320–322)

```c
struct link_map *tmap = (struct link_map *)
  ((char *) imap->l_scope[cnt]
   - offsetof (struct link_map, l_searchlist));
```

This `container_of` pattern recovers the enclosing `link_map` from a pointer
to its `l_searchlist` member. The comment at lines 315–317 documents the
assumption that `l_scope[]` entries are always either `l_symbolic_searchlist`
or some map's `l_searchlist`. If any code ever added a differently-typed scope
entry, this would compute a garbage pointer and corrupt memory.

#### 7. Destructor exceptions are fatal (line 264)

```c
if (imap->l_init_called)
  _dl_catch_exception (NULL, _dl_call_fini, imap);
```

`NULL` first argument means exceptions propagate rather than being caught. If
a destructor triggers a lazy binding failure or other dynamic linking error,
the process aborts. A library with an unresolvable lazy symbol in its
`__attribute__((destructor))` function will take down the entire process.

#### 8. Unique symbol table entries not cleaned on normal dlclose (lines 601–623)

The `force` parameter is only `true` when called from `_dl_open`'s error path
(rollback). Normal `dlclose` passes `force=false`, so `STB_GNU_UNIQUE` symbols
are never removed from the namespace hash table. This is by design (unique
symbols persist to maintain uniqueness guarantees), but it means the hash table
grows monotonically, never shrinking even when the defining objects are
unloaded.

#### 9. `remove_slotinfo` is recursive (line 59)

```c
if (remove_slotinfo (idx, listp->next, disp + listp->len,
                     should_be_there))
  return true;
```

The function recursively traverses the `dtv_slotinfo_list` linked list.
Recursion depth is `total_tls_modules / slots_per_list_node`. While typically
shallow, programs with many TLS-using shared libraries could produce
non-trivial recursion depth. An iterative approach would be more robust.

#### 10. `SCOPE_ELEMS` macro defined inside function body, never `#undef`'d (lines 341–342)

```c
#define SCOPE_ELEMS(imap) \
  (sizeof (imap->l_scope_mem) / sizeof (imap->l_scope_mem[0]))
```

Defined deep inside `_dl_close_worker` within a nested loop. It's equivalent
to the already-available `array_length(imap->l_scope_mem)`. Being never
`#undef`'d, it leaks to the rest of the translation unit.

#### 11. `(void *) -1` sentinel in cleanup (lines 661, 695–698)

```c
if (imap->l_origin != (char *) -1)
  free ((char *) imap->l_origin);
...
if (imap->l_rpath_dirs.dirs != (void *) -1)
  free (imap->l_rpath_dirs.dirs);
```

Same unnamed sentinel pattern as `dl-load.c`. Every free-site must remember to
check for `(void *) -1` or risk passing it to `free()`.

#### 12. Comment typos

- Line 43: `"Returns true we an non-empty was found."` → should be "Returns
  true if a non-empty entry was found."
- Line 104: `"No non-entry in this list element."` → should be "No non-empty
  entry"
- Line 293: `"it's own scope"` → should be "its own scope"
- Line 482: `"module specitic"` → should be "module specific"

### Strengths

- **Reachability-based garbage collection**: The `l_map_used` / `l_map_done`
  marking algorithm (lines 166–237) correctly handles transitive dependencies —
  marking an object as used triggers re-scanning of its dependencies. The
  `done_index` backtracking (lines 211–212, 232–233) ensures no dependency is
  missed even when the marking order doesn't match the index order.
- **Recursive dlclose handling**: The `dl_close_state` state machine
  (lines 117–123, 744–752) elegantly handles a destructor calling `dlclose`.
  Rather than deadlocking or corrupting state, the inner call just sets `rerun`
  and returns immediately. The outer call then performs a full garbage
  collection pass with `map = NULL`.
- **Global scope compaction**: The removal of objects from the global scope
  (lines 438–461) has an optimization for the common case where the most
  recently added objects are removed, avoiding O(n) compaction.
- **RCU-like scope replacement with deferred free**: New scope arrays are
  allocated and swapped in atomically (line 388), with old arrays freed via
  `_dl_scope_free` (line 393) which uses `THREAD_GSCOPE_WAIT` to ensure no
  thread is reading the old scope.
- **TLS generation bump**: The `dl_tls_generation` increment (lines 713–717)
  with `atomic_store_release` ensures all threads see that DTV entries for
  unloaded modules are stale and need refresh.
- **Dependency sorting for destructor order**: `_dl_sort_maps` (line 242)
  ensures destructors run in reverse dependency order, with the original
  `dlclose` argument forced first.
- **Atomic TLS slot clearing**: `remove_slotinfo` uses `atomic_store_relaxed`
  for both `gen` and `map` fields (lines 77–79), ensuring concurrent
  `__tls_get_addr` calls see consistent state even without locks on the read
  side.

### dl-fini.c / dl-call_fini.c — Exit-Time Destructor Orchestration

**Files:** `dl-fini.c` (146 lines) + `dl-call_fini.c` (51 lines)
**Purpose:** Run `DT_FINI_ARRAY` and `DT_FINI` callbacks for all loaded shared
objects across all namespaces at program exit, respecting dependency order.
**Companion reviewed:** `dl-init.c` (128 lines) — constructor counterpart.

`_dl_fini()` is registered as the process exit handler (via `__cxa_atexit`). It
iterates namespaces in reverse order, snapshots each namespace's loaded objects
into a VLA, topologically sorts them via `_dl_sort_maps`, releases the loader
lock, then calls destructors front-to-back (dependencies destroyed after
dependents). Audit-module namespaces are processed in a second pass via a
`goto again` loop.

| Function | File | Line | Description |
|---|---|---|---|
| `_dl_fini` | dl-fini.c | 25 | Process exit: run all destructors across all namespaces |
| `_dl_call_fini` | dl-call_fini.c | 23 | Invoke DT_FINI_ARRAY (reverse) then DT_FINI for one link_map |

**Potential Issues:**

1. **VLA `maps[nloaded]` — stack overflow risk (dl-fini.c:68):** Same pattern
   flagged in dl-close.c. `nloaded` comes from `_ns_nloaded` which can be
   arbitrarily large. A plugin-heavy application could overflow the stack.

2. **Comment typo (dl-fini.c:40):** "we pick run the destructors" should read
   "we run the destructors".

3. **No exception handling around `_dl_call_fini` (dl-fini.c:114):**
   Destructors are called directly without `_dl_catch_exception`. If a
   destructor throws (C++ `throw` escaping), remaining objects never get their
   destructors called and their `l_direct_opencount` is never decremented.
   Compare with `dl-close.c` which wraps destructor calls with
   `_dl_catch_exception(NULL, ...)`.

4. **`l_direct_opencount` not restored on exception (dl-fini.c:85, 122):**
   Each object's count is bumped before destructors and decremented after. If a
   destructor aborts or throws, the decrement at line 122 is never reached,
   leaving inflated open counts.

5. **`l_init_called` cleared BEFORE destructors run (dl-call_fini.c:32):**
   `_dl_call_fini` sets `l_init_called = 0` before invoking any destructor. If
   a destructor calls `dlopen` on the same library, the guard is already
   cleared, potentially allowing re-initialization of a partially-destroyed
   object — a reentrancy hazard.

6. **NULL dereference on DT_FINI_ARRAYSZ (dl-call_fini.c:39):** When
   `DT_FINI_ARRAY` is present but `DT_FINI_ARRAYSZ` is missing (malformed
   ELF), `map->l_info[DT_FINI_ARRAYSZ]` is NULL and the dereference will
   segfault. Same pattern exists in dl-init.c:70 for `DT_INIT_ARRAYSZ`.

7. **`nloaded` vs `nmaps` — VLA oversized (dl-fini.c:68, 89):** The VLA is
   allocated with `nloaded` entries but only `nmaps` are populated (proxy
   objects are skipped). Extra stack space is wasted but `_dl_sort_maps`
   correctly receives `nmaps`.

8. **Lock-free destructor window — TOCTOU with concurrent `dlclose`
   (dl-fini.c:103–114):** The loader lock is released before destructors run.
   The `l_direct_opencount` bump prevents unloading, but a concurrent `dlclose`
   could race on the `l_init_called` flag — if `dlclose` runs before the flag
   is cleared, the same destructor could execute twice from different threads.

9. **`goto again` control flow for audit namespaces (dl-fini.c:42–44,
   131–136):** The two-pass approach (`do_audit=0` then `do_audit=1`) via
   `goto again` re-iterates all namespaces on the second pass. Correct but
   harder to follow than a nested loop or function extraction.

10. **Init/fini ordering asymmetry (dl-init.c vs dl-fini.c):** `_dl_init`
    iterates `main_map->l_initfini[]` from the end; `_dl_fini` calls
    `_dl_sort_maps` for a fresh topological order. These are conceptually
    inverse but use different mechanisms. Intentional (dlopen may have changed
    the graph) but a source of subtle ordering bugs if the sort diverges.

**Strengths:**

- **Compact and focused**: At 146 lines, one of the most readable files in the
  dynamic linker.
- **Correct dependency-order destruction**: Uses `_dl_sort_maps` for
  dependents-before-dependencies ordering per ELF spec.
- **Anti-unload guard**: `l_direct_opencount` bump/restore prevents concurrent
  `dlclose` from unloading objects during destruction.
- **Proxy object filtering**: `l == l->l_real` check (line 75) correctly skips
  proxy link_map entries, avoiding double-destruction.
- **Audit namespace isolation**: Audit modules get destructors in a separate
  pass, ensuring audit hooks remain available during normal destruction.
- **`_dl_call_fini` separation**: Factoring per-object invocation into its own
  function enables reuse from both `_dl_fini` and `_dl_close_worker`, and lets
  dl-close.c wrap it with exception handling.
- **Debug tracing**: `DL_DEBUG_IMPCALLS` traces for destructor invocations,
  aiding debugging with `LD_DEBUG=impcalls`.

### dl-init.c — Constructor Initialization

**File:** `dl-init.c` (128 lines)
**Purpose:** Run constructors (`DT_PREINIT_ARRAY`, `DT_INIT`, `DT_INIT_ARRAY`)
for all loaded shared objects at startup, respecting dependency ordering
(dependencies initialized before dependents).

`_dl_init()` is called from `_dl_start_user` (architecture-specific assembly
trampoline) after all libraries are loaded and relocated. It first handles the
`DF_1_INITFIRST` priority object (if any), then runs `DT_PREINIT_ARRAY` for the
main executable, and finally iterates `main_map->l_initfini[]` in reverse order
(dependencies first) calling `call_init()` for each object. `call_init()` runs
`DT_INIT` then `DT_INIT_ARRAY` in forward order.

| Function | File | Line | Description |
|---|---|---|---|
| `_dl_init` | dl-init.c | 80 | Run constructors for all loaded objects at startup |
| `call_init` | dl-init.c | 26 | (static) Run DT_INIT then DT_INIT_ARRAY for one link_map |

**Potential Issues:**

1. **NULL dereference on `DT_INIT_ARRAYSZ` (line 70):** Same issue as
   dl-call_fini.c:39. When `DT_INIT_ARRAY` is present but `DT_INIT_ARRAYSZ` is
   missing (malformed ELF), `l->l_info[DT_INIT_ARRAYSZ]` is NULL and the
   dereference segfaults.

2. **NULL check inconsistency for `DT_PREINIT_ARRAYSZ` vs `DT_INIT_ARRAYSZ`
   (lines 93–95 vs 70):** `DT_PREINIT_ARRAYSZ` gets a NULL check before
   dereference, but `DT_INIT_ARRAYSZ` (line 70) and `DT_FINI_ARRAYSZ`
   (dl-call_fini.c:39) do not. Suggests the init-array/fini-array cases were
   missed when the defensive check was added.

3. **No exception handling around constructor calls (lines 60, 73–74):**
   Constructors are called directly without `_dl_catch_exception`. If a
   constructor throws or crashes, remaining constructors never run. At startup
   this is generally fatal anyway.

4. **Misleading `__builtin_expect` hint (line 46):** The expect value `'a'` is
   used as a hint that the name usually starts with a non-null character. While
   functionally correct, using the literal `'a'` is confusing — a clearer idiom
   would be `__glibc_likely (l->l_name[0] != '\0')`.

5. **`l->l_name[0] == '\0'` as executable detection (line 46):** The main
   executable's `l_name` is empty because it was loaded by the kernel. This
   implicit convention could break if the linker ever populated `l_name` for
   the main executable. Paired with `l->l_type == lt_executable` which makes it
   robust in practice, but the empty-name assumption is fragile.

6. **`dl_initfirst` global state — use-after-free window (lines 86–89):** If
   the `DF_1_INITFIRST` object is closed before `_dl_init` runs, the pointer is
   dangling. `dl-close.c:700–702` clears it to NULL, but there's a TOCTOU
   window. In practice only matters at startup where races are unlikely.

7. **`DF_1_INITFIRST` bypasses dependency ordering (lines 86–89):** The
   `dl_initfirst` object has its constructor called before the main dependency
   walk. If it has uninitialized dependencies, the ELF spec requirement
   (dependencies before dependents) is violated. `l_init_called` prevents
   double initialization in the subsequent loop.

8. **`ELF_INITFINI` platform inconsistency (line 59):** `DT_INIT` is
   conditionally executed based on `ELF_INITFINI`, which is `0` on
   generic/aarch64/arm and `1` on x86/powerpc/s390/etc. Legacy `DT_INIT`
   constructors are silently ignored on newer architectures — a portability
   trap for old binaries or hand-crafted ELF objects moved between platforms.

9. **Editorial comment tone (lines 109–117):** "Stupid users forced the ELF
   specification to be changed" and "Stupidity rules!" are unprofessional for a
   foundational system library shipped to billions of devices, though they
   document legitimate frustration with the spec change.

10. **`_dl_starting_up` dead code on Linux (lines 123–126):**
    `HAVE_INLINED_SYSCALLS` is defined to `1` on all Linux targets, making this
    code dead on all current glibc-supported platforms. The variable is still
    exported for ABI compatibility but the assignment in `_dl_init` never
    executes.

11. **Init ordering depends on `l_initfini` from `dl-deps.c` (line 121):** The
    reverse iteration relies on the `l_initfini` array being in "leaves last"
    order as built by `dl-deps.c`. If `_dl_sort_maps` is modified (as happened
    with the DFS→DSO sort algorithm switch), init ordering can silently change,
    causing constructor-ordering bugs that are extremely hard to diagnose.

**Strengths:**

- **Compact and well-structured**: Clean separation between `call_init`
  (per-object) and `_dl_init` (orchestrator) at 128 lines.
- **Circular dependency guard**: `l_init_called = 1` set before running
  constructors (line 43) prevents infinite recursion.
- **Proxy object filtering**: `l != l->l_real` check (line 29) prevents running
  constructors on proxy link_map entries in secondary namespaces.
- **Relocation assertion**: `assert(l->l_relocated || l->l_type ==
  lt_executable)` (line 35) catches running constructors with unresolved
  function pointers.
- **Correct constructor ordering**: `DT_PREINIT_ARRAY` first (main executable
  only), then `DT_INIT`, then `DT_INIT_ARRAY` — per ELF specification.
- **`DF_1_INITFIRST` support**: Handles the rare `-z initfirst` flag for
  libraries requiring early initialization.
- **Debug tracing**: `DL_DEBUG_IMPCALLS` messages for both preinit and init
  calls, enabling `LD_DEBUG=impcalls` diagnostics.

### dl-sort-maps.c — Topological Sort for Init/Fini Ordering

**File:** `dl-sort-maps.c` (311 lines)
**Purpose:** Topologically sort an array of `link_map` pointers according to
dependency order, used to determine constructor and destructor invocation order.
**Callers:** `dl-deps.c:552` (init ordering), `dl-fini.c:94` (exit
destructors), `dl-close.c:242` (dlclose destructors).

The file implements **two competing sorting algorithms**, selectable at runtime
via the `glibc.rtld.dynamic_sort` tunable:

- **Algorithm 1 (`_dl_sort_maps_original`)** — legacy algorithm (default
  through glibc 2.35). Iterative bubble-sort-like approach that moves objects
  backward past their dependents, using a `seen[]` counter array to detect and
  break cycles.
- **Algorithm 2 (`_dl_sort_maps_dfs`)** — DFS-based algorithm (default since
  glibc 2.36). Depth-first traversal producing Reverse-Postorder (RPO), which
  is a topological sort. Optionally does a second pass ignoring `l_reldeps` to
  prioritize static `DT_NEEDED` links over relocation dependencies in cycles.

| Function | Line | Description |
|---|---|---|
| `_dl_sort_maps` | 296 | Dispatch: call original or DFS algorithm based on tunable |
| `_dl_sort_maps_init` | 288 | Read tunable and set `dl_dso_sort_algo` |
| `_dl_sort_maps_original` | 29 | (static) Legacy iterative bubble-sort algorithm |
| `_dl_sort_maps_dfs` | 179 | (static) DFS-based topological sort |
| `dfs_traversal` | 135 | (static) Recursive DFS worker |

**Potential Issues:**

1. **VLA `seen[nmaps]` — stack overflow in original algorithm (line 42):**
   `uint16_t seen[nmaps]` allocated on stack with no bound on `nmaps`. Less
   severe than pointer VLAs (2 bytes vs 8 bytes per element) but compounds with
   caller VLAs already on the stack.

2. **VLA `rpo[nmaps]` — stack overflow in DFS algorithm (line 211):**
   `struct link_map *rpo[nmaps]` (8 bytes per element on 64-bit) compounds with
   the caller's own VLA (e.g., `dl-fini.c:68` already has `maps[nloaded]`).

3. **Unbounded recursion depth in `dfs_traversal` (lines 134–173):** The DFS
   is recursive. A deeply nested dependency chain could cause stack overflow.
   The comment acknowledges alloca usage in `_dl_map_object_deps` is "on the
   same order" but doesn't bound the actual depth.

4. **`l_visited` flag is global mutable state (lines 143, 184, 247):**
   `l_visited` is a 1-bit field in `struct link_map` used as the DFS visited
   marker. If `_dl_sort_maps_dfs` is called concurrently from different threads
   (e.g., two `dlclose` calls), the flags will race. Callers hold
   `dl_load_lock` in some but not all cases.

5. **`seen[]` overflow — `uint16_t` counter (lines 42, 47, 77):** The counter
   allows at most 65535 visits before silent wraparound. With pathological
   cyclic dependencies and large `nmaps`, the counter could wrap around to 0,
   causing cycle detection to fail and the algorithm to loop indefinitely.

6. **O(n^3) worst-case in original algorithm (lines 44–121):** The outer loop
   iterates objects; the inner loop scans backwards; dependency walks are
   linear. Combined with O(n) `memmove` operations on each move, worst case is
   O(n^3) or worse with circular dependencies.

7. **`__glibc_likely` hint is wrong (line 306):** The tunable default is `2`
   (DFS) per `dl-tunables.list:159`, but `__glibc_likely` is placed on the
   original algorithm path. The DFS path is the common case, so the branch hint
   misleads the predictor.

8. **`force_first` handled differently between algorithms (lines 34–35 vs
   276–284):** The original algorithm excludes `maps[0]` from sorting entirely
   via pointer advance. The DFS algorithm sorts all objects then forcibly moves
   the first map back to position 0 via `memmove`, which can introduce ordering
   violations (as noted in the comment at lines 265–274).

9. **Second DFS pass writes directly to `maps[]` (lines 249–259):** In the
   `do_reldeps` case, the second pass reads from `rpo[]` while writing to
   `maps[]`. Correct because the arrays don't alias, but subtle and
   non-obvious.

10. **`l_main_map` filter in DFS but not original (lines 151, 166):**
    `dfs_traversal` skips dependencies where `dep->l_main_map` is set. The
    original algorithm has no equivalent filter. Switching the tunable can
    change which objects appear in the sorted output.

11. **`goto` spaghetti in original algorithm (lines 70–120):** Uses 5 labels
    (`move`, `next_clear`, `next`, `skip`, `ignore`) and 6 `goto` statements.
    The `move` label is jumped to from two separate nested loops (lines 69 and
    106), making loop invariants very hard to reason about.

12. **`memmove` of `seen[]` tracks pre-move visit counts (line 84):** When an
    object moves from position `i` to `k`, its visit count is preserved from
    the original position. Accumulation across multiple moves is the intended
    cycle-detection behavior but is non-obvious.

**Strengths:**

- **Dual-algorithm design with runtime selection**: The tunable
  `glibc.rtld.dynamic_sort` (values 1/2) allows switching algorithms without
  recompilation, critical for diagnosing ordering regressions.
- **Well-documented DFS algorithm**: Extensive comments (lines 186–208) explain
  backward iteration rationale, the RPO property, and `maps[0]` handling.
- **Two-pass reldep refinement**: Optional second DFS pass (lines 244–261)
  ignoring `l_reldeps` ensures static `DT_NEEDED` links take priority over
  relocation dependencies when breaking cycles.
- **Early termination**: Both the DFS outer loop (line 226) and second pass
  (line 257) break early when all objects are placed.
- **Performance-conscious dispatch**: Direct `if`/`else` instead of function
  pointer avoids indirect branch + PTR_MANGLE overhead on small sorts.
- **Correct `l_faked` filtering**: `dfs_traversal` (line 140) skips faked
  descriptors, matching the filtering in `_dl_map_object_deps`.
- **Cycle tolerance**: Both algorithms handle cyclic dependencies gracefully
  rather than crashing or looping indefinitely.

### dl-tls.c — Thread-Local Storage Management

**File:** `dl-tls.c` (1108 lines)
**Purpose:** Manage all aspects of TLS (Thread-Local Storage) for the dynamic
linker — static TLS layout computation, DTV (Dynamic Thread Vector)
allocation/resizing, dynamic TLS allocation on first access via
`__tls_get_addr`, and slotinfo bookkeeping for module load/unload.

Two major TLS variants are controlled by architecture-specific macros:
`TLS_TCB_AT_TP` (TCB at thread pointer, TLS grows downward — x86) and
`TLS_DTV_AT_TP` (TCB at thread pointer, TLS grows upward — ARM, AArch64).

Each thread has a DTV array indexed by module ID. Entries are either pointers to
allocated TLS blocks, static TLS offsets, or the sentinel `TLS_DTV_UNALLOCATED`
(`(void *) -1`). The DTV carries a generation counter (`dtv[0].counter`)
compared against the global `GL(dl_tls_generation)` to detect staleness after
`dlopen`/`dlclose`. The global `dl_tls_dtv_slotinfo_list` is a linked list of
arrays mapping module IDs to `(link_map*, generation)` pairs — it grows
monotonically and is never shrunk, enabling lock-free reads from
`__tls_get_addr`.

| Function | Line | Description |
|---|---|---|
| `_dl_tls_static_surplus_init` | 96 | Compute static TLS surplus from tunables |
| `_dl_assign_tls_modid` | 124 | Assign a TLS module ID, reusing gaps |
| `_dl_count_modids` | 191 | Count active TLS modules (handles gaps) |
| `_dl_determine_tlsoffset` | 215 | Compute static TLS layout at startup |
| `_dl_allocate_tls_storage` | 416 | Allocate static TLS block + DTV for a new thread |
| `_dl_allocate_tls_init` | 522 | Initialize TLS block from init images |
| `_dl_allocate_tls` | 625 | Combined storage + init allocation |
| `_dl_deallocate_tls` | 635 | Free DTV and dynamic TLS blocks |
| `_dl_update_slotinfo` | 719 | Update thread's DTV to current generation |
| `__tls_get_addr` | 941 | Main TLS access entry point (called from generated code) |
| `_dl_tls_get_addr_soft` | 971 | Non-allocating TLS probe (for dlsym etc.) |
| `_dl_add_to_slotinfo` | 1016 | Register a new TLS module in the slotinfo list |
| `_dl_init_static_tls` | 1092 | Copy init image to all existing threads' static TLS |

**Potential Issues:**

1. **Fatal `oom()` abort on DTV resize (lines 117–120, 494–503):**
   `_dl_resize_dtv` calls `oom()` which is `__noreturn__` and calls
   `_dl_fatal_printf`. Any `malloc`/`realloc` failure during DTV resize kills
   the entire process. Called from `__tls_get_addr` (via `_dl_update_slotinfo`
   line 809), meaning any TLS access can abort the process. The comment
   "Resizing the dtv aborts on failure: bug 16134" acknowledges this.

2. **`free()` in `_dl_update_slotinfo` is not async-signal-safe (lines
   818–822):** The comment explicitly says "this is not AS-safe."
   `__tls_get_addr` can be called from signal handlers (any TLS access in a
   signal handler). If the signal interrupts `malloc`/`free`, calling `free`
   here corrupts the heap. The `XXX` comment notes a memory pool is needed but
   none has been implemented.

3. **Out-of-thin-air value disclaimer (lines 761–765):** The concurrency notes
   acknowledge the C11 memory model's OOTA problem for relaxed atomics and
   dismiss it as "not expected to be an issue in practice." A weak foundation
   for a core synchronization mechanism.

4. **`_dl_determine_tlsoffset` — complex alignment with gap reuse (lines
   215–355):** The `firstbyte` computation works around a GNU ld bug where TLS
   alignment is file-relative rather than block-relative. The arithmetic is very
   subtle with no unit tests — an off-by-one in the `roundup` + `firstbyte`
   logic would silently corrupt TLS data.

5. **`allocate_dtv` OOM handling inconsistency (lines 358–386):**
   `allocate_dtv` returns NULL on `calloc` failure and
   `_dl_allocate_tls_storage` propagates this gracefully, but
   `_dl_resize_dtv` calls `oom()` and aborts. Some paths return NULL, others
   kill the process.

6. **DTV generation counter set outside lock (line 618):**
   `dtv[0].counter = maxgen` is written after releasing `dl_load_tls_lock`. A
   concurrent `dlopen` could increment `GL(dl_tls_generation)` between unlock
   and write. Safe from data races (DTV is thread-local) but the logical
   generation could be stale.

7. **`_dl_tls_get_addr_soft` confusing `counter` dual meaning (line 986):**
   `dtv[-1].counter` is the DTV length; `dtv[0].counter` is the generation.
   Both are named `counter` in the `dtv_t` union. The bounds check
   `l->l_tls_modid >= dtv[-1].counter` is correct but the naming is confusing
   and error-prone for maintainers.

8. **`prevp` used uninitialized on degenerate path (line 1061):**
   `prevp = NULL` with comment "Needed to shut up gcc." If the slotinfo list
   were empty, `atomic_store_release(&prevp->next, listp)` dereferences NULL.
   Should never happen (rtld.c initializes it) but technically UB.

9. **`_dl_init_static_tls` iterates all threads under lock (lines
   1092–1106):** Holds `dl_stack_cache_lock` while iterating every thread and
   `memcpy`ing TLS init images. With thousands of threads, blocks thread
   creation/exit. Writes to other threads' static TLS blocks without
   synchronization — a thread reading its TLS during concurrent `dlopen` with
   static TLS optimization could see partially-initialized data.

10. **`tls_get_addr_tail` DTV update without barrier (lines 896–897):**
    When redirecting to static TLS, `to_free` is set to NULL and `val` is
    updated without barriers. A concurrent `_dl_tls_get_addr_soft` reading the
    DTV could see the pointer before `to_free` is cleared, risking double-free
    on thread exit.

11. **`allocate_dtv_entry` manual memalign emulation (lines 673–699):** When
    TLS alignment exceeds `_Alignof(max_align_t)`, manually aligns within a
    larger `malloc`. The `to_free` pointer invariant (must use `to_free`, not
    `val`, for freeing) is maintained but fragile — any code path freeing `val`
    directly would leak or corrupt memory.

12. **`LEGACY_TLS` runtime assertion instead of compile-time (line 110):**
    `assert(LEGACY_TLS >= 0)` fires at startup if tunable defaults change. A
    `_Static_assert` would catch this at compile time instead.

13. **Duplicate TLS init image copy pattern (lines 603–605, 710–712,
    1087–1088):** `memset(__mempcpy(dest, initimage, initsize), '\0',
    blocksize - initsize)` appears three times with no shared helper. If
    `l_tls_initimage` is NULL, `__mempcpy` receives a NULL source with size 0
    — technically UB per C standard even for zero-length copies.

**Strengths:**

- **Extensive concurrency documentation**: The `CONCURRENCY NOTES` block in
  `_dl_update_slotinfo` (lines 724–765) carefully enumerates all cases with
  generation counter reasoning — one of the most thorough memory-ordering
  analyses in glibc.
- **Lock-free fast path in `__tls_get_addr`**: The common case (generation
  current, entry allocated) is three loads and a return with no locks (lines
  943–963).
- **Monotonically growing slotinfo list**: Never shrinking enables lock-free
  traversal with only `atomic_load_acquire` on `next` pointers (line 839).
- **Gap reuse in `_dl_assign_tls_modid`**: Module IDs from unloaded libraries
  are reclaimed (lines 128–174), preventing unbounded ID growth.
- **Static TLS optimization**: `tls_get_addr_tail` (lines 875–903) detects
  modules promoted to static TLS and redirects to the static block, avoiding
  dynamic allocation.
- **Tunable-driven surplus sizing**: `rtld.nns` and `rtld.optional_static_tls`
  tunables allow trading stack space for TLS capacity, with `LEGACY_TLS`
  preserving backward compatibility.
- **Clean DTV lifecycle**: Allocation, resize, and deallocation properly handle
  the initial-DTV special case (bootstrap allocator, can't be freed).

## Tests

The directory contains approximately 393 test programs covering:

- Basic loading, circular dependencies, and initialization ordering
- `dlopen`/`dlclose`/`dlsym` behavior and error handling
- Symbol visibility (global, protected, weak, `RTLD_NEXT`)
- Thread-Local Storage (TLS) across all access models
- Indirect functions (ifuncs)
- The `LD_AUDIT` interface
- RELR relocations, hardware capabilities, and static PIE
- `ldconfig` and the library cache
- Security constraints for setuid programs
