install(DIRECTORY python/ DESTINATION ${CMAKE_INSTALL_PREFIX}/python/lsst/qserv/schema)
install(DIRECTORY bin/ DESTINATION ${CMAKE_INSTALL_BINDIR})

add_test(NAME test_schema_mig_mgr
  COMMAND python3 -m unittest lsst.qserv.schema.tests.test_schema_mig_mgr
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)
