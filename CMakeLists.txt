cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build/release")
endif()
project(mouse_input_mapping VERSION 0.0.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows x64 is required")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
endif()

# Shared sources are compiled directly into the three shipped executables.
set(runtime_sources src/config.cpp src/console_config.cpp src/game_process.cpp src/input_monitor.cpp src/pie_overlay.cpp)

configure_file(assets/interception.rc.in "${CMAKE_BINARY_DIR}/generated/interception.rc" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/generated/interception.rc" PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/interception/bin/interception.dll;${CMAKE_CURRENT_SOURCE_DIR}/third_party/interception/installer/install-interception.exe")
add_executable(mouse_input_mapping_kernel ${runtime_sources} src/main.cpp src/runtime.cpp src/deployment.cpp "${CMAKE_BINARY_DIR}/generated/interception.rc")
target_include_directories(mouse_input_mapping_kernel PRIVATE third_party/interception/include)
target_link_libraries(mouse_input_mapping_kernel PRIVATE user32 winmm shell32 advapi32 ole32 gdi32 gdiplus shcore)

add_executable(mouse_input_mapping_user ${runtime_sources} src/user_main.cpp src/user_runtime.cpp)
target_link_libraries(mouse_input_mapping_user PRIVATE user32 winmm gdi32 gdiplus shcore)
include(CheckIPOSupported)
check_ipo_supported(RESULT runtime_ipo_supported OUTPUT runtime_ipo_error LANGUAGES CXX)
if(runtime_ipo_supported)
    set_property(TARGET mouse_input_mapping_kernel mouse_input_mapping_user
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    message(STATUS "Runtime IPO unavailable: ${runtime_ipo_error}")
endif()

add_executable(mouse_input_mapping_config WIN32
    src/gui_main.cpp src/curve_editor.cpp src/config.cpp src/config_document.cpp src/gui_process.cpp
    src/input_monitor.cpp src/input_monitor_view.cpp src/input_preview.cpp
    assets/config_gui.manifest)
target_link_libraries(mouse_input_mapping_config PRIVATE user32 comctl32 ole32 shell32 uuid gdi32 winmm)
# The GUI writes the current schema and launches these adjacent executables.
# Building only the GUI must not leave older runtimes beside a newer editor.
add_dependencies(mouse_input_mapping_config mouse_input_mapping_user mouse_input_mapping_kernel)

install(TARGETS mouse_input_mapping_kernel mouse_input_mapping_user mouse_input_mapping_config RUNTIME DESTINATION .)
install(FILES third_party/interception/bin/interception.dll README.md LICENSE THIRD_PARTY_NOTICES.md CONTRIBUTING.md DESTINATION .)
install(FILES assets/config.example.ini DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs)
install(FILES third_party/interception/installer/install-interception.exe DESTINATION driver)
install(FILES third_party/interception/README.md DESTINATION third_party/interception)
install(DIRECTORY third_party/interception/licenses third_party/interception/source DESTINATION third_party/interception)

include(cmake/Packaging.cmake)
