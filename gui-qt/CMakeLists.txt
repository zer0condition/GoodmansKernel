cmake_minimum_required(VERSION 3.20)
project(goodmans_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_PREFIX_PATH "C:/Qt/6.9.3/msvc2022_64")

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Core)

add_executable(goodmans-gui WIN32
    main.cpp
    mainwindow.cpp
    mainwindow.h
    driver.cpp
    driver.h
    pages.cpp
    pages.h
    command_palette.cpp
    command_palette.h
    resources.qrc
)

target_include_directories(goodmans-gui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)

target_link_libraries(goodmans-gui PRIVATE Qt6::Widgets Qt6::Gui Qt6::Core)

if (WIN32)
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qmake_exe}" DIRECTORY)
    set(DEPLOY_GUI "${CMAKE_CURRENT_SOURCE_DIR}/../deploy/gui")

    # windeployqt straight into deploy/gui so we deploy once, not twice
    add_custom_command(TARGET goodmans-gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_GUI}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:goodmans-gui> "${DEPLOY_GUI}/"
        COMMAND "${_qt_bin}/windeployqt.exe" --no-translations --no-opengl-sw --no-system-d3d-compiler "${DEPLOY_GUI}/goodmans-gui.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../feature_guests/toolkit.wasm"
            "${DEPLOY_GUI}/toolkit.wasm"
        COMMENT "[deploy] staging gui -> deploy/gui"
    )
endif()
