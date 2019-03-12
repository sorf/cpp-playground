cmake_minimum_required(VERSION 3.1)

project(
    cpp_playground
    VERSION 0.1
    DESCRIPTION "C++ Playground"
    LANGUAGES CXX)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)
enable_testing()

# Defined a test executable without clang-tidy
function(define_test_executable_no_clang_tidy test_name sources)
    set(full_path_sources "")
    foreach(source ${sources})
        list(APPEND full_path_sources source/${source})
    endforeach(source)

    set(executable_binary ${test_name})
    add_executable(${executable_binary} ${full_path_sources})
    target_compile_features(${executable_binary} PRIVATE cxx_std_17)
    target_link_libraries(${executable_binary} PRIVATE CONAN_PKG::boost)
    add_test(NAME ${executable_binary} WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin COMMAND ${executable_binary})
endfunction(define_test_executable)

# Defined a test executable
function(define_test_executable test_name sources)
    define_test_executable_no_clang_tidy(${test_name} ${sources})

    set(executable_binary ${test_name})
    if(CLANG_TIDY_COMMAND)
        set_target_properties(${executable_binary} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
endfunction(define_test_executable)

set(TESTS
    asio_strand_test;
    asio_echo; asio_echo_v2; asio_echo_v3; asio_echo_v4;
    asio_composed_one_flow;
    #asio_composed_one_flow_async_op;
    asio_composed_one_flow_compose;
    asio_composed;
    asio_echo_server;
    leaf_context_activate_test;
    leaf_info_stack_test;
    #asio_beast_leaf_rpc;
    #asio_beast_leaf_rpc_v2;
    )

set(TESTS_NO_CLANG_TIDY
    asio_composed_one_flow_async_op;
    asio_beast_leaf_rpc;
    asio_beast_leaf_rpc_v2)

foreach(test_name ${TESTS})
    define_test_executable(${test_name} ${test_name}.cpp)
endforeach(test_name)

foreach(test_name ${TESTS_NO_CLANG_TIDY})
    define_test_executable_no_clang_tidy(${test_name} ${test_name}.cpp)
endforeach(test_name)

target_include_directories(asio_composed_one_flow_async_op SYSTEM PRIVATE tmp/include)
target_include_directories(asio_composed_one_flow_compose SYSTEM PRIVATE tmp/include)
target_include_directories(leaf_context_activate_test SYSTEM PRIVATE tmp/include)
target_include_directories(leaf_info_stack_test SYSTEM PRIVATE tmp/include)
target_include_directories(asio_beast_leaf_rpc SYSTEM PRIVATE tmp/include)
target_include_directories(asio_beast_leaf_rpc_v2 SYSTEM PRIVATE tmp/include)

