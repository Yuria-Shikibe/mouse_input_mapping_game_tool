#include "game_process.hpp"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--arguments")
        return argc == 6 && std::wstring_view(argv[2]) == L"hello world"
            && std::wstring_view(argv[3]) == L"quoted \"word\""
            && std::wstring_view(argv[4]) == L"tail\\"
            && std::wstring_view(argv[5]).empty() ? 0 : 7;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--hold") {
        Sleep(1000);
        return 0;
    }
    try {
        wchar_t path[MAX_PATH]{};
        require(GetModuleFileNameW(nullptr, path, MAX_PATH) > 0, "Cannot locate test executable");
        {
            mouse_mapping::game_process game({path, L"--arguments", L"hello world", L"quoted \"word\"", L"tail\\", L""});
            require(WaitForSingleObject(game.handle(), 5000) == WAIT_OBJECT_0, "Argument child did not exit");
            require(game.exited(), "Exited child not detected");
            DWORD code = 0;
            require(GetExitCodeProcess(game.handle(), &code) && code == 0, "Child arguments changed");
        }
        HANDLE copy = nullptr;
        {
            mouse_mapping::game_process game({path, L"--hold"});
            require(DuplicateHandle(GetCurrentProcess(), game.handle(), GetCurrentProcess(), &copy,
                0, FALSE, DUPLICATE_SAME_ACCESS), "Cannot duplicate child handle");
        }
        const auto alive = WaitForSingleObject(copy, 0) == WAIT_TIMEOUT;
        const auto finished = WaitForSingleObject(copy, 5000) == WAIT_OBJECT_0;
        CloseHandle(copy);
        require(alive && finished, "Closing the owner stopped the child");
        std::cout << "Daemon process tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
