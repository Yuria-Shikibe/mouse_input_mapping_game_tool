file(MAKE_DIRECTORY "${test_dir}/app" "${test_dir}/cwd")
file(COPY_FILE "${app}" "${test_dir}/app/mouse_input_mapping_user.exe")
set(executable "${test_dir}/app/mouse_input_mapping_user.exe")
file(WRITE "${test_dir}/answers.txt" "LEFT\n\nRIGHT\n\nUP\n\nDOWN\n\nF1\n\nF2\n\nF3\n\nF4\n\nF5\n\nF6\n\nF7\n\nF9\n\n\n\n\n\n\n")
execute_process(COMMAND "${executable}" --configure-text
    WORKING_DIRECTORY "${test_dir}/cwd" INPUT_FILE "${test_dir}/answers.txt"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "User configure failed: ${output}\n${error}")
endif()
file(READ "${test_dir}/app/config.user.ini" config)
foreach(field "left_key=0xe04b" "right_key=0xe04d" "up_key=0xe048" "down_key=0xe050" "toggle_key=0x0043"
        "lmb_key=0x003b" "rmb_key=0x003c" "cmb_key=0x003d" "x1_key=0x003e" "x2_key=0x003f"
        "wheel_up_key=0x0040" "wheel_down_key=0x0041" "x_keyboard_override_enabled=1")
    if(NOT config MATCHES "${field}")
        message(FATAL_ERROR "Missing user binding ${field}: ${config}")
    endif()
endforeach()
if(EXISTS "${test_dir}/cwd/config.user.ini" OR EXISTS "${test_dir}/app/config.ini")
    message(FATAL_ERROR "User configuration mixed with kernel configuration or CWD")
endif()
file(WRITE "${test_dir}/incomplete.txt" "A\n\nD\n\nW\n")
execute_process(COMMAND "${executable}" --configure-text INPUT_FILE "${test_dir}/incomplete.txt"
    RESULT_VARIABLE result ERROR_VARIABLE error TIMEOUT 10)
file(READ "${test_dir}/app/config.user.ini" after)
if(NOT result EQUAL 1 OR NOT "${after}" STREQUAL "${config}")
    message(FATAL_ERROR "Incomplete Y configuration changed saved settings")
endif()
file(WRITE "${test_dir}/invalid.ini" "up_key=A\n")
execute_process(COMMAND "${executable}" --config "${test_dir}/invalid.ini"
    RESULT_VARIABLE result ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 1 OR NOT error MATCHES "must be different")
    message(FATAL_ERROR "Conflicting XY binding not rejected: ${error}")
endif()
execute_process(COMMAND "${executable}" --install-driver
    RESULT_VARIABLE result ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 1 OR NOT error MATCHES "Unknown option")
    message(FATAL_ERROR "User executable accepted driver installation")
endif()
execute_process(COMMAND "${executable}" --daemon "${CMAKE_COMMAND}" -E sleep 1
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 0 OR NOT output MATCHES "Daemon: launched"
        OR NOT output MATCHES "Daemon: game process exited")
    message(FATAL_ERROR "User daemon failed: ${result}\n${output}\n${error}")
endif()
file(WRITE "${test_dir}/unbound.ini" "[mapping]\nleft_key=A\nright_key=D\n")
execute_process(COMMAND "${executable}" --config "${test_dir}/unbound.ini" --daemon "${CMAKE_COMMAND}" -E sleep 1
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 1 OR NOT error MATCHES "Unbound key: toggle_key" OR output MATCHES "Daemon: launched")
    message(FATAL_ERROR "Unbound daemon keys were accepted: ${result}\n${output}\n${error}")
endif()
execute_process(COMMAND "${executable}" --daemon "${test_dir}/missing-game.exe"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 1 OR NOT error MATCHES "Cannot launch game process")
    message(FATAL_ERROR "Missing game executable was accepted: ${result}\n${output}\n${error}")
endif()
