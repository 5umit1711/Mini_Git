cmake_minimum_required(VERSION 3.20)

project(MiniGit VERSION 0.1.0 LANGUAGES C)

find_package(ZLIB REQUIRED)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(mygit
    src/main.c
    src/commands/init.c
    src/commands/add.c
    src/commands/branch.c
    src/commands/commit.c
    src/commands/log.c
    src/commands/status.c
    src/util/index.c
    src/util/object.c
    src/util/repository.c
    src/util/sha1.c
    src/util/filesystem.c
)

target_include_directories(mygit PRIVATE src)
target_link_libraries(mygit PRIVATE ZLIB::ZLIB)

if(MSVC)
    target_compile_options(mygit PRIVATE /W4)
else()
    target_compile_options(mygit PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(test_sha1
        tests/test_sha1.c
        src/util/sha1.c
    )
    target_include_directories(test_sha1 PRIVATE src)
    add_test(NAME sha1 COMMAND test_sha1)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME workflow
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/workflow.py"
                    "$<TARGET_FILE:mygit>")
    endif()
endif()
