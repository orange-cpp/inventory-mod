# inventory-mod: an inventory system for Left 4 Dead 2 as an in-process DLL.
#
#   inventory_core    static library, the game-independent inventory model (no Windows, no ImGui)
#   inventory_tests   tests of the model
#   nc_ui             static library, imgui-nullcore's sources + Dear ImGui and its Win32 / D3D9 backends
#   inventory_mod     the DLL to load into left4dead2.exe (HUD, hooks)
#   inventory_mod_server  the native Linux dedicated-server plugin
#   injector          a small LoadLibrary injector for development
#
# L4D2 is a 32-bit process: configure with an x86 toolchain (build.bat does it: vcvarsall x86 + Ninja).

cmake_minimum_required(VERSION 3.25)
project(inventory_mod VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT: nothing to install next to the DLL
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/$<CONFIG>)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/$<CONFIG>)
endif()

if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "inventory-mod targets 32-bit Windows (left4dead2.exe is x86): use an x86 toolchain, see build.bat")
endif()

option(INVENTORY_LINUX_SERVER "Build the native Linux dedicated-server plugin" OFF)
if(INVENTORY_LINUX_SERVER)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "The Linux server plugin requires a Linux x86 toolchain (configure with -DCMAKE_CXX_FLAGS=-m32)")
    endif()

    set(INVENTORY_L4D2_SDK_DIR "" CACHE PATH "Checkout of the AlliedModders hl2sdk l4d2 branch")
    if(NOT EXISTS "${INVENTORY_L4D2_SDK_DIR}/public/engine/iserverplugin.h")
        message(FATAL_ERROR "Set INVENTORY_L4D2_SDK_DIR to the L4D2 branch of AlliedModders' hl2sdk")
    endif()

    # Match Windows x86 scalar precision for rule values and time comparisons.
    add_compile_options(-msse2 -mfpmath=sse)

    # Use the header-only target so a system installation of the 64-bit shared library cannot enter the x86 link.
    include(FetchContent)
    FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(tomlplusplus)
else()
    find_package(tomlplusplus CONFIG REQUIRED)
endif()

function(inventory_options target)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc /W4)
    endif()
endfunction()

# ---- the model ---------------------------------------------------------------------------------------------------
add_library(inventory_core STATIC
    src/game/infection_sounds.cpp
    src/game/infection_sounds.hpp
    src/game/remote_state.cpp
    src/game/remote_state.hpp
    src/inv/items.cpp
    src/inv/items.hpp
    src/inv/inventory.cpp
    src/inv/inventory.hpp
    src/inv/melee.cpp
    src/inv/melee.hpp
    src/inv/mutators.cpp
    src/inv/mutators.hpp
    src/inv/rules.cpp
    src/inv/rules.hpp)
target_include_directories(inventory_core PUBLIC src)
target_link_libraries(inventory_core PRIVATE tomlplusplus::tomlplusplus)
inventory_options(inventory_core)

enable_testing()
add_executable(inventory_tests tests/inventory_test.cpp)
target_link_libraries(inventory_tests PRIVATE inventory_core)
# the documented template must parse with no errors
target_compile_definitions(inventory_tests PRIVATE INV_EXAMPLE_RULES="${CMAKE_SOURCE_DIR}/inventory_mod.example.toml")
inventory_options(inventory_tests)
add_test(NAME inventory_tests COMMAND inventory_tests)

if(INVENTORY_LINUX_SERVER)
    add_library(inventory_mod_server MODULE
        src/server/linux_game.cpp
        src/server/linux_game.hpp
        src/server/linux_actions.cpp
        src/server/linux_infection.cpp
        src/server/linux_melee.cpp
        src/server/linux_memory.hpp
        src/server/linux_mutators.cpp
        src/server/linux_mutators.hpp
        src/server/linux_plugin.cpp
        src/server/linux_sdk.hpp
        src/server/linux_stamina.cpp)
    target_link_libraries(inventory_mod_server PRIVATE inventory_core tomlplusplus::tomlplusplus ${CMAKE_DL_LIBS})
    target_include_directories(inventory_mod_server SYSTEM PRIVATE
        "${INVENTORY_L4D2_SDK_DIR}/public"
        "${INVENTORY_L4D2_SDK_DIR}/public/tier0"
        "${INVENTORY_L4D2_SDK_DIR}/public/tier1")
    target_compile_definitions(inventory_mod_server PRIVATE _LINUX LINUX POSIX GNUC COMPILER_GCC)
    target_compile_options(inventory_mod_server PRIVATE -Wall -Wextra)
    # SRCDS preloads an older libstdc++. Keep the plugin's C++ runtime private.
    target_link_options(inventory_mod_server PRIVATE
        -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--no-undefined)
    set_target_properties(inventory_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(inventory_mod_server PROPERTIES CXX_VISIBILITY_PRESET hidden PREFIX "")
endif()

if(NOT WIN32)
    return()
endif()

set(INVENTORY_NULLCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui-nullcore" CACHE PATH "Checkout of imgui-nullcore")
if(NOT EXISTS "${INVENTORY_NULLCORE_DIR}/include/imgui_nullcore/imgui_nullcore.hpp")
    message(FATAL_ERROR "imgui-nullcore not found at ${INVENTORY_NULLCORE_DIR} (set INVENTORY_NULLCORE_DIR)")
endif()

# build.bat passes vcpkg's x86-windows-static triplet. Dear ImGui stays at the version used by imgui-nullcore.
find_package(minhook CONFIG REQUIRED)
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.8 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

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
    src/game/addons.cpp
    src/game/addons.hpp
    src/game/ammo_piles.cpp
    src/game/ammo_piles.hpp
    src/game/backend.cpp
    src/game/backend.hpp
    src/game/client_remote.cpp
    src/game/client_remote.hpp
    src/game/client_view.cpp
    src/game/client_view.hpp
    src/game/entities.cpp
    src/game/entities.hpp
    src/game/fog.cpp
    src/game/fog.hpp
    src/game/fuel.cpp
    src/game/fuel.hpp
    src/game/game.cpp
    src/game/game.hpp
    src/game/infection.cpp
    src/game/infection.hpp
    src/game/lifecycle.cpp
    src/game/lifecycle.hpp
    src/game/map_guns.cpp
    src/game/map_guns.hpp
    src/game/melee.cpp
    src/game/melee_client.cpp
    src/game/ping.cpp
    src/game/ping.hpp
    src/game/melee.hpp
    src/game/mutators.cpp
    src/game/mutators.hpp
    src/game/mem.cpp
    src/game/mem.hpp
    src/game/native_slots.cpp
    src/game/native_slots.hpp
    src/game/pickup.cpp
    src/game/pickup.hpp
    src/game/players.cpp
    src/game/players.hpp
    src/game/revive.cpp
    src/game/revive.hpp
    src/game/rounds.cpp
    src/game/rounds.hpp
    src/game/stamina.cpp
    src/game/stamina.hpp
    src/game/unload.cpp
    src/game/unload.hpp
    src/game/weapon_panel.cpp
    src/game/weapon_panel.hpp
    src/game/weapons.cpp
    src/game/weapons.hpp
    src/game/sdk.hpp
    src/ui/overlay.cpp
    src/ui/overlay.hpp
    src/ui/hud.cpp
    src/ui/hud.hpp
    src/ui/icons.cpp
    src/ui/icons.hpp
    src/ui/postfx.cpp
    src/ui/postfx.hpp
    src/ui/audio.cpp
    src/ui/audio.hpp
    src/ui/vgui.cpp
    src/ui/vgui.hpp)
target_include_directories(inventory_mod PRIVATE src)
target_link_libraries(inventory_mod PRIVATE inventory_core nc_ui minhook::minhook tomlplusplus::tomlplusplus windowscodecs ole32)
inventory_options(inventory_mod)
if(MSVC)
    target_link_options(inventory_mod PRIVATE /MAP /DEBUG /OPT:REF /OPT:ICF)
    target_compile_options(inventory_mod PRIVATE /Zi)
endif()

# ---- development tools -------------------------------------------------------------------------------------------
add_executable(injector tools/injector.cpp)
inventory_options(injector)
