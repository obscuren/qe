# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(test_buf
    test_buf.c
    ${CMAKE_SOURCE_DIR}/src/buf.c
    ${CMAKE_SOURCE_DIR}/src/utils.c
)
target_include_directories(test_buf PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_buf PRIVATE unity)
add_test(NAME buf COMMAND test_buf)

add_executable(test_undo
    test_undo.c
    ${CMAKE_SOURCE_DIR}/src/undo.c
    ${CMAKE_SOURCE_DIR}/src/buf.c
    ${CMAKE_SOURCE_DIR}/src/editor.c
    ${CMAKE_SOURCE_DIR}/src/git.c
    ${CMAKE_SOURCE_DIR}/src/syntax.c
    ${CMAKE_SOURCE_DIR}/src/terminal.c
    ${CMAKE_SOURCE_DIR}/src/utils.c
)
target_include_directories(test_undo PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_undo PRIVATE QE_TEST)
target_link_libraries(test_undo PRIVATE unity)
add_test(NAME undo COMMAND test_undo)

add_executable(test_search
    test_search.c
    ${CMAKE_SOURCE_DIR}/src/search.c
    ${CMAKE_SOURCE_DIR}/src/buf.c
    ${CMAKE_SOURCE_DIR}/src/utils.c
)
target_include_directories(test_search PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_search PRIVATE unity)
add_test(NAME search COMMAND test_search)
