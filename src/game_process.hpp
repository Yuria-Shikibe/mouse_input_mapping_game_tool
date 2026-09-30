#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace mouse_mapping {

using game_command = std::vector<std::wstring>;

class game_process {
public:
    explicit game_process(const game_command& command);
    ~game_process();
    game_process(const game_process&) = delete;
    game_process& operator=(const game_process&) = delete;

    // Returns true only when the launched process and all descendants have exited.
    bool exited() const;
private:
    HANDLE job_{};
};

} // namespace mouse_mapping
