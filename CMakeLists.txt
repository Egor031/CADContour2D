cmake_minimum_required(VERSION 3.24)

project(CADContour2D VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "CADContour2D requires Windows x64 and MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.12)

add_library(cadcontour_core STATIC
    src/core/project_state.h src/core/project_state.cpp
    src/core/command_result.h src/core/coordinates.h src/core/coordinates.cpp
)
set_target_properties(cadcontour_core PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(cadcontour_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(cadcontour_core PRIVATE /W4 /permissive- /fp:precise)

add_library(cadcontour_application STATIC
    src/application/project_controller.h src/application/project_controller.cpp
)
target_link_libraries(cadcontour_application PUBLIC cadcontour_core Qt6::Core Qt6::Gui)
target_compile_options(cadcontour_application PRIVATE /W4 /permissive- /fp:precise)

set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
qt_add_qml_module(cadcontour_ui STATIC
    URI CADContour2D
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/CADContour2D"
    QML_FILES qml/Main.qml
    SOURCES src/ui/project_view_model.h src/ui/project_view_model.cpp
)
target_include_directories(cadcontour_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
target_link_libraries(cadcontour_ui PUBLIC cadcontour_application Qt6::Qml
    PRIVATE Qt6::Quick Qt6::QuickControls2
)
target_compile_options(cadcontour_ui PRIVATE /W4 /permissive- /utf-8)

qt_add_executable(CADContour2D WIN32 src/main.cpp)
target_link_libraries(CADContour2D PRIVATE cadcontour_ui cadcontour_uiplugin Qt6::Quick)

include(CTest)
option(CADCONTOUR2D_BUILD_TOOLS "Build development/test tools" OFF)
if(CADCONTOUR2D_BUILD_TOOLS)
    add_subdirectory(tools/synthetic_cloud_generator)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
