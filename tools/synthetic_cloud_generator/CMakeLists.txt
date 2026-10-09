add_library(synthetic_generator STATIC input.cpp geometry.cpp scan.cpp artifacts.cpp output.cpp)
set_target_properties(synthetic_generator PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(synthetic_generator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(synthetic_generator PUBLIC Qt6::Core)
target_compile_options(synthetic_generator PRIVATE /W4 /permissive- /fp:precise)

add_executable(synthetic-cloud-generator main.cpp)
set_target_properties(synthetic-cloud-generator PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(synthetic-cloud-generator PRIVATE synthetic_generator)
