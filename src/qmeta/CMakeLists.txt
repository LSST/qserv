add_library(qserv_meta SHARED)

target_sources(qserv_meta PRIVATE
    JobStatus.cc
    MessageStore.cc
    QMeta.cc
    QMetaMysql.cc
    QMetaSelect.cc
    QMetaTransaction.cc
    QStatusMysql.cc
)

target_link_libraries(qserv_meta PUBLIC
    cconfig
    qserv_common
    log
    mysqlclient_r
    http 
)

install(TARGETS qserv_meta)
install(DIRECTORY schema/ DESTINATION ${CMAKE_INSTALL_PREFIX}/qserv/smig/qmeta/schema/)

add_executable(testQMeta testQMeta.cc)

target_link_libraries(testQMeta
    cconfig
    qserv_meta
    Boost::unit_test_framework
    Threads::Threads
)

# don't add test, needs fix; DM-30562: fix Qserv unit tests that prompt for an sql connection
# add_test(NAME testQMeta COMMAND testQMeta)

#-----------------------------------------------------------------------------

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/python/ DESTINATION ${CMAKE_INSTALL_PREFIX}/python/lsst/qserv/qmeta)
