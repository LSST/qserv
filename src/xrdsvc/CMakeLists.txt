add_library(qserv_xrdsvc OBJECT)
add_dependencies(qserv_xrdsvc proto)

target_sources(qserv_xrdsvc PRIVATE
    HttpModule.cc
    HttpMonitorModule.cc
    HttpReplicaMgtModule.cc
    HttpWorkerCzarModule.cc
    HttpSvc.cc
    SsiProvider.cc
    SsiService.cc
)

target_include_directories(qserv_xrdsvc PRIVATE
    ${XROOTD_INCLUDE_DIRS}
)

target_link_libraries(qserv_xrdsvc PUBLIC
    boost_filesystem
    boost_system
    log
    http
    XrdSsiLib
)
