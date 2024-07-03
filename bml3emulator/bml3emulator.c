//  Basic Master Level3 emulator
//
//  GP0: HSYNC
//  GP1: VSYNC
//  GP2: Blue
//  GP3: Red
//  GP4: Green
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/dma.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "tusb.h"
#include "bsp/board.h"

#include "vga16_graphics.h"
#include "bml3font.h"
#include "bml3rom.h"
#include "bml3keymap.h"
#include "mc6809.h"

#include "lfs.h"

// TEST TEST
#include "bml3test.h"

// VGAout configuration

#define DOTCLOCK 25000
#define CLOCKMUL 8
// Pico does not work at CLOCKMUL=7 (175MHz).

#define VGA_PIXELS_X 640
#define VGA_PIXELS_Y 400

#define VGA_CHARS_X 80
#define VGA_CHARS_Y 25

#define VRAM_PAGE_SIZE (VGA_PIXELS_X*VGA_PIXELS_Y/8)

extern unsigned char vga_data_array[];
volatile uint8_t fbcolor,cursor_x,cursor_y,video_mode;

volatile uint video_hsync,video_vsync,scanline;
volatile uint redraw_command=0;
struct repeating_timer timer;

// MC6809 configuration

mc6809__t      cpu;
uint8_t mainram[0x10000];
uint8_t subram[0x8000];
uint8_t colorram[0x4000];
uint8_t igram[0x1800];

// Memory Mapped IO

uint8_t ioport[0x100];
uint8_t crtc[16];
uint8_t crtc_mode;
uint igenable=0;

uint8_t keydata[0x80];

//volatile uint8_t keycode=0;
uint8_t keytimer=0;
uint keycheck_count=0;
uint8_t blink=0;

// UART

#define TAPE_THRESHOLD 200000

uint8_t uart_rx[32];
uint8_t uart_nibble=0;
uint8_t uart_count=0;
volatile uint8_t uart_write_ptr=0;
volatile uint8_t uart_read_ptr=0;
uint uart_cycle;

// UI

uint menumode=0;
uint menuitem=0;
uint8_t keypressed=0;  // pressed key in menumode
hid_keyboard_report_t prev_report = { 0, 0, {0} }; // previous report to check key released

// USB

extern void hid_app_task(void);
unsigned long hsync_cycle=0;
unsigned int usbcheck_count=0;
unsigned int kbhit=0;            // 4:Key pressed (timer stop)/3&2:Key depressed (timer running)/1:no key irq triggerd
uint8_t hid_dev_addr=255;
uint8_t hid_instance=255;
uint8_t hid_led;

//#define KEY_CHECK_INTERVAL 2000  // may be 1msec
//#define KEY_CHECK_INTERVAL 63  // may be 31.5us
#define HSYNC_INTERVAL 127  // may be 63.5us 
#define USB_CHECK_INTERVAL 30 // 31.5us*30=1ms

// Define the flash sizes
// This is setup to read a block of the flash from the end 
#define BLOCK_SIZE_BYTES (FLASH_SECTOR_SIZE)
#define HW_FLASH_STORAGE_BYTES  (512 * 1024)
//#define HW_FLASH_STORAGE_BASE   (PICO_FLASH_SIZE_BYTES - HW_FLASH_STORAGE_BYTES) // 655360
// for 1M flash pico
#define HW_FLASH_STORAGE_BASE   (1024*1024 - HW_FLASH_STORAGE_BYTES) // 655360

lfs_t lfs;
lfs_file_t lfs_file;

#define FILE_THREHSOLD 2000000
#define LFS_LS_FILES 9

uint load_enabled=0;
uint save_enabled=0;
uint file_cycle=0;

unsigned char filename[16];

static void draw_framebuffer(uint16_t);
static inline unsigned char tohex(int);
static inline unsigned char fromhex(int);
static inline void video_print(uint8_t *);

// Virtual H-Sync for emulation
bool __not_in_flash_func(hsync_handler)(struct repeating_timer *t) {

    if(scanline==0) {
        video_vsync=1;
    }

    scanline++;

//    sem_acquire_blocking(&hsynclock);
    if((scanline%2)==0) {
        video_hsync=1;
    }

    if(scanline>262) {
        scanline=0;
    }

//    sem_release(&hsynclock);
//    video_hsync=0;

    return true;

}

void __not_in_flash_func(uart_handler)(void) {

    uint8_t ch;

    if((cpu.cycles-uart_cycle)>TAPE_THRESHOLD) {
        uart_count=0;        
    }

    uart_cycle=cpu.cycles;

    if(uart_is_readable(uart0)) {
        ch=uart_getc(uart0);
        if(uart_count==0) {
            uart_nibble=fromhex(ch)<<4;
            uart_count++;
        } else {
            ch=fromhex(ch)+uart_nibble;
            uart_count=0;

            if(uart_read_ptr==uart_write_ptr+1) {  // buffer full
                return;
            }
            if((uart_read_ptr==0)&&(uart_write_ptr==31)) {
                return;
            }

        // unsigned char str[30];
        // sprintf(str,"%02d/%02d:%02x",uart_read_ptr,uart_write_ptr,ch);
            
        // cursor_x=0;
        // cursor_y=24;
        // fbcolor=7;
        // video_print(str);

            uart_rx[uart_write_ptr]=ch;
            uart_write_ptr++;
            if(uart_write_ptr>31) {
                uart_write_ptr=0;
            }
        }
    }

}

uint8_t acia_getc() {

    uint8_t mode,ch;

    mode=ioport[0xd0];

    if(((mode&0x20)==0)&&(load_enabled!=0)) { // Get data from LFS file

        lfs_file_read(&lfs,&lfs_file,&ch,1);

        // unsigned char str[30];
        // sprintf(str,"F:%02x",ch);
            
        // cursor_x=40;
        // cursor_y=24;
        // fbcolor=7;
        // video_print(str);

        load_enabled=2; 
        file_cycle=cpu.cycles;

        return ch;

    } else {    // Get data from UART

        if(uart_read_ptr==uart_write_ptr) {
            return 0;    // Empty
        }

        ch=uart_rx[uart_read_ptr];

        // unsigned char str[30];
        // sprintf(str,"%02d/%02d:%02x",uart_read_ptr,uart_write_ptr,ch);
            
        // cursor_x=40;
        // cursor_y=24;
        // fbcolor=7;
        // video_print(str);

        uart_read_ptr++;
        if(uart_read_ptr>31) {
            uart_read_ptr=0;
        }
        return ch;
    }

}

uint8_t acia_rx_available() {

    uint8_t mode,ch;


    mode=ioport[0xd0];

    if(((mode&0x20)==0)&&(load_enabled!=0)) { // Get data from LFS file

        return 1;

    } else {    // Get data from UART

        if(uart_read_ptr==uart_write_ptr) {
            return 0;    // Empty
        }

        return 1;
    }
}

void acia_write(uint8_t b) {

    uint8_t mode;

    mode=ioport[0xd0];

    if(((mode&0x20)==0)&&(save_enabled!=0)) {

            // unsigned char str[16];
            //  sprintf(str,"%F:02x",b);
            //  cursor_x=70;
            //  cursor_y=23;
            //  fbcolor=7;
            //  video_print(str);

        lfs_file_write(&lfs,&lfs_file,&b,1);
        save_enabled=2;
        file_cycle=cpu.cycles;

    } else {

            // sprintf(str,"%02x",b);
            // cursor_x=70;
            // cursor_y=23;
            // fbcolor=7;
            // video_print(str);

                uart_putc(uart0,tohex(b>>4));
                uart_putc(uart0,tohex(b&0xf));

///                printf("%02x",b);
    }

}

void main_core1(void) {

    uint redraw_start,redraw_length;

    multicore_lockout_victim_init();

    scanline=0;

    // set virtual Hsync timer

    add_repeating_timer_us(63,hsync_handler,NULL  ,&timer);

    while(1) {

        if(redraw_command!=0) {
            redraw_start=redraw_command>>16;
            redraw_length=redraw_command&0xffff;
            redraw_command=0;
            for(int i=0;i<redraw_length;i++) {
                draw_framebuffer(i+redraw_start);
                if(redraw_command!=0) break;
            }
        }
    }
}

static inline void video_cls() {
    memset(vga_data_array, 0x0, (640*400/2));
}

static inline void video_scroll() {

    memmove(vga_data_array, vga_data_array + VGA_PIXELS_X/2*16, (640*384/2));
    memset(vga_data_array + (640*384/2), 0, VGA_PIXELS_X/2*16);

}

static inline void video_print(uint8_t *string) {

    int len;
    uint8_t fdata;

    len = strlen(string);

    for (int i = 0; i < len; i++) {

        for(int slice=0;slice<16;slice++) {

            uint8_t ch=string[i];

            if(ch<0x80) {
                fdata=bml3font[(ch*16+(slice>>1)*2)];
            } else {
                if(video_mode==0) {
                    fdata=bml3font[((ch&0x7f)*16+(slice>>1)*2+1)];
                } else {
                    fdata=bml3font[(ch*16+slice)];
                }
            }

            uint vramindex=cursor_x*4+VGA_PIXELS_X*(cursor_y*16+slice)/2;

            for(int slice_x=0;slice_x<4;slice_x++){

                if(fdata&0x40) {
                    vga_data_array[vramindex+slice_x]=(fbcolor&7)<<4;
                } else {
                    vga_data_array[vramindex+slice_x]=fbcolor&0x70;  
                }

                  if(fdata&0x80) {
                    vga_data_array[vramindex+slice_x]+=fbcolor&7;
                } else {
                    vga_data_array[vramindex+slice_x]+=(fbcolor&0x70)>>4;  
                }              

                fdata<<=2;

            }

        }

        cursor_x++;
        if (cursor_x >= VGA_CHARS_X) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= VGA_CHARS_Y) {
                video_scroll();
                cursor_y = VGA_CHARS_Y - 1;
            }
        }
    }

}

void draw_menu(void) {

    cursor_x=10;
    cursor_y=5;
    fbcolor=7;
    video_print("+-------------------------------------+");
    for(int i=6;i<16;i++) {
        cursor_x=10;
        cursor_y=i;
        video_print("|                                     |");
    }

    cursor_x=10;
    cursor_y=16;
    fbcolor=7;
    video_print("+-------------------------------------+");

}

int draw_files(int num_selected,int page) {

    lfs_dir_t lfs_dirs;
    struct lfs_info lfs_dir_info;
    uint num_entry=0;
    unsigned char str[16];

    int err= lfs_dir_open(&lfs,&lfs_dirs,"/");

    if(err) return -1;

    for(int i=0;i<LFS_LS_FILES;i++) {
        cursor_x=32;
        cursor_y=i+6;
        fbcolor=7;
        video_print("          ");
    }

    while(1) {

        int res= lfs_dir_read(&lfs,&lfs_dirs,&lfs_dir_info);
        if(res<=0) {
            break;
        }

        cursor_x=36;
        cursor_y=15;
        fbcolor=7;
        sprintf(str,"Page %02d",page+1);

        video_print(str);

        switch(lfs_dir_info.type) {

            case LFS_TYPE_DIR:
                break;
            
            case LFS_TYPE_REG:

                if((num_entry>=LFS_LS_FILES*page)&&(num_entry<LFS_LS_FILES*(page+1))) {

                    cursor_x=32;
                    cursor_y=num_entry%LFS_LS_FILES+6;

                    if(num_entry==num_selected) {
                        fbcolor=0x70;
                        memcpy(filename,lfs_dir_info.name,16);
                    } else {
                        fbcolor=7;
                    }

                    video_print(lfs_dir_info.name);

                }

                num_entry++;

                break;

            default:
                break; 

        }

    }

    lfs_dir_close(&lfs,&lfs_dirs);

    return num_entry;

}

int file_selector(void) {

    uint num_selected=0;
    uint num_files=0;
    uint num_pages=0;

    num_files=draw_files(-1,0);

    if(num_files==0) {
         return -1;
    }

    while(1) {

        while(video_vsync==0) ;
        video_vsync=0;

        draw_files(num_selected,num_selected/LFS_LS_FILES);

        tuh_task();

        if(keypressed==0x52) { // up
            keypressed=0;
            if(num_selected>0) {
                num_selected--;
            }
        }

        if(keypressed==0x51) { // down
            keypressed=0;
            if(num_selected<num_files-1) {
                num_selected++;
            }
        }

        if(keypressed==0x28) { // Ret
            keypressed=0;

            return 0;
        }

        if(keypressed==0x29 ) {  // ESC

            return -1;

        }

    }
}

int enter_filename() {

    unsigned char new_filename[16];
    unsigned char str[32];
    uint8_t keycode;
    uint pos=0;

    memset(new_filename,0,16);

    while(1) {

        sprintf(str,"Filename:%s  ",new_filename);
        cursor_x=12;
        cursor_y=15;
        video_print(str);

        while(video_vsync==0) ;
        video_vsync=0;

        tuh_task();

        if(keypressed!=0) {

            if(keypressed==0x28) { // enter
                keypressed=0;
                if(pos!=0) {
                    memcpy(filename,new_filename,16);
                    return 0;
                } else {
                    return -1;
                }
            }

            if(keypressed==0x29) { // escape
                keypressed=0;
                return -1;
            }

            if(keypressed==0x2a) { // backspace
                keypressed=0;

                cursor_x=12;
                cursor_y=15;
                video_print("Filename:          ");

                new_filename[pos]=0;

                if(pos>0) {
                    pos--;
                }
            }

            if(keypressed<0x4f) {
                keycode=usbhidcode[keypressed*2];
                keypressed=0;

                if(pos<7) {

                    if((keycode>0x20)&&(keycode<0x5f)&&(keycode!=0x2f)) {

                        new_filename[pos]=keycode;
                        pos++;

                    }

                }
            }


        }
    }




}


static void draw_framebuffer(uint16_t addr){

    uint pos_x,pos_y,slice_y;
    uint baseaddr,offset;

    uint8_t fdat,fdat2;

    uint mode=ioport[0xd0];
    uint color;
    uint vramindex;
    uint igblue,igred,iggreen,igcolor;
    uint igbluemask,igredmask,iggreenmask;

    uint colormask,bgmask;
    uint32_t *vramptr=(uint32_t *)vga_data_array;

    baseaddr=crtc[12]*256+crtc[13];

    if(baseaddr<0x400) return;

    if(addr<baseaddr) return;   // out of screen

    offset=addr-baseaddr;

    if(mode&0x80) {         // 80 chars mode

        slice_y=offset/0x800;
        pos_x=(offset-slice_y*0x800)%80;
        pos_y=(offset-slice_y*0x800)/80;

    } else {                // 40 chars mode
        slice_y=offset/0x400;
        pos_x=(offset-slice_y*0x400)%40;
        pos_y=(offset-slice_y*0x400)/40;
    }

    if(pos_y>24) {
        return;
    }

    if((mode&0x40)&&(slice_y>0)) {
        return;
    }

    if(slice_y>7) return;

//    color=colorram[offset];
    color=colorram[addr-0x400];

    if(igenable==0) {
        color &= 0xdf;
    }

    if(color&0x20) {  // IG Enabled

       if(mode&0x80) {  // 80 Chars
            if(mode&0x40) { // Normal Mode
                uint ch=mainram[addr];
                for(int slice_yy=0;slice_yy<16;slice_yy++) {

                    vramindex=pos_x*4+(pos_y*16+slice_yy)*VGA_PIXELS_X/2;

                    if(ioport[0xe9]&1) {
                        igblue=0xff;
                        igred=0xff;
                        iggreen=0xff;
                    } else {
                        igblue= igram[ch*8+(slice_yy>>1)];
                        igred=  igram[ch*8+(slice_yy>>1)+0x800];
                        iggreen=igram[ch*8+(slice_yy>>1)+0x1000];
                    }

                    for(int slice_x=0;slice_x<4;slice_x++) {

                        igcolor=0;
                        if(igblue&0x40) igcolor|=0x10;
                        if(igblue&0x80) igcolor|=0x1;
                        if(igred&0x40) igcolor|=0x20;
                        if(igred&0x80) igcolor|=0x2;
                        if(iggreen&0x40) igcolor|=0x40;
                        if(iggreen&0x80) igcolor|=0x4;

                        if((igcolor&0x70)==0) {
                            igcolor |= (mode&7)<<4;
                            
                        }
                        if((igcolor&7)==0) {
                            igcolor |= (mode&7);
                        }

                        vga_data_array[vramindex+slice_x]=igcolor;

                        igblue<<=2;
                        igred<<=2;
                        iggreen<<=2;
                            
                    }
                }                                                
            } else { // Hi-reso

                    uint ch=mainram[addr];

                    if(ioport[0xe9]&1) {
                        igblue=0xff;
                        igred=0xff;
                        iggreen=0xff;
                    } else {
                        igblue= igram[ch*8+(slice_y)];
                        igred=  igram[ch*8+(slice_y)+0x800];
                        iggreen=igram[ch*8+(slice_y)+0x1000];
                    }


                    vramindex=pos_x*4+(pos_y*16+slice_y*2)*VGA_PIXELS_X/2;

                    for(int slice_x=0;slice_x<4;slice_x++) {

                        igcolor=0;
                        if(igblue&0x40) igcolor|=0x10;
                        if(igblue&0x80) igcolor|=0x1;
                        if(igred&0x40) igcolor|=0x20;
                        if(igred&0x80) igcolor|=0x2;
                        if(iggreen&0x40) igcolor|=0x40;
                        if(iggreen&0x80) igcolor|=0x4;

                        if((igcolor&0x70)==0) {
                            igcolor |= (mode&7)<<4;            
                        }

                        if((igcolor&7)==0) {
                            igcolor |= (mode&7);
                        }

                        vga_data_array[vramindex+slice_x]=igcolor;
                        vga_data_array[vramindex+slice_x+VGA_PIXELS_X/2]=igcolor;

                        igblue<<=2;
                        igred<<=2;
                        iggreen<<=2;
                                                                                
                    }
            }
        } else { // 40 chars
            if(mode&0x40) { // Normal Mode

                    uint ch=mainram[addr];
                    for(int slice_yy=0;slice_yy<16;slice_yy++) {

                        vramindex=pos_x*8+(pos_y*16+slice_yy)*VGA_PIXELS_X/2;

                        if(ioport[0xe9]&1) {
                            igblue=0xff;
                            igred=0xff;
                            iggreen=0xff;
                        } else {
                            igblue= igram[ch*8+(slice_yy>>1)];
                            igred=  igram[ch*8+(slice_yy>>1)+0x800];
                            iggreen=igram[ch*8+(slice_yy>>1)+0x1000];
                        }

                        for(int slice_x=0;slice_x<8;slice_x++) {

                            igcolor=0;

                            if(igblue&0x80) igcolor|=0x11;
                            if(igred&0x80) igcolor|=0x22;
                            if(iggreen&0x80) igcolor|=0x44;

                            if(igcolor==0) {
                                igcolor = (mode&7)*0x11;
                            
                            }

                            vga_data_array[vramindex+slice_x]=igcolor;

                            igblue<<=1;
                            igred<<=1;
                            iggreen<<=1;
                            
                        }                                                
                    }

                } else {  // Hi-reso
                    uint ch=mainram[addr];

                    if(ioport[0xe9]&1) {
                        igblue=0xff;
                        igred=0xff;
                        iggreen=0xff;
                    } else {
                        igblue= igram[ch*8+(slice_y)];
                        igred=  igram[ch*8+(slice_y)+0x800];
                        iggreen=igram[ch*8+(slice_y)+0x1000];
                    }


                    vramindex=pos_x*8+(pos_y*16+slice_y*2)*VGA_PIXELS_X/2;

                    for(int slice_x=0;slice_x<8;slice_x++) {

                        igcolor=0;
 
                        if(igblue&0x80) igcolor|=0x11;
                        if(igred&0x80) igcolor|=0x22;
                        if(iggreen&0x80) igcolor|=0x44;

                        if(igcolor==0) {
                            igcolor = (mode&7)*0x11;            
                        }

                        vga_data_array[vramindex+slice_x]=igcolor;
                        vga_data_array[vramindex+slice_x+VGA_PIXELS_X/2]=igcolor;

                        igblue<<=1;
                        igred<<=1;
                        iggreen<<=1;
                                                                                
                    }

                }

        }


    } else {

    if(color&0x10) {
            // Graphic mode
        if(mode&0x80) {  // 80 Chars

            if((mode&0x40)==0) {  // Hi-reso graphics 
                if(color&8) {
                    fdat=~mainram[addr];
                } else {
                    fdat=mainram[addr];
                }

                vramindex=pos_x+ pos_y*2*VGA_PIXELS_X+ slice_y*VGA_PIXELS_X/4;


                colormask=bitexpand80[fdat*2];
                bgmask=bitexpand80[fdat*2+1];

                colormask&=(color&7)*0x11111111;
                bgmask&=(mode&7)*0x11111111;

                *(vramptr+vramindex) = colormask | bgmask;
                *(vramptr+vramindex+VGA_PIXELS_X/8) = (colormask|bgmask);

            } else {
            // Semi-Graphic

                for(int slice_yy=0;slice_yy<16;slice_yy++) {
                    uint ch=mainram[addr];
                    
                    fdat=bml3semifont[ch*8+(slice_yy>>1)];
                        
                    if(color&8) {
                        fdat=~fdat;
                    }

                    vramindex=pos_x+ pos_y*2*VGA_PIXELS_X+ slice_yy*VGA_PIXELS_X/8;

                    colormask=bitexpand80[fdat*2];
                    bgmask=bitexpand80[fdat*2+1];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex) = colormask | bgmask;

                }

            }

        } else {  // 40 Chars

            if((mode&0x40)==0) {  // Hi-reso graphics 
                if(color&8) {
                    fdat=~mainram[addr];
                } else {
                    fdat=mainram[addr];
                }

                    vramindex=pos_x*2+pos_y*2*VGA_PIXELS_X+slice_y*VGA_PIXELS_X/4;
            

                    colormask=bitexpand40[fdat*4];
                    bgmask=bitexpand40[fdat*4+2];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex+1) = colormask | bgmask;
                    *(vramptr+vramindex+VGA_PIXELS_X/8+1) = (colormask|bgmask);

                    colormask=bitexpand40[fdat*4+1];
                    bgmask=bitexpand40[fdat*4+3];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;
                    *(vramptr+vramindex) = colormask | bgmask;
                    *(vramptr+vramindex+VGA_PIXELS_X/8) = (colormask|bgmask);

            } else {
            // Semi-Graphic

                for(int slice_yy=0;slice_yy<16;slice_yy++) {
                    uint ch=mainram[addr];
                    
                    fdat=bml3semifont[ch*8+(slice_yy>>1)];
                        
                    if(color&8) {
                        fdat=~fdat;
                    }

                    vramindex=pos_x*2+pos_y*2*VGA_PIXELS_X+slice_yy*VGA_PIXELS_X/8;

                    colormask=bitexpand40[fdat*4];
                    bgmask=bitexpand40[fdat*4+2];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex+1) = colormask | bgmask;

                    colormask=bitexpand40[fdat*4+1];
                    bgmask=bitexpand40[fdat*4+3];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;
                    *(vramptr+vramindex) = colormask | bgmask;

                }

            }
        }

    } else {
            // Charactor mode
        if(mode&0x80) {  // 80 Chars

            if(mode&0x40) { // Normal Mode

                    uint ch=mainram[addr];

                    for(int slice_yy=0;slice_yy<16;slice_yy++) {

                        if(ch<0x80) {
                            fdat=bml3font[(ch*16+(slice_yy>>1)*2)];
                        } else {
                            if(ioport[0xd6]&0x8) {
                                fdat=bml3font[(ch*16+slice_yy)];
                            } else {
                                fdat=bml3font[((ch&0x7f)*16+(slice_yy>>1)*2+1)];
                            }
                        }

                        if(color&8) {
                            fdat=~fdat;
                        }

                        vramindex=pos_x+ pos_y*2*VGA_PIXELS_X+ slice_yy*VGA_PIXELS_X/8;

                        colormask=bitexpand80[fdat*2];
                        bgmask=bitexpand80[fdat*2+1];

                        colormask&=(color&7)*0x11111111;
                        bgmask&=(mode&7)*0x11111111;

                        *(vramptr+vramindex) = colormask | bgmask;

                    
                }

            } else { // Hi-reso mode


                    uint ch=mainram[addr];

                    if(ch<0x80) {
                        fdat=bml3font[(ch*16+slice_y*2)];
                        fdat2=fdat;
                    } else {
                        if(ioport[0xd6]&0x8) {
                            fdat=bml3font[(ch*16+slice_y*2)];
                            fdat2=bml3font[(ch*16+slice_y*2+1)];
                        } else {
                            fdat=bml3font[((ch&0x7f)*16+slice_y*2+1)];
                            fdat2=fdat;
                        }
                    }


                    if(color&8) {
                        fdat=~fdat;
                        fdat2=~fdat2;
                    }

                    vramindex=pos_x+ pos_y*2*VGA_PIXELS_X+ slice_y*VGA_PIXELS_X/4;

                    colormask=bitexpand80[fdat*2];
                    bgmask=bitexpand80[fdat*2+1];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex) = colormask | bgmask;

                    colormask=bitexpand80[fdat2*2];
                    bgmask=bitexpand80[fdat2*2+1];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex+VGA_PIXELS_X/8) = (colormask|bgmask);

                
            }

        } else {  // 40 Chars

            if(mode&0x40) { // Normal Mode

                uint ch=mainram[addr];
                for(int slice_yy=0;slice_yy<16;slice_yy++) {

                    if(ch<0x80) {
                        fdat=bml3font[(ch*16+(slice_yy>>1)*2)];
                    } else {
                        if(ioport[0xd6]&0x8) {
                            fdat=bml3font[(ch*16+slice_yy)];
                        } else {
                            fdat=bml3font[((ch&0x7f)*16+(slice_yy>>1)*2+1)];
                        }
                    }

                    if(color&8) {
                        fdat=~fdat;
                    }

                    vramindex=pos_x*2+pos_y*2*VGA_PIXELS_X+slice_yy*VGA_PIXELS_X/8;

                    colormask=bitexpand40[fdat*4];
                    bgmask=bitexpand40[fdat*4+2];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex+1) = colormask | bgmask;

                    colormask=bitexpand40[fdat*4+1];
                    bgmask=bitexpand40[fdat*4+3];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;
                    *(vramptr+vramindex) = colormask | bgmask;

                
                }

            } else { // Hi-reso mode

                    uint ch=mainram[addr];

                    if(ch<0x80) {
                        fdat=bml3font[(ch*16+slice_y*2)];
                        fdat2=fdat;
                    } else {
                        if(ioport[0xd6]&0x8) {
                            fdat=bml3font[(ch*16+slice_y*2)];
                            fdat2=bml3font[(ch*16+slice_y*2+1)];
                        } else {
                            fdat=bml3font[((ch&0x7f)*16+slice_y*2+1)];
                            fdat2=fdat;
                        }
                    }


                    if(color&8) {
                        fdat=~fdat;
                        fdat2=~fdat2;
                    }

                    vramindex=pos_x*2+pos_y*2*VGA_PIXELS_X+slice_y*VGA_PIXELS_X/4;

                    colormask=bitexpand40[fdat*4];
                    bgmask=bitexpand40[fdat*4+2];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex+1) = colormask | bgmask;

                    colormask=bitexpand40[fdat*4+1];
                    bgmask=bitexpand40[fdat*4+3];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;
                    *(vramptr+vramindex) = colormask | bgmask;

                    colormask=bitexpand40[fdat2*4];
                    bgmask=bitexpand40[fdat2*4+2];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex+VGA_PIXELS_X/8+1) = (colormask|bgmask);

                    colormask=bitexpand40[fdat2*4+1];
                    bgmask=bitexpand40[fdat2*4+3];

                    colormask&=(color&7)*0x11111111;
                    bgmask&=(mode&7)*0x11111111;

                    *(vramptr+vramindex+VGA_PIXELS_X/8) = (colormask|bgmask);

                

            }


        }

    }
    } 
    
}

static void redraw(){

    uint baseaddr;
    uint totalbytes;
    uint mode;

    baseaddr=crtc[12]*256+crtc[13];

    // check VRAM range
    if(baseaddr<0x400) return;
    if(baseaddr>0x4400) return;

    mode=ioport[0xd0]&0xc0;

    if(mode==0) {   //  40 Hireso
        totalbytes=0x2000;
    } else if (mode==0x40) {  // 40 Normal
        totalbytes=0x400;
    } else if (mode==0x80) {  // 80 Hireso
        totalbytes=0x4000;
    } else if (mode==0xc0) { // 80 Normal
        totalbytes=0x800;
    }

//    for(uint16_t i=0;i<totalbytes;i++) {
//        draw_framebuffer(i+baseaddr);
//    }

    redraw_command=(baseaddr<<16) | totalbytes;

}

void show_cursor(void) {

    uint baseaddr,cursoraddr,offset;
    uint vramindex;
    uint mode;
    uint pos_x,pos_y,slice_y,color,start_y,end_y;

    baseaddr=crtc[12]*256+crtc[13];
    cursoraddr=crtc[14]*256+crtc[15];

    start_y=crtc[10]&0xf;
    end_y=crtc[11]&0xf;

    if(cursoraddr<baseaddr) return;

   offset=cursoraddr-baseaddr;

    if(cursoraddr<0x400) return;
    if(cursoraddr>0x4400) return;

    mode=ioport[0xd0];

    color=colorram[cursoraddr-0x400];
    if(color&8) {
        color=mode&7;
    } else {
        color=color&7;
    }

    if(mode&0x80) {         // 80 chars mode

        slice_y=offset/0x800;
        pos_x=(offset-slice_y*0x800)%80;
        pos_y=(offset-slice_y*0x800)/80;

    } else {                // 40 chars mode
        slice_y=offset/0x400;
        pos_x=(offset-slice_y*0x400)%40;
        pos_y=(offset-slice_y*0x400)/40;
    }    

    if(slice_y>1) return;

    if(mode&0x80) { // 80 chars
        vramindex=pos_x*4+pos_y*16*VGA_PIXELS_X/2;

        for(int yy=start_y;yy<=end_y;yy++) {
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+1]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+2]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+3]=color*0x11;
        }
    } else {
        vramindex=pos_x*8+pos_y*16*VGA_PIXELS_X/2;

        for(int yy=start_y;yy<=end_y;yy++) {
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+1]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+2]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+3]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+4]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+5]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+6]=color*0x11;
            vga_data_array[vramindex+yy*VGA_PIXELS_X/2+7]=color*0x11;
        }
    }



}

void erase_cursor(void) {

    uint cursoraddr;
    uint mode;

    cursoraddr=crtc[14]*256+crtc[15];

    // check VRAM range
    if(cursoraddr<0x400) return;
    if(cursoraddr>0x4400) return;

    mode=ioport[0xd0];

    if(mode&0x40) {  // Normal
        draw_framebuffer(cursoraddr);        
    } else {   // Hireso
        if(mode&0x80) {
            for(int i=0;i<8;i++) {
                draw_framebuffer(cursoraddr+0x800*i);
            }
        } else {
            for(int i=0;i<8;i++) {
                draw_framebuffer(cursoraddr+0x400*i);
            }
        }
    }
}

void process_cursor(void) {

    if(crtc[10]&0x40) { // cursor blink on
        if(crtc[10]&0x20) {
            if(blink<32) {
                show_cursor();
            } else if (blink<64) {
                erase_cursor();
                
            } else {
                blink=0;
            }
        } else {
            if(blink<16) {
                show_cursor();
            } else if (blink<32) {
                erase_cursor();
            } else {
                blink=0;
            }
        }
    } else {
        if((crtc[10]&0x20)==0) {
            show_cursor();
        }
            blink=0;
    }
}


static inline unsigned char tohex(int b) {

    if(b==0) {
        return '0';
    } 
    if(b<10) {
        return b+'1'-1;
    }
    if(b<16) {
        return b+'a'-10;
    }

    return -1;

}

static inline unsigned char fromhex(int b) {

    if(b=='0') {
        return 0;
    } 
    if((b>='1')&&(b<='9')) {
        return b-'1'+1;
    }
    if((b>='a')&&(b<='f')) {
        return b-'a'+10;
    }

    return -1;

}

// LittleFS

int pico_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    uint32_t fs_start = XIP_BASE + HW_FLASH_STORAGE_BASE;
    uint32_t addr = fs_start + (block * c->block_size) + off;
    
//    printf("[FS] READ: %p, %d\n", addr, size);
    
    memcpy(buffer, (unsigned char *)addr, size);
    return 0;
}

int pico_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    uint32_t fs_start = HW_FLASH_STORAGE_BASE;
    uint32_t addr = fs_start + (block * c->block_size) + off;
    
//    printf("[FS] WRITE: %p, %d\n", addr, size);
        
    uint32_t ints = save_and_disable_interrupts();
    multicore_lockout_start_blocking();     // pause another core
    flash_range_program(addr, (const uint8_t *)buffer, size);
    multicore_lockout_end_blocking();
    restore_interrupts(ints);
    
    return 0;
}

int pico_erase(const struct lfs_config *c, lfs_block_t block)
{           
    uint32_t fs_start = HW_FLASH_STORAGE_BASE;
    uint32_t offset = fs_start + (block * c->block_size);
    
//    printf("[FS] ERASE: %p, %d\n", offset, block);
        
    uint32_t ints = save_and_disable_interrupts();   
    multicore_lockout_start_blocking();     // pause another core
    flash_range_erase(offset, c->block_size);  
    multicore_lockout_end_blocking();
    restore_interrupts(ints);

    return 0;
}

int pico_sync(const struct lfs_config *c)
{
    return 0;
}

// configuration of the filesystem is provided by this struct
const struct lfs_config PICO_FLASH_CFG = {
    // block device operations
    .read  = &pico_read,
    .prog  = &pico_prog,
    .erase = &pico_erase,
    .sync  = &pico_sync,

    // block device configuration
    .read_size = FLASH_PAGE_SIZE, // 256
    .prog_size = FLASH_PAGE_SIZE, // 256
    
    .block_size = BLOCK_SIZE_BYTES, // 4096
    .block_count = HW_FLASH_STORAGE_BYTES / BLOCK_SIZE_BYTES, // 352
    .block_cycles = 16, // ?
    
    .cache_size = FLASH_PAGE_SIZE, // 256
    .lookahead_size = FLASH_PAGE_SIZE,   // 256    
};



//  Keyboard

void process_kbd_leds(void) {

    hid_led=0;

    if((ioport[0xe0]&4)==0) hid_led+=KEYBOARD_LED_CAPSLOCK;     // CAPS Lock
    if(ioport[0xe0]&1) hid_led+=KEYBOARD_LED_NUMLOCK;           // KANA -> Numlock
    if(ioport[0xe0]&2) hid_led+=KEYBOARD_LED_SCROLLLOCK;        // Hira -> Scrolllock

    if((hid_dev_addr!=255)&&(hid_instance!=255)) {
        tuh_hid_set_report(hid_dev_addr, hid_instance, 0, HID_REPORT_TYPE_OUTPUT, &hid_led, sizeof(hid_led));
    }

}

static inline bool find_key_in_report(hid_keyboard_report_t const *report, uint8_t keycode)
{
  for(uint8_t i=0; i<6; i++)
  {
    if (report->keycode[i] == keycode)  return true;
  }

  return false;
}

void process_kbd_report(hid_keyboard_report_t const *report) {

    int usbkey;

    if(menumode==0) { // Emulator mode

    // Clear keyboard
    memset(keydata,0xff,0x80);

    // Modifyer

    if(report->modifier&0x22) {  // SHIFT
           keydata[7]=0;
    }
    if(report->modifier&0x11) {  // CTRL
           keydata[6]=0;
    } 
    if(report->modifier&0x44) {  // ALT->GRAPH
            keydata[0xb]=0;
    }

    // other keys

    ioport[0xc8]=0;  // reset Break key flag

    for(int i=0;i<6;i++) {
        usbkey=bml3usbcode[report->keycode[i]*2];
        if(usbkey!=0) {
            keydata[usbkey&0x7f]=0;
        }

        // Break key
        if(report->keycode[i]==0x48) {
            ioport[0xc8]=0x80;
 
            if (ioport[0xe0]&0x80) {
                cpu.nmi=1;
            }
        }

        // TEST
        if(report->keycode[i]==0x42) {
//            memcpy(mainram+0x7900,bml3test,0x1570);
//            memcpy(mainram+0x6000,bml3test2,0x2400);  // Exec &h7400
//            memcpy(mainram+0x4000,bml3test3,0x4300);
//            memcpy(mainram+0x5000,bml3test4,0x1000);
                memcpy(mainram+0x4000,bml3test5,0x5000);
        }

        // Enter Menu (temporary)
        if(report->keycode[i]==0x45) {
            prev_report=*report;
            menumode=1;
        }                

        // bench
        // if(report->keycode[i]==0x44) {
                        

        //      uint64_t start_time=time_us_64();   

        //     for(int i=0;i<100;i++) {
        //         redraw();
        //     }            

        //     uint64_t end_time=time_us_64();
        //     unsigned char str[16];

        //     sprintf(str,"%ld",end_time-start_time);
            
        //     cursor_x=0;
        //     cursor_y=24;
        //     fbcolor=7;
        //     video_print(str);

        // }


    }
} else {  // menu mode

    for(uint8_t i=0; i<6; i++)
    {
        if ( report->keycode[i] )
        {
        if ( find_key_in_report(&prev_report, report->keycode[i]) )
        {
            // exist in previous report means the current key is holding
        }else
        {
            keypressed=report->keycode[i];
        }
        }
    } 
    prev_report = *report;
    }

}

/***********************************************************************/
static mc6809byte__t cpu_read(
        mc6809__t     *cpu,
        mc6809addr__t  addr,
        bool           ifetch __attribute__((unused))
)
{

    unsigned char str[16];
    uint d1,d2;

//    assert(cpu       != NULL);
  
    if(addr<0xa000) {
        // RAM area

        // Color register
        if((addr>=0x400)&&(addr<0x4400)) {
//            if((colorram[addr-0x400]&0x80)==0) {
//                ioport[0xd8]=colorram[addr-0x400];
//            }
            if((ioport[0xd8]&0x80)==0) {
                ioport[0xd8]=colorram[addr-0x400]&0x7f;
            }
        }

        return mainram[addr];
    } else {
        
        if((addr<0xff00)||(addr>=0xfff0)) {

            // Bank select
            if((addr>=0xa000)&&(addr<0xc000)&(ioport[0xc0]&2)) {
                return mainram[addr];
            }
            if((addr>=0xc000)&&(addr<0xe000)&(ioport[0xc0]&4)) {
                return mainram[addr];
            }
            if((addr>=0xe000)&&(addr<0xf000)&(ioport[0xc0]&8)) {
                return mainram[addr];
            }

            return bml3rom[addr-0xa000];

        } else {
            // Memory mapped IO

            switch(addr-0xff00) {

                case 0x18:  // FDC
                case 0x19:
                    return 0;

                case 0xc4:      // ACIA control

                // sprintf(str,"R:%02x",0);
                // cursor_x=65;
                // cursor_y=23;
                // fbcolor=7;
                // video_print(str);

                    if(acia_rx_available()==0) {
                        return 2; // TX ready
                    } else {
                        return 3; // TX+RX ready
                    }

                case 0xc5:      // ACIA data

//                    d1=fromhex(uart_getc(uart0));
//                    d2=fromhex(uart_getc(uart0));

            // sprintf(str,"%02x",d1*16+d2);
            // cursor_x=60;
            // cursor_y=24;
            // fbcolor=7;
            // video_print(str);

                    return acia_getc();
//                    return d1*16+d2;

                case 0xc7:  // CRTC
                    return crtc[ioport[0xc6]&0xf];

                case 0xe0: // Keyscan

                    if(kbhit==0) { // Key does not pressed
                        if(ioport[0xe0]&8) {
                            return keytimer & 7;
                        } else {
                            return keytimer;
                        }
                    } else {
                            kbhit--;
                            if(kbhit==0) {
                                return 0xff;
                            }
                            return keytimer|0x80;
                    }

                default:
                    return ioport[addr-0xff00];
            }
        }
    }

  return 0;
}

/************************************************************************/

static void cpu_write(
        mc6809__t     *cpu,
        mc6809addr__t  addr,
        mc6809byte__t  b
)
{

    if(addr<0xa000) {

        mainram[addr]=b;

        if((addr>=0x400)&&(addr<0x4400)) {

            colorram[addr-0x400]=ioport[0xd8];
            draw_framebuffer(addr);

        }

        return;
    }
    if((addr>=0xa000)&&(addr<0xa800)) {  // IGRAM

        if(ioport[0xe9]&1) {
            if(ioport[0xea]&1) igram[addr-0xa000]=b;
            if(ioport[0xea]&2) igram[addr-0xa000+0x800]=b;
            if(ioport[0xea]&4) igram[addr-0xa000+0x1000]=b;
        }
        return;
    }
    if((addr>=0xa000)&&(addr<0xc000)) {  // Bank 0xa000-0xbfff
        if((ioport[0xc0]&2)||(ioport[0xc0]&0x40)) {
            mainram[addr]=b;
            return;
        }
    }
    if((addr>=0xc000)&&(addr<0xe000)) {  // Bank 0xc000-0xdfff
        if((ioport[0xc0]&4)||(ioport[0xc0]&0x80)) {
            mainram[addr]=b;
            return;
        }
    }
    if((addr>=0xe000)&&(addr<0xf000)) {  // Bank 0xe000-0xefff
        if((ioport[0xc0]&8)||(ioport[0xc0]&0x80)) {
            mainram[addr]=b;
            return;
        }
    }
    if(addr>=0xff00) {
        // Memory mapped IO

        unsigned char str[80];

        switch(addr-0xff00) {

            case 0xc0:          // PIA Port A

                if(ioport[0xc1]&4) { // DDR A selected
                    ioport[0xc0]=b;
                }
                break;

            case 0xc4:          // ACIA control

                ioport[0xc4]=b;

            // sprintf(str,"W:%02x",b);
            // cursor_x=60;
            // cursor_y=23;
            // fbcolor=7;
            // video_print(str);

                break;

            case 0xc5:          // ACIA Data

                acia_write(b);

                break;

            case 0xc7:          // CRTC

                if((ioport[0xc6]==14)||(ioport[0xc6]==15)) {   // cursor move
                    erase_cursor();
                }

                crtc[ioport[0xc6]&0xf]=b;

                if((ioport[0xc6]==14)||(ioport[0xc6]==15)) {   // cursor move
                    erase_cursor();
                }

                if(ioport[0xc6]==12) {
                    redraw();
                }

                break;

            case 0xd0:
                if(ioport[addr-0xff00]!=b) {  // Display mode change
                    ioport[addr-0xff00]=b;
                    // redraw whole screen
                    redraw();
                }

                break;

            case 0xd3:      // Beep
                ioport[addr-0xff00]=b;  
                if(b&0x80) {
                    gpio_put(6,1);
                } else {
                    gpio_put(6,0);
                }

                break;

            case 0xd6:      
                if(ioport[addr-0xff00]!=b) {  // Display mode change
                    ioport[addr-0xff00]=b;
                    redraw();
                }

                break;

            case 0xe0: // Keyboard
                ioport[0xe0]=b;

                process_kbd_leds();

                break;

            case 0xe9:
                ioport[0xe9]=b;
                igenable=1;
                redraw();
                break;

            default:
                ioport[addr-0xff00]=b;

        }


    }
}

/************************************************************************/

static void cpu_fault(
        mc6809__t      *cpu,
        mc6809fault__t  fault
)
{
  assert(cpu != NULL);

  longjmp(cpu->err,fault);
}




int main() {

    uint menuprint=0;
    uint filelist=0;

 //   uint lastscanline;

    set_sys_clock_khz(DOTCLOCK * CLOCKMUL ,true);

   stdio_init_all();

    uart_init(uart0, 115200);

    initVGA();

    gpio_set_function(12, GPIO_FUNC_UART);
    gpio_set_function(13, GPIO_FUNC_UART);

    // gpio_set_slew_rate(0,GPIO_SLEW_RATE_FAST);
    // gpio_set_slew_rate(1,GPIO_SLEW_RATE_FAST);
    // gpio_set_slew_rate(2,GPIO_SLEW_RATE_FAST);
    // gpio_set_slew_rate(3,GPIO_SLEW_RATE_FAST);
    // gpio_set_slew_rate(4,GPIO_SLEW_RATE_FAST);

    gpio_set_drive_strength(2,GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(3,GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(4,GPIO_DRIVE_STRENGTH_2MA);

    // Beep

    gpio_init(6);
    gpio_set_dir(6,GPIO_OUT);
    gpio_put(6,0);

//    board_init();
    tuh_init(BOARD_TUH_RHPORT);

//    sem_init(&hsynclock,1,1);

    video_cls();

    video_hsync=0;
    video_vsync=0;

    video_mode=0;
    fbcolor=0x7;

    multicore_launch_core1(main_core1);

// uart handler

    irq_set_exclusive_handler(UART0_IRQ,uart_handler);
    irq_set_enabled(UART0_IRQ,true);
    uart_set_irq_enables(uart0,true,false);

// mount littlefs
   if(lfs_mount(&lfs,&PICO_FLASH_CFG)!=0) {
       cursor_x=0;
       cursor_y=0;
       fbcolor=7;
       video_print("Initializing LittleFS...");
       // format
       lfs_format(&lfs,&PICO_FLASH_CFG);
       lfs_mount(&lfs,&PICO_FLASH_CFG);
   }

//    multicore_launch_core1(main_core1);

//  setup emulator (for core 1)
    cpu.read  = cpu_read;
    cpu.write = cpu_write;
    cpu.fault = cpu_fault;

//  MB-6890 Initialize

    // test
    subram[0]=0;

    ioport[0xc9]=7;  // DIP SW
//    ioport[0xc9]=15;  // DIP SW
 
    memset(keydata,0xff,0x80);

    mc6809_reset(&cpu);

    uint cpuwait=0;

    while(1) {

//        while(video_hsync==1) ;
//        while(sem_available(&hsynclock)==0) ;

        if(menumode==0) { // Emulator mode

        int rc=mc6809_step(&cpu);

        if(video_vsync==1) { // Timer
            tuh_task();
            video_vsync=0;  
            if((ioport[0xd4]&0x80)==0) {
                ioport[0xca]=0x80;
                cpu.firq=1;
            } else {
                ioport[0xca]=0;
            } 
            // cursor
            blink++;
            process_cursor();
        }

        // Keyboard check

        if((cpu.cycles-hsync_cycle)>HSYNC_INTERVAL){
 
//            usbcheck_count++;
//            if(usbcheck_count>USB_CHECK_INTERVAL) {
//                usbcheck_count=0;
//    ///            tuh_task();
//            }

            keycheck_count++;
            if(keycheck_count>0) {

                if(kbhit!=4) {
                    keytimer++;
                    if(keytimer>127) {
                        keytimer=0;
                    }
                    if(keytimer==127) {
                        if(kbhit>0) {
                            kbhit--;
                            if(kbhit==1) {
                                if(ioport[0xe0]&0x40) { 
                                    cpu.irq=true;
                                }
                            }    
                        }
                    }
                    if(keydata[keytimer]==0) { // Key 
                        kbhit=4;
                        if(ioport[0xe0]&0x40) { 
                            cpu.irq=true;
                        }
                    } 
                }
                keycheck_count=0;
            }

            hsync_cycle=cpu.cycles;

            while(video_hsync==0);

            video_hsync=0;

            if((save_enabled==2)&&((cpu.cycles-file_cycle)>FILE_THREHSOLD)) {
                lfs_file_close(&lfs,&lfs_file);
                save_enabled=0;
            }

            if((load_enabled==2)&&((cpu.cycles-file_cycle)>FILE_THREHSOLD)) {
                lfs_file_close(&lfs,&lfs_file);
                load_enabled=0;
            }


        } 

        } else { // Menu Mode

            unsigned char str[80];

            fbcolor=7;
            
            if(menuprint==0) {

                draw_menu();
                menuprint=1;
                filelist=0;
            }

            cursor_x=12;
            cursor_y=6;
            video_print("MENU");

            uint used_blocks=lfs_fs_size(&lfs);
            sprintf(str,"Free:%d Blocks",(HW_FLASH_STORAGE_BYTES/BLOCK_SIZE_BYTES)-used_blocks);
            cursor_x=12;
            cursor_y=7;
            video_print(str);

            cursor_x=12;            
            cursor_y=9;
            if(menuitem==0) { fbcolor=0x70; } else { fbcolor=7; } 
            if(save_enabled==0) {
                video_print("SAVE: UART");
            } else {
                sprintf(str,"SAVE: %8s",filename);
//                sprintf(str,"SAVE: %s","TEST");
                video_print(str);
            }
            cursor_x=12;
            cursor_y=10;

            if(menuitem==1) { fbcolor=0x70; } else { fbcolor=7; } 
            if(load_enabled==0) {
                video_print("LOAD: UART");
            } else {
                sprintf(str,"LOAD: %8s",filename);
//                sprintf(str,"LOAD: %s","TEST");
                video_print(str);
            }

            cursor_x=12;
            cursor_y=11;

            if(menuitem==2) { fbcolor=0x70; } else { fbcolor=7; } 
            video_print("DELETE File");
            cursor_x=12;
            cursor_y=12;

            if(menuitem==3) { fbcolor=0x70; } else { fbcolor=7; } 
            video_print("Reset");

            cursor_x=12;
            cursor_y=13;

            if(menuitem==4) { fbcolor=0x70; } else { fbcolor=7; } 
            video_print("Power Cycle");

            if(filelist==0) {
                draw_files(-1,0);
                filelist=1;
            }

            while(video_vsync==0);

            video_vsync=0;

                tuh_task();

                if(keypressed==0x52) { // Up
                    keypressed=0;
                    if(menuitem>0) menuitem--;
                }

                if(keypressed==0x51) { // Down
                    keypressed=0;
                    if(menuitem<4) menuitem++; 
                }

                if(keypressed==0x28) {  // Enter
                    keypressed=0;

                    if(menuitem==0) {  // SAVE

                        if((load_enabled==0)&&(save_enabled==0)) {

                            uint res=enter_filename();

                            if(res==0) {
                                lfs_file_open(&lfs,&lfs_file,filename,LFS_O_RDWR|LFS_O_CREAT);
                                save_enabled=1;
                                file_cycle=cpu.cycles;
                            }

                        } else if (save_enabled!=0) {
                            lfs_file_close(&lfs,&lfs_file);
                            save_enabled=0;
                        }
                        menuprint=0;
                    }

                    if(menuitem==1) { // LOAD

                        if((load_enabled==0)&&(save_enabled==0)) {

                            uint res=file_selector();

                            if(res==0) {
                                lfs_file_open(&lfs,&lfs_file,filename,LFS_O_RDONLY);
                                load_enabled=1;
                                file_cycle=cpu.cycles;
                            }
                        } else if(load_enabled!=0) {
                            lfs_file_close(&lfs,&lfs_file);
                            load_enabled=0;
                        }
                        menuprint=0;
                    }

                    if(menuitem==2) { // Delete

                        if((load_enabled==0)&&(save_enabled==0)) {
                            uint res=enter_filename();

                            if(res==0) {
                                lfs_remove(&lfs,filename);
                            }
                        }

                        menuprint=0;

                    }


                    if(menuitem==3) { // reset
                        menumode=0;
                        menuprint=0;
                        redraw();
                        mc6809_reset(&cpu);
                    }

                    if(menuitem==4) { // power cycle
                        menumode=0;
                        menuprint=0;
                        redraw();

                        memset(mainram,0,0x10000);
                        memset(colorram,0,0x4000);
                        memset(igram,0,0x1800);

                        mc6809_reset(&cpu);

                    }



                }

                if(keypressed==0x45) {
                    keypressed=0;
                    menumode=0;
                    menuprint=0;
                    redraw();
                //  break;     // escape from menu
                }
            
        }

    }

}
