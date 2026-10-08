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
gtest_discover_tests(dependency_smoke DISCOVERY_MODE PRE_TEST)

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
