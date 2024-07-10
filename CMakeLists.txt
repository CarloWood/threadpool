cmake_minimum_required(VERSION 3.12...3.16)

project(threadpool
  LANGUAGES C CXX
  DESCRIPTION "A thread pool with support for multiple queues and ultra fast timers"
)

include(AICxxProject)

#==============================================================================
# DEPENDENCIES
#

include(CheckFunctionExists)

# Use check_function_exists because we want to check if we can link it without using -lrt.
check_function_exists(timer_create RT_TIMER_CREATE)

if (RT_TIMER_CREATE)
  set(RT_LIBRARY)
else ()
  # This project uses timer_create and timer_settime.
  find_package(rt)
endif ()

# Specify threadpool specific configure file.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/config.h
               @ONLY)

#==============================================================================
# BUILD PROJECT
#

# This project is an OBJECT-library, used by other git submodules and the main project.
add_library(threadpool_ObjLib OBJECT)

# Tell cwds/sys.h that we have a config.h.
target_compile_definitions(threadpool_ObjLib
  PUBLIC
    HAVE_THREADPOOL_CONFIG_H
)

# The list of source files.
target_sources(threadpool_ObjLib
  PRIVATE
    "AIThreadPool.cxx"
    "RunningTimers.cxx"
    "Timer.cxx"

    "AIObjectQueue.h"
    "AIQueueHandle.h"
    "AIThreadPool.h"
    "RunningTimers.h"
    "Timer.h"
    "TimerQueue.h"
)

# Required include search-paths.
get_target_property(CWDS_INTERFACE_INCLUDE_DIRECTORIES AICxx::cwds INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(threadpool_ObjLib
  PUBLIC
    "${CWDS_INTERFACE_INCLUDE_DIRECTORIES}"     # For sys.h and debug.h.
)

# Require support for C++17.
target_compile_features(threadpool_ObjLib
  PUBLIC cxx_std_17
)

# Set link dependencies.
target_link_libraries(threadpool_ObjLib
  INTERFACE
    ${RT_LIBRARY} 
  PUBLIC
    AICxx::threadsafe
)

# Create an ALIAS target.
add_library(AICxx::threadpool ALIAS threadpool_ObjLib)

# Prepend this object library to the list.
set(AICXX_OBJECTS_LIST AICxx::threadpool ${AICXX_OBJECTS_LIST} CACHE INTERNAL "List of OBJECT libaries that this project uses.")
