cmake_minimum_required(VERSION 2.8.0 FATAL_ERROR)

set(CMAKE_COLOR_MAKEFILE ON)

set(PG_JIEBA_PROJECT_NAME "pg_jieba")
set(PG_JIEBA_PROJECT_ID "pg_jieba")
project("${PG_JIEBA_PROJECT_ID}")

message(STATUS "Setting ${CMAKE_PROJECT_NAME} build type - ${CMAKE_BUILD_TYPE}")

find_package(PostgreSQL REQUIRED)
if(NOT PostgreSQL_FOUND)
    message(FATAL_ERROR " Please check your PostgreSQL installation.")
endif(NOT PostgreSQL_FOUND)

include_directories(${PostgreSQL_INCLUDE_DIRS})
link_directories(${PostgreSQL_LIBRARY_DIRS})

set(CPPJIEBA_DIR "libjieba")
INCLUDE_DIRECTORIES(${CPPJIEBA_DIR}/deps
        ${CPPJIEBA_DIR}/include)

ADD_DEFINITIONS(-O0 -Wall -g)

set(SOURCE_FILES
    pg_jieba.cpp)

set(PG_JIEBA_POSTGRESQL_DIR "${CMAKE_INSTALL_PREFIX}"
        CACHE PATH "PostgreSQL binary directory")
set(PG_JIEBA_POSTGRESQL_VERSION "unknown"
        CACHE STRING "PostgreSQL version")

set(PG_JIEBA_DLL_NAME "pg_jieba")
set(PG_JIEBA_LIBRARY_NAME "${PG_JIEBA_DLL_NAME}")

find_program(PostgreSQL_PG_CONFIG NAMES pg_config
        PATHS
        /usr/lib/postgresql/*/bin/)
execute_process(
        COMMAND ${PostgreSQL_PG_CONFIG} --pkglibdir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE PG_JIEBA_EXTENSION_DIR
        RESULT_VARIABLE d)
if(NOT PG_JIEBA_EXTENSION_DIR)
    message(FATAL_ERROR "pg_config --pkglibdir failed to return a value. Please check your PostgreSQL installation!")
endif()
execute_process(
        COMMAND ${PostgreSQL_PG_CONFIG} --sharedir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE PG_SHARE_DIR)
if(NOT PG_SHARE_DIR)
    message(FATAL_ERROR "pg_config --sharedir failed to return a value. Please check your PostgreSQL installation!")
endif()
set(PG_JIEBA_EXTENSION_DATA_DIR "${PG_SHARE_DIR}/extension")
set(PG_JIEBA_SEARCH_DATA_DIR "${PG_SHARE_DIR}/tsearch_data")

execute_process(COMMAND ${PostgreSQL_PG_CONFIG} --bindir
                OUTPUT_STRIP_TRAILING_WHITESPACE
                OUTPUT_VARIABLE T_POSTGRESQL_BIN)

# Checking POSTGRESQL_EXECUTABLE in all the dir (*) - implies that
find_program(PostgreSQL_EXECUTABLE NAMES postgres
             PATHS
             ${T_POSTGRESQL_BIN})
message(STATUS "POSTGRESQL_EXECUTABLE is " ${PostgreSQL_EXECUTABLE})

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${PG_JIEBA_PROJECT_ID}.control"
        PG_JIEBA_CONTROL)

string(REGEX REPLACE ".*default_version = '([0-9.]+)'.*" "\\1"
        PG_JIEBA_VERSION "${PG_JIEBA_CONTROL}")
string(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+)" "\\1"
        PG_JIEBA_VERSION_MAJOR "${PG_JIEBA_VERSION}")
string(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+)" "\\2"
        PG_JIEBA_VERSION_MINOR "${PG_JIEBA_VERSION}")
string(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+)" "\\3"
        PG_JIEBA_VERSION_MICRO "${PG_JIEBA_VERSION}")

if(APPLE)
    set(LIBRARY_MODE_TARGET "MODULE")
else(APPLE)
    set(LIBRARY_MODE_TARGET "SHARED")
endif(APPLE)

add_library(${PG_JIEBA_LIBRARY_NAME} ${LIBRARY_MODE_TARGET} ${SOURCE_FILES})
set_target_properties("${PG_JIEBA_LIBRARY_NAME}" PROPERTIES
                      OUTPUT_NAME "${PG_JIEBA_DLL_NAME}"
                      CXX_STANDARD 11
                      CXX_STANDARD_REQUIRED YES
                      CXX_EXTENSIONS NO
                      POSITION_INDEPENDENT_CODE ON
                      PREFIX "")
if(APPLE)
    set_target_properties(${PG_JIEBA_LIBRARY_NAME}
                          PROPERTIES
                          LINK_FLAGS "-flat_namespace -undefined suppress")
endif(APPLE)
if(WIN32 AND MSVC)
    set_target_properties(${PG_JIEBA_LIBRARY_NAME} PROPERTIES PREFIX "lib")
endif(WIN32 AND MSVC)

install(TARGETS "${PG_JIEBA_LIBRARY_NAME}"
        DESTINATION "${PG_JIEBA_EXTENSION_DIR}")
install(FILES
        "${PROJECT_SOURCE_DIR}/${PG_JIEBA_PROJECT_ID}.control"
        DESTINATION "${PG_JIEBA_EXTENSION_DATA_DIR}")
install(FILES
        "${PROJECT_SOURCE_DIR}/${PG_JIEBA_PROJECT_ID}.sql"
        RENAME "${PG_JIEBA_PROJECT_ID}--${PG_JIEBA_VERSION}.sql"
        DESTINATION "${PG_JIEBA_EXTENSION_DATA_DIR}")
file(GLOB PG_JIEBA_UPGRADE_SQL_FILES "${PG_JIEBA_PROJECT_ID}--*--*.sql")
install(FILES
        ${PG_JIEBA_UPGRADE_SQL_FILES}
        DESTINATION "${PG_JIEBA_EXTENSION_DATA_DIR}")
install(FILES
        "${PROJECT_SOURCE_DIR}/libjieba/dict/jieba.dict.utf8"
        DESTINATION "${PG_JIEBA_SEARCH_DATA_DIR}")
install(FILES
        "${PROJECT_SOURCE_DIR}/libjieba/dict/hmm_model.utf8"
        RENAME "jieba.hmm_model.utf8"
        DESTINATION "${PG_JIEBA_SEARCH_DATA_DIR}")
install(FILES
        "${PROJECT_SOURCE_DIR}/libjieba/dict/user.dict.utf8"
        RENAME "jieba.user.dict.utf8"
        DESTINATION "${PG_JIEBA_SEARCH_DATA_DIR}")
install(FILES
        "${PROJECT_SOURCE_DIR}/libjieba/dict/stop_words.utf8"
        RENAME "jieba.stop"
        DESTINATION "${PG_JIEBA_SEARCH_DATA_DIR}")
install(FILES
        "${PROJECT_SOURCE_DIR}/libjieba/dict/idf.utf8"
        RENAME "jieba.idf.utf8"
        DESTINATION "${PG_JIEBA_SEARCH_DATA_DIR}")