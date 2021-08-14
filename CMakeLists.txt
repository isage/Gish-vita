cmake_minimum_required(VERSION 3.4.1)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

project(Gish)

set(TARGET_NAME ${PROJECT_NAME})

find_package(SDL2 REQUIRED)
find_package(OpenAL REQUIRED)

include_directories(${PROJECT_NAME}
                    ${SDL2_INCLUDE_DIR}
                    ${OPENAL_INCLUDE_DIR})

add_definitions(-DGLES -DPC_GLES)

set(GISH_SOURCES    ${PROJECT_NAME}/audio/audio.c
                    ${PROJECT_NAME}/game/ai.c
                    ${PROJECT_NAME}/game/animation.c
                    ${PROJECT_NAME}/game/block.c
                    ${PROJECT_NAME}/game/boss.c
                    ${PROJECT_NAME}/game/config.c
                    ${PROJECT_NAME}/game/credits.c
                    ${PROJECT_NAME}/game/custom.c
                    ${PROJECT_NAME}/game/damage.c
                    ${PROJECT_NAME}/game/debug.c
                    ${PROJECT_NAME}/game/editor.c
                    ${PROJECT_NAME}/game/game.c
                    ${PROJECT_NAME}/game/gameaudio.c
                    ${PROJECT_NAME}/game/gamemenu.c
                    ${PROJECT_NAME}/game/gameobject.c
                    ${PROJECT_NAME}/game/gametexture.c
                    ${PROJECT_NAME}/game/high.c
                    ${PROJECT_NAME}/game/level.c
                    ${PROJECT_NAME}/game/lighting.c
                    ${PROJECT_NAME}/game/logic.c
                    ${PROJECT_NAME}/game/mainmenu.c
                    ${PROJECT_NAME}/game/mappack.c
                    ${PROJECT_NAME}/game/music.c
                    ${PROJECT_NAME}/game/objedit.c
                    ${PROJECT_NAME}/game/objfunc.c
                    ${PROJECT_NAME}/game/options.c
                    ${PROJECT_NAME}/game/physics.c
                    ${PROJECT_NAME}/game/player.c
                    ${PROJECT_NAME}/game/prerender.c
                    ${PROJECT_NAME}/game/random.c
                    ${PROJECT_NAME}/game/record.c
                    ${PROJECT_NAME}/game/render.c
                    ${PROJECT_NAME}/game/replay.c
                    ${PROJECT_NAME}/game/ropeedit.c
                    ${PROJECT_NAME}/game/setup.c
                    ${PROJECT_NAME}/game/socket.c
                    ${PROJECT_NAME}/game/sprite.c
                    ${PROJECT_NAME}/game/vsmode.c
                    ${PROJECT_NAME}/input/joystick.c
                    ${PROJECT_NAME}/input/keyboard.c
                    ${PROJECT_NAME}/input/mouse.c
                    ${PROJECT_NAME}/math/intersec.c
                    ${PROJECT_NAME}/math/vector.c
                    ${PROJECT_NAME}/menu/menu.c
                    ${PROJECT_NAME}/parser/parser.c
                    ${PROJECT_NAME}/physics/bond.c
                    ${PROJECT_NAME}/physics/object.c
                    ${PROJECT_NAME}/physics/particle.c
                    ${PROJECT_NAME}/sdl/endian.c
                    ${PROJECT_NAME}/sdl/event.c
                    ${PROJECT_NAME}/sdl/file.c
                    ${PROJECT_NAME}/sdl/video.c
                    ${PROJECT_NAME}/video/glfunc.c
                    ${PROJECT_NAME}/video/opengl.c
                    ${PROJECT_NAME}/video/text.c
                    ${PROJECT_NAME}/video/texture.c
                    ${PROJECT_NAME}/main.c)

add_executable(${TARGET_NAME} ${GISH_SOURCES})

add_definitions("-Dmemcpy=sceClibMemcpy")
add_definitions("-Dmemcmp=sceClibMemcmp")
add_definitions("-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ftree-vectorize -ffast-math -fsigned-char")

target_link_libraries(${TARGET_NAME}
    SDL2::SDL2-static
    openal
    vorbisfile
    vorbis
    ogg
    pthread
    z
    m
    SceAudio_stub
    SceAudioIn_stub
    liblibGLESv1_CM_stub_weak.a liblibgpu_es4_ext_stub_weak.a liblibIMGEGL_stub_weak.a
)

set(VITA_APP_NAME "Gish")
set(VITA_TITLEID  "GISH00001")
set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

vita_create_self(${TARGET_NAME}.self ${TARGET_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${TARGET_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys sce_sys
    FILE prx/libGLESv1_CM.suprx libGLESv1_CM.suprx
    FILE prx/libgpu_es4_ext.suprx libgpu_es4_ext.suprx
    FILE prx/libIMGEGL.suprx libIMGEGL.suprx
    FILE prx/libpvrPSP2_WSEGL.suprx libpvrPSP2_WSEGL.suprx
    FILE data data
)
