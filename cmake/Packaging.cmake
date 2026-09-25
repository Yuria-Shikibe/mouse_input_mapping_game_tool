# Package only the install tree: no tests, local configuration or build caches.
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME "mouse_input_mapping")
set(CPACK_PACKAGE_VENDOR "mo_yanxi")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
include(CPack)
