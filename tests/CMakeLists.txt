find_package(OpenCV 4 REQUIRED COMPONENTS core)
find_package(Eigen3 CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(dependency_smoke dependency_smoke.cpp)
set_target_properties(dependency_smoke PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(dependency_smoke PRIVATE
    opencv_core Eigen3::Eigen GTest::gtest_main spdlog::spdlog
)

include(GoogleTest)
gtest_discover_tests(dependency_smoke DISCOVERY_MODE PRE_TEST PROPERTIES LABELS production)

add_executable(cache_tests cache_codec_tests.cpp cache_format_tests.cpp cache_file_tests.cpp cache_stream_sort_tests.cpp
    cache_writer_tests.cpp cache_reader_tests.cpp)
set_target_properties(cache_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(cache_tests PRIVATE cadcontour_cache GTest::gtest_main)
target_compile_options(cache_tests PRIVATE /W4 /permissive- /fp:precise /utf-8)
gtest_discover_tests(cache_tests DISCOVERY_MODE PRE_TEST TEST_FILTER "-CacheReaderIntegration.*"
    PROPERTIES LABELS production TIMEOUT 30)
add_test(NAME cache_reader_integration COMMAND cache_tests --gtest_filter=CacheReaderIntegration.*)
set_tests_properties(cache_reader_integration PROPERTIES LABELS cache_integration TIMEOUT 300)

add_executable(project_tests project_tests.cpp coordinate_tests.cpp)
set_target_properties(project_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(project_tests PRIVATE cadcontour_application GTest::gtest_main)
target_compile_options(project_tests PRIVATE /W4 /permissive- /fp:precise)
gtest_discover_tests(project_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS production)

add_executable(operation_tests operation_test_main.cpp operation_tests.cpp)
set_target_properties(operation_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(operation_tests PRIVATE cadcontour_application GTest::gtest)
target_compile_options(operation_tests PRIVATE /W4 /permissive- /utf-8)
gtest_discover_tests(operation_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS production)

add_executable(view_model_tests view_model_tests.cpp)
set_target_properties(view_model_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(view_model_tests PRIVATE cadcontour_ui cadcontour_uiplugin GTest::gtest_main)
target_compile_options(view_model_tests PRIVATE /W4 /permissive- /utf-8)
gtest_discover_tests(view_model_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS production)

qt_add_executable(qml_integration_tests qml_test_main.cpp qml_integration_tests.cpp)
target_link_libraries(qml_integration_tests PRIVATE
    cadcontour_ui cadcontour_uiplugin Qt6::Quick GTest::gtest)
target_compile_options(qml_integration_tests PRIVATE /W4 /permissive- /utf-8)
gtest_discover_tests(qml_integration_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS production)

set_source_files_properties(ProbeWindow.qml PROPERTIES QT_RESOURCE_ALIAS ProbeWindow.qml)
qt_add_qml_module(cadcontour_verification STATIC
    URI CADContour2D.Verification VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CADContour2D/Verification"
    QML_FILES ProbeWindow.qml
    SOURCES operation_probe.h operation_probe.cpp
    IMPORTS CADContour2D
)
target_link_libraries(cadcontour_verification PUBLIC cadcontour_ui cadcontour_uiplugin Qt6::Quick)
target_compile_options(cadcontour_verification PRIVATE /W4 /permissive- /utf-8)
qt_add_executable(operation_probe operation_probe_main.cpp)
target_link_libraries(operation_probe PRIVATE cadcontour_verification cadcontour_verificationplugin)
target_compile_options(operation_probe PRIVATE /W4 /permissive- /utf-8)

if(CADCONTOUR2D_BUILD_TOOLS)
    add_executable(synthetic_generator_tests synthetic_generator_tests.cpp)
    set_target_properties(synthetic_generator_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
    target_link_libraries(synthetic_generator_tests PRIVATE synthetic_generator GTest::gtest_main)
    target_compile_definitions(synthetic_generator_tests PRIVATE
        SYNTHETIC_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/synthetic"
        SYNTHETIC_GENERATOR_EXE="$<TARGET_FILE:synthetic-cloud-generator>"
    )
    add_dependencies(synthetic_generator_tests synthetic-cloud-generator)
    gtest_discover_tests(synthetic_generator_tests DISCOVERY_MODE PRE_TEST)
endif()
