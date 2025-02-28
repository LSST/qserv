add_library(rproc OBJECT)
add_dependencies(rproc proto)

target_sources(rproc PRIVATE
    InfileMerger.cc
    ProtoRowBuffer.cc
)

target_link_libraries(rproc PUBLIC
    boost_regex
    cconfig
    log
)

install(DIRECTORY python/ DESTINATION ${CMAKE_INSTALL_PREFIX}/python/lsst/qserv/rproc)
install(DIRECTORY schema/ DESTINATION ${CMAKE_INSTALL_PREFIX}/qserv/smig/rproc/schema/)

FUNCTION(rproc_tests)
    FOREACH(TEST IN ITEMS ${ARGV})
        add_executable(${TEST} ${TEST}.cc)
        target_link_libraries(${TEST} PUBLIC
            cconfig
            ccontrol
            czar
            parser
            qana
            qdisp
            qproc
            query
            qserv_css
            qserv_meta
            rproc
            Boost::unit_test_framework
            Threads::Threads
        )
        add_test(NAME ${TEST} COMMAND ${TEST})
    ENDFOREACH()
ENDFUNCTION()

rproc_tests(
    testInvalidJobAttemptMgr
    testProtoRowBuffer
)
