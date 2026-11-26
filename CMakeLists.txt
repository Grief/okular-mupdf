cmake_minimum_required(VERSION 3.22)
project(okular-mupdf VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(QT_MAJOR_VERSION 6)
find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs6)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Xml PrintSupport)
find_package(KF6CoreAddons REQUIRED)
find_package(Okular6 REQUIRED)

find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h REQUIRED)
find_library(MUPDF_LIBRARY mupdf REQUIRED)

set(OKULAR_GENERATOR_INSTALL_DIR "${KDE_INSTALL_PLUGINDIR}/okular_generators" CACHE PATH "Directory Okular loads generator plugins from")

add_library(okularGenerator_mupdf MODULE generator_mupdf.cpp fitz.cpp)
set_target_properties(okularGenerator_mupdf PROPERTIES PREFIX "")
target_include_directories(okularGenerator_mupdf SYSTEM PRIVATE "${MUPDF_INCLUDE_DIR}")
target_compile_options(okularGenerator_mupdf PRIVATE -Wall -Wextra)
target_link_options(okularGenerator_mupdf PRIVATE -Wl,--no-undefined)
target_link_libraries(okularGenerator_mupdf PRIVATE
    Okular::Core
    KF6::CoreAddons
    Qt6::Core
    Qt6::Gui
    Qt6::Xml
    Qt6::PrintSupport
    "${MUPDF_LIBRARY}")

install(TARGETS okularGenerator_mupdf LIBRARY DESTINATION "${OKULAR_GENERATOR_INSTALL_DIR}")
