# =============================================================================
# SimCity 4 Deluxe - static recompilation host (32-bit, MSVC x86)
#
#   py -3 run_lift.py --all  # -> src/recomp/gen/  (needs work/functions.json)
#   build.cmd                # vcvarsall x86 + CMake + Ninja -> build/sc4.exe
#   build\sc4.exe --run --watchdog 60
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(sc4 C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the host must be 32-bit: use build.cmd (vcvarsall x86)")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Static CRT: a fresh test VM has no 32-bit VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty -- run `py -3 run_lift.py --all` first")
endif()

# The host's own files: every .c in src/runtime, so adding one needs no edit
# here (an edit here rebuilds all of src/recomp/gen on the farm).
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/runtime/*.c")

add_executable(sc4
  ${HOST_SOURCES}
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(sc4 PRIVATE
  src/runtime src/recomp/gen ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(sc4 PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                   RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, so the guest's 0x00400000..0x00BC8000 is free.
target_link_options(sc4 PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                              /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x1000000
                              /MANIFESTUAC:NO)
set(GEN_OPT "/O2" CACHE STRING "optimisation for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "/W3")
target_link_libraries(sc4 PRIVATE user32 gdi32 kernel32 ole32 uuid opengl32)

option(SC4_TRACE "record a ring buffer of lifted function entries" OFF)
if(SC4_TRACE)
  target_compile_definitions(sc4 PRIVATE RECOMP_TRACE=1)
endif()
