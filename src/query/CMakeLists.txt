add_library(query OBJECT)
add_dependencies(query proto)

target_sources(query PRIVATE
    AggOp.cc
    AndTerm.cc
    AreaRestrictor.cc
    BetweenPredicate.cc
    BoolFactor.cc
    BoolTerm.cc
    BoolTermFactor.cc
    ColumnRef.cc
    CompPredicate.cc
    FromList.cc
    FuncExpr.cc
    GroupByClause.cc
    HavingClause.cc
    InPredicate.cc
    JoinRef.cc
    JoinSpec.cc
    LikePredicate.cc
    LogicalTerm.cc
    NullPredicate.cc
    OrderByClause.cc
    OrTerm.cc
    PassListTerm.cc
    PassTerm.cc
    Predicate.cc
    QueryContext.cc
    QueryTemplate.cc
    SecIdxRestrictor.cc
    SelectList.cc
    SelectStmt.cc
    TableRef.cc
    TestFactory.cc
    UnknownTerm.cc
    ValueExpr.cc
    ValueExprPredicate.cc
    ValueFactor.cc
    WhereClause.cc
)

target_link_libraries(query PUBLIC
    log
    sphgeom
)

FUNCTION(query_tests)
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
            qserv_css
            qserv_meta
            query
            rproc
            Boost::unit_test_framework
            Threads::Threads
        )
        add_test(NAME ${TEST} COMMAND ${TEST})
    ENDFOREACH()
ENDFUNCTION()

query_tests(
    testAreaRestrictor
    testColumnRef
    testOrderBy
    testQueryContext
    testRepr
    testSecIdxRestrictor
    testTableRef
    testValueExpr
)
