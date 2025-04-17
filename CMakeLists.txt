cmake_minimum_required(VERSION 3.10)

# --- make sure to be a good programmer ---
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

# --- src files ---
project(ddnet_frametee)
set(SRCS 
	src/main.c
	src/graphics_backend.c
	src/user_interface.c
)

# --- find deps ---
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

set(IMGUI_SOURCES 
	libs/cimgui/imgui/imgui.cpp
	libs/cimgui/imgui/imgui_draw.cpp
	libs/cimgui/imgui/imgui_demo.cpp
	libs/cimgui/imgui/imgui_widgets.cpp
	libs/cimgui/imgui/backends/imgui_impl_vulkan.cpp
	libs/cimgui/imgui/backends/imgui_impl_glfw.cpp
)
add_executable(${PROJECT_NAME} ${SRCS} ${IMGUI_SOURCES})

# --- cimgui stuff ---
add_subdirectory(libs/cimgui)
target_compile_definitions(${PROJECT_NAME} PRIVATE 
	CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
	CIMGUI_USE_GLFW=1
	CIMGUI_USE_VULKAN=1
)

# --- linking ---
target_link_libraries(${PROJECT_NAME} PRIVATE
	cimgui
	glfw
	Vulkan::Vulkan
)

# --- compile options ---
target_compile_options(${PROJECT_NAME} PRIVATE -Wno-typedef-redefinition)
