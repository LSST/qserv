add_library(proto OBJECT)

target_compile_options(proto PRIVATE
    -Wno-maybe-uninitialized
)

protobuf_generate_cpp(PROTO_PB_SRCS PROTO_PB_HDRS worker.proto)

target_sources(proto PRIVATE
    ${PROTO_PB_SRCS}
    ${PROTO_PB_HDRS}
    FrameBuffer.cc
    ProtoHeaderWrap.cc
)

target_link_libraries(proto PUBLIC
    log
    protobuf
)

