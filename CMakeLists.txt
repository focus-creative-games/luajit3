cmake_minimum_required(VERSION 3.16)
project(luajit3 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LJ3_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LJ3_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(LJ3_BUILD_TESTS "Build tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(LJ3_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if(LJ3_ENABLE_UBSAN AND NOT MSVC)
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()

set(LJ3_SOURCES
  src/runtime/value.cpp
  src/runtime/string.cpp
  src/runtime/table.cpp
  src/runtime/closure.cpp
  src/runtime/upvalue.cpp
  src/runtime/userdata.cpp
  src/gc/gc.cpp
  src/frontend/lexer.cpp
  src/frontend/parser.cpp
  src/frontend/sema.cpp
  src/frontend/lowering.cpp
  src/vm/bytecode.cpp
  src/vm/interpreter.cpp
  src/vm/debug_hook.cpp
  src/vm/state.cpp
  src/vm/builtins.cpp
  src/lib/linit.cpp
  src/lib/lbaselib.cpp
  src/lib/lcorolib.cpp
  src/lib/lpackagelib.cpp
  src/lib/lstrlib.cpp
  src/lib/ltablib.cpp
  src/lib/lmathlib.cpp
  src/lib/lutf8lib.cpp
  src/lib/liolib.cpp
  src/lib/loslib.cpp
  src/lib/ldblib.cpp
  src/lib/ldump.cpp
  src/vm/meta.cpp
  src/api/lua_api.cpp
  src/jit/hotness.cpp
  src/jit/ic.cpp
  src/jit/baseline.cpp
  src/jit/opt.cpp
  src/jit/deopt.cpp
  src/jit/background.cpp
  src/tools/dump.cpp
  src/tools/profile.cpp
)

add_library(luajit3 STATIC ${LJ3_SOURCES})
target_include_directories(luajit3 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(luajit3_cli src/main.cpp)
target_link_libraries(luajit3_cli PRIVATE luajit3)
set_target_properties(luajit3_cli PROPERTIES OUTPUT_NAME luajit3)

if(LJ3_BUILD_TESTS)
  enable_testing()
  add_executable(lj3_tests
    tests/test_main.cpp
    tests/test_lexer.cpp
    tests/test_value.cpp
    tests/test_vm.cpp
    tests/test_differential.cpp
    tests/test_lang53.cpp
    tests/lang53/test_types_coercion.cpp
    tests/lang53/test_ops.cpp
    tests/lang53/test_statements.cpp
    tests/lang53/test_tables_meta.cpp
    tests/lang53/test_functions.cpp
    tests/lang53/test_visibility_goto.cpp
    tests/lang53/test_errors_coro.cpp
    tests/lang53/test_baselib.cpp
    tests/test_parser_sema.cpp
    tests/test_gc_stress.cpp
  )
  target_link_libraries(lj3_tests PRIVATE luajit3)
  target_include_directories(lj3_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  add_test(NAME lj3_tests COMMAND lj3_tests)
  add_test(NAME lj3_cli_version COMMAND luajit3_cli --version)
endif()

install(TARGETS luajit3 luajit3_cli
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)
