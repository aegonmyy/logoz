cmake_minimum_required(VERSION 3.14)
project(LogosChroniclePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/metadata.json ${CMAKE_CURRENT_BINARY_DIR}/metadata.json COPYONLY)

logos_module(
    NAME chronicle
    SOURCES
        src/chronicle_interface.h
        src/chronicle_plugin.h
        src/chronicle_plugin.cpp
        src/chronicle_helpers.h
        src/chronicle_helpers.cpp
        src/chronicle_anchor_config.h
        src/chronicle_anchor_config.cpp
        src/chronicle_anchor_client.h
        src/chronicle_anchor_client.cpp
)
