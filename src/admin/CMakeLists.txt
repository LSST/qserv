install(DIRECTORY python/lsst/qserv/admin DESTINATION ${CMAKE_INSTALL_PREFIX}/python/lsst/qserv)
install(FILES bin/entrypoint DESTINATION ${CMAKE_INSTALL_BINDIR} PERMISSIONS OWNER_READ OWNER_EXECUTE WORLD_READ WORLD_EXECUTE)
install(DIRECTORY schema/ DESTINATION ${CMAKE_INSTALL_PREFIX}/qserv/smig/admin/schema/)
install(DIRECTORY etc/ DESTINATION ${CMAKE_INSTALL_PREFIX}/etc FILE_PERMISSIONS OWNER_READ OWNER_WRITE WORLD_READ WORLD_WRITE)
install(DIRECTORY templates/ DESTINATION ${CMAKE_INSTALL_PREFIX}/qserv/templates USE_SOURCE_PERMISSIONS)
install(FILES etc/log.cnf DESTINATION ${CMAKE_INSTALL_PREFIX}/log PERMISSIONS OWNER_READ OWNER_WRITE WORLD_READ)

add_test (NAME test_watcher
  COMMAND python3 -m unittest tests.admin.test_watcher
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)

set_tests_properties(test_watcher PROPERTIES WILL_FAIL 1)

add_test(NAME test_smig
  COMMAND python3 -m unittest lsst.qserv.admin.tests.test_smig
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)

set_tests_properties(test_smig PROPERTIES ENVIRONMENT QSERV_SMIG_DIRECTORY=${CMAKE_INSTALL_PREFIX}/qserv/smig)

add_test(NAME test_qserv_cli
  COMMAND python3 -m unittest lsst.qserv.admin.tests.test_qserv_cli
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)

add_test(NAME test_itest
  COMMAND python3 -m unittest lsst.qserv.admin.tests.test_itest
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)

add_test(NAME test_utils
  COMMAND python3 -m unittest lsst.qserv.admin.tests.test_utils
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)

add_test(NAME test_options_file_option
  COMMAND python3 -m unittest lsst.qserv.admin.tests.test_options_file_option
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)

add_test(NAME test_qserv_log
  COMMAND python3 -m unittest lsst.qserv.admin.tests.test_qserv_log
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)

add_test(NAME test_render_targs
  COMMAND python3 -m unittest lsst.qserv.admin.tests.test_render_targs
  WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/python/
)
