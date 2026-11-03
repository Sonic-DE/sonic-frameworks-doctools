# The conversion itself, shared by the program and its tests. It declares no logging component:
# each binary that links it declares its own (see Sonic/Logging/Log).
add_library(docbook2markdown_core STATIC converter.cpp)
target_include_directories(docbook2markdown_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(docbook2markdown_core PUBLIC Qt6::Core Sonic::Logging)

add_executable(docbook2markdown6 main.cpp)
ecm_mark_nongui_executable(docbook2markdown6)
target_include_directories(docbook2markdown6 PRIVATE "${CMAKE_BINARY_DIR}/src") # kdoctools_version.h
target_link_libraries(docbook2markdown6 PRIVATE docbook2markdown_core)
install(TARGETS docbook2markdown6 EXPORT KF6DocToolsToolsTargets ${KF_INSTALL_TARGETS_DEFAULT_ARGS})
add_executable(KF6::docbook2markdown6 ALIAS docbook2markdown6)
