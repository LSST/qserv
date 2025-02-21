
add_library(replica SHARED)

target_compile_options(replica PRIVATE
    -Wno-maybe-uninitialized
)

target_include_directories(replica PRIVATE
    ${XROOTD_INCLUDE_DIRS}
)

target_link_libraries(replica PUBLIC
    replica_apps
    replica_config
    replica_contr
    replica_export
    replica_ingest
    replica_jobs
    replica_mysql
    replica_proto
    replica_qserv
    replica_registry
    replica_requests
    replica_services
    replica_util
    replica_worker
    qserv_css
    xrdsvc
    XrdCl
    XrdSsiLib
    http
    qhttp
    sphgeom
    partition
    protobuf
    boost_filesystem
    boost_system
    log
    pthread
    crypto
    curl
    cpp-httplib
    aws-cpp-sdk-core
    aws-cpp-sdk-s3
)

function(REPLICA_UTILS)
    foreach(UTIL IN ITEMS ${ARGV})
        add_executable(${UTIL})
        target_sources(${UTIL} PRIVATE tools/${UTIL}.cc)
        target_include_directories(${UTIL} PRIVATE ${XROOTD_INCLUDE_DIRS})
        target_link_libraries(${UTIL} PRIVATE
            replica
        )
        install(TARGETS ${UTIL})
    endforeach()
endfunction()

replica_utils(
    qserv-replica-calc-cs
    qserv-replica-config-test
    qserv-replica-config
    qserv-replica-controller-cmd
    qserv-replica-file
    qserv-replica-job
    qserv-replica-master-http
    qserv-replica-registry
    qserv-replica-test
    qserv-replica-worker-notify
    qserv-replica-worker
)

install(DIRECTORY python/ DESTINATION ${CMAKE_INSTALL_PREFIX}/python/lsst/qserv/replica)
install(DIRECTORY schema/ DESTINATION ${CMAKE_INSTALL_PREFIX}/qserv/smig/replica/schema/)

install(
    TARGETS replica
)

function(REPLICA_TESTS)
    foreach(TEST IN ITEMS ${ARGV})
        add_executable(${TEST} tests/${TEST}.cc)
        target_link_libraries(${TEST} PUBLIC
            replica
            Boost::unit_test_framework
            Threads::Threads
        )
        add_test(NAME ${TEST} COMMAND ${TEST})
    endforeach()
endfunction()

replica_tests(
    testApplicationParser
    testChunkLocker
    testChunkNumber
    testChunkedTable
    testConnectionParams
    testCsv
    testFileIngestApp
    testFileUtils
    testIngestRequestMgr
    testJson
    testMessageQueue
    testNamedMutexRegistry
    testQueryGenerator
    testReplicaInfo
    testSemanticMap
    testSqlResultSet
    testSqlSchemaUtils
    testTaskSelector
    testTypes
    testConfiguration
)

add_subdirectory(apps)
add_subdirectory(config)
add_subdirectory(contr)
add_subdirectory(export)
add_subdirectory(ingest)
add_subdirectory(jobs)
add_subdirectory(mysql)
add_subdirectory(proto)
add_subdirectory(qserv)
add_subdirectory(registry)
add_subdirectory(requests)
add_subdirectory(services)
add_subdirectory(util)
add_subdirectory(worker)
