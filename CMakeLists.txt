# Modified by Shinyflvres, 2026-08-23. Part of SpaceSync, a modified version of OpenVR-SpaceOverride by Nyabsi (AGPL-3.0). See NOTICE.md
# Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md
cmake_minimum_required(VERSION 3.20)

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

project(SpaceSync)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The Vulkan validation layer adds extra reporting that also surfaces validation    
# issues originating from external sources (e.g. SteamVR). Defaults on.
option(ENABLE_VULKAN_VALIDATION "Enable Vulkan validation layers" OFF)

set(OpenVR_ROOT ${CMAKE_SOURCE_DIR}/3rdparty/OpenVR)

find_package(OpenVR REQUIRED)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Include rest of the CMake bindings
add_subdirectory(3rdparty)

message(STATUS "ENABLE_VULKAN_VALIDATION = ${ENABLE_VULKAN_VALIDATION}")

add_executable(SpaceSync
    "src/overlay/Main.cpp"
    "src/overlay/VulkanRenderer.cpp"
    "src/overlay/ImGuiWindow.cpp"
    "src/overlay/Calibration.cpp"
    "src/overlay/Configuration.cpp"
    $<IF:$<PLATFORM_ID:Windows>,src/overlay/IPCClient.cpp,src/overlay/IPCClientPosix.cpp>
    "src/overlay/UserInterface.cpp"
    "src/overlay/Theme.cpp"
    "src/overlay/TrackingPreview.cpp"
    $<IF:$<PLATFORM_ID:Windows>,src/overlay/Lighthouse.cpp,src/overlay/LighthousePosix.cpp>
    "src/overlay/Localization.cpp"
    "src/overlay/Sound.cpp"
    "src/overlay/EmbeddedFiles.cpp"
)

target_compile_features(SpaceSync PUBLIC cxx_std_20)

target_include_directories(SpaceSync
    PRIVATE
        "${CMAKE_SOURCE_DIR}/include/overlay"
        "${CMAKE_SOURCE_DIR}/include/shared"
)

target_compile_definitions(SpaceSync
    PRIVATE
        _HAS_DEPRECATED_NEGATORS=1
        _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
)

if (ENABLE_VULKAN_VALIDATION)
    target_compile_definitions(SpaceSync PRIVATE ENABLE_VULKAN_VALIDATION)
endif()

if (MSVC)
    target_compile_options(SpaceSync PRIVATE /bigobj)
endif()

# Windows executable icon
if (WIN32)
    target_sources(SpaceSync PRIVATE "resources/winicon.rc")
endif()


target_link_libraries(SpaceSync
    PRIVATE
        OpenVR::API
        ImGui
        Eigen
        PicoJSON
)

if (WIN32)
    target_link_libraries(SpaceSync PRIVATE winmm windowsapp)
else()
    # Find libopenvr_api.so next to the binary, like openvr_api.dll on Windows.
    set_target_properties(SpaceSync PROPERTIES INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH TRUE)
    target_link_options(SpaceSync PRIVATE -static-libstdc++ -static-libgcc)
endif()

if (WIN32)
    set_target_properties(SpaceSync PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(SpaceSync PRIVATE "/ENTRY:mainCRTStartup")
    target_compile_options(SpaceSync PRIVATE "/utf-8")
endif()

add_library(driver_spacesync SHARED
    "src/driver/Main.cpp"
    "src/driver/ServerTrackedDeviceProvider.cpp"
    "src/driver/IPCServer.cpp"
    $<$<NOT:$<PLATFORM_ID:Windows>>:src/driver/IPCServerPosix.cpp>
    "src/driver/InterfaceHookInjector.cpp"
    "src/driver/Hooking.cpp"
    "src/driver/Logging.cpp"
    "src/driver/KalmanFilter.cpp"
)

target_compile_features(driver_spacesync PRIVATE cxx_std_20)

target_include_directories(driver_spacesync PRIVATE
    "${CMAKE_SOURCE_DIR}/include/driver"
    "${CMAKE_SOURCE_DIR}/include/shared"
)

target_link_libraries(driver_spacesync PRIVATE
    OpenVR::Driver
)

if (WIN32)
    target_link_libraries(driver_spacesync PRIVATE MinHook)
else()
    # vrserver loads the driver with RTLD_LOCAL, but keep our symbols out of
    # the global namespace anyway so nothing collides with other drivers.
    set_target_properties(driver_spacesync PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # SteamVR runs inside the Steam runtime, whose libstdc++ can be older than
    # the build host's; carry our own so the driver loads regardless.
    target_link_options(driver_spacesync PRIVATE "-Wl,--no-undefined" -static-libstdc++ -static-libgcc)
endif()

set_property(TARGET driver_spacesync PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
set_target_properties(driver_spacesync PROPERTIES PREFIX "")