# Full build (HEROES_LORE_FULL_BUILD=ON): when the embedded SoundFont subset is missing, download
# GeneralUser GS 2.0.3 from its official repository and generate the subset with
# tools/build_psp_soundfont.py from the note list in dist/soundfont (no game files needed).
# Needs Python 3 and ffmpeg on the build host. See docs/build/soundfont.md.
set(HEROES_LORE_GUGS_URL
    "https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/97049183643d5fc5a9322a69c5b09efb667c6c3a/GeneralUser-GS.sf2")
set(HEROES_LORE_GUGS_SHA256 "9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe")

function(heroes_lore_full_build_soundfont output)
    if(EXISTS "${output}")
        return()
    endif()
    # Host tools: cross toolchains restrict program lookup to their sysroot.
    find_program(HEROES_LORE_PYTHON NAMES python3 python py NO_CMAKE_FIND_ROOT_PATH)
    find_program(HEROES_LORE_FFMPEG ffmpeg NO_CMAKE_FIND_ROOT_PATH)
    if(NOT HEROES_LORE_PYTHON OR NOT HEROES_LORE_FFMPEG)
        message(FATAL_ERROR "HEROES_LORE_FULL_BUILD needs Python 3 and ffmpeg (docs/build/soundfont.md)")
    endif()
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/assets/soundfont/GeneralUser-GS.sf2")
    if(NOT EXISTS "${source}")
        message(STATUS "Full build: downloading GeneralUser GS 2.0.3 (32 MB)")
        file(DOWNLOAD "${HEROES_LORE_GUGS_URL}" "${source}.part"
            EXPECTED_HASH SHA256=${HEROES_LORE_GUGS_SHA256} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${source}.part")
            message(FATAL_ERROR "GeneralUser GS download failed: ${status}")
        endif()
        file(RENAME "${source}.part" "${source}")
    endif()
    message(STATUS "Full build: generating the embedded SoundFont subset")
    set(work "${CMAKE_CURRENT_BINARY_DIR}/soundfont")
    execute_process(
        COMMAND "${HEROES_LORE_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build_psp_soundfont.py"
            --source "${source}" --used "${CMAKE_CURRENT_SOURCE_DIR}/dist/soundfont/used-notes.json"
            --ffmpeg "${HEROES_LORE_FFMPEG}" --output "${work}/heroes_lore.sf2"
        RESULT_VARIABLE result OUTPUT_QUIET)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "SoundFont subset generation failed")
    endif()
    configure_file("${work}/heroes_lore.sf2.zlib" "${output}" COPYONLY)
endfunction()
