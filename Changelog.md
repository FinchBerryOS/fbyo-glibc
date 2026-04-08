# FBYO C Library / rtld Changelog

## Dynamic String Tokens (DST)

### Added `$EXEC_PATH`
A new dynamic string token was added to the runtime linker.

- **Token:** `$EXEC_PATH`
- **Meaning:** Resolves to the directory of the main executable of the current namespace
- **Purpose:** Provides Apple-style stable main-binary-relative path resolution, unlike `$ORIGIN`, which is relative to the currently loading object

### Modified functions
- `dl-load.c`
  - `_dl_dst_substitute`
    - Added support for `$EXEC_PATH`
    - Resolves via `GL(dl_ns)[l->l_ns]._ns_loaded->l_executable_path`
    - Falls back to `main_map->l_origin` if `l_executable_path` is `NULL`
  - `_dl_dst_count`
    - Added `$EXEC_PATH` token recognition

---

## Runtime Linker State

### Added `l_executable_path` to `struct link_map`
The runtime linker now stores the main executable directory explicitly.

### Modified file
- `link.h`
  - `struct link_map`
    - Added:
      ```c
      char *l_executable_path;
      ```

### Purpose
- Keeps a stable reference to the main executable directory
- Enables `$EXEC_PATH` expansion across the whole dependency tree

---

## Main Executable Path Initialization

### Main executable directory is now captured at startup
The runtime linker stores the executable directory for later `$EXEC_PATH` expansion.

### Modified file
- `rtld.c`
  - `dl_main`
    - After `rtld_setup_main_map(main_map)`:
      - `main_map->l_executable_path = strdup(main_map->l_origin);`

### Purpose
- Provides deterministic executable-root-relative linking
- Keeps loader agnostic to bundle semantics

---

## dlopen / Dynamic Loading Policy

### Reworked `_dl_map_new_object`
The `dlopen` loading policy was adjusted for FBYO.

### Modified file
- `dl-load.c`
  - `_dl_map_new_object`

### New behavior

#### 1. Explicit paths (`name` contains `/`)
Examples:
- `/System/Frameworks/CoreSystem.frameworkb/CoreSystem`
- `$ORIGIN/foo.so`
- `$EXEC_PATH/foo.so`

Behavior:
- Expand DSTs using `expand_dynamic_string_token`
- Load the resulting path directly
- No search-path lookup is performed

#### 2. Bare names (`name` contains no `/`)
Example:
- `foo.so`

Search order:
1. `LD_LIBRARY_PATH`
2. `DT_RUNPATH` of the calling object
3. legacy `DT_RPATH` fallback
4. default system paths

### Removed / disabled behavior
- `ld.so.cache` lookup removed from this path
- no custom `$rpath/...` redirection kept
- no special framework logic added to glibc

---

## RUNPATH / RPATH Expansion

### `$EXEC_PATH` now works inside RUNPATH/RPATH entries
Verified through existing loader path decomposition flow.

### Relevant flow
- `cache_rpath`
- `decompose_rpath`
- `fillin_rpath`
- `expand_dynamic_string_token`
- `_dl_dst_substitute`

### Effect
Entries like:
```text
$EXEC_PATH/Frameworks
$ORIGIN