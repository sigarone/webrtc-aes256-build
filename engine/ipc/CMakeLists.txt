add_library(qmedia_ipc STATIC
  src/status.cpp
  src/secure.cpp
  src/cbor.cpp
  src/schema.cpp
  src/message.cpp
  src/frame.cpp
  src/session.cpp)
target_include_directories(qmedia_ipc PUBLIC include)
if(WIN32)
  target_link_libraries(qmedia_ipc PUBLIC bcrypt)
endif()

if(WIN32)
  add_library(qmedia_pipe STATIC src/pipe.cpp)
  target_link_libraries(qmedia_pipe PUBLIC qmedia_ipc advapi32)
endif()

if(QMEDIA_BUILD_TESTS)
  set(QMEDIA_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus")
  set(QMEDIA_CDDL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/schema.cddl")

  add_library(qmedia_testsupport STATIC tests/gen_messages.cpp)
  target_link_libraries(qmedia_testsupport PUBLIC qmedia_ipc)
  target_include_directories(qmedia_testsupport PUBLIC tests)

  add_executable(qmedia_ipc_tests
    tests/test_main.cpp
    tests/test_cbor.cpp
    tests/test_frame.cpp
    tests/test_validator.cpp
    tests/test_session.cpp
    tests/test_cddl.cpp
    tests/test_corpus.cpp)
  target_link_libraries(qmedia_ipc_tests PRIVATE qmedia_testsupport)
  target_compile_definitions(qmedia_ipc_tests PRIVATE
    QMEDIA_CORPUS_DIR="${QMEDIA_CORPUS_DIR}"
    QMEDIA_CDDL_FILE="${QMEDIA_CDDL_FILE}")
  add_test(NAME ipc_unit COMMAND qmedia_ipc_tests)
  set_tests_properties(ipc_unit PROPERTIES TIMEOUT 120)

  # Rewrites the seed corpus from the schema: qmedia_corpus_gen <dir>.
  add_executable(qmedia_corpus_gen tests/corpus_gen.cpp)
  target_link_libraries(qmedia_corpus_gen PRIVATE qmedia_testsupport)

  if(WIN32)
    add_executable(qmedia_pipe_tests tests/test_main.cpp tests/test_pipe.cpp)
    target_link_libraries(qmedia_pipe_tests PRIVATE qmedia_pipe qmedia_testsupport)
    if(QMEDIA_BUILD_ENGINE)
      add_dependencies(qmedia_pipe_tests qaudion-media)
      target_compile_definitions(qmedia_pipe_tests PRIVATE
        QMEDIA_ENGINE_EXE="$<TARGET_FILE:qaudion-media>")
    endif()
    add_test(NAME pipe COMMAND qmedia_pipe_tests)
    set_tests_properties(pipe PROPERTIES TIMEOUT 180)
  endif()
endif()

if(QMEDIA_BUILD_FUZZERS)
  # libFuzzer targets (clang). The seed corpus lives in fuzz/corpus/<target>.
  foreach(name fuzz_message fuzz_session)
    add_executable(${name} fuzz/${name}.cpp)
    target_link_libraries(${name} PRIVATE qmedia_ipc)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()
