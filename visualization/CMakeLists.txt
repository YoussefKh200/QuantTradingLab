# Visualization module - Dear ImGui trading dashboard
# This module is optional and requires OpenGL + Dear ImGui + GLFW

# Find required packages for visualization
find_package(OpenGL QUIET)
find_package(glfw3 QUIET)

# Only build visualization if dependencies are found
if(OpenGL_FOUND AND glfw3_FOUND)
    # In production, you would also find Dear ImGui
    # For now, we'll create a stub library that can be built without dependencies
    
    add_library(qtl_visualization STATIC
        gui/GuiFramework.cpp
        charts/PriceChart.cpp
        dashboards/TradingDashboard.cpp
        heatmaps/Heatmap.cpp
    )
    
    target_include_directories(qtl_visualization PUBLIC ${CMAKE_SOURCE_DIR})
    target_link_libraries(qtl_visualization PUBLIC 
        qtl_core
        qtl_analytics
        ${OPENGL_LIBRARIES}
        glfw
    )
    
    message(STATUS "Visualization module enabled")
else()
    message(STATUS "Visualization module disabled - missing OpenGL/GLFW dependencies")
    # Create an empty interface library to satisfy dependencies
    add_library(qtl_visualization INTERFACE)
    target_include_directories(qtl_visualization INTERFACE ${CMAKE_SOURCE_DIR})
endif()
