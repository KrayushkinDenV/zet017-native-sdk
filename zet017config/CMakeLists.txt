cmake_minimum_required(VERSION 3.15)

# CMP0091: позволяет задавать рантайм MSVC через MSVC_RUNTIME_LIBRARY.
cmake_policy(SET CMP0091 NEW)

project(zet017config VERSION 1.0.0 LANGUAGES C)

# --------------------------------------------------------------------
# Под Windows поддерживается ТОЛЬКО MSVC (cl.exe).
#
# Библиотека предназначена для загрузки в LabView, и вся платформенная
# логика ниже завёрнута в if(MSVC): статический рантайм /MT, экспорт по
# .def, отключение предупреждений CRT. Под MinGW/Clang-GNU эти ветки
# молча пропускаются — сборка проходит, но DLL получается без нужных
# экспортов и с зависимостью от чужого рантайма. Падать здесь дешевле,
# чем разбираться с этим в LabView.
# --------------------------------------------------------------------
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "Windows-сборка поддерживается только компилятором MSVC (cl.exe). "
        "Обнаружен: ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER}). "
        "Запустите CMake из Native Tools Command Prompt нужной разрядности "
        "или задайте -DCMAKE_C_COMPILER=cl.")
endif()

# Разрядность DLL обязана совпадать с разрядностью LabView. Генератор
# Ninja берёт компилятор из окружения, поэтому пресет vs-x86, запущенный
# не из x86-окружения, молча собрал бы x64 в каталоге с именем x86.
if(ZET017_EXPECT_ARCH)
    if(ZET017_EXPECT_ARCH STREQUAL "x86" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR
            "Пресет требует x86, а компилятор собирает ${CMAKE_SIZEOF_VOID_P}-байтные указатели. "
            "Запустите CMake из \"x86 Native Tools Command Prompt for VS\".")
    elseif(ZET017_EXPECT_ARCH STREQUAL "x64" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "Пресет требует x64, а компилятор собирает ${CMAKE_SIZEOF_VOID_P}-байтные указатели. "
            "Запустите CMake из \"x64 Native Tools Command Prompt for VS\".")
    endif()
endif()

# Статический рантайм MSVC (/MT) — чтобы DLL не требовала
# VCRUNTIME140*/ucrtbase* рядом (аналогично zet017tcp).
#
# ВАЖНО: libxml2 из vcpkg должна быть собрана ТЕМ ЖЕ рантаймом,
# иначе будет конфликт CRT. Для статического рантайма берите
# vcpkg-триплет x86-windows-static (см. CMakePresets.json).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(BUILD_SHARED "Build shared library" ON)
option(BUILD_STATIC "Build static library" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ZET017CONFIG_INCLUDE include/zet017config.h)
set(ZET017CONFIG_SOURCES src/zet017config.c)

# libxml2 — нужна ТОЛЬКО этому модулю. Через vcpkg (manifest-режим)
# зависимость ставится автоматически по vcpkg.json в корне модуля.
find_package(LibXml2 REQUIRED)

if(BUILD_STATIC)
    add_library(zet017config_static STATIC ${ZET017CONFIG_SOURCES} ${ZET017CONFIG_INCLUDE})
    target_include_directories(zet017config_static PRIVATE src include)
    target_link_libraries(zet017config_static PRIVATE LibXml2::LibXml2)
    set_target_properties(zet017config_static PROPERTIES
        OUTPUT_NAME            "zet017config_s"
        VERSION                ${PROJECT_VERSION}
        SOVERSION              ${PROJECT_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
    )
    if(MSVC)
        target_compile_definitions(zet017config_static PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

if(BUILD_SHARED)
    add_library(zet017config_shared SHARED ${ZET017CONFIG_SOURCES} ${ZET017CONFIG_INCLUDE})
    target_include_directories(zet017config_shared PRIVATE src include)
    target_link_libraries(zet017config_shared PRIVATE LibXml2::LibXml2)
    if(MSVC)
        # .def управляет экспортом (чистое недекорированное имя для LabView)
        target_sources(zet017config_shared PRIVATE win32/zet017config.def)
        target_compile_definitions(zet017config_shared PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_target_properties(zet017config_shared PROPERTIES
        OUTPUT_NAME            "zet017config"
        VERSION                ${PROJECT_VERSION}
        SOVERSION              ${PROJECT_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
    )
endif()

if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()
