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
