cmake_minimum_required(VERSION 3.21)
project(Magnifier VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "Magnifier builds with MSVC (Visual Studio 2022 Build Tools).")
endif()

# UIAccess lets the overlay sit above the Start menu and taskbar flyouts.
# It only works when the exe is signed with a trusted certificate and
# installed under Program Files, so it is off by default.
option(MAGNIFIER_UIACCESS "Request uiAccess in the manifest" OFF)
if(MAGNIFIER_UIACCESS)
    set(UIACCESS_VALUE "true")
else()
    set(UIACCESS_VALUE "false")
endif()
configure_file(src/app.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/app.manifest @ONLY)

# Test and tuning modes (--render, --text-test, --capture-fixture,
# --scan-probe, --snapshot). They read and write files named on the command
# line, so they are left out of the UIAccess build that gets installed.
if(MAGNIFIER_UIACCESS)
    set(DEV_TOOLS_DEFAULT OFF)
else()
    set(DEV_TOOLS_DEFAULT ON)
endif()
option(MAGNIFIER_DEV_TOOLS "Include the test and tuning modes" ${DEV_TOOLS_DEFAULT})

# The C and C++ runtimes are linked in, so the exe needs nothing but Windows.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/W4 /permissive- /utf-8 /EHsc /sdl /guard:cf /guard:ehcont)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# Control Flow Guard, CET shadow stacks (with exception-handling continuation
# metadata), and the DLLs it loads only from System32.
add_link_options(/guard:cf /guard:ehcont /CETCOMPAT /DEPENDENTLOADFLAG:0x800)

# Shaders are compiled with fxc at build time and linked in as bytecode, for
# Direct3D feature level 11 (shader model 5) and 10 (shader model 4).
find_program(FXC fxc REQUIRED)
set(SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders.hlsl)
set(SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(SHADER_ENTRIES
    FullscreenVS:vs CursorVS:vs
    NearestPS:ps BilinearPS:ps BicubicPS:ps LanczosPS:ps SmoothPS:ps
    StatsHPS:ps StatsVPS:ps CursorPS:ps CursorInvertPS:ps)
set(SHADER_HEADERS)
foreach(item ${SHADER_ENTRIES})
    string(REPLACE ":" ";" parts ${item})
    list(GET parts 0 entry)
    list(GET parts 1 stage)
    foreach(model 5_0 4_0)
        set(header ${SHADER_DIR}/${entry}_${model}.h)
        add_custom_command(OUTPUT ${header}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
            COMMAND ${FXC} /nologo /O3 /WX /T ${stage}_${model} /E ${entry} /Vn k${entry}_${model} /Fh ${header} ${SHADER_SOURCE}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "fxc ${entry} ${stage}_${model}"
            VERBATIM)
        list(APPEND SHADER_HEADERS ${header})
    endforeach()
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_HEADERS})

# Pure maths, shared by the app and the tests.
add_library(magcore STATIC src/viewport.cpp)
target_include_directories(magcore PUBLIC src)

add_executable(Magnifier WIN32
    src/main.cpp
    src/capture.cpp
    src/renderer.cpp
    src/cursor.cpp
    src/tracking.cpp
    src/overlay.cpp
    src/settings.cpp
    src/settings_window.cpp
    src/shell_icons.cpp
    src/image_io.cpp
    src/cursor_hide.cpp
    src/text_source.cpp
    src/ocr.cpp
    src/readback.cpp
    src/fonts.cpp
    src/resources.rc
    ${CMAKE_CURRENT_BINARY_DIR}/app.manifest
)
if(MAGNIFIER_DEV_TOOLS)
    target_sources(Magnifier PRIVATE src/text_fixture.cpp)
    target_compile_definitions(Magnifier PRIVATE MAGNIFIER_DEV_TOOLS=1)
endif()
add_dependencies(Magnifier shaders)
target_include_directories(Magnifier PRIVATE ${SHADER_DIR})
target_compile_definitions(Magnifier PRIVATE
    MAGNIFIER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MAGNIFIER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MAGNIFIER_VERSION_PATCH=${PROJECT_VERSION_PATCH})
set_source_files_properties(src/resources.rc PROPERTIES COMPILE_DEFINITIONS
    "MAGNIFIER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};MAGNIFIER_VERSION_MINOR=${PROJECT_VERSION_MINOR};MAGNIFIER_VERSION_PATCH=${PROJECT_VERSION_PATCH}")
# app.manifest carries the execution level and uiAccess; without this the
# linker adds its own uiAccess='false' and mt.exe rejects the mismatch.
target_link_options(Magnifier PRIVATE /MANIFESTUAC:NO)
target_link_libraries(Magnifier PRIVATE magcore
    d3d11 d2d1 dwrite dxgi dcomp dwmapi windowsapp oleacc shcore windowscodecs magnification wtsapi32 shlwapi)

add_executable(viewport_tests tests/viewport_tests.cpp)
target_link_libraries(viewport_tests PRIVATE magcore)

enable_testing()
add_test(NAME viewport_tests COMMAND viewport_tests)
