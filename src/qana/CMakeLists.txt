add_library(qana OBJECT)
add_dependencies(qana proto)

target_sources(qana PRIVATE
    AggregatePlugin.cc
    ColumnVertexMap.cc
    DuplSelectExprPlugin.cc
    MatchTablePlugin.cc
    PostPlugin.cc
    QservRestrictorPlugin.cc
    QueryMapping.cc
    RelationGraph.cc
    ScanTablePlugin.cc
    TableInfo.cc
    TableInfoPool.cc
    TablePlugin.cc
    WherePlugin.cc
)

target_link_libraries(qana PUBLIC
    log
)

FUNCTION(qana_tests)
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

qana_tests(
    testDuplSelectExprPlugin
    testPlugins
    testTablePlugin
)

set_tests_properties(testPlugins testTablePlugin PROPERTIES WILL_FAIL 1)
