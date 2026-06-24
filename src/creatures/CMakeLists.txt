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
    add_library(bot_engine SHARED
        players/bot/bot_engine.cpp
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

    # Reuse precompiled headers from the main target if available
    if(USE_PRECOMPILED_HEADER)
        target_precompile_headers(bot_engine REUSE_FROM ${CORE_TARGET_NAME})
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
