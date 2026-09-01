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
if(UNIX)
    # BOT_NAV_REALISM Phase 11/12 (module split): the engine spans several translation
    # units that all link into this SINGLE shared library, so /cavebot reload is
    # unchanged. Shared includes/types/class declaration live in bot_engine_impl.hpp.
    add_library(bot_engine SHARED
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
endif()
