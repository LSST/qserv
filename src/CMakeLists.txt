set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ANTLR_EXECUTABLE /usr/share/java/antlr-4.8-complete.jar)
find_package(ANTLR REQUIRED)
find_package(antlr4-runtime REQUIRED)

find_package(Boost REQUIRED
    filesystem REQUIRED
    program_options REQUIRED
    regex REQUIRED
    system REQUIRED
    unit_test_framework REQUIRED
)

find_package(Lua51 REQUIRED)
find_package(Protobuf REQUIRED)
find_package(pybind11 REQUIRED)
find_package(Threads REQUIRED)
find_package(XRootD REQUIRED)
find_package(httplib REQUIRED)
find_package(aws-c-auth REQUIRED)
find_package(aws-c-cal REQUIRED)
find_package(aws-c-common REQUIRED)
find_package(aws-c-compression REQUIRED)
find_package(aws-c-event-stream REQUIRED)
find_package(aws-c-http REQUIRED)
find_package(aws-c-io REQUIRED)
find_package(aws-c-mqtt REQUIRED)
find_package(aws-c-s3 REQUIRED)
find_package(aws-c-sdkutils REQUIRED)
find_package(aws-checksums REQUIRED)
find_package(aws-crt-cpp REQUIRED)

add_compile_options(-Og -g -Wall -Wextra -Wno-unused-parameter)
add_compile_definitions(BOOST_TEST_DYN_LINK=1)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

file(GLOB_RECURSE CLANG_FILES *.cc *.h)

add_custom_target(clang-format
    COMMAND /usr/bin/clang-format -style=file --verbose -i ${CLANG_FILES}
)

add_custom_target(clang-format-check
    COMMAND /usr/bin/clang-format -style=file --verbose -i --dry-run -Werror ${CLANG_FILES}
)

#-----------------------------------------------------------------------------

add_subdirectory(admin)
add_subdirectory(cconfig)
add_subdirectory(ccontrol)
add_subdirectory(css)
add_subdirectory(czar)
add_subdirectory(global)
add_subdirectory(http)
add_subdirectory(memman)
add_subdirectory(mimic)
add_subdirectory(mysql)
add_subdirectory(parser)
add_subdirectory(partition)
add_subdirectory(proto)
add_subdirectory(protojson)
add_subdirectory(proxy)
add_subdirectory(qana)
add_subdirectory(qdisp)
add_subdirectory(qhttp)
add_subdirectory(qmeta)
add_subdirectory(qproc)
add_subdirectory(query)
add_subdirectory(replica)
add_subdirectory(rproc)
add_subdirectory(schema)
add_subdirectory(sql)
add_subdirectory(util)
add_subdirectory(wbase)
add_subdirectory(wconfig)
add_subdirectory(wcontrol)
add_subdirectory(worker)
add_subdirectory(wdb)
add_subdirectory(wpublish)
add_subdirectory(wsched)
add_subdirectory(www)
add_subdirectory(xrdlog)
add_subdirectory(xrdsvc)

#-----------------------------------------------------------------------------

add_library(qserv_common SHARED)

target_link_libraries(qserv_common PUBLIC
    global
    memman
    proto
    mysql
    sql
    util
    protojson
)

install(
    TARGETS qserv_common
)

#-----------------------------------------------------------------------------

add_library(xrdsvc SHARED)

target_link_libraries(xrdsvc PUBLIC
    wbase
    wcontrol
    wconfig
    wdb
    wpublish
    wsched
    qserv_xrdsvc
    qserv_common
)

install(
    TARGETS xrdsvc
)

#-----------------------------------------------------------------------------

add_library(qserv_czar SHARED)

target_link_libraries(qserv_czar PUBLIC
    ccontrol
    czar
    parser
    qana
    query
    qdisp
    qproc
    rproc
    qserv_css
    qserv_meta
)

install(
    TARGETS qserv_czar
)
