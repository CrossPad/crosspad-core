# crosspad_git_rev(<out_var> <dir>): the short commit checked out in <dir>, for
# the submodule revisions Settings -> Info shows. Best effort: a source tree
# without git (release tarball, CI export) reports "unknown" rather than
# failing the configure step.

function(crosspad_git_rev out_var dir)
    set(rev "unknown")
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY "${dir}"
        OUTPUT_VARIABLE rev_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE rev_res)
    if(rev_res EQUAL 0 AND rev_out)
        set(rev "${rev_out}")
    endif()
    set(${out_var} "${rev}" PARENT_SCOPE)
endfunction()
