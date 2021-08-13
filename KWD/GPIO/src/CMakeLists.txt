add_definitions("-DACSDK_LOG_MODULE=GPIOKeywordDetector")
add_library(GPIO SHARED
    GPIOKeywordDetector.cpp)

target_include_directories(GPIO PUBLIC
    "${KWD_SOURCE_DIR}/include"
    "${GPIO_SOURCE_DIR}/include")

target_link_libraries(GPIO KWD AVSCommon wiringPi)
