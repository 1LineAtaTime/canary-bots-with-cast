# Bot engine ABI tag generator (#58) -- runs at BUILD time in script mode:
#
#   cmake -DINPUTS=<bot_abi_inputs.cmake> -DOUTPUT=<bot_engine_abi_tag.hpp> -P BotEngineAbiTag.cmake
#
# INPUTS is written at configure time by src/creatures/CMakeLists.txt and sets:
#   BOT_ABI_TOOLCHAIN  compiler id/version, global C++ flags, build type, target arch
#   BOT_ABI_ROOT       absolute path of src/ (header paths are hashed relative to it)
#   BOT_ABI_HEADERS    absolute paths of every header whose layout the engine can see
#
# Each header contributes "<relative path>:<sha256 of its text with CR stripped>", so a
# Windows (CRLF) and a Linux (LF) checkout of the same commit hash identically. The
# output header is rewritten only when the tag changes, so an mtime-only touch of a
# header does not recompile the two TUs that include it.

if(NOT INPUTS OR NOT OUTPUT)
    message(FATAL_ERROR "BotEngineAbiTag.cmake: INPUTS and OUTPUT are required")
endif()
include("${INPUTS}")

set(_abi_input "contract=2;${BOT_ABI_TOOLCHAIN}")
set(_headers ${BOT_ABI_HEADERS})
list(SORT _headers)
foreach(_hdr IN LISTS _headers)
    if(NOT EXISTS "${_hdr}")
        # Deleted since configure: the CONFIGURE_DEPENDS glob re-runs configure on the
        # next build; until then a missing file simply is not part of the contract.
        continue()
    endif()
    file(READ "${_hdr}" _text)
    string(REPLACE "\r" "" _text "${_text}")
    string(SHA256 _hash "${_text}")
    file(RELATIVE_PATH _rel "${BOT_ABI_ROOT}" "${_hdr}")
    string(APPEND _abi_input "\n${_rel}:${_hash}")
endforeach()
string(SHA256 _tag "${_abi_input}")

set(_content "// Generated at build time by cmake/BotEngineAbiTag.cmake -- do not edit.
// Both canary (the loader) and libbot_engine.so compile this header in; the loader
// finds BOT_ENGINE_ABI_MARKER in the .so file bytes BEFORE dlopen and refuses a mismatch.
#pragma once
#define BOT_ENGINE_ABI_TAG \"${_tag}\"
#define BOT_ENGINE_ABI_MARKER_PREFIX \"BOTENGINE_ABI_TAG=\"
#define BOT_ENGINE_ABI_MARKER BOT_ENGINE_ABI_MARKER_PREFIX BOT_ENGINE_ABI_TAG
")

set(_old "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _old)
endif()
if(NOT _old STREQUAL _content)
    list(LENGTH _headers _count)
    message(STATUS "Bot engine ABI tag: ${_tag} (${_count} headers)")
    file(WRITE "${OUTPUT}" "${_content}")
endif()
