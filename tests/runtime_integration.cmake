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
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "MOUSE_MAPPING_TEST_IDLE=1"
        "${test_dir}/mouse_input_mapping.exe" --daemon "${CMAKE_COMMAND}" -E sleep 1
    RESULT_VARIABLE daemon_result OUTPUT_VARIABLE daemon_output ERROR_VARIABLE daemon_error TIMEOUT 10)
if(NOT daemon_result EQUAL 0 OR NOT daemon_output MATCHES "Daemon: launched"
        OR NOT daemon_output MATCHES "Daemon: game process exited")
    message(FATAL_ERROR "Kernel daemon failed: ${daemon_result}\n${daemon_output}\n${daemon_error}")
endif()
file(WRITE "${test_dir}/unbound.ini" "[mapping]\nleft_key=A\nright_key=D\n")
execute_process(COMMAND "${test_dir}/mouse_input_mapping.exe" --config "${test_dir}/unbound.ini"
        --daemon "${CMAKE_COMMAND}" -E sleep 1
    RESULT_VARIABLE daemon_result OUTPUT_VARIABLE daemon_output ERROR_VARIABLE daemon_error TIMEOUT 10)
if(NOT daemon_result EQUAL 1 OR NOT daemon_error MATCHES "Unbound key: toggle_key"
        OR daemon_output MATCHES "Daemon: launched")
    message(FATAL_ERROR "Kernel accepted unbound daemon key: ${daemon_result}\n${daemon_output}\n${daemon_error}")
endif()
