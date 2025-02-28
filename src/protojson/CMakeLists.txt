add_library(protojson SHARED)

target_sources(protojson PRIVATE
	ScanTableInfo.cc
    UberJobMsg.cc
    WorkerQueryStatusData.cc
)

target_link_libraries(protojson PUBLIC
    curl
    http
    log
    qhttp
    util
    Boost::filesystem
    Boost::regex
    Boost::system
    cpp-httplib
)

install(TARGETS protojson)

function(PROTOJSON_TESTS)
    foreach(TEST IN ITEMS ${ARGV})
        add_executable(${TEST} ${TEST}.cc)
        target_link_libraries(${TEST} PUBLIC
            global
            http
            protojson
            Boost::unit_test_framework
            Threads::Threads
        )
        add_test(NAME ${TEST} COMMAND ${TEST})
    endforeach()
endfunction()

protojson_tests(
    testStatusData
    testUberJobMsg
)
