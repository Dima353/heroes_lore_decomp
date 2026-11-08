cmake_minimum_required(VERSION 3.18)
project(heroes_lore_port VERSION 0.9.2 LANGUAGES C CXX) # C: the fetched zlib / minizip sources

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Russian literals are UTF-8; the sources assume standard conformance and std::min/max.
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

set(HEROES_LORE_PORTLIBS "" CACHE PATH "devkitPro portlibs prefix")
option(HEROES_LORE_FETCH_DEPS "Download and statically build zlib, minizip and SDL2 (desktop without system packages)" OFF)
option(HEROES_LORE_FETCH_SDL "With HEROES_LORE_FETCH_DEPS, also build SDL2 (off: use the system / sysroot SDL2)" ON)
if(HEROES_LORE_PORTLIBS)
    find_path(ZLIB_INCLUDE_DIR zlib.h PATHS "${HEROES_LORE_PORTLIBS}/include" REQUIRED NO_DEFAULT_PATH)
    find_library(ZLIB_LIBRARY z PATHS "${HEROES_LORE_PORTLIBS}/lib" REQUIRED NO_DEFAULT_PATH)
    add_library(ZLIB::ZLIB UNKNOWN IMPORTED)
    set_target_properties(ZLIB::ZLIB PROPERTIES
        IMPORTED_LOCATION "${ZLIB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR}")
    find_path(MINIZIP_INCLUDE_DIR minizip/unzip.h PATHS "${HEROES_LORE_PORTLIBS}/include" REQUIRED NO_DEFAULT_PATH)
    find_library(MINIZIP_LIBRARY minizip PATHS "${HEROES_LORE_PORTLIBS}/lib" REQUIRED NO_DEFAULT_PATH)
elseif(HEROES_LORE_FETCH_DEPS)
    # Self-contained desktop build (the Windows preset): zlib with its contrib/minizip, and SDL2,
    # downloaded from their release archives and linked statically.
    include(FetchContent)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE) # the package holds only the game
    FetchContent_MakeAvailable(zlib)
    target_include_directories(zlibstatic PUBLIC "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
    add_library(heroes_lore_minizip STATIC
        "${zlib_SOURCE_DIR}/contrib/minizip/unzip.c" "${zlib_SOURCE_DIR}/contrib/minizip/ioapi.c")
    target_link_libraries(heroes_lore_minizip PUBLIC zlibstatic)
    if(PSP OR NINTENDO_3DS OR VITA)
        target_compile_definitions(heroes_lore_minizip PRIVATE USE_FILE32API)
    endif()
    set(MINIZIP_INCLUDE_DIR "${zlib_SOURCE_DIR}/contrib")
    set(MINIZIP_LIBRARY heroes_lore_minizip)
    if(HEROES_LORE_FETCH_SDL)
        FetchContent_Declare(SDL2
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL2)
    endif()
else()
    find_package(ZLIB REQUIRED)
    find_path(MINIZIP_INCLUDE_DIR minizip/unzip.h REQUIRED)
    find_library(MINIZIP_LIBRARY minizip REQUIRED)
endif()

add_library(heroes_lore_map src/map.cpp src/sprite.cpp src/npc_sprite.cpp src/eif.cpp
    src/hero_movement.cpp src/character_sprite.cpp src/event_runtime.cpp
    src/npc_movement.cpp src/event_trigger.cpp src/language.cpp src/language_pack.cpp src/bitmap_font.cpp
    src/save_game.cpp src/item_catalog.cpp src/shop_transaction.cpp src/service.cpp src/audio.cpp src/new_game.cpp src/event_visual.cpp src/equipment.cpp src/viewport.cpp src/combat.cpp src/combat_boss.cpp src/combat_guardian.cpp)
target_include_directories(heroes_lore_map PUBLIC src ${MINIZIP_INCLUDE_DIR})
# PUBLIC zlib: map.hpp includes minizip/unzip.h, which includes zlib.h.
target_link_libraries(heroes_lore_map PRIVATE ${MINIZIP_LIBRARY} PUBLIC ZLIB::ZLIB)

if(NOT VITA AND NOT PSP AND NOT NINTENDO_3DS) # desktop / WSL tools
add_executable(heroes_lore_inspect src/inspect.cpp)
target_link_libraries(heroes_lore_inspect PRIVATE heroes_lore_map)

add_executable(heroes_lore_frame src/frame_export.cpp)
target_link_libraries(heroes_lore_frame PRIVATE heroes_lore_map)
endif()

option(HEROES_LORE_BUILD_SDL "Build the SDL2 map preview" ON)
if(HEROES_LORE_BUILD_SDL)
    if(HEROES_LORE_PORTLIBS)
        find_package(SDL2 CONFIG REQUIRED PATHS "${HEROES_LORE_PORTLIBS}/lib/cmake/SDL2" NO_DEFAULT_PATH)
    else()
        if(NOT (HEROES_LORE_FETCH_DEPS AND HEROES_LORE_FETCH_SDL))
            find_package(SDL2 CONFIG REQUIRED)
        endif()
    endif()
    add_executable(heroes_lore src/main.cpp src/visual_ui.cpp src/embedded_soundfont.cpp)
    # Compressed minimal SoundFont (tools/build_psp_soundfont.py) built into the program: the music on
    # every platform unless an external .sf2 is found (desktop, Switch, Vita). Local file, not in git.
    set(HEROES_LORE_SOUNDFONT_ZLIB "${CMAKE_CURRENT_SOURCE_DIR}/assets/psp/heroes_lore.sf2.zlib" CACHE FILEPATH
        "Compressed minimal SoundFont embedded in the program")
    option(HEROES_LORE_FULL_BUILD "Download GeneralUser GS and generate the embedded SoundFont when it is missing" OFF)
    if(HEROES_LORE_FULL_BUILD)
        include(cmake/full_build.cmake)
        heroes_lore_full_build_soundfont("${HEROES_LORE_SOUNDFONT_ZLIB}")
    endif()
    if(EXISTS "${HEROES_LORE_SOUNDFONT_ZLIB}")
        if(MSVC)
            configure_file(cmake/embedded_soundfont.rc.in "${CMAKE_CURRENT_BINARY_DIR}/embedded_soundfont.rc" @ONLY)
            set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/embedded_soundfont.rc" PROPERTIES OBJECT_DEPENDS "${HEROES_LORE_SOUNDFONT_ZLIB}")
            target_sources(heroes_lore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/embedded_soundfont.rc")
        else()
            enable_language(ASM)
            configure_file(cmake/embedded_soundfont.S.in "${CMAKE_CURRENT_BINARY_DIR}/embedded_soundfont_data.S" @ONLY)
            set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/embedded_soundfont_data.S" PROPERTIES OBJECT_DEPENDS "${HEROES_LORE_SOUNDFONT_ZLIB}")
            target_sources(heroes_lore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/embedded_soundfont_data.S")
        endif()
        target_compile_definitions(heroes_lore PRIVATE HEROES_LORE_EMBEDDED_SOUNDFONT=1)
        message(STATUS "Embedded SoundFont: ${HEROES_LORE_SOUNDFONT_ZLIB}")
    elseif(PSP)
        message(FATAL_ERROR "Generate the SoundFont with tools/build_psp_soundfont.py (see docs/build/psp.md)")
    else()
        message(STATUS "Embedded SoundFont: none (built-in synth unless an external .sf2 is found)")
    endif()
    # A statically built SDL2 (HEROES_LORE_FETCH_DEPS) only defines SDL2::SDL2-static.
    if(TARGET SDL2::SDL2)
        target_link_libraries(heroes_lore PRIVATE heroes_lore_map SDL2::SDL2)
    else()
        target_link_libraries(heroes_lore PRIVATE heroes_lore_map SDL2::SDL2-static)
    endif()

    if(NOT HEROES_LORE_PORTLIBS AND NOT VITA AND NOT PSP AND NOT NINTENDO_3DS)
        # Desktop (Windows / Linux): a windowed app; the folder holds the program, its DLLs and
        # README.txt, and the player adds the game's .jar beside it.
        if(WIN32)
            set_target_properties(heroes_lore PROPERTIES WIN32_EXECUTABLE ON)
            if(TARGET SDL2::SDL2main)
                target_link_libraries(heroes_lore PRIVATE SDL2::SDL2main)
            endif()
        endif()
        install(TARGETS heroes_lore RUNTIME DESTINATION .)
        if(WIN32 AND NOT (HEROES_LORE_FETCH_DEPS AND HEROES_LORE_FETCH_SDL) AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
            install(FILES $<TARGET_RUNTIME_DLLS:heroes_lore> DESTINATION .)
        endif()
        install(FILES dist/README.txt DESTINATION .)
        set(CPACK_PACKAGE_NAME "HeroesLore")
        set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
        if(WIN32)
            set(CPACK_GENERATOR ZIP)
        else()
            set(CPACK_GENERATOR TGZ)
        endif()
        set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
        include(CPack)
    endif()

    if(HEROES_LORE_PORTLIBS)
        target_compile_definitions(heroes_lore PRIVATE HEROES_LORE_SWITCH_BUILD=1)
        find_program(HEROES_LORE_ELF2NRO elf2nro
            HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
        find_program(HEROES_LORE_NACPTOOL nacptool
            HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
        set(HEROES_LORE_APP_TITLE "Heroes Lore Wind of Soltia" CACHE STRING "Switch homebrew title")
        set(HEROES_LORE_APP_AUTHOR "Heroes Lore Recomp" CACHE STRING "Switch homebrew author")
        set(HEROES_LORE_APP_VERSION "${PROJECT_VERSION}")
        set(HEROES_LORE_NACP "${CMAKE_CURRENT_BINARY_DIR}/heroes_lore.nacp")
        set(HEROES_LORE_NRO "${CMAKE_CURRENT_BINARY_DIR}/heroes_lore.nro")
        add_custom_command(OUTPUT "${HEROES_LORE_NACP}"
            COMMAND "${HEROES_LORE_NACPTOOL}" --create
                "${HEROES_LORE_APP_TITLE}" "${HEROES_LORE_APP_AUTHOR}"
                "${HEROES_LORE_APP_VERSION}" "${HEROES_LORE_NACP}"
            VERBATIM)
        set(HEROES_LORE_NRO_ARGS "$<TARGET_FILE:heroes_lore>" "${HEROES_LORE_NRO}"
            "--nacp=${HEROES_LORE_NACP}")
        # Local 256x256 JPEG icon (cpp/assets/icon.jpg, not tracked in git); libnx default otherwise.
        set(HEROES_LORE_DEFAULT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
        if(NOT EXISTS "${HEROES_LORE_DEFAULT_ICON}")
            set(HEROES_LORE_DEFAULT_ICON "$ENV{DEVKITPRO}/libnx/default_icon.jpg")
        endif()
        if(EXISTS "${HEROES_LORE_DEFAULT_ICON}")
            list(APPEND HEROES_LORE_NRO_ARGS "--icon=${HEROES_LORE_DEFAULT_ICON}")
        endif()
        add_custom_command(OUTPUT "${HEROES_LORE_NRO}"
            COMMAND "${HEROES_LORE_ELF2NRO}" ${HEROES_LORE_NRO_ARGS}
            DEPENDS heroes_lore "${HEROES_LORE_NACP}"
            VERBATIM)
        add_custom_target(heroes_lore_nro ALL DEPENDS "${HEROES_LORE_NRO}")
    endif()

    if(HEROES_LORE_SYSROOT) # PortMaster cross build (cmake/aarch64-portmaster.cmake)
        target_sources(heroes_lore PRIVATE src/glibc_compat.c)
    endif()

    if(PSP)
        target_compile_definitions(heroes_lore_map PUBLIC HEROES_LORE_PSP_BUILD=1)
        target_sources(heroes_lore PRIVATE src/platform_psp.cpp src/texture_tiles.cpp)
        set_source_files_properties(src/audio.cpp PROPERTIES COMPILE_OPTIONS "-fsingle-precision-constant")
        target_link_libraries(heroes_lore PRIVATE SDL2::SDL2main pspsdk)
        set(HEROES_LORE_PSP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psp/icon0.png")
        set(HEROES_LORE_PSP_ICON_ARGS)
        if(EXISTS "${HEROES_LORE_PSP_ICON}") # 144x80 XMB icon, local like the Vita LiveArea art
            set(HEROES_LORE_PSP_ICON_ARGS ICON_PATH "${HEROES_LORE_PSP_ICON}")
            set_property(TARGET heroes_lore APPEND PROPERTY LINK_DEPENDS "${HEROES_LORE_PSP_ICON}") # repack on icon change
        endif()
        create_pbp_file(TARGET heroes_lore TITLE "Heroes Lore Wind of Soltia" VERSION "00.91" BUILD_PRX MEMSIZE 1 ${HEROES_LORE_PSP_ICON_ARGS})
    endif()

    if(NINTENDO_3DS)
        # Top screen 400x240 through SDL2's N3DS backend (software renderer); built-in synth only.
        target_compile_definitions(heroes_lore_map PUBLIC HEROES_LORE_3DS_BUILD=1)
        option(HEROES_LORE_3DS_GPU "3DS: draw through citro2d on the GPU (stereoscopic 3D); OFF: SDL software renderer" ON)
        if(HEROES_LORE_3DS_GPU)
            target_sources(heroes_lore PRIVATE src/gpu_3ds.cpp)
            target_compile_definitions(heroes_lore PRIVATE HEROES_LORE_3DS_GPU=1)
            target_link_libraries(heroes_lore PRIVATE citro2d citro3d)
        endif()
        if(TARGET SDL2::SDL2main)
            target_link_libraries(heroes_lore PRIVATE SDL2::SDL2main)
        endif()
        # 3DS art: local files in cpp/assets/3ds override the published set in cpp/dist/3ds.
        set(HEROES_LORE_3DS_ART "${CMAKE_CURRENT_SOURCE_DIR}/assets/3ds")
        if(NOT EXISTS "${HEROES_LORE_3DS_ART}/banner.png")
            set(HEROES_LORE_3DS_ART "${CMAKE_CURRENT_SOURCE_DIR}/dist/3ds")
        endif()
        set(HEROES_LORE_3DS_ICON "${HEROES_LORE_3DS_ART}/icon.png")
        if(NOT EXISTS "${HEROES_LORE_3DS_ICON}") # 48x48 PNG
            set(HEROES_LORE_3DS_ICON "${CTR_DEFAULT_ICON}")
        endif()
        ctr_generate_smdh(heroes_lore.smdh NAME "Heroes Lore Wind of Soltia"
            DESCRIPTION "Heroes Lore port v${PROJECT_VERSION}" AUTHOR "Heroes Lore Recomp" ICON "${HEROES_LORE_3DS_ICON}")
        ctr_create_3dsx(heroes_lore SMDH heroes_lore.smdh OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/heroes_lore.3dsx")
        # Installable .cia (HOME Menu title) when makerom and bannertool are on the path and the
        # 256x128 banner art exists (see docs/build/3ds-cia.md).
        find_program(HEROES_LORE_MAKEROM makerom HINTS "$ENV{DEVKITPRO}/tools/bin" NO_CMAKE_FIND_ROOT_PATH)
        find_program(HEROES_LORE_BANNERTOOL bannertool HINTS "$ENV{DEVKITPRO}/tools/bin" NO_CMAKE_FIND_ROOT_PATH)
        set(HEROES_LORE_3DS_BANNER "${HEROES_LORE_3DS_ART}/banner.png")
        set(HEROES_LORE_3DS_BANNER_AUDIO "${HEROES_LORE_3DS_ART}/banner.wav")
        if(NOT EXISTS "${HEROES_LORE_3DS_BANNER_AUDIO}")
            set(HEROES_LORE_3DS_BANNER_AUDIO "${CMAKE_CURRENT_SOURCE_DIR}/cmake/3ds/banner-silence.wav")
        endif()
        if(HEROES_LORE_MAKEROM AND HEROES_LORE_BANNERTOOL AND EXISTS "${HEROES_LORE_3DS_BANNER}")
            # Opt-in: -DHEROES_LORE_3DS_JAR=<path> packs the game JAR into the .cia's romfs, so nothing
            # has to be copied to the SD card (a JAR in sdmc:/3ds/heroeslore still wins). The .3dsx never
            # carries the JAR.
            set(HEROES_LORE_3DS_JAR "" CACHE FILEPATH "Game JAR packed into the 3DS .cia (empty: none)")
            set(HEROES_LORE_3DS_ROMFS "${CMAKE_CURRENT_BINARY_DIR}/romfs")
            file(MAKE_DIRECTORY "${HEROES_LORE_3DS_ROMFS}")
            # Rewritten only when the packed JAR changes, so switching the option rebuilds the .cia.
            set(HEROES_LORE_3DS_ROMFS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/romfs.stamp")
            set(HEROES_LORE_3DS_JAR_HASH "none")
            if(HEROES_LORE_3DS_JAR)
                file(REMOVE "${HEROES_LORE_3DS_ROMFS}/README.txt")
                if(NOT EXISTS "${HEROES_LORE_3DS_JAR}")
                    message(FATAL_ERROR "HEROES_LORE_3DS_JAR not found: ${HEROES_LORE_3DS_JAR}")
                endif()
                file(SHA256 "${HEROES_LORE_3DS_JAR}" HEROES_LORE_3DS_JAR_HASH)
                configure_file("${HEROES_LORE_3DS_JAR}" "${HEROES_LORE_3DS_ROMFS}/Heroes_Lore.jar" COPYONLY)
                message(STATUS "3DS .cia packs ${HEROES_LORE_3DS_JAR}")
            else()
                file(REMOVE "${HEROES_LORE_3DS_ROMFS}/Heroes_Lore.jar")
                file(WRITE "${HEROES_LORE_3DS_ROMFS}/README.txt" "Put the game JAR in sdmc:/3ds/heroeslore/.\n")
            endif()
            file(CONFIGURE OUTPUT "${HEROES_LORE_3DS_ROMFS_STAMP}" CONTENT "${HEROES_LORE_3DS_JAR_HASH}")
            set(HEROES_LORE_BNR "${CMAKE_CURRENT_BINARY_DIR}/heroes_lore.bnr")
            set(HEROES_LORE_CIA "${CMAKE_CURRENT_BINARY_DIR}/heroes_lore.cia")
            add_custom_command(OUTPUT "${HEROES_LORE_BNR}"
                COMMAND "${HEROES_LORE_BANNERTOOL}" makebanner -i "${HEROES_LORE_3DS_BANNER}"
                    -a "${HEROES_LORE_3DS_BANNER_AUDIO}" -o "${HEROES_LORE_BNR}"
                DEPENDS "${HEROES_LORE_3DS_BANNER}" "${HEROES_LORE_3DS_BANNER_AUDIO}" VERBATIM)
            add_custom_command(OUTPUT "${HEROES_LORE_CIA}"
                COMMAND "${HEROES_LORE_MAKEROM}" -f cia -o "${HEROES_LORE_CIA}" -target t -exefslogo
                    -elf "$<TARGET_FILE:heroes_lore>" -rsf "${CMAKE_CURRENT_SOURCE_DIR}/cmake/3ds/app.rsf"
                    "-DAPP_ROMFS=${HEROES_LORE_3DS_ROMFS}"
                    -icon "${CMAKE_CURRENT_BINARY_DIR}/heroes_lore.smdh" -banner "${HEROES_LORE_BNR}"
                    -major ${PROJECT_VERSION_MAJOR} -minor ${PROJECT_VERSION_MINOR} -micro ${PROJECT_VERSION_PATCH}
                DEPENDS heroes_lore heroes_lore_3dsx "${HEROES_LORE_BNR}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/3ds/app.rsf" "${HEROES_LORE_3DS_ROMFS_STAMP}"
                VERBATIM)
            add_custom_target(heroes_lore_cia ALL DEPENDS "${HEROES_LORE_CIA}")
        else()
            message(STATUS "3DS .cia skipped: needs makerom and bannertool (docs/build/3ds-cia.md)")
        endif()
    endif()

    if(VITA)
        # PS Vita (VitaSDK toolchain): the JAR, saves and an optional .sf2 live in ux0:data/heroeslore.
        # LiveArea art comes from cpp/assets/vita/sce_sys when present (local, like the Switch icon).
        include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
        set(VITA_APP_NAME "Heroes Lore Wind of Soltia" CACHE STRING "LiveArea title" FORCE)
        set(VITA_TITLEID "HLWS00001" CACHE STRING "Nine-character title ID")
        set(VITA_VERSION "00.91") # XX.YY, follows PROJECT_VERSION
        target_compile_definitions(heroes_lore PRIVATE HEROES_LORE_VITA_BUILD=1)
        if(NOT HEROES_LORE_FETCH_DEPS)
            # vitasdk's minizip-ng static library also needs these (minizip.pc Libs.private, plus ppmd).
            # Newer vitasdk images build it against a different OpenSSL: the vita preset uses the
            # fetched zlib/minizip instead.
            target_link_libraries(heroes_lore_map PRIVATE bz2 lzma zstd ppmd ssl crypto)
        endif()
        # libstdc++ reaches pthread through weak references, which never pull archive members.
        target_link_libraries(heroes_lore PRIVATE -Wl,--whole-archive pthread -Wl,--no-whole-archive)
        # This SDL2 is built with vitaGL, whose DMA copies and shader compiler need stubs that its
        # link list puts before vitaGL.
        string(APPEND CMAKE_CXX_STANDARD_LIBRARIES " -lSceKernelDmacMgr_stub -lSceShaccCg_stub -lSceShaccCgExt -ltaihen_stub")
        vita_create_self(heroes_lore.self heroes_lore UNSAFE)
        set(HEROES_LORE_VITA_FILES)
        set(HEROES_LORE_VITA_ART "${CMAKE_CURRENT_SOURCE_DIR}/assets/vita/sce_sys")
        foreach(art icon0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml)
            if(EXISTS "${HEROES_LORE_VITA_ART}/${art}")
                # vita-pack-vpk splits its arguments at spaces: pack a copy from the build folder.
                configure_file("${HEROES_LORE_VITA_ART}/${art}" "${CMAKE_CURRENT_BINARY_DIR}/sce_sys/${art}" COPYONLY)
                list(APPEND HEROES_LORE_VITA_FILES FILE "${CMAKE_CURRENT_BINARY_DIR}/sce_sys/${art}" "sce_sys/${art}")
            endif()
        endforeach()
        vita_create_vpk(heroes_lore.vpk "${VITA_TITLEID}" heroes_lore.self
            VERSION "${VITA_VERSION}" NAME "${VITA_APP_NAME}" ${HEROES_LORE_VITA_FILES})
    endif()
endif()

option(HEROES_LORE_BUILD_TESTS "Build runtime regression checks" OFF)
if(HEROES_LORE_BUILD_TESTS)
    enable_testing()
    if(HEROES_LORE_BUILD_SDL AND NOT PSP)
        add_executable(heroes_lore_texture_tiles_test tests/texture_tiles_test.cpp src/texture_tiles.cpp)
        target_include_directories(heroes_lore_texture_tiles_test PRIVATE src)
        if(TARGET SDL2::SDL2)
            target_link_libraries(heroes_lore_texture_tiles_test PRIVATE SDL2::SDL2)
        else()
            target_link_libraries(heroes_lore_texture_tiles_test PRIVATE SDL2::SDL2-static)
        endif()
        if(TARGET SDL2::SDL2main)
            target_link_libraries(heroes_lore_texture_tiles_test PRIVATE SDL2::SDL2main)
        endif()
        add_test(NAME texture_tiles COMMAND heroes_lore_texture_tiles_test)
    endif()
    add_executable(heroes_lore_shop_test tests/shop_transaction_test.cpp)
    target_link_libraries(heroes_lore_shop_test PRIVATE heroes_lore_map)
    add_test(NAME shop_transaction COMMAND heroes_lore_shop_test)
    add_executable(heroes_lore_event_test tests/event_transition_test.cpp)
    target_link_libraries(heroes_lore_event_test PRIVATE heroes_lore_map)
    add_test(NAME event_transition COMMAND heroes_lore_event_test)
    add_executable(heroes_lore_new_game_test tests/new_game_test.cpp)
    target_link_libraries(heroes_lore_new_game_test PRIVATE heroes_lore_map)
    add_test(NAME new_game COMMAND heroes_lore_new_game_test)
    add_executable(heroes_lore_equipment_test tests/equipment_test.cpp)
    target_link_libraries(heroes_lore_equipment_test PRIVATE heroes_lore_map)
    add_test(NAME equipment COMMAND heroes_lore_equipment_test)
    add_executable(heroes_lore_presentation_test tests/event_presentation_test.cpp)
    target_link_libraries(heroes_lore_presentation_test PRIVATE heroes_lore_map)
    add_test(NAME event_presentation COMMAND heroes_lore_presentation_test)
    add_executable(heroes_lore_startup_dialogue_test tests/startup_dialogue_test.cpp)
    target_link_libraries(heroes_lore_startup_dialogue_test PRIVATE heroes_lore_map)
    add_test(NAME startup_dialogue COMMAND heroes_lore_startup_dialogue_test)
    add_executable(heroes_lore_title_test tests/title_menu_test.cpp)
    target_include_directories(heroes_lore_title_test PRIVATE src)
    add_test(NAME title_menu COMMAND heroes_lore_title_test)
    add_executable(heroes_lore_settings_test tests/startup_settings_test.cpp)
    target_link_libraries(heroes_lore_settings_test PRIVATE heroes_lore_map)
    add_test(NAME startup_settings COMMAND heroes_lore_settings_test)
    add_executable(heroes_lore_audio_test tests/audio_test.cpp)
    target_link_libraries(heroes_lore_audio_test PRIVATE heroes_lore_map)
    add_test(NAME audio COMMAND heroes_lore_audio_test)
    add_executable(heroes_lore_ending_test tests/ending_test.cpp)
    target_include_directories(heroes_lore_ending_test PRIVATE src)
    add_test(NAME ending COMMAND heroes_lore_ending_test)
    add_executable(heroes_lore_service_test tests/service_test.cpp)
    target_link_libraries(heroes_lore_service_test PRIVATE heroes_lore_map)
    add_test(NAME service COMMAND heroes_lore_service_test)
    add_executable(heroes_lore_save_test tests/save_game_test.cpp)
    target_link_libraries(heroes_lore_save_test PRIVATE heroes_lore_map)
    add_test(NAME save_game COMMAND heroes_lore_save_test)
    add_executable(heroes_lore_progress_test tests/hero_progress_test.cpp)
    target_link_libraries(heroes_lore_progress_test PRIVATE heroes_lore_map)
    add_test(NAME hero_progress COMMAND heroes_lore_progress_test)
    add_executable(heroes_lore_consumable_test tests/consumable_guardian_test.cpp)
    target_link_libraries(heroes_lore_consumable_test PRIVATE heroes_lore_map)
    add_test(NAME consumable_guardian COMMAND heroes_lore_consumable_test)
    add_executable(heroes_lore_world_test tests/world_presentation_test.cpp)
    target_link_libraries(heroes_lore_world_test PRIVATE heroes_lore_map)
    add_test(NAME world_presentation COMMAND heroes_lore_world_test)
    add_executable(heroes_lore_viewport_test tests/viewport_test.cpp)
    target_link_libraries(heroes_lore_viewport_test PRIVATE heroes_lore_map)
    add_test(NAME viewport COMMAND heroes_lore_viewport_test)
    add_executable(heroes_lore_hero_movement_test tests/hero_movement_test.cpp)
    target_link_libraries(heroes_lore_hero_movement_test PRIVATE heroes_lore_map)
    add_test(NAME hero_movement COMMAND heroes_lore_hero_movement_test)
    add_executable(heroes_lore_combat_test tests/combat_test.cpp)
    target_link_libraries(heroes_lore_combat_test PRIVATE heroes_lore_map)
    add_test(NAME combat COMMAND heroes_lore_combat_test)
    add_executable(heroes_lore_bitmap_test tests/bitmap_text_test.cpp)
    target_link_libraries(heroes_lore_bitmap_test PRIVATE heroes_lore_map)
    add_test(NAME bitmap_text COMMAND heroes_lore_bitmap_test)
    set(HEROES_LORE_TEST_JAR "${CMAKE_CURRENT_SOURCE_DIR}/../../Heroes_Lore_Wind_Of_Soltia_ENG__Multiscreen.jar"
        CACHE FILEPATH "Optional source JAR for the opening-scene regression test")
    if(EXISTS "${HEROES_LORE_TEST_JAR}")
        add_test(NAME world_presentation_jar COMMAND heroes_lore_world_test "${HEROES_LORE_TEST_JAR}")
        add_test(NAME combat_jar COMMAND heroes_lore_combat_test "${HEROES_LORE_TEST_JAR}")
        add_test(NAME event_presentation_jar COMMAND heroes_lore_presentation_test "${HEROES_LORE_TEST_JAR}")
        add_test(NAME new_game_opening COMMAND heroes_lore_new_game_test "${HEROES_LORE_TEST_JAR}")
    endif()
endif()
