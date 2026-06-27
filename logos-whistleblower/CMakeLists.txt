cmake_minimum_required(VERSION 3.14)
project(WhistleblowerPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME whistleblower
    REP_FILE src/whistleblower.rep
    SOURCES
        src/whistleblower_interface.h
        src/whistleblower_plugin.h
        src/whistleblower_plugin.cpp
)
