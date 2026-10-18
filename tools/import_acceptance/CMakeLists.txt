qt_add_executable(import_acceptance main.cpp validation.cpp)
target_link_libraries(import_acceptance PRIVATE cadcontour_ui cadcontour_uiplugin Qt6::Quick Psapi)
target_include_directories(import_acceptance PRIVATE ../../tests)
target_compile_options(import_acceptance PRIVATE /W4 /permissive- /fp:precise /utf-8)
