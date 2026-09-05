cmake_minimum_required(VERSION 3.16)

project(isotp_cpp VERSION 0.1.0 LANGUAGES C CXX
        DESCRIPTION "C++ wrapper for the isotp-c ISO-TP library")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ISOTP_CPP_BUILD_TESTS "Build the isotp_cpp test suite" ON)

# The upstream C library is a single translation unit. It is compiled here
# directly (not via isotp-c/CMakeLists.txt) so this wrapper controls the flags
# and the feature macros consistently for all configurations (upstream adds
# /O2 outside single-config Debug, which conflicts with MSVC Debug /RTC1).
#
# Feature macros that change the IsoTpLink ABI/API are PUBLIC so any consumer
# sees the same layout. Padding is left off: frames are sent with their exact
# length (matches the rest of the j2534ble host/device code).
set(ISOTP_C_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/isotp-c)

add_library(isotp_c STATIC ${ISOTP_C_ROOT}/isotp.c)
add_library(isotp_cpp::isotp_c ALIAS isotp_c)
target_include_directories(isotp_c PUBLIC ${ISOTP_C_ROOT})
target_compile_definitions(
    isotp_c PUBLIC
    ISO_TP_MAX_CAN_FRAME_SIZE=8
    ISO_TP_DEFAULT_TX_DL=8
    ISO_TP_DEFAULT_BLOCK_SIZE=255
    ISO_TP_DEFAULT_RESPONSE_TIMEOUT_US=5000000
    ISO_TP_TRANSMIT_COMPLETE_CALLBACK
    ISO_TP_RECEIVE_COMPLETE_CALLBACK
    ISO_TP_USER_SEND_CAN_ARG
)
if(MSVC)
    target_compile_options(isotp_c PRIVATE /W4)
else()
    target_compile_options(isotp_c PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # GCC 12+ stringop-overflow false positive in upstream isotp.c: the SF_DL
    # escape path is unreachable with classic CAN (send_size <= 7), but the
    # compiler cannot prove it and -Werror would fail the build.
    target_compile_options(isotp_c PRIVATE $<$<C_COMPILER_ID:GNU>:-Wno-stringop-overflow>)
endif()

add_library(isotp_cpp src/isotp.cpp)
add_library(isotp_cpp::isotp_cpp ALIAS isotp_cpp)
target_include_directories(isotp_cpp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(isotp_cpp PUBLIC isotp_c)

if(ISOTP_CPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
