file(MAKE_DIRECTORY "${test_dir}")
file(COPY_FILE "${app}" "${test_dir}/mouse_input_mapping.exe")
file(COPY_FILE "${mock}" "${test_dir}/interception.dll")
file(WRITE "${test_dir}/config.ini" "[mapping]\nleft_key=F23\nright_key=F24\ntoggle_key=F8\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "MOUSE_MAPPING_TEST_REPORT=${test_dir}/report.txt"
        "${test_dir}/mouse_input_mapping.exe"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 1 OR NOT error MATCHES "Driver input receive failed")
    message(FATAL_ERROR "Unexpected runtime result: ${result}\n${output}\n${error}")
endif()
file(READ "${test_dir}/report.txt" report)
if(NOT report MATCHES "^PASS")
    message(FATAL_ERROR "Input packet integration failed: ${report}\n${output}\n${error}")
endif()
message(STATUS "${report}")
