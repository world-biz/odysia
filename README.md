# Odysia Beta 5

Odysia `0.5.0-beta5` is a C99 and GTK3 desktop source explorer for the Linux kernel. It builds a navigable map of functions, types, data structures, variables, build declarations, source locations, and documentation across the languages and source formats used by the kernel tree.

## Features

- Parses functions, structs, unions, enums, enumerators, typedefs, and global variables.
- Finds fields, parameters, local variables, function pointers, return types, and function calls.
- Recognizes C99 keywords and common Linux/GNU declaration decorations and kernel macros.
- Indexes Rust, assembly, Python, shell, Perl, AWK, device tree, Coccinelle, Lex/Yacc, linker scripts, Make/Kbuild, and Kconfig declarations.
- Discovers extensionless Python, shell, Perl, and AWK tools from their shebang lines.
- Displays symbols in separate `Symbol`, `Type`, and `Directory / Source` columns.
- Organizes the tree by programming language, then construct type, then `A` through `Z` or `#`, then symbol.
- Renders function names in C form, such as `schedule()`.
- Filters symbols live and sorts by name, source location, or symbol kind.
- Provides clickable links between related symbols and types.
- Colorizes C source snippets, including keywords, kernel-specific tokens, strings, comments, numbers, and preprocessor directives.
- Reads kernel-style `/** ... */` comments and `.rst`, `.txt`, and `.md` documentation.
- Marks entries with inline or external documentation using an exclamation icon in the `Docs` tree column.
- Saves and reloads the complete parsed index as an SQLite database.
- Uses a space-and-time application icon in GTK windows, taskbars, desktop launchers, and native application packages.
- Parses source files concurrently with a configurable GLib worker pool.
- Defaults parser threads to all detected logical CPUs, with CPU, 2x, and 3x presets.
- Lets users choose separate fonts for the detail and source panes and saves those choices across launches.
- Includes a Reset Defaults action for parser threads and both pane fonts.
- Streams kernel build output and shows the compilation unit currently being built.
- Keeps long indexing operations responsive with cancellation and staged progress reporting.
- Locks inputs, the symbol tree, both text panes, and all unrelated menu actions while indexing or loading an SQLite index; Stop remains available to cancel indexing.

## Supported Inputs

| Language or format | Files | Indexed constructs |
| --- | --- | --- |
| C | `.c`, `.h` | Functions, structs, unions, enums, enumerators, typedefs, globals, fields, parameters, locals, function pointers, return types, and calls |
| Rust | `.rs` | Functions, structs, enums, unions, traits, modules, aliases, constants, statics, and `macro_rules!` macros |
| Assembly | `.S`, `.s`, `.asm` | Kernel function-entry macros, assembly macros, and labels |
| Python | `.py`, recognized shebang scripts | Functions and classes |
| Shell | `.sh`, recognized shebang scripts | Functions |
| Perl | `.pl`, `.pm`, recognized shebang scripts | Subroutines and packages |
| AWK | `.awk`, recognized shebang scripts | Functions |
| Device tree | `.dts`, `.dtsi` | Labeled and unlabeled device nodes |
| Coccinelle | `.cocci` | Named semantic-patch rules |
| Lex and Yacc | `.l`, `.y` | Named grammar rules |
| Linker scripts | `.lds`, `.lds.S` | Labels and sections |
| Make and Kbuild | `Makefile*`, `Kbuild*`, `.mk` | Targets and variables |
| Kconfig | `Kconfig*` | Configuration and menu declarations |
| Documentation | `.rst`, `.txt`, `.md` | Symbol-matching documentation excerpts |

The C parser performs the deepest analysis, including scope-aware child declarations and call relationships. Other scanners are declaration-oriented and intentionally heuristic.

## Interface

### Symbol tree

The tree is organized as:

```text
Programming language
  Construct kind
    A-Z or #
      Symbol
        Owned fields, parameters, locals, or enumerators
```

The columns show the symbol-kind icon, documentation status, symbol name, type or kind, and source location. Functions render with parentheses, such as `schedule()`. An exclamation icon in the `Docs` column identifies entries with inline or matching external documentation.

The live filter searches names, source paths, signatures, type text, documentation, and symbol kinds. The sort selector supports name, source line, and symbol kind.

### Details and source

Selecting an entry displays its signature or type, source location, inline documentation, children, call/type/alias/keyword relationships, and matching external documentation. Related symbols and types are clickable. The source pane displays the indexed snippet and applies C/kernel-aware highlighting to comments, strings, numbers, directives, C keywords, and common kernel tokens.

### Menus

`File` provides source-tree selection, SQLite open/save, Clear Data, and Quit. Clear Data cancels active work and removes loaded index data from the application.

`Actions` provides Index, Build Kernel, Stop, and Settings. Stop cancels active indexing or terminates an active monitored build. Settings configures the bounded parser worker pool and separate fonts for the detail and source panes. Reset Defaults selects one thread per detected logical CPU, `Sans 11` for details, and `Monospace 11` for source; choose Apply to save the reset values.

`Help` provides package version and application information through About.

## Indexing and Performance

Indexing runs outside the GTK main thread and reports separate discovery, source parsing, documentation parsing, and tree-construction stages. Source files are parsed by a bounded GLib worker pool. Each worker builds an isolated partial index, which is merged safely into the result.

The default parser count is the number of detected logical CPUs. Settings includes CPU, 2x, and 3x presets plus a numeric control. Higher counts can help I/O-heavy trees but are not guaranteed to improve performance.

Tree construction is incremental on the GTK main loop, and both indexing and application shutdown use cancellation-aware lifetimes to keep the interface responsive.

While source indexing, SQLite loading, or loaded-tree construction is active, inputs, tree rows, text panes, and unrelated menu actions are disabled. Stop remains available during source indexing and its tree construction, allowing cancellation without unlocking other controls. Progress bars and status text remain visible. Controls are restored only after the operation finishes, is cancelled, or encounters an error.

## SQLite Persistence

`File > Save SQLite Index` writes the complete current index to a local SQLite database. Saving to an existing file transactionally replaces its Odysia tables. The database stores:

- Source-tree root metadata.
- Symbols, display names, kinds, source paths, and line numbers.
- Signatures, types, snippets, and inline documentation.
- Ordered parent-child relationships.
- Call, type, alias, and keyword relationships.
- Parsed external documentation files.

`File > Open SQLite Index` reads the database on a worker thread, restores those records, and rebuilds the language-organized tree. Persisted source metadata preserves language grouping. The original source tree is not required to inspect saved data, although Build Kernel still requires a valid configured tree.

## Saved Settings

Odysia saves parser thread count, detail-pane font, and source-pane font in a cross-platform GLib key file:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/odysia/settings.ini   Linux and BSD
$HOME/Library/Application Support/odysia/settings.ini  macOS
```

On Windows the file is stored below the GLib user configuration directory. Settings are loaded when the application starts and written when Apply is selected. Reset Defaults changes the dialog values to detected CPU count, `Sans 11`, and `Monospace 11`; Apply persists them.

## Requirements

All platforms need:

- A C99 compiler
- GTK 3 development files
- GLib and GIO development files (normally installed with GTK3)
- `glib-compile-resources` (normally included with GLib development tools)
- SQLite 3 development files
- `pkg-config`
- GNU Autoconf and Automake when building from a source checkout
- GNU Make or a compatible `make`

## Linux

### Debian and Ubuntu

```sh
sudo apt update
sudo apt install build-essential autoconf automake pkg-config libgtk-3-dev libsqlite3-dev
```

### Fedora and RHEL-family distributions

```sh
sudo dnf install gcc make autoconf automake pkgconf-pkg-config gtk3-devel sqlite-devel
```

### Arch Linux and Manjaro

```sh
sudo pacman -S --needed base-devel autoconf automake pkgconf gtk3 sqlite
```

### openSUSE

```sh
sudo zypper install gcc make autoconf automake pkg-config gtk3-devel sqlite3-devel
```

After installing dependencies, follow the compilation instructions below.

## macOS

Install the Xcode Command Line Tools:

```sh
xcode-select --install
```

Using Homebrew:

```sh
brew install autoconf automake pkg-config gtk+3 sqlite
```

Or using MacPorts:

```sh
sudo port install autoconf automake pkgconfig gtk3 sqlite3
```

Then compile normally. Launching `./odysia` from Terminal ensures the package-manager GTK libraries are available in the expected environment.

Create a Finder-launchable application bundle with the native macOS icon:

```sh
make macos-bundle
open dist/Odysia.app
```

## Windows

The supported native Windows build environment is MSYS2 UCRT64.

1. Install MSYS2 from [msys2.org](https://www.msys2.org/).
2. Open the **MSYS2 UCRT64** terminal.
3. Update packages and install dependencies:

```sh
pacman -Syu
pacman -S --needed make autoconf automake \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-gtk3 \
  mingw-w64-ucrt-x86_64-sqlite3
```

If MSYS2 asks you to close the terminal during the update, reopen the UCRT64 terminal, run `pacman -Syu` again, and then install the dependencies. Compile from that same UCRT64 terminal. The resulting program is `odysia.exe`, with `assets/odysia.ico` embedded by MinGW `windres`.

## BSD

### FreeBSD

```sh
pkg install autoconf automake pkgconf gtk3 sqlite3 gmake
```

Use `gmake` instead of `make` in the commands below.

### OpenBSD

Install the equivalent packages with `pkg_add`:

```sh
pkg_add autoconf automake pkgconf gtk+3 sqlite3 gmake
```

Package names or version-suffixed Autotools commands can vary by BSD release. Use the version selected by the package manager and run GNU Make as `gmake`.

## Compile

### From a source checkout

Generate the configure script, detect dependencies, compile, and test:

```sh
autoreconf -fi
./configure
make
make check
```

On systems where GNU Make is named `gmake`:

```sh
autoreconf -fi
./configure
gmake
gmake check
```

### From a release archive

Release archives include the generated `configure` script, so `autoreconf` is not required:

```sh
./configure
make
make check
```

Useful configure options include:

```sh
./configure --prefix=/usr/local
./configure CFLAGS="-O2 -g"
```

Install after a successful build:

```sh
make install
```

The default prefix is `/usr/local`. Override it at configure time when needed:

```sh
./configure --prefix="$HOME/.local"
make
make install
```

Use `DESTDIR` for staged packaging:

```sh
make install DESTDIR=/tmp/odysia-package
```

On Linux, installation also places the desktop launcher and icon in the standard `share/applications` and `share/icons/hicolor` locations. Automake installs `odysia.1` in `${mandir}/man1`, normally `/usr/local/share/man/man1` with the default prefix.

Remove installed files with:

```sh
make uninstall
```

Use an appropriate writable prefix or your platform's normal privilege mechanism when installing system-wide.

## Manual Page

After installation, read the complete command and interface reference with:

```sh
man 1 odysia
```

Before installation, render the source page with `mandoc odysia.1` or an implementation of `man` that supports reading local files. The manual is distributed in release archives and installed through Automake's standard `man1` directory handling.

## Run

```sh
./odysia
```

On Windows/MSYS2:

```sh
./odysia.exe
```

1. Select a Linux kernel or supported source tree from `File > Select Source Tree`.
2. Choose `Actions > Settings` to adjust parser threads if needed.
3. Choose `Actions > Index`.
4. Filter or sort symbols, select a row, and follow links in the detail pane.
5. Save the result with `File > Save SQLite Index` for later use.
6. Reload a saved index with `File > Open SQLite Index`.

The CPU preset uses one parser thread per detected logical CPU. The 2x and 3x presets are useful for experimenting with workloads that spend time waiting on file I/O; higher values are not guaranteed to be faster on every machine.

## Kernel Build Monitor

`Actions > Build Kernel` runs `make -j1 V=1` in the selected source tree, streams output, and displays the current compilation unit. This action requires a configured Linux kernel tree and the appropriate host or cross-compilation toolchain. Source browsing and indexing do not require a kernel build toolchain.

The build dialog merges standard error into standard output and provides Stop and Close controls. Odysia monitors a build; it does not configure the kernel or select a cross compiler on the user's behalf.

## Platform Integration

- GTK embeds the application PNG so windows and taskbars have an icon independent of the working directory.
- Linux installation includes a freedesktop desktop entry and hicolor icon.
- Windows/MSYS2 builds use `windres` to embed the native ICO in `odysia.exe`.
- `make macos-bundle` creates `dist/Odysia.app` with a native ICNS and `Info.plist`.
- All platforms use the application identifier `org.odysia.Odysia`.

## Test

```sh
make check
```

The regression suite covers Linux-style C declarations, function pointers, structs, enums and enumerators, documentation extraction, call relations, supported kernel source formats, extensionless scripts, linker and grammar files, and equivalent output from single-threaded and multithreaded parsing. It also saves an index to SQLite twice, reloads it, and verifies symbols, children, relations, source metadata, language classification, and documentation survive the round trip.

For a full release-build and installation check:

```sh
make distcheck
```

## Project Layout

| Path | Purpose |
| --- | --- |
| `main.c` | Application entry point |
| `src/indexer.c`, `src/indexer.h` | Discovery, parsing, relationships, threading, documentation, and SQLite persistence |
| `src/ui.c`, `src/ui.h` | GTK interface, filtering, tree construction, details, highlighting, settings, and build monitor |
| `tests/test_indexer.c` | Parser, threading, language, and SQLite round-trip regression tests |
| `tests/fixtures/linux_sample` | Multi-language Linux-style test tree |
| `assets` | Embedded PNG, SVG master, Windows ICO/resource, macOS ICNS, and GLib resource manifest |
| `data` | Linux desktop entry and macOS bundle metadata |
| `odysia.1` | Section 1 manual page |
| `configure.ac`, `Makefile.am` | Autotools configuration and install rules |

## Privacy and Data

Parsing, filtering, source display, documentation extraction, and SQLite persistence happen locally. Odysia does not require network access, an account, telemetry, or an AI service, and it does not upload the selected source tree.

SQLite index files can contain source snippets and documentation from the selected tree. Treat exported databases according to the source tree's confidentiality requirements.

The settings file contains only the parser thread count and selected font descriptions.

## Troubleshooting

- If `configure` cannot find GTK3 or SQLite, verify that their development packages are installed and visible to `pkg-config`.
- If installed Linux icons do not appear immediately, refresh the desktop environment's icon cache or log out and back in.
- If `man odysia` does not resolve a nonstandard prefix, add that prefix's `share/man` directory to `MANPATH`.
- If Build Kernel fails immediately, run `make -j1 V=1` in the selected tree directly to verify its configuration and toolchain.
- If indexing is slower with 2x or 3x threads, return to the CPU preset; storage and memory bandwidth can become the limiting factors.

## Something Cool

Odysia can turn a large kernel tree into a local, hyperlinkable symbol map and preserve that map as a portable SQLite file. The project itself has been developed collaboratively with GitHub Copilot, while source analysis remains local and deterministic: indexing does not require an AI service or upload the selected source tree.

## Notes and Limitations

- The C parser extracts declarations, scopes, nested calls, and relationships. Other language scanners are declaration-oriented and intentionally heuristic rather than compiler-exact semantic analyzers.
- Very complex macro-generated declarations may not produce complete symbols.
- GTK appearance and available symbolic icons vary by desktop theme and operating system.
- Cross-platform support depends on GTK3 and SQLite3 being available for the target platform.
- SQLite indexes contain persisted source-derived data and are not intended as a stable interchange format with unrelated tools.
- Beta 5 is a preview release; keep important SQLite indexes backed up when testing new builds.