# Audition Data Preloader

A lightweight Windows utility that reads Audition's `.acv` resource files before you launch the game, warming the Windows file cache to help reduce stuttering caused by disk reads.

Results depend on available memory, storage performance, and how the game loads its resources. This utility does not guarantee higher FPS or eliminate all stuttering.

## How it works

The program checks for `Audition.exe` in the selected game folder, finds `.acv` files directly inside its `Data` folder, and reads them using a reusable **4 MiB buffer**.

Windows manages the resulting file cache. When the game requests data that is still cached, it may be served from memory instead of the disk.

The current version attempts to read all matching files. Reading 8 GiB of files does **not** allocate an 8 GiB application buffer or reserve an 8 GiB cache. Windows can reclaim cached memory when other applications need it.

## Features

- Select the game folder with `-path`.
- Use the current working directory when no path is specified.
- Support Unicode paths and quoted paths containing spaces.
- Check for `Audition.exe` before preloading.
- Display file names, file counts, per-file progress, overall progress, and elapsed time.
- Read resource files without modifying them.
- Use Win32 APIs and C runtime functions without the C++ STL.
- Provide optional commands to purge the system standby list or trim process working sets.

## Usage

Run the utility before launching Audition:

```bat
AuDataPreload.exe -path C:\Audition
```

For paths containing spaces, use double quotes:

```bat
AuDataPreload.exe -path "C:\Games\Audition TH"
```

The selected folder must contain both `Audition.exe` and the `Data` folder. Pass the game folder, not the `Data` folder itself.

Alternatively, run it from the game folder:

```bat
cd /d C:\Audition
C:\Tools\AuDataPreload.exe
```

Without `-path`, the program uses the shell's current working directory, which may differ from the folder containing the preloader executable. Relative paths are also resolved against the current working directory.

Wait for preloading to finish, then launch Audition normally. The utility does not launch the game automatically and does not need to remain running afterward.

Show usage information:

```bat
AuDataPreload.exe --help
```

The current help screen waits for Enter before closing.

## Command-line options

| Option | Aliases | Behavior |
| --- | --- | --- |
| No arguments | — | Preload from the current working directory. |
| `-path <folder>` | — | Preload from the specified game folder. |
| `--help` | `-h`, `/?` | Display usage information. |
| `--clearcache` | `-cc`, `/cc` | Request a system-wide standby-list purge. |
| `--emptyworking` | `-ew`, `/ew` | Request system-wide process working-set trimming. |

Use one mode per invocation. The maintenance options cannot be combined with `-path`.

## Optional memory maintenance

Run the following commands from an elevated Command Prompt (**Run as administrator**). They require `SeProfileSingleProcessPrivilege` and use the internal Windows `NtSetSystemInformation` interface. Availability and behavior may vary by Windows version.

### Clear the standby list

```bat
AuDataPreload.exe -cc
```

This requests removal of cached pages from the system standby list, including preloaded game data that is currently on that list. It affects the whole system, not only Audition, and does not delete files or clear all active memory. Standby memory can grow again during normal use.

Use this separately after closing the game if you want to discard standby cache. Running it immediately after preloading can undo the benefit of preloading.

### Trim process working sets

```bat
AuDataPreload.exe -ew
```

This requests trimming of process working sets across the system. Removed pages may move to standby or require additional processing before their memory can be reused. This is different from purging the standby list.

Working-set trimming is not a general gaming performance improvement. Applications may need to fault pages back into their working sets afterward, potentially causing additional disk activity and stuttering.

## Build

Use Visual Studio with the **Desktop development with C++** workload and a Windows SDK installed.

1. Create a C++ **Console App** project.
2. Add `AuDataPreload.cpp` and remove any generated source file containing another `main()` function.
3. Disable precompiled headers for this file if the project requires `pch.h`.
4. Select **Release / x64** and build.

Alternatively, use an **x64 Native Tools Command Prompt for Visual Studio**:

```bat
cl /nologo /W4 /EHsc /utf-8 /std:c++17 AuDataPreload.cpp /link /SUBSYSTEM:CONSOLE
```

The source links `User32.lib`, `Advapi32.lib`, and `ntdll.lib` through `#pragma comment` directives. No third-party protection SDK is required.

## Limitations

- Only `.acv` files directly inside `Data` are scanned; subfolders are not included.
- Cached data is not locked in RAM and may be evicted before the game uses it.
- Preloading does not perform the game's decompression, decryption, texture creation, or GPU uploads.
- The utility generates disk activity while it runs. Let it finish before starting the game.
- Reported progress measures bytes read, not bytes guaranteed to remain cached.
- An error exit code may indicate that only part of the requested data was read.

## Current implementation notes

- The program's RAM advisory recommends **at least 14 GB of RAM**. This is a recommendation, not a hard requirement or a guarantee that all resource data will remain cached. The warning does not limit the amount read: the program still attempts to preload all matching files.
- The memory-maintenance functions currently do not check the status returned by `NtSetSystemInformation`. Their exit codes must not be treated as proof that a purge or trim succeeded.
- The privilege helper currently checks only the Boolean result of `AdjustTokenPrivileges`; it does not check `ERROR_NOT_ALL_ASSIGNED` or restore the previous privilege state. These checks should be addressed before relying on the maintenance modes in automation.

## License

Licensed under the [MIT License](LICENSE).

The license applies to this project's code. Audition and its game assets belong to their respective owners. This is an unofficial utility and is not affiliated with or endorsed by the game's developers or publishers.
