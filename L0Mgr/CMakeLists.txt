# TODO:
# if(ENABLE_XPU)

# FIXME
set(L0_LIBRARIES ze_loader)

add_library(L0Mgr L0Mgr.cpp)
link_directories(/usr/local/lib)
target_link_libraries(L0Mgr ${L0_LIBRARIES})