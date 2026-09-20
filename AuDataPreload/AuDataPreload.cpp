// AuDataPreload.cpp
// Visual Studio: Console application, Release/x64, C++11 or newer.
// Disable precompiled headers for this file if your project requires pch.h.
// Developer Command Prompt:
// cl /nologo /W4 /EHsc /utf-8 AuDataPreload.cpp /link /SUBSYSTEM:CONSOLE
//
// Usage:
// AuDataPreload.exe -path C:\Audition
// AuDataPreload.exe -path "C:\Games\Audition TH"
// AuDataPreload.exe                 (uses the current working directory)
// AuDataPreload.exe --help
// Relative paths are resolved against the current working directory.
//
// No std/STL. Only Win32 and C runtime console output.
// Add your Themida/Code Virtualizer SDK header below to enable its markers.
// The unprotected build works without the SDK; no dummy markers are defined.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <Windows.h>
#include <stdio.h>
#include <conio.h>
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "ntdll.lib")

typedef enum _SYSTEM_MEMORY_LIST_COMMAND
{
    MemoryCaptureAccessedBits,
    MemoryCaptureAndResetAccessedBits,
    MemoryEmptyWorkingSets,
    MemoryFlushModifiedList,
    MemoryPurgeStandbyList,
    MemoryPurgeLowPriorityStandbyList,
    MemoryCommandMax
} SYSTEM_MEMORY_LIST_COMMAND;

extern "C" LONG NTAPI NtSetSystemInformation(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength
);

// Synchronous: call before entering the game, or from a worker thread.
// Returns 0 on success, 1 on error. Does not launch Audition.exe.
// RAM information is displayed; the read cap is disabled in this version.
// gamePath == nullptr selects the current working directory.
// The function does not change the process current directory.
// Cache residency is controlled by Windows and is not guaranteed.
int PreloadGameResources(LPCWSTR gamePath = nullptr)
{
    struct FILE_ITEM
    {
        FILE_ITEM* next;
        ULONGLONG size;
        WCHAR name[MAX_PATH];
    };

    const ULONGLONG GB = 1024ull * 1024ull * 1024ull;
    const DWORD BUFFER_SIZE = 4u * 1024u * 1024u;
    const DWORD PATH_CAPACITY = 32768;

    HANDLE heap = GetProcessHeap();
    HANDLE search = INVALID_HANDLE_VALUE;
    HANDLE file = INVALID_HANDLE_VALUE;
    FILE_ITEM* first = nullptr;
    FILE_ITEM* last = nullptr;
    LPWSTR path = nullptr;
    LPVOID buffer = nullptr;
    DWORD error = ERROR_SUCCESS;
    ULONGLONG totalSize = 0;
    ULONGLONG targetBytes = 0;
    ULONGLONG totalRead = 0;
    DWORD fileCount = 0;
    DWORD attempted = 0;
    DWORD completed = 0;
    DWORD partial = 0;
    DWORD failed = 0;

    do
    {
        // 1. Resolve the selected folder, then check Audition.exe before Data.
        path = static_cast<LPWSTR>(
            HeapAlloc(heap, 0, PATH_CAPACITY * sizeof(WCHAR)));
        if (path == nullptr)
        {
            error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }

        if (gamePath != nullptr && gamePath[0] == L'\0')
        {
            error = ERROR_INVALID_PARAMETER;
            break;
        }

        const DWORD currentLength = gamePath != nullptr
            ? GetFullPathName(gamePath, PATH_CAPACITY, path, nullptr)
            : GetCurrentDirectory(PATH_CAPACITY, path);
        if (currentLength == 0)
        {
            error = GetLastError();
            break;
        }
        if (currentLength >= PATH_CAPACITY ||
            currentLength + MAX_PATH + 8 >= PATH_CAPACITY)
        {
            error = ERROR_FILENAME_EXCED_RANGE;
            break;
        }

        SIZE_T offset = currentLength;
        if (path[offset - 1] != L'\\' && path[offset - 1] != L'/')
            path[offset++] = L'\\';

        lstrcpy(path + offset, L"Audition.exe");
        const DWORD attributes = GetFileAttributes(path);
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            error = GetLastError();
            printf("Cannot access Audition.exe. Win32 error: %lu\n", error);
            MessageBox(nullptr,
                (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                ? L"Audition.exe was not found in the selected game folder.\n"
                L"Use -path to select the folder containing Audition.exe "
                L"and the Data folder."
                : L"Cannot access Audition.exe in the selected game folder.\n"
                L"Please check the folder and access permissions.",
                L"Preload Error", MB_OK | MB_ICONERROR);
            break;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            error = ERROR_FILE_NOT_FOUND;
            MessageBox(nullptr,
                L"Audition.exe is a directory, not a game executable file.",
                L"Preload Error", MB_OK | MB_ICONERROR);
            break;
        }
        printf("Audition.exe found.\n\n");

        // 2. Query RAM and scan the selected folder\Data only (no recursion).
        MEMORYSTATUSEX memory = {};
        memory.dwLength = sizeof(memory);
        if (!GlobalMemoryStatusEx(&memory))
        {
            error = GetLastError();
            break;
        }
        printf("RAM usable   : %.2f GiB\n",
            static_cast<double>(memory.ullTotalPhys) / GB);
        printf("RAM available: %.2f GiB\n\n",
            static_cast<double>(memory.ullAvailPhys) / GB);

        lstrcpy(path + offset, L"Data\\");
        const SIZE_T nameOffset = offset + 5;
        lstrcpy(path + nameOffset, L"*");
        WIN32_FIND_DATAW data = {};
        search = FindFirstFile(path, &data);
        if (search == INVALID_HANDLE_VALUE)
        {
            error = GetLastError();
            printf("Cannot scan the selected game folder\\Data.\n");
            break;
        }
        printf("Scanning the selected game folder\\Data\\*.acv ...\n");

        // Snapshot the names and sizes for the total progress denominator.
        for (;;)
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                LPCWSTR extension = nullptr;
                for (LPCWSTR p = data.cFileName; *p; ++p)
                    if (*p == L'.') extension = p;

                if (extension != nullptr && lstrcmpi(extension, L".acv") == 0)
                {
                    FILE_ITEM* item = static_cast<FILE_ITEM*>(
                        HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(FILE_ITEM)));
                    if (item == nullptr)
                    {
                        error = ERROR_NOT_ENOUGH_MEMORY;
                        break;
                    }
                    item->size =
                        (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
                        data.nFileSizeLow;
                    if (item->size > (~0ull - totalSize) || fileCount == MAXDWORD)
                    {
                        HeapFree(heap, 0, item);
                        error = ERROR_ARITHMETIC_OVERFLOW;
                        break;
                    }
                    lstrcpy(item->name, data.cFileName);
                    if (last != nullptr) last->next = item;
                    else first = item;
                    last = item;
                    totalSize += item->size;
                    ++fileCount;
                }
            }
            if (!FindNextFile(search, &data))
            {
                const DWORD findError = GetLastError();
                if (findError != ERROR_NO_MORE_FILES) error = findError;
                break;
            }
        }
        FindClose(search);
        search = INVALID_HANDLE_VALUE;
        if (error != ERROR_SUCCESS) break;
        if (fileCount == 0)
        {
            printf("No .acv files found.\n");
            error = ERROR_FILE_NOT_FOUND;
            break;
        }

        // 3. User-requested RAM policy. Never read beyond total file size.
        targetBytes = totalSize;

        if (memory.ullTotalPhys <= 14ull * GB)
            MessageBox(GetForegroundWindow(), L"If you have less than 14GB of RAM, the program's performance may not be optimal.\n\nWe recommend having at least 14GB of RAM.", L"Message", MB_ICONEXCLAMATION);

        printf("Files        : %lu\n", fileCount);
        printf("Total size   : %.3f GiB\n", static_cast<double>(totalSize) / GB);
        printf("Read target  : %.3f GiB\n\n", static_cast<double>(targetBytes) / GB);
        if (targetBytes == 0)
        {
            printf("All matching files are empty. Nothing to read.\n");
            break;
        }

        buffer = HeapAlloc(heap, 0, BUFFER_SIZE);
        if (buffer == nullptr)
        {
            error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        const ULONGLONG started = GetTickCount64();

        // 4. Normal buffered reads warm the Windows file cache.
        // Enumeration order is used; no sorting or resource prioritization.
        for (FILE_ITEM* item = first;
            item != nullptr && totalRead < targetBytes;
            item = item->next)
        {
            ++attempted;
            char displayName[MAX_PATH * 3 + 1] = {};
            if (WideCharToMultiByte(CP_UTF8, 0, item->name, -1,
                displayName, static_cast<int>(sizeof(displayName)),
                nullptr, nullptr) == 0)
                lstrcpyA(displayName, "(filename unavailable)");

            printf("[%lu/%lu] Preloading: %s\n", attempted, fileCount, displayName);
            fflush(stdout);
            lstrcpy(path + nameOffset, item->name);
            file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                const DWORD openError = GetLastError();
                printf("  Open failed: %lu\n\n", openError);
                ++failed;
                if (error == ERROR_SUCCESS) error = openError;
                continue;
            }

            ULONGLONG fileRead = 0;
            ULONGLONG lastLog = 0;
            DWORD readError = ERROR_SUCCESS;
            // Read the scanned size. Do not follow later file growth.
            while (fileRead < item->size && totalRead < targetBytes)
            {
                ULONGLONG amount = item->size - fileRead;
                if (amount > BUFFER_SIZE) amount = BUFFER_SIZE;
                const ULONGLONG remaining = targetBytes - totalRead;
                if (amount > remaining) amount = remaining;
                DWORD received = 0;
                if (!ReadFile(file, buffer, static_cast<DWORD>(amount),
                    &received, nullptr))
                {
                    readError = GetLastError();
                    break;
                }
                if (received == 0)
                {
                    readError = ERROR_HANDLE_EOF;
                    break;
                }
                fileRead += received;
                totalRead += received;
                const ULONGLONG now = GetTickCount64();
                if (now - lastLog >= 100 || fileRead == item->size ||
                    totalRead == targetBytes)
                {
                    printf("\r  File: %6.2f%% | Total: %6.2f%% | %.3f / %.3f GiB   ",
                        100.0 * static_cast<double>(fileRead) / item->size,
                        100.0 * static_cast<double>(totalRead) / targetBytes,
                        static_cast<double>(totalRead) / GB,
                        static_cast<double>(targetBytes) / GB);
                    fflush(stdout);
                    lastLog = now;
                }
            }
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            if (readError != ERROR_SUCCESS)
            {
                ++failed;
                printf("\n  Read failed: %lu\n\n", readError);
                if (error == ERROR_SUCCESS) error = readError;
            }
            else if (fileRead == item->size)
            {
                ++completed;
                printf("\n  Completed.\n\n");
            }
            else
            {
                ++partial;
                printf("\n  Partial: read target reached.\n\n");
            }
        }

        printf("---------- Result ----------\n");
        printf("Completed files : %lu\n", completed);
        printf("Partial files   : %lu\n", partial);
        printf("Failed files    : %lu\n", failed);
        printf("Not attempted   : %lu\n", fileCount - attempted);
        printf("Read            : %.3f / %.3f GiB (%.2f%%)\n",
            static_cast<double>(totalRead) / GB,
            static_cast<double>(targetBytes) / GB,
            100.0 * static_cast<double>(totalRead) / targetBytes);
        printf("Time            : %.2f seconds\n",
            (GetTickCount64() - started) / 1000.0);
    } while (false);

    // All ordinary exits pass cleanup and the END marker before return.
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    if (buffer != nullptr) HeapFree(heap, 0, buffer);
    if (path != nullptr) HeapFree(heap, 0, path);
    while (first != nullptr)
    {
        FILE_ITEM* next = first->next;
        HeapFree(heap, 0, first);
        first = next;
    }

    if (error == ERROR_SUCCESS)
    {
        if (FindWindow(L"DLightClass", nullptr) == NULL)
        {
            if (MessageBox(GetForegroundWindow(), L"Would you like to launch the game with TAC2?", L"Message", MB_ICONINFORMATION | MB_YESNO) == IDYES)
            {
                WCHAR szPath[260]{}, szCurrentDir[260]{};
                GetCurrentDirectory(260, szCurrentDir);
                swprintf_s(szPath, __crt_countof(szPath), L"%s\\TAC2Loader.exe", szCurrentDir);
                DWORD attributes = GetFileAttributes(szPath);
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == false)
                {
                    STARTUPINFO lpStartupInfo;
                    ZeroMemory(&lpStartupInfo, sizeof(STARTUPINFO));
                    lpStartupInfo.cb = sizeof(STARTUPINFO);
                    PROCESS_INFORMATION lpProcessInformation;
                    ZeroMemory(&lpProcessInformation, sizeof(PROCESS_INFORMATION));
                    if (CreateProcess(NULL, szPath, NULL, NULL, TRUE, NULL, NULL, szCurrentDir, &lpStartupInfo, &lpProcessInformation))
                    {
                        CloseHandle(lpProcessInformation.hProcess);
                        CloseHandle(lpProcessInformation.hThread);
                    }
                }
            }
        }
    }
    else
        printf("\nFinished with Win32 error: %lu\n", error);
    return error == ERROR_SUCCESS ? 0 : 1;
}

BOOL SetSeDebugPrivilege(LPCSTR lpPrivilegeName)
{
    BOOL blResult = FALSE;
    HANDLE hToken;
    TOKEN_PRIVILEGES tTokenPrivileges{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken) == FALSE)
    {
        printf("SetSeDebugPrivilege: OpenProcessToken FALSE\n");
    }
    else
    {
        if (LookupPrivilegeValueA(NULL, lpPrivilegeName, &tTokenPrivileges.Privileges[0].Luid) == FALSE)
        {
            printf("SetSeDebugPrivilege: LookupPrivilegeValue FALSE\n");
        }
        else
        {
            tTokenPrivileges.PrivilegeCount = 1;
            tTokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (AdjustTokenPrivileges(hToken, FALSE, &tTokenPrivileges, sizeof(tTokenPrivileges), NULL, NULL) == FALSE)
            {
                printf("SetSeDebugPrivilege: AdjustTokenPrivileges FALSE\n");
            }
            else
            {
                blResult = TRUE;
            }
        }
        CloseHandle(hToken);
    }
   return blResult;
}

DWORD Memory_ClearStandbyCache()
{
    DWORD dwResult = -1;
    WCHAR szMessage[1024]{};
    DWORD dwLastError = 0;
    if (SetSeDebugPrivilege("SeProfileSingleProcessPrivilege"))
    {
        dwResult = ERROR_SUCCESS;
        SYSTEM_MEMORY_LIST_COMMAND smlCmd = MemoryPurgeStandbyList;
        LONG NtStatus_FlushModifie = NtSetSystemInformation(80, &smlCmd, sizeof(smlCmd));
    }
    else
    {
        dwResult = GetLastError();
        ZeroMemory(szMessage, sizeof(szMessage));
        printf("SetSeDebugPrivilege Failure [ERROR CODE 0x%08X (%u)]\n", dwLastError, dwLastError);
    }
    return dwResult;
}

DWORD Memory_EmptyWorkingSet()
{
    DWORD dwResult = -1;
    WCHAR szMessage[1024]{};
    DWORD dwLastError = 0;
    if (SetSeDebugPrivilege("SeProfileSingleProcessPrivilege"))
    {
        dwResult = ERROR_SUCCESS;
        SYSTEM_MEMORY_LIST_COMMAND smlCmd = MemoryEmptyWorkingSets;
        LONG NtStatus_FlushModifie = NtSetSystemInformation(80, &smlCmd, sizeof(smlCmd));
    }
    else
    {
        dwResult = GetLastError();
        ZeroMemory(szMessage, sizeof(szMessage));
        printf("SetSeDebugPrivilege Failure [ERROR CODE 0x%08X (%u)]\n", dwLastError, dwLastError);
    }
    return dwResult;
}

int wmain(int argc, wchar_t* argv[])
{
    BOOL ownConsole = FALSE;
    BOOL attachedConsole = FALSE;
    if (GetConsoleWindow() == nullptr)
    {
        attachedConsole = AttachConsole(ATTACH_PARENT_PROCESS);
        if (!attachedConsole) ownConsole = AllocConsole();
    }

    const BOOL hasConsole = GetConsoleWindow() != nullptr;
    UINT previousCodePage = 0;
    BOOL codePageChanged = FALSE;
    if (hasConsole)
    {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
        previousCodePage = GetConsoleOutputCP();
        codePageChanged = SetConsoleOutputCP(CP_UTF8);
        SetConsoleTitle(L"Audition Data Preloader");
    }

    int result = 0;
    LPCWSTR gamePath = nullptr;
    BOOL showHelp = FALSE;
    BOOL validArguments = TRUE;

    // wmain uses the CRT Unicode command-line parser, including quoted paths.
    // Accept one optional -path <folder>; do not modify the current directory.
    if (argc == 2 &&
        (lstrcmpi(argv[1], L"--help") == 0 ||
         lstrcmpi(argv[1], L"-h") == 0 ||
         lstrcmpi(argv[1], L"/?") == 0))
    {
        showHelp = TRUE;
    }
    else if (argc == 2 &&
        (lstrcmpi(argv[1], L"--clearcache") == 0 ||
            lstrcmpi(argv[1], L"-cc") == 0 ||
            lstrcmpi(argv[1], L"/cc") == 0))
    {
        return Memory_ClearStandbyCache();
    }
    else if (argc == 2 &&
        (lstrcmpi(argv[1], L"--emptyworking") == 0 ||
            lstrcmpi(argv[1], L"-ew") == 0 ||
            lstrcmpi(argv[1], L"/ew") == 0))
    {
        return Memory_EmptyWorkingSet();
    }
    else
    {
        for (int i = 1; i < argc; ++i)
        {
            if (lstrcmpi(argv[i], L"-path") != 0 || gamePath != nullptr)
            {
                validArguments = FALSE;
                break;
            }
            if (i + 1 >= argc || argv[i + 1][0] == L'\0' ||
                argv[i + 1][0] == L'-')
            {
                validArguments = FALSE;
                break;
            }
            gamePath = argv[++i];
        }
    }

    if (showHelp || !validArguments)
    {
        if (!validArguments)
        {
            printf("Invalid arguments: use one -path followed by a folder.\n\n");
            result = 1;
        }
        printf("Usage:\n"
            "  AuDataPreload.exe\n"
            "  AuDataPreload.exe -path C:\\Audition\n"
            "  AuDataPreload.exe -path \"C:\\Games\\Audition TH\"\n"
            "Without -path, the current working directory is used.\n"
            "The selected folder must contain Audition.exe and Data.\n"
            "Put paths containing spaces in double quotes.\n\n"
            "  AuDataPreload.exe -cc\n"
            "Clear Standby Cache, Remove data preload\n"
            "  AuDataPreload.exe -ew\n"
            "Set Memory Working, EmptyWorkingSet\n"
        );
        getchar();
    }
    else
    {
        result = PreloadGameResources(gamePath);
    }

    // Keep a newly allocated window open so a double-click user can read logs.
    // Do not pause when using an existing CMD console.
    if (ownConsole)
    {
        printf("\nPress Enter to close...\n");
        fflush(stdout);
        (void)getchar();
    }
    if (codePageChanged && previousCodePage != 0)
        SetConsoleOutputCP(previousCodePage);
    if (ownConsole || attachedConsole) FreeConsole();

    return result;
}
