# Convenience build for the unmodified upstream user-mode library source.
cmake_minimum_required(VERSION 3.24)
project(interception LANGUAGES C)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(interception SHARED interception.c)
target_compile_definitions(interception PRIVATE INTERCEPTION_EXPORT _CRT_SECURE_NO_WARNINGS)
