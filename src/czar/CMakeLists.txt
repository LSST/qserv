add_library(czar OBJECT)
add_dependencies(czar proto)

target_sources(czar PRIVATE
    ActiveWorker.cc
    ChttpModule.cc
    Czar.cc
    CzarChunkMap.cc
    CzarRegistry.cc
    HttpCzarIngestCsvModule.cc
    HttpCzarIngestModuleBase.cc
    HttpCzarIngestModule.cc
    HttpCzarQueryModule.cc
    HttpCzarSvc.cc
    HttpCzarWorkerModule.cc
    HttpMonitorModule.cc
    HttpSvc.cc
    MessageTable.cc
    QhttpModule.cc
    WorkerIngestProcessor.cc
)

target_include_directories(czar PRIVATE
    ${XROOTD_INCLUDE_DIRS}
)

target_link_libraries(czar PUBLIC
    cconfig
    http
    protojson
    qdisp
    qhttp
    util
    log
    XrdSsiLib
    cpp-httplib
    boost_program_options
)

function(CZAR_UTILS)
    foreach(UTIL IN ITEMS ${ARGV})
        add_executable(${UTIL})
        target_sources(${UTIL} PRIVATE ${UTIL}.cc)
        target_include_directories(${UTIL} PRIVATE ${XROOTD_INCLUDE_DIRS})
        target_link_libraries(${UTIL} PRIVATE
            cconfig
            ccontrol
            czar
            global
            mysql
            parser
            qana
            qdisp
            qproc
            qserv_meta
            query
            rproc
            sql
        )
        install(TARGETS ${UTIL})
    endforeach()
endfunction()

czar_utils(
    qserv-czar-http
)

function(czar_tests)
    foreach(TEST IN ITEMS ${ARGV})
        add_executable(${TEST} ${TEST}.cc)
        target_link_libraries(${TEST} PUBLIC
            cconfig
            ccontrol
            czar
            global
            mysql
            parser
            qana
            qdisp
            qproc
            qserv_meta
            query
            rproc
            sql
            Boost::unit_test_framework
        )
        add_test(NAME ${TEST} COMMAND ${TEST})
    endforeach()
endfunction()

czar_tests(
    testCzar
)

