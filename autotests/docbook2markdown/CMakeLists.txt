find_package(Qt6Test ${REQUIRED_QT_VERSION} CONFIG REQUIRED)
include(ECMAddTests)

ecm_add_test(docbook2markdowntest.cpp
    TEST_NAME docbook2markdowntest
    LINK_LIBRARIES docbook2markdown_core Qt6::Test
)
target_compile_definitions(docbook2markdowntest PRIVATE DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# The program end to end on the fixture pages, converted as two modules into one fresh output
# directory: each run exits 0, and the second run's toc.yaml covers both modules.
set(output "${CMAKE_CURRENT_BINARY_DIR}/output")
add_test(NAME docbook2markdown6_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${output}")
add_test(NAME docbook2markdown6_run
    COMMAND docbook2markdown6 --module testmodule --qt-docs-dir "${CMAKE_CURRENT_SOURCE_DIR}/data/qtdocs"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/docbook" "${output}"
)
add_test(NAME docbook2markdown6_run_second_module
    COMMAND docbook2markdown6 --module othermodule --qt-docs-dir "${CMAKE_CURRENT_SOURCE_DIR}/data/qtdocs"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/docbook" "${output}"
)
add_test(NAME docbook2markdown6_output
    COMMAND ${CMAKE_COMMAND} -E cat
        "${output}/toc.yaml"
        "${output}/index.md"
        "${output}/testmodule/module.json"
        "${output}/testmodule/testwatcher.md"
        "${output}/othermodule/testmodule-module.md"
)
set_tests_properties(docbook2markdown6_run PROPERTIES DEPENDS docbook2markdown6_clean)
set_tests_properties(docbook2markdown6_run_second_module PROPERTIES DEPENDS docbook2markdown6_run)
set_tests_properties(docbook2markdown6_output PROPERTIES DEPENDS docbook2markdown6_run_second_module)
# The combined toc.yaml lists both modules' pages (one regex per test: CTest passes on any match).
foreach(module testmodule othermodule)
    add_test(NAME docbook2markdown6_toc_lists_${module} COMMAND ${CMAKE_COMMAND} -E cat "${output}/toc.yaml")
    set_tests_properties(docbook2markdown6_toc_lists_${module} PROPERTIES
        DEPENDS docbook2markdown6_run_second_module
        PASS_REGULAR_EXPRESSION "      ${module}/testwatcher: "
    )
endforeach()
