#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wchar.h>
#include <stdio.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    (void) instance;
    (void) previous;
    (void) command;
    (void) show;
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(NULL, path, 32768);
    if (!length || length >= 32768) {
        MessageBoxW(NULL, L"无法读取程序位置。", L"虚拟摄像头", MB_ICONERROR);
        return 1;
    }
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    *slash = L'\0';
    wchar_t executable[32768];
    wchar_t arguments[32768];
    int n = swprintf(executable, 32768, L"%ls\\scrcpy.exe", path);
    if (n < 0 || n >= 32768) return 1;
    n = swprintf(arguments, 32768, L"\"%ls\" --video-source=camera --no-audio", executable);
    if (n < 0 || n >= 32768) return 1;
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(executable, arguments, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, path, &startup, &process)) {
        MessageBoxW(NULL, L"无法启动摄像头程序，请确认程序文件完整。", L"虚拟摄像头", MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
