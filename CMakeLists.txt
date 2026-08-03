cmake_minimum_required(VERSION 3.16)
project(luatier VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LUATIER_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LUATIER_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(LUATIER_BUILD_TESTS "Build tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /D_CRT_SECURE_NO_WARNINGS)
  # CMake defaults to /EHsc, which treats extern "C" as non-throwing. Our Lua C
  # API throws LuatierError across that boundary; under /EHsc MSVC calls
  # std::terminate → Windows exit 0xc0000409 (seen on Release ctest).
  foreach(_luatier_eh_var IN ITEMS
      CMAKE_CXX_FLAGS
      CMAKE_CXX_FLAGS_DEBUG
      CMAKE_CXX_FLAGS_RELEASE
      CMAKE_CXX_FLAGS_RELWITHDEBINFO
      CMAKE_CXX_FLAGS_MINSIZEREL)
    if(DEFINED ${_luatier_eh_var})
      string(REPLACE "/EHsc" "/EHs" _luatier_eh_val "${${_luatier_eh_var}}")
      set(${_luatier_eh_var} "${_luatier_eh_val}" CACHE STRING "" FORCE)
    endif()
  endforeach()
  unset(_luatier_eh_var)
  unset(_luatier_eh_val)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(LUATIER_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if(LUATIER_ENABLE_UBSAN AND NOT MSVC)
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()

set(LUATIER_SOURCES
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
  src/vm/ldebug.cpp
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

add_library(luatier STATIC ${LUATIER_SOURCES})
target_include_directories(luatier PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(luatier_cli src/main.cpp src/standalone.cpp)
target_link_libraries(luatier_cli PRIVATE luatier)
set_target_properties(luatier_cli PROPERTIES OUTPUT_NAME luatier)
# PUC allows ~200 syntactic nestings; our recursive parser/sema/lowering frames
# are heavier than lparser.c, so raise the Windows default 1MB stack.
if(MSVC)
  target_link_options(luatier_cli PRIVATE /STACK:8388608)
endif()

if(LUATIER_BUILD_TESTS)
  enable_testing()
  add_executable(luatier_tests
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
  target_link_libraries(luatier_tests PRIVATE luatier)
  target_include_directories(luatier_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  if(MSVC)
    target_link_options(luatier_tests PRIVATE /STACK:8388608)
  endif()
  add_test(NAME luatier_tests COMMAND luatier_tests)
  # PUC-compatible CLI: version is `-v` (not `--version`).
  add_test(NAME luatier_cli_version COMMAND luatier_cli -v)
  set_tests_properties(luatier_cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "Lua 5\\.3\\.4.*PUC-Rio"
  )
endif()

install(TARGETS luatier luatier_cli
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)
