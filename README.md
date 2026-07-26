# Odysia Beta 3

Odysia `0.3.0-beta3` is a C99 and GTK3 desktop source explorer for the Linux kernel. It builds a navigable map of functions, types, data structures, variables, build declarations, source locations, and documentation across the languages and source formats used by the kernel tree.

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
- Saves and reloads the complete parsed index as an SQLite database.
- Parses source files concurrently with a configurable GLib worker pool.
- Defaults parser threads to all detected logical CPUs, with CPU, 2x, and 3x presets.
- Streams kernel build output and shows the compilation unit currently being built.
- Keeps long indexing operations responsive with cancellation and staged progress reporting.

## Requirements

All platforms need:

- A C99 compiler
- GTK 3 development files
- GLib and GIO development files (normally installed with GTK3)
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

If MSYS2 asks you to close the terminal during the update, reopen the UCRT64 terminal, run `pacman -Syu` again, and then install the dependencies. Compile from that same UCRT64 terminal. The resulting program is `odysia.exe`.

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

Use an appropriate writable prefix or your platform's normal privilege mechanism when installing system-wide.

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

## Test

```sh
make check
```

The regression suite covers Linux-style C declarations, function pointers, structs, enums and enumerators, documentation extraction, call relations, supported kernel source formats, extensionless scripts, linker and grammar files, and equivalent output from single-threaded and multithreaded parsing.

## Something Cool

Odysia can turn a large kernel tree into a local, hyperlinkable symbol map and preserve that map as a portable SQLite file. The project itself has been developed collaboratively with GitHub Copilot, while source analysis remains local and deterministic: indexing does not require an AI service or upload the selected source tree.

## Notes and Limitations

- The C parser extracts declarations, scopes, nested calls, and relationships. Other language scanners are declaration-oriented and intentionally heuristic rather than compiler-exact semantic analyzers.
- Very complex macro-generated declarations may not produce complete symbols.
- GTK appearance and available symbolic icons vary by desktop theme and operating system.
- Cross-platform support depends on GTK3 and SQLite3 being available for the target platform.
- Beta 3 is a preview release; keep important SQLite indexes backed up when testing new builds.