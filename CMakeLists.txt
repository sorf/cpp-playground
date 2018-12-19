cmake_minimum_required(VERSION 3.1)

project(
    dev_new
    VERSION 0.1
    DESCRIPTION "C++ Playground"
    LANGUAGES CXX)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)
enable_testing()

# Defined a test executable
function(define_test_executable test_name sources)
    set(full_path_sources "")
    foreach(source ${sources})
        list(APPEND full_path_sources source/${source})
    endforeach(source)

    set(executable_binary ${test_name})
    add_executable(${executable_binary} ${full_path_sources})
    target_compile_features(${executable_binary} PRIVATE cxx_std_17)
    target_link_libraries(${executable_binary} PRIVATE CONAN_PKG::boost)
    if(CLANG_TIDY_COMMAND)
        set_target_properties(${executable_binary} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND}")
    endif()
    add_test(NAME ${executable_binary} WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin COMMAND ${executable_binary})
endfunction(define_test_executable)


set(TESTS
    asio_composed)
foreach(test_name ${TESTS})
    define_test_executable(${test_name} ${test_name}.cpp)
endforeach(test_name)

