# Run on GLFW 3.4's source (the working directory) as FetchContent fetches it: replaces the start of
# the Windows event loop, _glfwPollEventsWin32, with win32_poll_events.c.in, which ends the loop at
# the second mouse move. With a mouse polling at 4-8 kHz the stock loop does not end while the mouse
# moves (glfw/glfw#2684). Done with CMake alone, so the build needs no git or patch tool, and it
# leaves a source it was already run on as it is. A source with an earlier version of the
# replacement gets this one instead (CMakeLists.txt passes the replacement's hash, so that a change
# to it runs this again).
#
# It cannot go wrong quietly:
# - the code it replaces has to be exactly GLFW 3.4's, so a newer GLFW stops the configure here until
#   the replacement has been checked against it (and EXPECTED_SHA256 updated);
# - the replacement defines frametee_glfw_win32_mouse_motion_patch, which frametee refers to on
#   Windows (graphics_backend.c), so a build that ends up with any other GLFW does not link.
set(EXPECTED_SHA256 e3e82c202352c6c78bd8afa71f8ee609e83804633d3bd0bb1ca86285acec7b7a)

set(file "${CMAKE_CURRENT_SOURCE_DIR}/src/win32_window.c")
file(READ "${file}" source)
file(READ "${CMAKE_CURRENT_LIST_DIR}/win32_poll_events.c.in" replacement)
string(FIND "${source}" "${replacement}" already)
if(NOT already EQUAL -1)
    return()
endif()

set(begin_text "void _glfwPollEventsWin32(void)\n{")
set(end_text "    // HACK: Release modifier keys that the system did not emit KEYUP for")
# (the first line of every version of the replacement)
set(earlier_text "// frametee refers to this on Windows")
string(FIND "${source}" "${earlier_text}" begin)
set(earlier TRUE)
if(begin EQUAL -1)
    string(FIND "${source}" "${begin_text}" begin)
    set(earlier FALSE)
endif()
string(FIND "${source}" "${end_text}" end)
if(begin EQUAL -1 OR end EQUAL -1 OR NOT end GREATER begin)
    message(FATAL_ERROR "GLFW: ${file} has no _glfwPollEventsWin32 as GLFW 3.4 has it; "
                        "${CMAKE_CURRENT_LIST_FILE} has to be redone for this GLFW")
endif()
math(EXPR length "${end} - ${begin}")
string(SUBSTRING "${source}" ${begin} ${length} original)
string(SHA256 original_sha256 "${original}")
if(NOT earlier AND NOT original_sha256 STREQUAL EXPECTED_SHA256)
    message(FATAL_ERROR "GLFW: _glfwPollEventsWin32 in ${file} is not GLFW 3.4's (SHA256 ${original_sha256}). "
                        "Check ${CMAKE_CURRENT_LIST_DIR}/win32_poll_events.c.in against it, then update "
                        "EXPECTED_SHA256 in ${CMAKE_CURRENT_LIST_FILE}")
endif()

string(SUBSTRING "${source}" 0 ${begin} head)
string(SUBSTRING "${source}" ${end} -1 tail)
file(WRITE "${file}" "${head}${replacement}${tail}")
