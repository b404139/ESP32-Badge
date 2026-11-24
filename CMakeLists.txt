cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(badge_pet)

# Pack GIFs and the Grokbot atlas into the storage SPIFFS partition and flash it with the app.
spiffs_create_partition_image(storage media/pet FLASH_IN_PROJECT)