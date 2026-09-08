cmake_minimum_required(VERSION 3.20)

set(IDF_TARGET esp32c3)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vorotabot_esp32)

# Ensure spiffs_image directory exists so build succeeds even before frontend build
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/spiffs_image)
file(TOUCH ${CMAKE_SOURCE_DIR}/spiffs_image/.gitkeep)

# Create SPIFFS image from spiffs_image folder and flash into 'storage' partition
spiffs_create_partition_image(storage ${CMAKE_SOURCE_DIR}/spiffs_image FLASH_IN_PROJECT)
