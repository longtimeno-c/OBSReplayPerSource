cmake_minimum_required(VERSION 3.28...3.30)
project(obs-replay-plugin)

add_library(obs-replay-plugin MODULE
    obs-replay-plugin.cpp
)
target_link_libraries(obs-replay-plugin PRIVATE OBS::libobs)


target_link_libraries(
  obs-replay-plugin
  PRIVATE OBS::libobs
          OBS::frontend-api
          OBS::websocket-api
          Qt6::Widgets)

# Optional: Tidy up folder name and omit 'lib' prefix
set_target_properties(obs-replay-plugin
    PROPERTIES
      FOLDER "plugins"
      PREFIX ""  # For macOS/Linux .so/.dylib naming
)

install(TARGETS obs-replay-plugin
    LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
)
