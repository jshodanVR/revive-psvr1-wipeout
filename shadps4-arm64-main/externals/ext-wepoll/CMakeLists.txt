cmake_minimum_required(VERSION 3.16)
project(wepoll LANGUAGES C)

add_library(wepoll STATIC
    wepoll.c
    wepoll.h
)

target_include_directories(wepoll PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})