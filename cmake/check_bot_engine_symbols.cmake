if(NOT EXISTS "${LIBRARY}")
    message(FATAL_ERROR "Missing bot engine artifact: ${LIBRARY}")
endif()
execute_process(COMMAND "${READELF}" --dyn-syms --wide "${LIBRARY}"
    RESULT_VARIABLE RESULT OUTPUT_VARIABLE SYMBOLS ERROR_VARIABLE ERROR)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot inspect bot engine symbols: ${ERROR}")
endif()

# #50: a library that fails a check is deleted, so a failed build cannot leave a shippable
# libbot_engine.so behind in bin/ (ninja keeps the output of a failed POST_BUILD step).
set(BOT_SYMBOL_ERRORS "")

if(SYMBOLS MATCHES "[ \t]UNIQUE[ \t]")
    string(APPEND BOT_SYMBOL_ERRORS
        "\n  bot_engine exports GNU_UNIQUE symbols; unloading can retain or share engine state")
endif()

# #50: objects that can outlive the library must not be allocated inside it. make_shared<T>
# writes the control block's vptr in the calling TU, and the executable keeps those vtables
# local (LTO), so a control block created here points into this image and crashes on release
# after dlclose. Only engine-private types, destroyed with the engine, may have one here;
# anything else is allocated by the executable (src/creatures/players/bot/bot_host.hpp).
set(BOT_ALLOWED_CONTROL_BLOCKS
    # std::regex internals (engine-local regex objects)
    "_ZTVSt23_Sp_counted_ptr_inplaceINSt8__detail4_NFAINSt7__cxx1112regex_traitsIcEEEESaIvELN9__gnu_cxx12_Lock_policyE2EE"
    # zgraph flood cache (BotEngine member)
    "_ZTVSt23_Sp_counted_ptr_inplaceISt13unordered_setImSt4hashImESt8equal_toImESaImEESaIvELN9__gnu_cxx12_Lock_policyE2EE"
)
# Engine code schedules through botHostScheduleEvent, never through Dispatcher directly, and
# never constructs a Task itself. (Even addEvent/asyncEvent, whose Tasks are not engine-allocated,
# would queue an engine lambda that nothing cancels.) What this gate CANNOT see is whether each
# botHostScheduleEvent caller registers its id in BotEngine::cancelPendingDispatcherEvents --
# that stays a review rule.
set(BOT_FORBIDDEN_IMPORT_REGEX
    "^(_ZN10Dispatcher(13scheduleEvent|10cycleEvent|8addEvent|10asyncEvent|12addWalkEvent|8safeCall)|_ZN4TaskC[12]E)")

# CMake treats [ ] inside list elements as grouping: neutralise them (and ;) before splitting.
string(REPLACE "[" "(" SYMBOLS_FLAT "${SYMBOLS}")
string(REPLACE "]" ")" SYMBOLS_FLAT "${SYMBOLS_FLAT}")
string(REPLACE ";" "," SYMBOLS_FLAT "${SYMBOLS_FLAT}")
string(REPLACE "\n" ";" SYMBOL_LINES "${SYMBOLS_FLAT}")
set(BOT_SYMBOLS_PARSED 0)
foreach(line IN LISTS SYMBOL_LINES)
    # "  Num:    Value          Size Type    Bind   Vis      Ndx Name"
    if(NOT line MATCHES "^[ \t]*[0-9]+:[ \t]+[0-9a-fA-F]+[ \t]+[0-9a-fx]+[ \t]+[A-Z_]+[ \t]+[A-Z_]+[ \t]+[A-Z_]+[ \t]+([A-Z0-9_]+)[ \t]+([^ \t@]+)")
        continue()
    endif()
    math(EXPR BOT_SYMBOLS_PARSED "${BOT_SYMBOLS_PARSED} + 1")
    set(ndx "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    if(ndx STREQUAL "UND")
        if(name MATCHES "${BOT_FORBIDDEN_IMPORT_REGEX}")
            string(APPEND BOT_SYMBOL_ERRORS
                "\n  bot_engine calls ${name} directly: schedule through botHostScheduleEvent"
                " (bot_host.hpp) and register the event id in cancelPendingDispatcherEvents")
        endif()
    elseif(name MATCHES "^_ZTVSt[0-9]+_Sp_counted")
        list(FIND BOT_ALLOWED_CONTROL_BLOCKS "${name}" allowedIndex)
        if(allowedIndex EQUAL -1)
            string(APPEND BOT_SYMBOL_ERRORS
                "\n  bot_engine defines the shared_ptr control block ${name}: allocate that object in"
                " the executable (bot_host.hpp) or, if it can never outlive the engine, add it to"
                " BOT_ALLOWED_CONTROL_BLOCKS in cmake/check_bot_engine_symbols.cmake")
        endif()
    endif()
endforeach()
# A readelf format change must not turn the per-symbol checks into a silent pass.
if(BOT_SYMBOLS_PARSED LESS 100)
    string(APPEND BOT_SYMBOL_ERRORS
        "\n  parsed only ${BOT_SYMBOLS_PARSED} dynamic symbols from readelf; the gate cannot vouch for this library")
endif()

if(NOT BOT_SYMBOL_ERRORS STREQUAL "")
    file(REMOVE "${LIBRARY}")
    message(FATAL_ERROR "bot_engine symbol gate failed (#50), ${LIBRARY} deleted:${BOT_SYMBOL_ERRORS}")
endif()
message(STATUS "bot_engine: zero GNU_UNIQUE dynamic symbols, no engine-allocated host control blocks (${BOT_SYMBOLS_PARSED} symbols checked)")
