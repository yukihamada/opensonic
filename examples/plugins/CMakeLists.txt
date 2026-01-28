# Example DSP Plugins
#
# Build as shared libraries for runtime loading by solunad.

add_library(gain_plugin SHARED gain_plugin.cpp)
target_include_directories(gain_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(gain_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "gain_plugin"
)
