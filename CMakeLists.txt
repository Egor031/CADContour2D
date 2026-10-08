cmake_minimum_required(VERSION 3.24)

project(CADContour2D VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "CADContour2D requires Windows x64 and MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.12)

qt_add_executable(CADContour2D WIN32 src/main.cpp)
set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
qt_add_qml_module(CADContour2D
    URI CADContour2D
    VERSION 1.0
    QML_FILES qml/Main.qml
)
target_link_libraries(CADContour2D PRIVATE
    Qt6::Core Qt6::Qml Qt6::Quick Qt6::QuickControls2
)

include(CTest)
option(CADCONTOUR2D_BUILD_TOOLS "Build development/test tools" OFF)
if(CADCONTOUR2D_BUILD_TOOLS)
    add_subdirectory(tools/synthetic_cloud_generator)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
