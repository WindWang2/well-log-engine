if(NOT DEFINED ENGINE_BUILD_DIR OR
   NOT DEFINED CONSUMER_SOURCE_DIR OR
   NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "consumer test paths were not provided")
endif()

set(prefix_dir "${TEST_ROOT}/prefix")
set(consumer_build_dir "${TEST_ROOT}/build")
file(REMOVE_RECURSE "${TEST_ROOT}")

set(config_args)
set(ctest_config_args)
set(configure_args)
set(generator_args)
if(DEFINED TEST_CONFIG AND NOT TEST_CONFIG STREQUAL "")
    list(APPEND config_args --config "${TEST_CONFIG}")
    list(APPEND ctest_config_args -C "${TEST_CONFIG}")
    # Single-config generators (Ninja, Unix Makefiles) ignore --config at
    # build time; build type is fixed at configure. Without CMAKE_BUILD_TYPE,
    # MSVC defaults to Debug (MDd / _ITERATOR_DEBUG_LEVEL=2) while the engine
    # package under CI is Release (MD / IDL=0) → LNK2038 (#286).
    list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${TEST_CONFIG}")
endif()
if(DEFINED ENGINE_GENERATOR AND NOT ENGINE_GENERATOR STREQUAL "")
    list(APPEND generator_args -G "${ENGINE_GENERATOR}")
endif()
if(DEFINED ENGINE_GENERATOR_PLATFORM AND
   NOT ENGINE_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND generator_args -A "${ENGINE_GENERATOR_PLATFORM}")
endif()
if(DEFINED ENGINE_GENERATOR_TOOLSET AND
   NOT ENGINE_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND generator_args -T "${ENGINE_GENERATOR_TOOLSET}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --install "${ENGINE_BUILD_DIR}"
        --prefix "${prefix_dir}" ${config_args}
    RESULT_VARIABLE install_result
    COMMAND_ECHO STDOUT
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "WellLogEngine installation failed")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${CONSUMER_SOURCE_DIR}"
        -B "${consumer_build_dir}"
        ${generator_args}
        ${configure_args}
        "-DCMAKE_PREFIX_PATH=${prefix_dir}"
        "-DCMAKE_CXX_COMPILER=${ENGINE_CXX_COMPILER}"
        "-DCMAKE_CXX_FLAGS=${ENGINE_CXX_FLAGS}"
        "-DCMAKE_EXE_LINKER_FLAGS=${ENGINE_EXE_LINKER_FLAGS}"
    RESULT_VARIABLE configure_result
    COMMAND_ECHO STDOUT
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "external consumer configuration failed")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --build "${consumer_build_dir}" ${config_args}
    RESULT_VARIABLE build_result
    COMMAND_ECHO STDOUT
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "external consumer build failed")
endif()

if(WIN32)
    set(path_separator ";")
else()
    set(path_separator ":")
endif()
# Shared installs put DLLs under bin (Windows) and .so under lib (Unix). PATH
# alone is enough for Windows; Unix needs LD_LIBRARY_PATH for libwelllog-*.so.
set(consumer_path "${prefix_dir}/bin${path_separator}$ENV{PATH}")
set(consumer_lib_path
    "${prefix_dir}/lib${path_separator}${prefix_dir}/lib64${path_separator}$ENV{LD_LIBRARY_PATH}"
)

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "PATH=${consumer_path}"
        "LD_LIBRARY_PATH=${consumer_lib_path}"
        "${CMAKE_CTEST_COMMAND}"
        --test-dir "${consumer_build_dir}"
        --output-on-failure ${ctest_config_args}
    RESULT_VARIABLE test_result
    COMMAND_ECHO STDOUT
)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "external consumer execution failed")
endif()
