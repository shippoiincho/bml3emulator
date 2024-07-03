add_executable(bml3emulator)

# by default the header is generated into the build dir
#pico_generate_pio_header(bml3emulator ${CMAKE_CURRENT_LIST_DIR}/vgaout.pio)
pico_generate_pio_header(bml3emulator ${CMAKE_CURRENT_LIST_DIR}/hsync.pio)
pico_generate_pio_header(bml3emulator ${CMAKE_CURRENT_LIST_DIR}/vsync.pio)
pico_generate_pio_header(bml3emulator ${CMAKE_CURRENT_LIST_DIR}/rgb.pio)
# however, alternatively you can choose to generate it somewhere else (in this case in the source tree for check in)
#pico_generate_pio_header(pio_blink ${CMAKE_CURRENT_LIST_DIR}/blink.pio OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR})

# Make sure TinyUSB can find tusb_config.h
target_include_directories(bml3emulator PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_sources(bml3emulator PRIVATE bml3emulator.c mc6809.c hid_app.c vga16_graphics.c lfs.c lfs_util.c)

pico_enable_stdio_usb(bml3emulator 0)
pico_enable_stdio_uart(bml3emulator 1)

target_link_libraries( bml3emulator PRIVATE pico_stdlib hardware_pio hardware_timer hardware_dma hardware_uart hardware_flash pico_multicore tinyusb_host tinyusb_board)
pico_add_extra_outputs(bml3emulator)
