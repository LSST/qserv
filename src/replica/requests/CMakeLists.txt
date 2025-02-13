add_library(replica_requests OBJECT)
add_dependencies(replica_requests replica_proto)
target_sources(replica_requests PRIVATE
    DeleteRequest.cc
    DirectorIndexRequest.cc
    DisposeRequest.cc
    EchoRequest.cc
    FindAllRequest.cc
    FindRequest.cc
    Messenger.cc
    MessengerConnector.cc
    ReplicationRequest.cc
    Request.cc
    RequestTracker.cc
    ServiceManagementRequest.cc
    ServiceManagementRequestBase.cc
    SqlAlterTablesRequest.cc
    SqlCreateDbRequest.cc
    SqlCreateIndexesRequest.cc
    SqlCreateTableRequest.cc
    SqlCreateTablesRequest.cc
    SqlDeleteDbRequest.cc
    SqlDeleteTablePartitionRequest.cc
    SqlDeleteTableRequest.cc
    SqlDisableDbRequest.cc
    SqlDropIndexesRequest.cc
    SqlEnableDbRequest.cc
    SqlGetIndexesRequest.cc
    SqlGrantAccessRequest.cc
    SqlQueryRequest.cc
    SqlRemoveTablePartitionsRequest.cc
    SqlRequest.cc
    SqlResultSet.cc
    SqlRowStatsRequest.cc
    StatusRequest.cc
    StopRequest.cc
)
target_link_libraries(replica_requests PUBLIC
    log
)
