add_executable(cache_benchmark main.cpp)
set_target_properties(cache_benchmark PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(cache_benchmark PRIVATE cadcontour_cache Psapi)
target_compile_options(cache_benchmark PRIVATE /W4 /permissive- /fp:precise /utf-8)
