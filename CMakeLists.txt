cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

project(vid-tiberius)

include(CheckCSourceCompiles)
CHECK_C_SOURCE_COMPILES("#ifndef __MINGW32__\n#error fail\n#endif\nint main() {}" IS_MINGW)

if(NOT IS_MINGW)
  include(FindPkgConfig)
  pkg_check_modules(ffmpeg REQUIRED IMPORTED_TARGET libavformat libavcodec libavdevice libswscale libavutil)
  add_executable(vid-sender vid_sender.cpp control_interface.h ff_helpers.h)
  target_link_libraries(vid-sender PUBLIC avformat avcodec avdevice swscale avutil)
  target_link_options(vid-sender PUBLIC -pthread)
  add_executable(vid-simplereceiver vid_simplereceiver.cpp vid_receiver_common.h
                 control_interface.h ff_helpers.h)
  target_link_libraries(vid-simplereceiver PUBLIC PkgConfig::ffmpeg)
else()
  get_filename_component(COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vid_receiver_res.o"
          COMMAND "${COMPILER_DIR}/i386-mingw32ce-windres" "${CMAKE_CURRENT_SOURCE_DIR}/vid_receiver_res.rc"
          "${CMAKE_CURRENT_BINARY_DIR}/vid_receiver_res.o" DEPENDS vid_receiver_res.rc)
  add_executable(vid-receiver vid_receiver.cpp vid_receiver_common.h
                 control_interface.h ff_helpers.h command_line_to_argvw.c vid_receiver_res.o)

  set(HMI On)
  if(HMI)
    target_compile_definitions(vid-receiver PUBLIC __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
            WINCE _WIN32_IE=0x0600 _WIN32_WCE=0x0600 __CE_VERSION__=0x0600 PTW32_STATIC_LIB)
    target_compile_options(vid-receiver PUBLIC -mwin32 -march=pentium3)
    target_link_options(vid-receiver PUBLIC
            -Wl,--major-subsystem-version,6
            -Wl,--minor-subsystem-version,0 -static -lstdc++)
    target_link_libraries(vid-receiver PUBLIC avformat avcodec swscale avutil pthreadGC2 ws2 commctrl)
  else()
    target_compile_definitions(vid-receiver PUBLIC __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS)
    target_compile_options(vid-receiver PUBLIC -mwin32)
    target_link_options(vid-receiver PUBLIC -static -lstdc++)
    target_link_libraries(vid-receiver PUBLIC avformat avcodec swscale avutil pthread ws2_32 gdi32 bcrypt)
  endif()

  set_target_properties(vid-receiver PROPERTIES SUFFIX ".exe")
endif()
