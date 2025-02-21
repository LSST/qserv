add_library(qproc OBJECT)
add_dependencies(qproc proto)

target_sources(qproc PRIVATE
    ChunkQuerySpec.cc
    ChunkSpec.cc
    DatabaseModels.cc
    IndexMap.cc
    QuerySession.cc
    SecondaryIndex.cc
)

target_link_libraries(qproc PRIVATE
    cconfig
    log
    sphgeom
)

FUNCTION(qproc_tests)
    FOREACH(TEST IN ITEMS ${ARGV})
        add_executable(${TEST} ${TEST}.cc ../tests/QueryAnaHelper.cc)
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

qproc_tests(
    testChunkSpec
    testGeomAdapter
    testIndexMap
    testQueryAnaAggregation
    testQueryAnaBetween
    testQueryAnaDuplSelectExpr
    testQueryAnaGeneral
    testQueryAnaIn
    testQueryAnaOrderBy
)
