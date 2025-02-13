add_library(replica_worker OBJECT)
add_dependencies(replica_worker replica_proto)
target_sources(replica_worker PRIVATE
    FileClient.cc
    FileServer.cc
    FileServerConnection.cc
    WorkerCreateReplicaHttpRequest.cc
    WorkerDeleteReplicaHttpRequest.cc
    WorkerDeleteRequest.cc
    WorkerDirectorIndexHttpRequest.cc
    WorkerDirectorIndexRequest.cc
    WorkerEchoHttpRequest.cc
    WorkerEchoRequest.cc
    WorkerFindAllRequest.cc
    WorkerFindAllReplicasHttpRequest.cc
    WorkerFindReplicaHttpRequest.cc
    WorkerFindRequest.cc
    WorkerHttpProcessor.cc
    WorkerHttpProcessorThread.cc
    WorkerHttpRequest.cc
    WorkerHttpSvc.cc
    WorkerHttpSvcMod.cc
    WorkerProcessor.cc
    WorkerProcessorThread.cc
    WorkerReplicationRequest.cc
    WorkerRequest.cc
    WorkerServer.cc
    WorkerServerConnection.cc
    WorkerSqlHttpRequest.cc
    WorkerSqlRequest.cc
)
target_link_libraries(replica_worker PUBLIC
    log
)
