cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build/release")
endif()
project(mouse_input_mapping VERSION 0.0.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows x64 is required")
endif()
set(CMAKE_CXX_STANDARD 20)
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

add_library(mapping_core src/config.cpp)
target_include_directories(mapping_core PUBLIC src)
target_link_libraries(mapping_core PUBLIC user32)

configure_file(assets/interception.rc.in "${CMAKE_BINARY_DIR}/generated/interception.rc" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/generated/interception.rc" PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/interception/bin/interception.dll;${CMAKE_CURRENT_SOURCE_DIR}/third_party/interception/installer/install-interception.exe")
add_executable(mouse_input_mapping src/main.cpp src/runtime.cpp src/deployment.cpp "${CMAKE_BINARY_DIR}/generated/interception.rc")
target_include_directories(mouse_input_mapping PRIVATE third_party/interception/include)
target_link_libraries(mouse_input_mapping PRIVATE mapping_core winmm shell32 advapi32 ole32)

add_executable(raw_input_probe tools/raw_input_probe.cpp)
target_link_libraries(raw_input_probe PRIVATE user32)

include(CTest)
if(BUILD_TESTING)
    add_executable(mapping_tests tests/mapping_tests.cpp)
    target_link_libraries(mapping_tests PRIVATE mapping_core)
    add_test(NAME mapping_tests COMMAND mapping_tests)
    add_executable(console_binding_tests tests/console_binding_tests.cpp)
    target_link_libraries(console_binding_tests PRIVATE mapping_core)
    add_test(NAME console_binding_tests COMMAND console_binding_tests)
    add_executable(asset_extraction_tests tests/asset_extraction_tests.cpp src/deployment.cpp "${CMAKE_BINARY_DIR}/generated/interception.rc")
    target_link_libraries(asset_extraction_tests PRIVATE mapping_core shell32 advapi32 ole32)
    add_test(NAME asset_extraction_tests COMMAND asset_extraction_tests)
    add_test(NAME cli_help COMMAND mouse_input_mapping --help)
    add_test(NAME probe_help COMMAND raw_input_probe --help)
    add_library(fake_interception SHARED tests/fake_interception.cpp)
    target_compile_definitions(fake_interception PRIVATE INTERCEPTION_EXPORT)
    target_include_directories(fake_interception PRIVATE third_party/interception/include)
    target_link_libraries(fake_interception PRIVATE user32)
    set_target_properties(fake_interception PROPERTIES
        OUTPUT_NAME interception RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake_driver")
    add_test(NAME runtime_integration COMMAND ${CMAKE_COMMAND}
        "-Dapp=$<TARGET_FILE:mouse_input_mapping>" "-Dmock=$<TARGET_FILE:fake_interception>"
        "-Dtest_dir=${CMAKE_BINARY_DIR}/runtime_integration" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_integration.cmake")
    add_test(NAME cli_smoke COMMAND ${CMAKE_COMMAND}
        "-Dapp=$<TARGET_FILE:mouse_input_mapping>" "-Dtest_dir=${CMAKE_BINARY_DIR}/cli_smoke"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake")
endif()

install(TARGETS mouse_input_mapping raw_input_probe RUNTIME DESTINATION .)
install(FILES third_party/interception/bin/interception.dll README.md LICENSE THIRD_PARTY_NOTICES.md CONTRIBUTING.md DESTINATION .)
install(FILES assets/config.example.ini DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs)
install(FILES third_party/interception/installer/install-interception.exe DESTINATION driver)
install(FILES third_party/interception/README.md DESTINATION third_party/interception)
install(DIRECTORY third_party/interception/licenses third_party/interception/source DESTINATION third_party/interception)

include(cmake/Packaging.cmake)
