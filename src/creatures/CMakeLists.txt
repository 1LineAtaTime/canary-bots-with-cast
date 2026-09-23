target_sources(
    ${CORE_TARGET_NAME}
    PRIVATE appearance/mounts/mounts.cpp
            appearance/outfit/outfit.cpp
            appearance/attached_effects/attached_effects.cpp
            combat/combat.cpp
            combat/condition.cpp
            combat/spells.cpp
            creature.cpp
            interactions/chat.cpp
            monsters/monster.cpp
            monsters/monsters.cpp
            monsters/spawns/spawn_monster.cpp
            npcs/npc.cpp
            npcs/npcs.cpp
            npcs/spawns/spawn_npc.cpp
            players/animus_mastery/animus_mastery.cpp
            players/grouping/familiars.cpp
            players/grouping/groups.cpp
            players/grouping/guild.cpp
            players/grouping/party.cpp
            players/imbuements/imbuements.cpp
            players/management/ban.cpp
            players/management/waitlist.cpp
            players/storages/storages.cpp
            players/player.cpp
            players/components/player_achievement.cpp
            players/components/player_attached_effects.cpp
            players/components/player_badge.cpp
            players/components/player_cyclopedia.cpp
            players/components/player_forge_history.cpp
            players/components/player_storage.cpp
            players/components/player_title.cpp
            players/components/wheel/player_wheel.cpp
            players/components/player_vip.cpp
            players/components/wheel/wheel_gems.cpp
            players/vocations/vocation.cpp
            players/bot/bot_engine_loader.cpp
)

# ===========================================================================
# Bot Engine shared library (hot-reloadable via dlopen)
# ===========================================================================
# The engine's 21 translation units. Shared by both platform branches below so the
# Windows and Linux builds can never drift to different source sets.
set(BOT_ENGINE_SOURCES
    players/bot/bot_engine.cpp   # lifecycle: registration, hibernation, persistence
    players/bot/bot_tick.cpp     # main tick, per-bot dispatch, virtual sim, floor changes
    players/bot/bot_combat.cpp   # combat, spells/AoE/runes, PvP realism, gang-PK
    players/bot/bot_debug.cpp    # debug stream: grid, event log, heartbeat, status text
    players/bot/bot_data.cpp     # DB/config loaders + static data: hunts, POIs, routes, equipment
    players/bot/bot_nav.cpp      # A* pool, pathcore adapter, nav + door helpers
    players/bot/bot_poi.cpp      # point-of-interest selection
    players/bot/bot_waypoint.cpp # unified waypoint following
    players/bot/bot_hunt.cpp     # hunt phase machine
    players/bot/bot_travel.cpp   # boat/carpet travel
    players/bot/bot_party.cpp    # party + autonomous party hunt
    players/bot/bot_chat.cpp     # chat subsystem
    players/bot/bot_liveness.cpp # hibernation/wake liveness
    players/bot/bot_command.cpp  # /cavebot admin + debug commands
    players/bot/bot_zgraph.cpp   # TRUE MULTI-FLOOR: portal graph + z-route planner
    players/bot/bot_supply.cpp   # potions, food, rune crafting, fishing
    players/bot/bot_house.cpp    # house visits: idle, hireling, locker, dummy training
    players/bot/bot_shrine.cpp   # reward / imbuing shrine idle
    players/bot/bot_roam.cpp     # ambient roam: off-screen staging + nearby wander
    players/bot/bot_csv.cpp      # BOT_CSV: tolerant CSV reader/writer + authored-data dump
    players/bot/bot_csvedit.cpp  # BOT_CSV: in-game CSV editors + file-reading viewers
)

if(UNIX)
    # BOT_NAV_REALISM Phase 11/12 (module split): the engine spans several translation
    # units that all link into this SINGLE shared library, so /cavebot reload is
    # unchanged. Shared includes/types/class declaration live in bot_engine_impl.hpp.
    add_library(bot_engine SHARED ${BOT_ENGINE_SOURCES})

    # Same include directories as the main binary
    target_include_directories(bot_engine
        PRIVATE ${CMAKE_SOURCE_DIR}/src
                ${BOOST_DI_INCLUDE_DIRS}
                ${LUAJIT_INCLUDE_DIRS}
                ${PARALLEL_HASHMAP_INCLUDE_DIRS}
                ${ATOMIC_QUEUE_INCLUDE_DIRS}
    )

    # Link against libraries directly used by bot_engine.cpp
    target_link_libraries(bot_engine
        PRIVATE fmt::fmt
                spdlog::spdlog
                unofficial::libmariadb
                absl::any
                absl::log
                absl::base
                ${LUAJIT_LIBRARIES}
    )

    # Disable LTO and unity build — the .so resolves engine symbols at runtime via -rdynamic
    set_target_properties(bot_engine PROPERTIES
        UNITY_BUILD OFF
        INTERPROCEDURAL_OPTIMIZATION OFF
        POSITION_INDEPENDENT_CODE ON
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    target_compile_options(bot_engine
        PRIVATE -fno-lto
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-O3 -march=native>
    )
    target_link_options(bot_engine PRIVATE -fno-lto)

    # Own precompiled header — do NOT reuse canary's.
    #
    # canary builds its PCH with -fpie (executable); this shared library compiles
    # -fPIC, so GCC rejects the reused PCH outright:
    #   "cmake_pch.hxx.gch: created and used with different settings of -fpie"
    # Every TU then re-parses the full include set. That cost was invisible while
    # the engine was one TU and became the dominant term at 11 (measured: 204s for
    # a bot_engine_impl.hpp touch).
    #
    # pch.hpp must stay in the list: USE_PRECOMPILED_HEADERS is what makes ~40
    # headers skip their own std includes, so defining it without pch.hpp in the
    # PCH would break the build.
    if(USE_PRECOMPILED_HEADER)
        target_precompile_headers(bot_engine
            PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.hpp
                    players/bot/bot_engine_impl.hpp
        )
        target_compile_definitions(bot_engine PUBLIC USE_PRECOMPILED_HEADERS)
    endif()

    # Compiler warnings matching main target
    if(CMAKE_COMPILER_IS_GNUCXX)
        target_compile_options(bot_engine PRIVATE -Wno-deprecated-declarations)
    endif()

    target_compile_definitions(bot_engine
        PUBLIC $<$<CONFIG:Release>:NDEBUG> $<$<CONFIG:RelWithDebInfo>:NDEBUG>
    )

elseif(WIN32)
    # =======================================================================
    # Windows: STATIC library linked into canary.exe -- deliberately not a DLL.
    #
    # A hot-reloadable DLL is not merely harder here, it is unsound. The base
    # preset sets BUILD_STATIC_LIBRARY=ON, so setup_target() pins
    # MSVC_RUNTIME_LIBRARY to /MT (BaseConfig.cmake:407-415). A DLL would then
    # carry its OWN static CRT and heap, while IBotEngine passes std::string,
    # std::vector and std::shared_ptr<Player> across the boundary
    # (bot_engine_interface.hpp:491+) -- allocate in one CRT, free in the other,
    # and that is heap corruption rather than a link error. Making a DLL viable
    # would mean forcing /MD across the whole project and diverging from
    # upstream's x64-windows-static triplet.
    #
    # Consequence, by design: no /cavebot reload on Windows. See the note on
    # BotEngineLoader::supportsHotReload(). Restarting a local dev server costs
    # seconds; hot-reload exists for the production host, where it does not.
    # =======================================================================
    add_library(bot_engine STATIC ${BOT_ENGINE_SOURCES})

    # Without this the target gets NEITHER cxx_std_23 (via project_options) NOR
    # the /MT runtime that the rest of the build uses. Omitting it fails first
    # with thousands of C++23 syntax errors and then, once that is fixed, with
    # LNK2038 runtime-library mismatches. It is not optional.
    setup_target(bot_engine)

    target_include_directories(bot_engine
        PRIVATE ${CMAKE_SOURCE_DIR}/src
                ${BOOST_DI_INCLUDE_DIRS}
                ${LUAJIT_INCLUDE_DIRS}
                ${PARALLEL_HASHMAP_INCLUDE_DIRS}
                ${ATOMIC_QUEUE_INCLUDE_DIRS}
    )

    target_link_libraries(bot_engine
        PRIVATE fmt::fmt
                spdlog::spdlog
                unofficial::libmariadb
                absl::any
                absl::log
                absl::base
                ${LUAJIT_LIBRARIES}
    )

    set_target_properties(bot_engine PROPERTIES
        UNITY_BUILD OFF
        INTERPROCEDURAL_OPTIMIZATION OFF
    )

    target_compile_options(bot_engine
        PRIVATE
            # /bigobj: unity is OFF here, so each TU carries the full 6.5k-line
            # bot_engine_impl.hpp plus pch.hpp. bot_tick.cpp (6.9k lines) exceeds
            # the 65,535-section COFF limit in Debug (/Od inlines nothing) and
            # fails with C1128. Costs nothing when not needed.
            /bigobj
            # /utf-8: NOT set anywhere else in the build. The engine's status and
            # log strings contain UTF-8 inside string literals (e.g. the em-dash in
            # bot_debug.cpp:296 "PARTY — following {}"); without this MSVC decodes
            # the source as the ANSI code page and every such string is mojibake.
            /utf-8
            # C4068: the five `#pragma GCC diagnostic` lines the engine uses to
            # quiet -Wunused-function on GCC. They are correct where they came
            # from and meaningless here; MSVC just cannot parse them.
            /wd4068
    )

    # PRIVATE, not PUBLIC as on the UNIX branch. Nothing links the .so there, so
    # the distinction is invisible; here canary_core DOES link this target, and
    # PUBLIC would push the engine's defines onto the whole core.
    if(USE_PRECOMPILED_HEADER)
        target_precompile_headers(bot_engine
            PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.hpp
                    players/bot/bot_engine_impl.hpp
        )
        target_compile_definitions(bot_engine PRIVATE USE_PRECOMPILED_HEADERS)
    endif()

    target_compile_definitions(bot_engine
        PRIVATE $<$<CONFIG:Release>:NDEBUG> $<$<CONFIG:RelWithDebInfo>:NDEBUG>
    )

    # bot_engine_loader.cpp lives in the core target (line 38 above) and calls
    # createBotEngine() directly on Windows, so core needs this archive. PUBLIC so
    # the dependency propagates to everything linking canary_core -- notably the
    # test executables (tests/CMakeLists.txt:49), which would otherwise fail
    # LNK2019 on the factory. MSVC's linker resolves the mutual reference between
    # the two archives without the ordering constraints GNU ld would impose.
    target_link_libraries(${CORE_TARGET_NAME} PUBLIC bot_engine)
endif()
