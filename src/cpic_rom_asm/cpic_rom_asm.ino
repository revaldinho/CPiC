/*
   CPIC - An Amstrad CPC ROM/RAM Board in a Teensy 3.5 microcontroller board

   (C) 2017 Revaldinho

   Teensy3.5 is a micro controller with the following key features
   - 120 MHz ARM Cortex-M4 with Floating Point Unitct
   - 512K Flash, 192K RAM, 4K EEPROM
   - Microcontroller Chip MK64FX512VMD12
   - 1 CAN Bus Port
   - 16 General Purpose DMA Channels
   - 5 Volt Tolerance On All Digital I/O Pins

   CPIC uses the Teensy to expand an Amstrad CPC providing
   - 16 Upper (Application) ROM slots
   - Lower (Firmware) ROM replacement

   All ROM data is stored in flash memory to be persistent, but accessing it does
   incur some wait states on the MIPS CPU whereas the RAM runs at the full 120MHz
   core speed. Copying the ROM contents into RAM is possible to speed this up,
   but then limits the number of ROMs available. Copying will take a while so may
   need to reboot the CPC after the ROM board: if sharing the CPC PSU then need
   to provide a CPC reset button on the shield.

   CPC Peripheral Operation
   ========================

   ROM Selection performed by writing the ROM number to an IO address with A13 low.

   ROM is accessed whenever ROMEN_B goes low. Addresses with Address[15] set go to upper (foreground)
   ROMs and those with Address[15] low go to lower ROM.

 Per-pin control notes
 ---------------------
 
 val = 0b0000_0000_0000_0000_0000_0001_0100_0100;
                                   ---  -    -
                                    \    \    \_ Slew rate enable: 1=Fast, 0=Slow 
                                     \    \_____ Drive strength  : 1=High, 0=Low
                                      \_________ Pin control     : 001 = GPIO

 // Set ROMDIS/RAMDIS to have fast slew + high drive

 PORTB_PCR8 = 0x00000144;
 PORTB_PCR9 = 0x00000144;

 OR can use the PORTX_GPCLR to set bits globally for an entire port, ie

 PORTB_GPCLR = 0x0144FFFF;

 ...picks bits from lower 16 bits and writes to selected bits[15:0] identified in the upper 16bit field.
 e.g. set all bits in a port to GPIO control and default to slow slew, low drive

 PORTB_GPCLR = 0x01440100 ;

  Worst case timings from Zilog 8400A Datasheet
  ---------------------------------------------
  
                :<----- 250ns------>:<----- 250ns------>:<----- 250ns------>:
                :        T1         :       T2/3/4      :       T3/4/5
                :_________          :_________          :_________
  CLK __________/         \_________/         \_________/         \_________
                : max:    .                             :
                : 110:    .                             :
      __________:____:____._____________________________:____________________
  ADR __________:____X____._____________________________:____________________
                :         .                             :
                :max:     .                             :
                :100:     .                             :
      __________:___:     .                             :____________________
  M1*           :   \_____._____________________________/
                :         .                             :
                :         . max.                        :
                :         . 75 .                        :
      __________:_________.____.                        :___________________
  IOREQ*        :         .    \________________________/
                :         . max .                       :
                :         . 85  .                       :
      __________:_________._____.                       :___________________
  MREQ*         :         .     \_______________________/
                :         . max                         :
                :         . 95   .                      :
      __________:_________.______.                      :___________________
  RD*           :         .      \______________________/
                :         .                             :
                :         .                         :Min: 
                :         .                         :30 :  
                :         .                          ______
  DATA ---------:-----------------------------------<______>-----------------
  
  ie time from M1* -> other control signals value ~ (125-100)+95 = 120ns.
     If triggering on M1, need to resample control signals at least 120ns (~15 cycles @ 120MHz) later 
     to determine whether ROM or RAM is being accessed!
  
  Instr Fetch time: M1* (T1) -> clock rise (T3)
  
                = (250-100)+ (250-30) = 370ns
  
  Data Read time: MREQ* (T1) -> clock rise (T4)
  
                = (250-125-85) + 250 + (250-30) = 510ns
  
  IO Read time: IOREQ* (T1) -> clock rise (T5)  
  
                = (250-125-75) + 250 + 250 + (250-30) = 770ns

  while ((ctrladr=PORTX & MASK) == MASK ) {}
  get address_hi                          // ~7 instr after sample here = 58ns later but address already valid
  compute address = f(ctrladr,address_hi)
  pre-fetch ROM data (choose upper or lower) - longest latency
  resample ctrladr                        // resample ctrl signals at least 120ns after original trigger
  
  ... proceed as normal through if-then-else
  
  Reset oscillator, pullups on all control inputs signal, and pulldowns on ROMDIS, RAMDIS
  
  o M1* -> ROMDIS time   < 370ns   RD_B -> ROMDIS time < 120ns[1]    M1, RD_B    -> Osc  ROMEN* -> low  
  o M1* -> RAMDIS time   < 370ns   RD_B -> RAMDIS time < 120ns[1]    M1, RD_B    -> Osc  RAMRD* -> low
  o MREQ* -> ROMDIS time < 510ns   RD_B -> ROMDIS time < 120ns[1]    MREQ*, RD_B -> Osc  ROMEN* -> low
  o MREQ* -> RAMDIS time < 510ns   RD_B -> RAMDIS time < 120ns[1]    MREQ*, RD_B -> Osc  RAMRD* -> low
  
  [1] only for debug-> RD_B doesn't normally tristate these signals, but does to make the event easy
      to observe when DEBUG=1
  
  Osc @ 0.5MHz -> 1000ns high/low time
*/
#include <string.h>
#include <stdio.h>




// ----- Some string processing definitions to enable use of C tokens in assembler statements
#define str(a) st(a)
#define st(a) #a

#define USE_ASM_LOOP       1

// --- Port allocations
// Data direction is the opposite of ChipKit - on Teensy 0=input, 1=Output
// Some absolute addresses for the assembler code
#define GPIO_BASE          0x400FF000
#define GPIOB_PDIR_OFFSET  0x50
#define GPIOD_PDIR_OFFSET  0xD0
#define GPIOC_PDDR_OFFSET  0x94
#define GPIOC_PDOR_OFFSET  0x80

#define GPIOB_PDIR_ADDR    0x400FF050
#define GPIOD_PDIR_ADDR    0x400FF0D0
#define GPIOC_PDDR_ADDR    0x400FF094
#define GPIOC_PDOR_ADDR    0x400FF080

// Arduino defined tokens (including unit32_t*) casts for C
#define  CTRLADRHI_IN     GPIOB_PDIR
#define  CTRLADRHI_MODE   GPIOB_PDDR
#define  DATACTRL_IN      GPIOC_PDIR
#define  DATACTRL_OUT     GPIOC_PDOR
#define  DATACTRL_MODE    GPIOC_PDDR
#define  DATACTRL_CLEAR   GPIOC_PCOR
#define  ADRLO_DINLO_IN        GPIOD_PDIR
#define  ADRLO_DINLO_MODE      GPIOD_PDDR

// --- Bit allocations
#define DATA              0x000000FF
#define ROMVALID          0x00000100
#define WAIT_B            0x00000400

#define MREQ_B            0x00000001 // Port B0
#define IOREQ_B           0x00000002
#define WR_B              0x00000004
#define RAMRD_B           0x00000008
#define ROMEN_B           0x00000010
#define M1_B              0x00000020 // Port B5 
#define RD_B              0x00000400 // Port B10 (because 6-9 not available)  
#define CLK4              0x00000800
#define MASK              0x00000424 // Mask on M1,RD,WR

#define ADDR_HI           0x00FF0000  // bits 16-23 on control input data
#define ADDR_LO           0x000000FF  // bits  0-7 on address input
#define ADR_13_RAW        0x00200000  // bit 13 in raw position in ctrl/adr word - ie bit 21
#define ADR_14_RAW        0x00400000  // bit 14 in raw position in ctrl/adr word - ie bit 22
#define ADR_15_RAW        0x00800000  // bit 15 in raw position in ctrl/adr word - ie bit 23
#define DIN_LO            0x0000F000  // bits 12..15

#define VALIDRAMSELMASK   0x000000C0  // Top two bits of Data must be set for a valid RAM selection
#define RAMCODEMASK       0x00000038  // Next 3 bits are the 'code'
#define RAMBANKMASK       0x00000007  // Bottom 3 bits are the 'code'

// ---- Constants and macros
#define MAXROMS           8
#define RAMBLKSIZE        16384
#define ROMSIZE           16384

#define ROMSEL            (!(ctrladrhi&(IOREQ_B|ADR_13_RAW|WR_B)))
#define HIROMRD           ((ctrladrhi&(ROMEN_B|ADR_14_RAW))==(ADR_14_RAW))

// Global variables
const char upperrom[MAXROMS*ROMSIZE] = {
#include "/Users/richarde/Documents/Development/git/CPiC/src/CSV/BASIC_1.1.CSV"
#include "/Users/richarde/Documents/Development/git/CPiC/src/CSV/Manic_Miner.CSV"
#include "/Users/richarde/Documents/Development/git/CPiC/src/CSV/Thrust.CSV"
#include "/Users/richarde/Documents/Development/git/CPiC/src/CSV/PROTEXT.CSV"
#include "/Users/richarde/Documents/Development/git/CPiC/src/CSV/MAXAM150.CSV"
#include "/Users/richarde/Documents/Development/git/CPiC/src/CSV/BCPL.CSV"
#include "/Users/richarde/Documents/Development/git/CPiC/src/CSV/UTOPIA.CSV"
#include "/Users/richarde/Documents/Development/git/CPiC/src/CSV/ALL_ZEROS.CSV"
};

char ram[MAXROMS*ROMSIZE] ;

const int valid_upperrom[MAXROMS] = {
  0,ROMVALID,ROMVALID,ROMVALID,ROMVALID,ROMVALID,ROMVALID,0
};

void setup() {
  // Set all pins to input mode using pinMode instructions as easy way of setting up
  // GPIO control - can use port registers after this initialization
  for (int i = 0 ; i < 58 ; i++ ) {
    pinMode(i, INPUT);
  }
  memcpy( ram, upperrom, MAXROMS*ROMSIZE);
  DATACTRL_OUT  = 0x00 |  0     ; 
  DATACTRL_MODE = 0xFF |  ROMVALID ; // Always drive out - OE taken care of externally
}

void asmloop( int *romvalid, char *romdata ) {
#include "/Users/richarde/Documents/Development/git/CPiC/src/cpic_rom_asmloop.inc"
}


void loop() { 
#ifdef USE_ASM_LOOP
  __disable_irq();
  asmloop( (int *) valid_upperrom, (char *) ram);
  __enable_irq();
#else
  int ctrladrhi;       
  int address;         
  char *romptr = NULL;
  char romdata = 0;  
  int romvalid = 0;
  int adrlo_dinlo = 0;  
  __disable_irq();
  while (true) {
    while ( ((ctrladrhi=CTRLADRHI_IN)&(RD_B|WR_B)) != (RD_B|WR_B) ) { }
    adrlo_dinlo = ADRLO_DINLO_IN;
    if (HIROMRD) {   
      address =((ctrladrhi>>8)&0x3F00)|(adrlo_dinlo&0x00FF);
      romdata = *(romptr+address) ; 
      DATACTRL_OUT  = romdata | romvalid   ;  
    } else if ( ROMSEL ) {
      int romnum = (adrlo_dinlo&0x0F000)>>12;
      romvalid = valid_upperrom[romnum] ;
      romptr = (char *) &(ram[romnum<<14]) ;
      DATACTRL_OUT  = 0x00 | romvalid ;  
    } 
  }
  __enable_irq();

#endif
}
            



