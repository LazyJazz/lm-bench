find_package(embree 4 REQUIRED)

file(GLOB_RECURSE DEMO_SOURCES "*.cpp" "*.h")
add_executable(${DEMO_NAME} ${DEMO_SOURCES})
target_link_libraries(${DEMO_NAME} PRIVATE LongMarch embree)
LONGMARCH_DEPLOY_RUNTIME_DEPENDENCIES(${DEMO_NAME})
