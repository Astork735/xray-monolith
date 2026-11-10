cmake_minimum_required(VERSION 3.20)
project(xray_vr_tests LANGUAGES CXX)
enable_testing()
add_executable(vr_tests tests.cpp
    ../../src/xrEngine/Vr/vr_tracking.cpp
    ../../src/xrEngine/Vr/vr_swapchain.cpp
    ../../src/xrEngine/Vr/vr_input.cpp
    ../../src/xrEngine/Vr/vr_graphics_d3d11.cpp
    ../../src/xrEngine/Vr/vr_system.cpp
    ../../src/xrEngine/Vr/vr_body_tracking.cpp
    ../../src/xrEngine/Vr/vr_performance.cpp)
target_compile_features(vr_tests PRIVATE cxx_std_17)
target_compile_definitions(vr_tests PRIVATE VR_ENABLED XR_VR_TEST NOMINMAX)
target_include_directories(vr_tests PRIVATE ../../sdk/include)
target_link_libraries(vr_tests PRIVATE d3d11 dxgi)
add_test(NAME vr_regressions COMMAND vr_tests)

add_executable(vr_runtime_probe runtime_probe.cpp)
target_compile_features(vr_runtime_probe PRIVATE cxx_std_17)
target_include_directories(vr_runtime_probe PRIVATE ../../sdk/include)

add_executable(vr_pistol_tests pistol_tests.cpp)
target_compile_features(vr_pistol_tests PRIVATE cxx_std_17)
add_test(NAME vr_pistol_state COMMAND vr_pistol_tests)

add_executable(vr_pickup_tests pickup_tests.cpp)
target_compile_features(vr_pickup_tests PRIVATE cxx_std_17)
add_test(NAME vr_pickup_policy COMMAND vr_pickup_tests)

add_executable(vr_body_tests body_tests.cpp)
target_compile_features(vr_body_tests PRIVATE cxx_std_17)
add_test(NAME vr_body_slots_crouch COMMAND vr_body_tests)

add_executable(vr_mechanism_tests mechanism_tests.cpp)
target_compile_features(vr_mechanism_tests PRIVATE cxx_std_17)
add_test(NAME vr_mechanism_parts COMMAND vr_mechanism_tests)

add_executable(vr_save_tests save_tests.cpp)
target_compile_features(vr_save_tests PRIVATE cxx_std_17)
add_test(NAME vr_save_ledger COMMAND vr_save_tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME vr_weapon_profiles COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/weapon_profile_tests.py)
endif()

add_executable(vr_blood_tests blood_tests.cpp)
target_compile_features(vr_blood_tests PRIVATE cxx_std_17)
add_test(NAME vr_blood COMMAND vr_blood_tests)

add_executable(vr_contact_tests contact_tests.cpp)
target_compile_features(vr_contact_tests PRIVATE cxx_std_17)
add_test(NAME vr_contact COMMAND vr_contact_tests)

# Not a test: prints timings. Run the Release build.
add_executable(vr_contact_bench contact_bench.cpp)
target_compile_features(vr_contact_bench PRIVATE cxx_std_17)

# Synthetic pose preparation only; not a headset/frame-time measurement.
add_executable(vr_pose_bench pose_bench.cpp)
target_compile_features(vr_pose_bench PRIVATE cxx_std_17)

add_executable(vr_body_pose_tests body_pose_tests.cpp)
target_compile_features(vr_body_pose_tests PRIVATE cxx_std_17)
add_test(NAME vr_body_pose COMMAND vr_body_pose_tests)

add_executable(vr_body_tracking_tests body_tracking_tests.cpp)
target_compile_features(vr_body_tracking_tests PRIVATE cxx_std_17)
add_test(NAME vr_body_tracking COMMAND vr_body_tracking_tests)

add_executable(vr_mechanics_kind_tests mechanics_tests.cpp)
target_compile_features(vr_mechanics_kind_tests PRIVATE cxx_std_17)
add_test(NAME vr_weapon_mechanics COMMAND vr_mechanics_kind_tests)

add_executable(vr_performance_tests performance_tests.cpp
    ../../src/xrEngine/Vr/vr_performance.cpp)
target_compile_features(vr_performance_tests PRIVATE cxx_std_17)
target_compile_definitions(vr_performance_tests PRIVATE VR_ENABLED XR_VR_TEST XR_VR_PERF_TEST NOMINMAX)
target_include_directories(vr_performance_tests PRIVATE ../../sdk/include)
target_link_libraries(vr_performance_tests PRIVATE d3d11 dxgi)
# The capture test writes a real collector dump that the report test parses.
add_test(NAME vr_performance_capture COMMAND vr_performance_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(vr_performance_capture PROPERTIES FIXTURES_SETUP vr_perf_capture_log)
if(Python3_Interpreter_FOUND)
    add_test(NAME vr_performance_report COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/performance_report_tests.py)
    set_tests_properties(vr_performance_report PROPERTIES
        FIXTURES_REQUIRED vr_perf_capture_log
        ENVIRONMENT "VR_PERF_CAPTURE_LOG=${CMAKE_CURRENT_BINARY_DIR}/performance-test-capture.log")
endif()
