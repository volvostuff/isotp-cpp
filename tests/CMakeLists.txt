add_executable(isotp_cpp_tests main.cpp)
target_link_libraries(isotp_cpp_tests PRIVATE isotp_cpp)

add_test(NAME isotp_cpp_tests COMMAND isotp_cpp_tests)
