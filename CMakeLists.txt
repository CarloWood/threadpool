cmake_minimum_required( VERSION 3.12...3.16 )

project( threadpool
         LANGUAGES C CXX
         DESCRIPTION "A thread pool with support for multiple queues and ultra fast timers" )

include( AICxxProject )

#==============================================================================
# DEPENDENCIES
#

# This project uses timer_create and timer_settime.
find_package( rt )

#==============================================================================
# BUILD PROJECT
#

# This project is an OBJECT-library, used by other git submodules and the main project.
add_library( threadpool_ObjLib OBJECT )

# Set SOURCE_DIR to the current directory.
set_property(
    TARGET     threadpool_ObjLib
    PROPERTY   SOURCE_DIR       "${CMAKE_CURRENT_SOURCE_DIR}"
)

# The list of source files.
target_sources( threadpool_ObjLib
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
get_target_property( CWDS_INTERFACE_INCLUDE_DIRECTORIES AICxx::cwds INTERFACE_INCLUDE_DIRECTORIES )
target_include_directories( threadpool_ObjLib
    PUBLIC
        "${CWDS_INTERFACE_INCLUDE_DIRECTORIES}" # For sys.h and debug.h.
)

# Require support for C++17.
target_compile_features( threadpool_ObjLib
    PUBLIC cxx_std_17
)

# Set link dependencies.
target_link_libraries( threadpool_ObjLib
    INTERFACE
        ${RT_LIBRARY} 
    PUBLIC
        AICxx::threadsafe
)

# Create an ALIAS target.
add_library( AICxx::threadpool ALIAS threadpool_ObjLib )

# Prepend this object library to the list.
set( AICXX_OBJECTS_LIST AICxx::threadpool ${AICXX_OBJECTS_LIST} CACHE INTERNAL "List of OBJECT libaries that this project uses." )
