protobuf_generate_cpp(REPLICA_PB_SRCS REPLICA_PB_HDRS protocol.proto)

add_library(replica_proto OBJECT)
target_sources(replica_proto PRIVATE
    ${REPLICA_PB_SRCS}
    ${REPLICA_PB_HDRS}
    Protocol.cc
)
