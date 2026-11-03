find_package(Qt6Test ${REQUIRED_QT_VERSION} CONFIG REQUIRED)
include(ECMAddTests)

ecm_add_test(docbook2markdowntest.cpp
    TEST_NAME docbook2markdowntest
    LINK_LIBRARIES docbook2markdown_core Qt6::Test
)
target_compile_definitions(docbook2markdowntest PRIVATE DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# The program end to end on the fixture pages: exit code 0 and its files written.
add_test(NAME docbook2markdown6_run
    COMMAND docbook2markdown6 --module testmodule --qt-docs-dir "${CMAKE_CURRENT_SOURCE_DIR}/data/qtdocs"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/docbook" "${CMAKE_CURRENT_BINARY_DIR}/output"
)
add_test(NAME docbook2markdown6_output
    COMMAND ${CMAKE_COMMAND} -E cat
        "${CMAKE_CURRENT_BINARY_DIR}/output/toc.yaml"
        "${CMAKE_CURRENT_BINARY_DIR}/output/testmodule/testwatcher.md"
        "${CMAKE_CURRENT_BINARY_DIR}/output/testmodule/testmodule-module.md"
)
set_tests_properties(docbook2markdown6_output PROPERTIES DEPENDS docbook2markdown6_run)
