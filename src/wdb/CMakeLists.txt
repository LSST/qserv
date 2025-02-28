add_library(wdb OBJECT)
add_dependencies(wdb proto)

target_sources(wdb PRIVATE
    ChunkResource.cc
    QueryRunner.cc
    SQLBackend.cc
)

target_include_directories(wdb PRIVATE
    ${XROOTD_INCLUDE_DIRS}
)

target_link_libraries(wdb PUBLIC
    log
    XrdSsiLib
)

FUNCTION(wdb_tests)
    FOREACH(TEST IN ITEMS ${ARGV})
        add_executable(${TEST} ${TEST}.cc)
        target_include_directories(${TEST} PRIVATE
            ${XROOTD_INCLUDE_DIRS}
        )
        target_link_libraries(${TEST} PUBLIC
            crypto
            xrdsvc
            Boost::unit_test_framework
            Threads::Threads
        )
        add_test(NAME ${TEST} COMMAND ${TEST})
    ENDFOREACH()
ENDFUNCTION()

wdb_tests(
    testChunkResource
    testQueryRunner
)

# For this test to work, a mariadb server needs to be available.
# This functionality is covered by integration tests.
set_tests_properties(testQueryRunner PROPERTIES WILL_FAIL 1)
