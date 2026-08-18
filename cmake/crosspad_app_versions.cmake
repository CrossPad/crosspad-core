# Generates the AppVersion inventory table declared in
# crosspad/app/AppVersions.hpp — the build-time record of which app submodules
# went into this binary, at which commit, against which manifest pin.
#
# Shared by every CMake-based platform (ESP-IDF firmware, PC simulator) so the
# two report the same thing in the same order. PlatformIO has its own generator
# emitting the same table from Python.
#
#   crosspad_generate_app_versions(
#       OUTPUT   <path to generated .cpp>
#       APPS     <dirs>       # app component/submodule dirs (registry ids)
#       SHARED   <dirs>       # crosspad-core, crosspad-gui, ...
#       MANIFEST <apps.json>  # optional; supplies the recorded ref per app id
#   )

set(CROSSPAD_APP_VERSIONS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_crosspad_manifest_ref out_var manifest_text app_id)
    set(${out_var} "" PARENT_SCOPE)
    if(manifest_text STREQUAL "" OR CMAKE_VERSION VERSION_LESS 3.19)
        return()
    endif()
    string(JSON _installed ERROR_VARIABLE _err GET "${manifest_text}" "installed")
    if(_err)
        return()
    endif()
    string(JSON _entry ERROR_VARIABLE _err GET "${_installed}" "${app_id}")
    if(_err)
        return()
    endif()
    string(JSON _ref ERROR_VARIABLE _err GET "${_entry}" "ref")
    if(NOT _err)
        set(${out_var} "${_ref}" PARENT_SCOPE)
    endif()
endfunction()

function(_crosspad_appver_entry out_var dir app_id manifest_text)
    set(_commit "unknown")
    set(_ref "")
    set(_dirty "false")

    execute_process(COMMAND git rev-parse --short HEAD
                    WORKING_DIRECTORY "${dir}"
                    OUTPUT_VARIABLE _out OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _res)
    if(_res EQUAL 0 AND _out)
        set(_commit "${_out}")
    endif()

    execute_process(COMMAND git rev-parse --abbrev-ref HEAD
                    WORKING_DIRECTORY "${dir}"
                    OUTPUT_VARIABLE _branch OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _res)
    if(_res EQUAL 0 AND _branch AND NOT _branch STREQUAL "HEAD")
        set(_ref "${_branch}")
    endif()

    # The manifest pin wins over the checked-out branch: it is what the project
    # asked for, and a divergence between the two is exactly what a reader of
    # this telemetry wants to see.
    if(NOT app_id STREQUAL "")
        _crosspad_manifest_ref(_pin "${manifest_text}" "${app_id}")
        if(NOT _pin STREQUAL "")
            set(_ref "${_pin}")
        endif()
    endif()

    execute_process(COMMAND git status --porcelain
                    WORKING_DIRECTORY "${dir}"
                    OUTPUT_VARIABLE _status OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _res)
    if(_res EQUAL 0 AND NOT _status STREQUAL "")
        set(_dirty "true")
    endif()

    get_filename_component(_name "${dir}" NAME)
    set(${out_var}
        "    { \"${_name}\", \"${app_id}\", \"${_commit}\", \"${_ref}\", ${_dirty} },\n"
        PARENT_SCOPE)
endfunction()

function(crosspad_generate_app_versions)
    cmake_parse_arguments(CAV "" "OUTPUT;MANIFEST" "APPS;SHARED" ${ARGN})
    if(NOT CAV_OUTPUT)
        message(FATAL_ERROR "crosspad_generate_app_versions: OUTPUT is required")
    endif()

    set(_manifest "")
    if(CAV_MANIFEST AND EXISTS "${CAV_MANIFEST}")
        file(READ "${CAV_MANIFEST}" _manifest)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CAV_MANIFEST}")
    endif()

    set(_APPVER_ENTRIES "")
    foreach(_dir ${CAV_SHARED})
        if(IS_DIRECTORY "${_dir}")
            _crosspad_appver_entry(_entry "${_dir}" "" "${_manifest}")
            string(APPEND _APPVER_ENTRIES "${_entry}")
        endif()
    endforeach()
    foreach(_dir ${CAV_APPS})
        if(NOT IS_DIRECTORY "${_dir}")
            continue()
        endif()
        get_filename_component(_name "${_dir}" NAME)
        string(REGEX REPLACE "^crosspad-" "" _app_id "${_name}")
        _crosspad_appver_entry(_entry "${_dir}" "${_app_id}" "${_manifest}")
        string(APPEND _APPVER_ENTRIES "${_entry}")
    endforeach()

    # Re-run configure when a submodule moves, otherwise the baked-in inventory
    # keeps describing whatever was checked out the day the build dir was made.
    foreach(_dir ${CAV_SHARED} ${CAV_APPS})
        set(_head "")
        if(IS_DIRECTORY "${_dir}/.git")
            set(_head "${_dir}/.git/HEAD")
        elseif(EXISTS "${_dir}/.git")
            # Submodule: .git is a file pointing at the real gitdir, and its own
            # mtime never moves on checkout — depend on the HEAD it points to.
            file(READ "${_dir}/.git" _gitdir_line)
            string(REGEX REPLACE "^gitdir:[ \t]*([^\r\n]*).*$" "\\1"
                   _gitdir "${_gitdir_line}")
            string(STRIP "${_gitdir}" _gitdir)
            if(NOT IS_ABSOLUTE "${_gitdir}")
                set(_gitdir "${_dir}/${_gitdir}")
            endif()
            set(_head "${_gitdir}/HEAD")
        endif()
        if(_head AND EXISTS "${_head}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                         "${_head}")
        endif()
    endforeach()

    configure_file("${CROSSPAD_APP_VERSIONS_DIR}/app_versions.cpp.in"
                   "${CAV_OUTPUT}" @ONLY)
endfunction()
