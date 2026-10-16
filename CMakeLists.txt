# inventory-mod: an inventory system for Left 4 Dead 2 as an in-process DLL.
#
#   inventory_core    static library, the game-independent inventory model (no Windows, no ImGui)
#   inventory_tests   tests of the model
#   nc_ui             static library, imgui-nullcore's sources + Dear ImGui and its Win32 / D3D9 backends
#   inventory_mod     the DLL to load into left4dead2.exe (HUD, hooks)
#   injector          a small LoadLibrary injector for development
#
# L4D2 is a 32-bit process: configure with an x86 toolchain (build.bat does it: vcvarsall x86 + Ninja).

cmake_minimum_required(VERSION 3.25)
project(inventory_mod VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT: nothing to install next to the DLL
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/$<CONFIG>)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "inventory-mod targets 32-bit Windows (left4dead2.exe is x86): use an x86 toolchain, see build.bat")
endif()

set(INVENTORY_NULLCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui-nullcore" CACHE PATH "Checkout of imgui-nullcore")
if(NOT EXISTS "${INVENTORY_NULLCORE_DIR}/include/imgui_nullcore/imgui_nullcore.hpp")
    message(FATAL_ERROR "imgui-nullcore not found at ${INVENTORY_NULLCORE_DIR} (set INVENTORY_NULLCORE_DIR)")
endif()

include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.8 GIT_SHALLOW TRUE)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG v1.3.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui minhook)

function(inventory_options target)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc /W4)
    endif()
endfunction()

# ---- the model ---------------------------------------------------------------------------------------------------
add_library(inventory_core STATIC
    src/inv/items.cpp
    src/inv/items.hpp
    src/inv/inventory.cpp
    src/inv/inventory.hpp)
target_include_directories(inventory_core PUBLIC src)
inventory_options(inventory_core)

enable_testing()
add_executable(inventory_tests tests/inventory_test.cpp)
target_link_libraries(inventory_tests PRIVATE inventory_core)
inventory_options(inventory_tests)
add_test(NAME inventory_tests COMMAND inventory_tests)

# ---- ImGui + the nullcore look -----------------------------------------------------------------------------------
add_library(nc_ui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${INVENTORY_NULLCORE_DIR}/src/imgui_nullcore.cpp
    ${INVENTORY_NULLCORE_DIR}/src/color_picker.cpp
    ${INVENTORY_NULLCORE_DIR}/src/lists.cpp
    ${INVENTORY_NULLCORE_DIR}/src/menus.cpp
    ${INVENTORY_NULLCORE_DIR}/src/tables.cpp
    ${INVENTORY_NULLCORE_DIR}/src/gdi_fonts.cpp
    ${INVENTORY_NULLCORE_DIR}/src/baked_fonts.cpp)
target_include_directories(nc_ui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp
                                        ${INVENTORY_NULLCORE_DIR}/include
                           PRIVATE ${INVENTORY_NULLCORE_DIR}/src)
target_compile_definitions(nc_ui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
                                        _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(nc_ui PUBLIC d3d9 gdi32 usp10 dwmapi)
if(MSVC)
    target_compile_options(nc_ui PRIVATE /utf-8 /EHsc /W3)
endif()

# ---- the DLL -----------------------------------------------------------------------------------------------------
add_library(inventory_mod SHARED
    src/dllmain.cpp
    src/core/log.cpp
    src/core/log.hpp
    src/core/hook.hpp
    src/core/config.cpp
    src/core/config.hpp
    src/game/backend.cpp
    src/game/backend.hpp
    src/game/game.cpp
    src/game/game.hpp
    src/game/mem.cpp
    src/game/mem.hpp
    src/ui/overlay.cpp
    src/ui/overlay.hpp
    src/ui/hud.cpp
    src/ui/hud.hpp
    src/ui/icons.cpp
    src/ui/icons.hpp)
target_include_directories(inventory_mod PRIVATE src)
target_link_libraries(inventory_mod PRIVATE inventory_core nc_ui minhook)
inventory_options(inventory_mod)
if(MSVC)
    target_link_options(inventory_mod PRIVATE /MAP /DEBUG /OPT:REF /OPT:ICF)
    target_compile_options(inventory_mod PRIVATE /Zi)
endif()

# ---- development tools -------------------------------------------------------------------------------------------
add_executable(injector tools/injector.cpp)
inventory_options(injector)
