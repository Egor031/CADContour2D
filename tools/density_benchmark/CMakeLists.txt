add_library(density_experiment STATIC dataset.cpp oracle.cpp measurement.cpp)
set_target_properties(density_experiment PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(density_experiment PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(density_experiment PUBLIC cadcontour_cache PRIVATE Psapi)
target_compile_options(density_experiment PRIVATE /W4 /permissive- /fp:precise /utf-8)

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE density_head OUTPUT_STRIP_TRAILING_WHITESPACE)
target_compile_definitions(density_experiment PRIVATE
    DENSITY_BUILD_HEAD="${density_head}" DENSITY_BUILD_CONFIG="$<CONFIG>"
    DENSITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")

add_executable(density_benchmark main.cpp)
set_target_properties(density_benchmark PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(density_benchmark PRIVATE density_experiment synthetic_generator)
target_compile_options(density_benchmark PRIVATE /W4 /permissive- /fp:precise /utf-8)
target_compile_definitions(density_benchmark PRIVATE
    DENSITY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_subdirectory(construction)
