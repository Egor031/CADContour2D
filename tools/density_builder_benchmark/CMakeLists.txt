add_executable(density_builder_benchmark main.cpp)
set_target_properties(density_builder_benchmark PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(density_builder_benchmark PRIVATE cadcontour_density cadcontour_cache Psapi)
target_compile_options(density_builder_benchmark PRIVATE /W4 /permissive- /fp:strict /utf-8)
target_compile_definitions(density_builder_benchmark PRIVATE
    DENSITY_BUILDER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    DENSITY_BUILDER_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    DENSITY_BUILDER_CONFIGURATION="$<CONFIG>")
