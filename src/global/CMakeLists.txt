add_library(global OBJECT)

target_sources(global PRIVATE
    DbTable.cc
    debugUtil.cc
    ResourceUnit.cc
    sqltoken.cc
    stringUtil.cc
)

target_link_libraries(global PUBLIC
    log
)
