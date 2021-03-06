enable_testing()

set(LIBS nekit)
if (UNIX AND NOT APPLE)
  set(CMAKE_THREAD_PREFER_PTHREAD ON)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  if (CMAKE_VERSION VERSION_LESS "3.1")
    if(CMAKE_USE_PTHREADS_INIT)
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
    endif()
  else ()
    cmake_policy(SET CMP0028 NEW)
    set(LIBS ${LIBS} Threads::Threads)
  endif ()
endif()

include_directories(include)

option(NE_ENABLE_MEMCHECK "Enable valgrind when running tests." ON)
find_program(CTEST_MEMORYCHECK_COMMAND valgrind)
set(CTEST_MEMORYCHECK_COMMAND_OPTIONS --trace-children=yes --leak-check=full)

function(add_mem_test name)
  if (CTEST_MEMORYCHECK_COMMAND AND NE_ENABLE_MEMCHECK AND "${NEKIT_ARCH}" STREQUAL "x64")
    message(STATUS "Adding valgrind test for ${name}.")
    set(MEMCHECK_PARAMS ${CTEST_MEMORYCHECK_COMMAND} ${CTEST_MEMORYCHECK_COMMAND_OPTIONS} $<TARGET_FILE:${name}>)
    add_test(NAME ${name} COMMAND ${MEMCHECK_PARAMS})
  else()
    message(STATUS "Adding test for ${name}.")
    add_test(${name} ${name})
  endif()
endfunction()

add_executable(test_memory_pool utils/test_memory_pool.c)
target_link_libraries(test_memory_pool ${LIBS})
add_mem_test(test_memory_pool)

add_executable(test_tcp_socket socket/test_tcp_socket.c)
target_link_libraries(test_tcp_socket ${LIBS})
add_mem_test(test_tcp_socket)
