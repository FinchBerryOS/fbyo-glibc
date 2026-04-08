<p align="center">
  <img src="https://raw.githubusercontent.com/FinchBerryOS/.github/refs/heads/main/profile/assets/Glibc_github.png" alt="FinchBerryOS Logo" width="400">
</p>

This directory contains the sources of the FBYO C Library, a heavily modified
fork of the GNU C Library (glibc) designed specifically for FBYO.
See the file `version.h` for the exact release version.

The FBYO C Library is the standard system C library for FBYO. It works with
the Linux kernel to implement the operating system behavior, but significantly
modifies the runtime dynamic linker (`ld.so`) to support deterministic,
bundle-friendly application layout and Apple-style executable-root-relative
linking semantics.

# FBYO EXCLUSIVE MODIFICATIONS: DETERMINISTIC DYLD-STYLE LINKING

While standard Linux systems typically rely on SONAME-oriented dependency
resolution, global library search paths, `ld.so.cache`, and path conventions
such as `/lib` and `/usr/lib`, the FBYO runtime linker is tuned for a more
deterministic model:

- direct path-based loading for explicit library references
- stable executable-root-relative resolution
- reduced reliance on global library lookup
- support for movable application bundles and nested framework trees
- strict separation between low-level path resolution and higher-level bundle semantics

The runtime linker itself remains **filesystem-agnostic**: it does not parse
bundle manifests or detect application package structures. It only operates on
explicit paths and dynamic string token expansion. Bundle and framework
semantics are intentionally delegated to higher-level FBYO frameworks.

## Key Modifications to the Dynamic Linker

### 1. Main Executable Path Tracking (`struct link_map` extension)

The linker now stores the directory of the main executable explicitly in the
linker map:

- added field: `l_executable_path`

This value is initialized during startup in `rtld.c` and remains stable for the
entire process lifetime. Unlike `$ORIGIN`, which depends on the currently
loading object, this path always refers to the directory of the original main
executable.

This is the basis for executable-root-relative linking semantics comparable to
Apple's `@executable_path`.

### 2. New Dynamic String Token: `$EXEC_PATH`

A new Dynamic String Token (DST) was added to the loader:

- **`$EXEC_PATH`**
  - resolves to the directory of the main executable of the current namespace
  - remains constant across the full dependency tree
  - falls back to the main executable's `l_origin` if no dedicated executable path is available

This token is supported by the same DST substitution pipeline already used for
standard glibc tokens such as:

- `$ORIGIN`
- `$LIB`
- `$PLATFORM`

### 3. DST Expansion Extended in the Runtime Linker

The DST handling pipeline was modified so that `$EXEC_PATH` is recognized and
expanded in the same places where glibc normally expands dynamic string tokens.

Modified internal functions include:

- `_dl_dst_count`
- `_dl_dst_substitute`
- `expand_dynamic_string_token`

This means `$EXEC_PATH` can now be used in:

- explicit runtime loading paths
- `DT_RUNPATH`
- `DT_RPATH`
- other dynamic linker string expansion contexts that already support DSTs

### 4. Startup Initialization of Executable Root

During runtime linker startup, the main executable map is initialized with a
persistent executable directory:

- `main_map->l_executable_path = strdup(main_map->l_origin);`

This produces a stable executable-root anchor independent of the currently
loading dependency.

### 5. Revised `dlopen()` Resolution Policy

The runtime loading path in `_dl_map_new_object` was adjusted to follow a more
deterministic FBYO policy.

#### Explicit paths (`name` contains `/`)
Examples:
- `/System/Frameworks/CoreSystem.frameworkb/CoreSystem`
- `$ORIGIN/libfoo.so`
- `$EXEC_PATH/libbar.so`
- `plugins/libbaz.so`

Behavior:
- dynamic string tokens are expanded
- the resulting path is loaded directly
- no search path resolution is performed

This makes explicit `dlopen()` calls path-authoritative.

#### Bare names (`name` contains no `/`)
Example:
- `dlopen("libfoo.so", ...)`

Behavior:
The object is searched in the following order:

1. `LD_LIBRARY_PATH`
2. `DT_RUNPATH` of the calling object
3. legacy `DT_RPATH` fallback
4. default runtime search directories

Notably:

- `ld.so.cache` lookup is intentionally not part of the FBYO `dlopen()` policy in this path
- no implicit current-working-directory lookup is introduced
- no framework-specific magic name resolution is added to glibc

This keeps runtime loading deterministic while still supporting traditional
search-based `dlopen("name.so")` usage when required.

### 6. `RUNPATH` / `RPATH` Expansion Includes `$EXEC_PATH`

The runtime linker's path decomposition flow now naturally supports
`$EXEC_PATH` within `DT_RUNPATH` and `DT_RPATH` entries because those entries
are processed through the same DST expansion path.

Relevant internal flow:

- `cache_rpath`
- `decompose_rpath`
- `fillin_rpath`
- `expand_dynamic_string_token`
- `_dl_dst_substitute`

As a result, entries such as:

```text
$EXEC_PATH/Frameworks
$ORIGIN
$EXEC_PATH/../Libraries