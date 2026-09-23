cmake_minimum_required(VERSION 3.15)

# CMP0091: позволяет задавать рантайм MSVC через MSVC_RUNTIME_LIBRARY.
cmake_policy(SET CMP0091 NEW)

project(zet017tcp VERSION 1.0.1 LANGUAGES C)

# --------------------------------------------------------------------
# Под Windows поддерживается ТОЛЬКО MSVC (cl.exe).
#
# Библиотека предназначена для загрузки в LabView, и вся платформенная
# логика ниже завёрнута в if(MSVC): статический рантайм /MT, экспорт по
# .def, отключение предупреждений CRT. Под MinGW/Clang-GNU эти ветки
# молча пропускаются — сборка проходит, но DLL получается без нужных
# экспортов и с зависимостью от внешнего рантайма. Остановить сборку здесь
# предпочтительнее, чем выявлять последствия в LabVIEW.
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

# Статический рантайм MSVC (/MT, /MTd) — рантайм вкомпилировывается
# в DLL. Тогда VCRUNTIME140*/ucrtbase* НЕ нужны рядом с DLL, и она
# грузится в LabVIEW на любой машине (в т.ч. без Visual Studio
# и без Visual C++ Redistributable), независимо от Debug/Release.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(BUILD_SHARED  "Build shared library" ON)
option(BUILD_STATIC  "Build static library" ON)
option(BUILD_EXAMPLES "Build examples"      ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ZET017TCP_INCLUDE include/zet017tcp.h)
set(ZET017TCP_SOURCES src/zet017tcp.c)

# Основная библиотека НЕ зависит от libxml2 — разбор XML вынесен
# в отдельный модуль zet017config (см. ../zet017config).

if(WIN32)
    set(PLATFORM_LIBS ws2_32)
else()
    set(PLATFORM_LIBS pthread m)
endif()

if(BUILD_STATIC)
    add_library(zet017tcp_static STATIC ${ZET017TCP_SOURCES} ${ZET017TCP_INCLUDE})
    target_include_directories(zet017tcp_static PRIVATE src include)
    target_link_libraries(zet017tcp_static PRIVATE ${PLATFORM_LIBS})
    target_compile_definitions(zet017tcp_static PRIVATE ZET017TCP_STATIC)
    # Версия попадает в баннер сессии лог-файла
    target_compile_definitions(zet017tcp_static PRIVATE ZET017TCP_VERSION="${PROJECT_VERSION}")
    set_target_properties(zet017tcp_static PROPERTIES
        OUTPUT_NAME            "zet017tcp_s"
        VERSION                ${PROJECT_VERSION}
        SOVERSION              ${PROJECT_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
    )
    if(MSVC)
        target_compile_definitions(zet017tcp_static PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

if(BUILD_SHARED)
    add_library(zet017tcp_shared SHARED ${ZET017TCP_SOURCES} ${ZET017TCP_INCLUDE})
    target_include_directories(zet017tcp_shared PRIVATE src include)
    target_link_libraries(zet017tcp_shared PRIVATE ${PLATFORM_LIBS})
    target_compile_definitions(zet017tcp_shared PRIVATE ZET017TCP_SHARED)
    # Версия попадает в баннер сессии лог-файла
    target_compile_definitions(zet017tcp_shared PRIVATE ZET017TCP_VERSION="${PROJECT_VERSION}")
    if(MSVC)
        # CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS убран — .def файл управляет экспортом
        target_sources(zet017tcp_shared PRIVATE win32/zet017tcp.def)
        target_compile_definitions(zet017tcp_shared PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set(ZET017TCP_SHARED_LIB_PATH $<TARGET_FILE:zet017tcp_shared>      CACHE INTERNAL "")
    set(ZET017TCP_SHARED_LIB_NAME $<TARGET_FILE_NAME:zet017tcp_shared> CACHE INTERNAL "")
    set_target_properties(zet017tcp_shared PROPERTIES
        OUTPUT_NAME            "zet017tcp"
        VERSION                ${PROJECT_VERSION}
        SOVERSION              ${PROJECT_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
    )
endif()

if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()
