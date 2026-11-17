cmake_minimum_required(VERSION 3.21)
project(ShoalWallpaper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick OpenGL DBus)
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
qt_standard_project_setup(REQUIRES 6.5)

# The engine, Linux helpers and shaders are shared with the other builds (../ShoalCore).
set(SHOAL_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../ShoalCore)
file(GLOB SHOAL_SHADERS ${SHOAL_CORE}/shaders/*)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders_gen.cpp
    COMMAND Python3::Interpreter ${SHOAL_CORE}/embed_shaders.py ${SHOAL_CORE}/shaders ${CMAKE_CURRENT_BINARY_DIR}/shaders_gen.cpp
    DEPENDS ${SHOAL_SHADERS} ${SHOAL_CORE}/embed_shaders.py
    COMMENT "Embedding ShoalCore shaders")

# The QML module org.shoal.wallpaper (ShoalView item + ShoalConfig singleton), built as a plugin only.
qt_add_qml_module(shoalwallpaperplugin
    URI org.shoal.wallpaper
    VERSION 1.0
    PLUGIN_TARGET shoalwallpaperplugin
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml/org/shoal/wallpaper
    SOURCES src/shoalitem.h src/shoalitem.cpp src/shoaloffload.h src/shoaloffload.cpp
        ${SHOAL_CORE}/shoal_engine.h ${SHOAL_CORE}/shoal_engine.cpp ${SHOAL_CORE}/shoal_linux.h ${SHOAL_CORE}/shoal_linux.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/shaders_gen.cpp
)

target_include_directories(shoalwallpaperplugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${SHOAL_CORE})
target_link_libraries(shoalwallpaperplugin PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::OpenGL Qt6::DBus OpenGL::OpenGL OpenGL::EGL)

# Installed into a user QML import path (see install.sh), not system-wide.
set(SHOAL_QML_DIR "$ENV{HOME}/.local/lib/qt6/qml" CACHE PATH "QML import directory to install the plugin into")
set(_dest ${SHOAL_QML_DIR}/org/shoal/wallpaper)
install(TARGETS shoalwallpaperplugin LIBRARY DESTINATION ${_dest})
install(FILES
    ${CMAKE_BINARY_DIR}/qml/org/shoal/wallpaper/qmldir
    ${CMAKE_BINARY_DIR}/qml/org/shoal/wallpaper/shoalwallpaperplugin.qmltypes
    DESTINATION ${_dest})
