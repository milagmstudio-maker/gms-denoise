# DeepFilterNet3 modes: builds third_party/dfbridge (Rust) as a static library.
#
# Needs cargo (https://rustup.rs). DeepFilterNet (MIT OR Apache-2.0) is fetched
# at a pinned commit into build-deps/DeepFilterNet on the first configure; the
# two DFN3 model files (~45 MB) are embedded in the plug-in binary.
#
# Result: target zd_dfbridge (imported static library + system libs) and the
# compile definition ZD_HAS_DFN=1 on zd_dsp.

set(ZD_DFN_COMMIT 7b14b895fe7fc42cab72b4a4c1e49c982bd6404b)
set(ZD_DFN_DIR ${CMAKE_SOURCE_DIR}/build-deps/DeepFilterNet)

find_package(Git REQUIRED)
if(NOT EXISTS ${ZD_DFN_DIR}/libDF/Cargo.toml)
  message(STATUS "Fetching DeepFilterNet ${ZD_DFN_COMMIT}")
  file(MAKE_DIRECTORY ${ZD_DFN_DIR})
  execute_process(COMMAND ${GIT_EXECUTABLE} init -q WORKING_DIRECTORY ${ZD_DFN_DIR})
  execute_process(COMMAND ${GIT_EXECUTABLE} remote add origin https://github.com/Rikorose/DeepFilterNet.git WORKING_DIRECTORY ${ZD_DFN_DIR})
  execute_process(COMMAND ${GIT_EXECUTABLE} fetch -q --depth 1 origin ${ZD_DFN_COMMIT}
                  WORKING_DIRECTORY ${ZD_DFN_DIR} RESULT_VARIABLE r1)
  execute_process(COMMAND ${GIT_EXECUTABLE} checkout -q FETCH_HEAD WORKING_DIRECTORY ${ZD_DFN_DIR} RESULT_VARIABLE r2)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0)
    message(FATAL_ERROR "Could not fetch DeepFilterNet into ${ZD_DFN_DIR}")
  endif()
endif()

find_program(ZD_CARGO cargo HINTS $ENV{HOME}/.cargo/bin $ENV{USERPROFILE}/.cargo/bin REQUIRED)
set(ZD_DFB_SRC ${CMAKE_SOURCE_DIR}/third_party/dfbridge)
set(ZD_DFB_TARGET_DIR ${CMAKE_BINARY_DIR}/dfbridge)
set(ZD_DFB_DEPS ${ZD_DFB_SRC}/Cargo.toml ${ZD_DFB_SRC}/Cargo.lock ${ZD_DFB_SRC}/src/lib.rs ${ZD_DFB_SRC}/src/resample.rs)

if(MSVC)
  # static C runtime like the rest of the plug-in (/MT)
  set(ZD_DFB_LIB ${ZD_DFB_TARGET_DIR}/x86_64-pc-windows-msvc/release/dfbridge.lib)
  add_custom_command(OUTPUT ${ZD_DFB_LIB}
    COMMAND ${CMAKE_COMMAND} -E env "RUSTFLAGS=-C target-feature=+crt-static" "CARGO_TARGET_DIR=${ZD_DFB_TARGET_DIR}"
            ${ZD_CARGO} build --release --locked --lib --target x86_64-pc-windows-msvc --manifest-path ${ZD_DFB_SRC}/Cargo.toml
    DEPENDS ${ZD_DFB_DEPS} COMMENT "cargo: dfbridge (DeepFilterNet3)" VERBATIM)
  set(ZD_DFB_SYSLIBS kernel32 advapi32 bcrypt ntdll userenv ws2_32 synchronization dbghelp)
elseif(APPLE)
  set(ZD_DFB_LIB ${ZD_DFB_TARGET_DIR}/universal/libdfbridge.a)
  set(_archs ${CMAKE_OSX_ARCHITECTURES})
  if(NOT _archs)
    set(_archs ${CMAKE_HOST_SYSTEM_PROCESSOR})
  endif()
  set(_cmds)
  set(_libs)
  foreach(a IN LISTS _archs)
    if(a STREQUAL "arm64")
      set(t aarch64-apple-darwin)
    else()
      set(t x86_64-apple-darwin)
    endif()
    list(APPEND _cmds COMMAND ${CMAKE_COMMAND} -E env "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}" "CARGO_TARGET_DIR=${ZD_DFB_TARGET_DIR}"
         ${ZD_CARGO} build --release --locked --lib --target ${t} --manifest-path ${ZD_DFB_SRC}/Cargo.toml)
    list(APPEND _libs ${ZD_DFB_TARGET_DIR}/${t}/release/libdfbridge.a)
  endforeach()
  add_custom_command(OUTPUT ${ZD_DFB_LIB}
    ${_cmds}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZD_DFB_TARGET_DIR}/universal
    COMMAND lipo -create ${_libs} -output ${ZD_DFB_LIB}
    DEPENDS ${ZD_DFB_DEPS} COMMENT "cargo: dfbridge (DeepFilterNet3), ${_archs}" VERBATIM)
  set(ZD_DFB_SYSLIBS "-framework CoreFoundation" "-framework Security" iconv)
else()
  set(ZD_DFB_LIB ${ZD_DFB_TARGET_DIR}/release/libdfbridge.a)
  add_custom_command(OUTPUT ${ZD_DFB_LIB}
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${ZD_DFB_TARGET_DIR}"
            ${ZD_CARGO} build --release --locked --lib --manifest-path ${ZD_DFB_SRC}/Cargo.toml
    DEPENDS ${ZD_DFB_DEPS} COMMENT "cargo: dfbridge (DeepFilterNet3)" VERBATIM)
  set(ZD_DFB_SYSLIBS pthread dl m)
endif()

add_custom_target(zd_dfbridge_build DEPENDS ${ZD_DFB_LIB})
add_library(zd_dfbridge STATIC IMPORTED GLOBAL)
set_target_properties(zd_dfbridge PROPERTIES
  IMPORTED_LOCATION ${ZD_DFB_LIB}
  INTERFACE_INCLUDE_DIRECTORIES ${ZD_DFB_SRC}/include
  INTERFACE_LINK_LIBRARIES "${ZD_DFB_SYSLIBS}")
add_dependencies(zd_dfbridge zd_dfbridge_build)
