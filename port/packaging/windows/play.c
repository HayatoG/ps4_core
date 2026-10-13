/* Play <Game>.exe: starts the game with the settings saved by the launcher, without opening
 * the launcher (it runs `<Game>.exe --play` from its own folder and waits for it, so Steam
 * and other front ends see the game running). Extra arguments are passed on. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show)
{
    (void)instance, (void)previous, (void)show;
    static wchar_t dir[32768], exe[32768], line[32768];
    DWORD length = GetModuleFileNameW(NULL, dir, 32768);
    if (length == 0 || length >= 32768)
        return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;
    /* "Play <Game>.exe" starts "<Game>.exe" next to it: the name comes from this file's own. */
    const wchar_t *self = slash ? slash + 1 : dir;
    const wchar_t *game = wcsncmp(self, L"Play ", 5) == 0 ? self + 5 : self;
    swprintf(exe, 32768, L"%ls\\%ls", dir, game);
    swprintf(line, 32768, L"\"%ls\" --play%ls%ls", exe, arguments && *arguments ? L" " : L"",
             arguments ? arguments : L"");

    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, dir, &startup, &process)) {
        MessageBoxW(NULL, L"The game launcher was not found next to this file.", game,
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
