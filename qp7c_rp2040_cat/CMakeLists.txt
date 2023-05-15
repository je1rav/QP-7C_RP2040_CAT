cmake_minimum_required(VERSION 3.5)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../hw/bsp/family_support.cmake)

# gets PROJECT name for the example (e.g. <BOARD>-<DIR_NAME>)
family_get_project_name(PROJECT ${CMAKE_CURRENT_LIST_DIR})

project(${PROJECT})

# Checks this example is valid for the family and initializes the project
family_initialize_project(${PROJECT} ${CMAKE_CURRENT_LIST_DIR})

add_executable(${PROJECT})

# Make ws2812.pio.h headter file for Neopixel
pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# Example source
target_sources(${PROJECT} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/si5351.c 
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_audio.c 
        )

# Example include
target_include_directories(${PROJECT} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        )


target_link_libraries(${PROJECT} PUBLIC
        pico_stdlib
        hardware_pio
        hardware_adc
        hardware_i2c
        )

pico_add_extra_outputs(${PROJECT})

# Configure compilation flags and libraries for the example... see the corresponding function
# in hw/bsp/FAMILY/family.cmake for details.
family_configure_device_example(${PROJECT})