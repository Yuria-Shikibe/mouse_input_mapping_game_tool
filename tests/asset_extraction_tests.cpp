#include "deployment.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

// This executable deliberately does not link the runtime or call installation.
namespace mouse_mapping {
bool driver_available() { throw std::runtime_error("Unexpected driver access in extraction test"); }
}

int main() {
    using namespace mouse_mapping;
    try {
        for (const auto asset : {embedded_asset::library, embedded_asset::installer}) {
            std::filesystem::path extracted_path;
            {
                embedded_file file(asset);
                extracted_path = file.path();
                std::ifstream input(file.path(), std::ios::binary);
                const std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
                const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(static_cast<int>(asset)), RT_RCDATA);
                const auto loaded = LoadResource(nullptr, resource);
                const auto data = LockResource(loaded);
                if (bytes.size() != SizeofResource(nullptr, resource) || std::memcmp(bytes.data(), data, bytes.size()) != 0)
                    throw std::runtime_error("Extracted file differs from embedded resource");
                if (asset == embedded_asset::library) {
                    const auto module = LoadLibraryExW(file.path().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                    if (!module) throw std::runtime_error("Cannot load extracted DLL");
                    const bool exported = GetProcAddress(module, "interception_create_context") != nullptr;
                    FreeLibrary(module);
                    if (!exported) throw std::runtime_error("Wrong embedded DLL");
                }
            }
            if (std::filesystem::exists(extracted_path) || std::filesystem::exists(extracted_path.parent_path()))
                throw std::runtime_error("Temporary asset was not cleaned up");
        }
        std::cout << "Both embedded resources extracted byte-for-byte and cleaned up. No driver was installed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
