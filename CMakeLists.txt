# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pimoroni_picolipo_4mb CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(RavingPixies C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable.
add_subdirectory(dependencies/ssd1306)
add_subdirectory(dependencies/button)
add_executable(RavingPixies RavingPixies.cpp)

pico_set_program_name(RavingPixies "RavingPixies")
pico_set_program_version(RavingPixies "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(RavingPixies 0)
pico_enable_stdio_usb(RavingPixies 1)

# Add the standard library to the build
target_link_libraries(RavingPixies
        pico_stdlib pico_multicore hardware_i2c pico_ssd1306 button)

# Add the standard include files to the build
target_include_directories(RavingPixies PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/dependencies/button
        ${CMAKE_CURRENT_LIST_DIR}/dependencies/ssd1306
)

# Add any user requested libraries
target_link_libraries(RavingPixies 
        hardware_i2c
        )

pico_add_extra_outputs(RavingPixies)

