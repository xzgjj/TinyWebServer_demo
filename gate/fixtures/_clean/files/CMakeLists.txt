cmake_minimum_required(VERSION 3.16)
project(fixture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(INTEGRATION_TESTS
    test_basic
)

foreach(test_name ${INTEGRATION_TESTS})
    set(TEST_FILE ${PROJECT_SOURCE_DIR}/test/${test_name}.cpp)
    if(EXISTS ${TEST_FILE})
        add_executable(${test_name} test/${test_name}.cpp)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()
