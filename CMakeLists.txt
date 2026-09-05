cmake_minimum_required(VERSION 3.16)

project(isotp_cpp VERSION 0.1.0 LANGUAGES C CXX
        DESCRIPTION "C++ wrapper for the isotp-c ISO-TP library")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ISOTP_CPP_BUILD_TESTS "Build the isotp_cpp test suite" ON)

# Feature options the wrapper relies on. The C library compiles isotp.c with
# these only privately, so they are also (re)declared PUBLIC below to keep the
# IsoTpLink ABI and the isotp_set_*_cb API visible to consumers.
set(isotpc_ENABLE_TRANSCEIVE_EVENTS ON CACHE BOOL "" FORCE)
set(isotpc_ENABLE_CAN_SEND_ARG ON CACHE BOOL "" FORCE)

add_subdirectory(isotp-c)

add_library(isotp_cpp src/isotp.cpp)
add_library(isotp_cpp::isotp_cpp ALIAS isotp_cpp)
target_include_directories(isotp_cpp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(isotp_cpp PUBLIC isotp)

target_compile_definitions(
    isotp_cpp PUBLIC
    ISO_TP_TRANSMIT_COMPLETE_CALLBACK
    ISO_TP_RECEIVE_COMPLETE_CALLBACK
    ISO_TP_USER_SEND_CAN_ARG
)
target_compile_definitions(
    isotp PUBLIC
    ISO_TP_TRANSMIT_COMPLETE_CALLBACK
    ISO_TP_RECEIVE_COMPLETE_CALLBACK
    ISO_TP_USER_SEND_CAN_ARG
)

if(ISOTP_CPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
