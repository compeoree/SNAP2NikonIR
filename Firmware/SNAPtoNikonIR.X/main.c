/*
 * File:   main.c
 * Author: Louis McCarthy
 *
 * Created on October 7, 2020
 * Version 1.0
 * 
 * Compiler: XC8 (v2.30)
 * C Flavor: C99
 * MCU: PIC16F18313
 *                ______
 *   VCC (3.0V) -|*     |- VSS
 *   IR LED (+) -|A5  A0|- ICSPDATA
 *   IR LED (-) -|A4  A1|- ICSPCLK
 *         MCLR -|____A2|- Trigger
 *
 * *** Bill of Materials (BOM) ***
 *   1) PIC16F18313 (MCU)
 *   2) LTE-3371TL  (IR LED)
 *   3) SMTM1632    (1632 battery holder)
 *   4) B3F-1000    (momentary switch)
 *   5) 100R 0603   (100 ohm SMD resistor)
 *   6) 1x5 Header  (programming header 2.54mm pitch)
 *   7) SJ1-2533    (2.5mm headphone jack)
 * 
 * *** Nikon IR Spec ***
 * Carrier Frequency
 * 38.4 kHz = 0.0000260417 seconds (.0260417 ms or 26.0417 us)
 * 26 us = ~38462 Hz
 *     ______                   ____        ____          ____
 *    | 2000 |        28       |400 | 1580 |400 |  3580  |400 |  63.2
 *    |  us  |        ms       | us |  us  | us |   us   | us |   ms
 * ___|      |_________________|    |______|    |________|    |___________ x2
 *       77          1077        15    61    15    138     15    2431
 *          26 us ticks
 * Source: https://www.sbprojects.net/projects/nikon/   
 *         http://www.bigmike.it/ircontrol/
 *         http://allynh.com/blog/making-an-ir-remote-control-for-a-nikon-camera/
 *         https://www.christidis.info/index.php/personal-projects/arduino-nikon-infrared-command-code
 *  
 * NCO module has too much error for large frequency mismatch between Fosc and Foverflow
 * At slowest Fosc of 1MHz, output is 38.147kHz or 38.624kHz, depending on incr.
 * 
 * The PWM module outputs 38.461kHz at 8MHz HFINTOSC
 * 
 * *** SNAP Spec ***
 *       _____
 *   ___/     |_________     C = Common          (sleeve, "GND")
 *   ___      |____|__|_>    F = Focus           (ring, switch closure to common)
 *      \_____| C   F  S     S = Shutter release (tip, switch closure to common)
 * Source: https://www.doc-diy.net/photo/remote_pinout/canon_pinout.png
 * 
 * *** CR1632 data ***
 * Datasheet: https://data.energizer.com/pdfs/cr1632.pdf
 * Nominal capacity 100-120 mAh, 3.0V to 2.0V discharge
 * 217uA @ 3.227V - force enable HFINTOSC but CPU and memory not clocked (SLEEP())
 * 12.8uA @ 3.235V - full sleep
 * 7.9uA @ 3.213V - LDO off, full sleep
 */
#pragma warning disable 1395  // Disable notable code message - for xc8 developers

#include <xc.h>
#include <stdbool.h>

// <editor-fold defaultstate="collapsed" desc="Configuration Parameters">
// Config 1
#pragma config FEXTOSC  = OFF       // External OSC disabled
#pragma config RSTOSC   = HFINT1    // HFINTOSC at 1MHz
#pragma config CLKOUTEN = OFF       // Clock out disabled
#pragma config CSWEN    = ON        // Software clock switching enabled
#pragma config FCMEN    = OFF       // Fail-Safe monitor disabled

// Config 2
#pragma config MCLRE    = ON        // MCLR is reset/pgm pin
#pragma config PWRTE    = OFF       // Power-up timer disabled
#pragma config WDTE     = OFF       // WDT disabled
#pragma config LPBOREN  = OFF       // ULPBOR disabled
#pragma config BOREN    = OFF       // BOR disabled
#pragma config BORV     = LOW       // BOR at 2.45V
#pragma config PPS1WAY  = ON        // PPS single set only
#pragma config STVREN   = ON        // Stack overflow/underflow reset enabled
#pragma config DEBUG    = OFF       // Automatically changed by ICSP - don't set on here

// Config 3
#pragma config WRT      = ALL       // Write protection on
#pragma config LVP      = OFF       // LVPGM disabled, standard mode

// Config 4
#pragma config CP       = OFF       // Code protect off
#pragma config CPD      = OFF       // Data EEPROM protect off
// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="Variables & Constants">
#define _XTAL_FREQ  2000000         // HFINTOSC / 4
#define IR_PLUS     LATAbits.LATA5  // RA5/Pin 2
#define BYTE        uint8_t         // Convenience type

volatile bool signalCamera = false;
// </editor-fold>

void main(void) {
    // <editor-fold defaultstate="collapsed" desc="Initialize">
    // Set the PIC to a known configuration
    
    // Set Fosc
    OSCCON1 = 0b01100010;   // Internal 1Mhz clock, clock div. 4
    OSCCON3 = 0b00000000;   // SO uses pins, low power mode, auto clock switch
    OSCEN = 0b00000000;     // ADC osc off, SO off, LFINT off, HFINT on, EXTOSC off
    OSCFRQ = 0b00000100;    // Nominal freq 8MHz - adjust PR2 and PWM5DC too!
    //OSCFRQ = 0b00000011;    // Nominal freq 4MHz - adjust PR2 and PWM5DC too!
    //OSCFRQ = 0b00000001;    // Nominal freq 2MHz - adjust PR2 and PWM5DC too!
    //OSCFRQ = 0b00000000;    // Nominal freq 1MHz - adjust PR2 and PWM5DC too!

    // Clear all ISR flags
    PIR0 = 0x00;
    PIR1 = 0x00;
    PIR2 = 0x00;
    PIR3 = 0x00;
    PIR4 = 0x00;
    
    // Disable all ISRs
    PIE0 = 0x00;    // Ext INT, TMR0
    PIE1 = 0x00;    // TMR1, TMR2, UART, ADC
    PIE2 = 0x00;    // NCO, NVM, compare
    PIE3 = 0x00;    // clock interrupts
    PIE4 = 0x00;    // CCP/CWG
    INTCON = 0x00;  // Disable peripheral and global, Ext INT on falling edge
    
    // Fixed Voltage Regulator - disabled
    FVRCON = 0b00000000;
    
    // Analog to Digital Converter - disabled
    ADCON0 = 0b11110100;    // Disabled but pointing to temperature indicator
    ADCON1 = 0b11100011;    // Vref+ to FVR, Vref- to Vss, Fosc/64, right justified
    ADACT = 0b00000000;     // No auto-conversion trigger
    ANSELA = 0x00;          // All pins as digital I/O
    
    // Digital to Analog Converter - disabled
    DACCON0 = 0b00001000;   // DAC uses Vss & FVR, not connected to pin, disabled
    DACCON1 = 0b00000000;   // DAC voltage is lowest
    
    // UART - disabled
    TX1STA = 0b00000100;    // High BAUD, no synch break, async mode, no TX, 8bit
    RC1STA = 0b00000000;    // no addr bit, no cont. RX, 8bit, serial port disabled
    BAUD1CON = 0b00001000;  // no auto baud, no wakeup, 16bit baud, std idle 
    SP1BRGL = 12;           // 12=19.2k, 25=9600 for 1MHz Fosc
    SP1BRGH = 0x00;         // upper word of SPBRG
    
    // Set pin directions and modes
    TRISA = 0b00000100;     // RA2 is input  
    LATA =  0b00000000;     // Set all outputs low
    WPUA = 0b00001100;      // RA2 has weak pullup (MCLR too)
    ODCONA = 0b00010000;    // RA4 is open-drain (sink only)
    SLRCONA = 0b00000000;   // No slew rate control (not default)
    INLVLA = 0b00111111;    // ST input levels (not TTL)
    RA0PPS = 0x00;          // DIO on A0
    RA1PPS = 0x00;          // DIO on A1
    RA2PPS = 0x00;          // DIO on A2
    RA4PPS = 0b00000010;    // PWM5 on A4
    RA5PPS = 0x00;          // DIO on A5
    INTPPS = 0b00000010;    // INT on A2
    
    // Timer 0 - disabled (see page 257)
    T0CON0 = 0b00000000;    // 1:1 postscaler, 8-bit mode, disabled
    T0CON1 = 0b01000000;    // 1:1 prescaler, sync to Fosc/4, source Fosc/4
    TMR0H = 12;             // 24us @ 2MHz Fosc
    TMR0L = 0;              // Reset 8-bit timer
    
    // Timer 2 - PWM mode (see page 174)
    PR2 = 12;               //(PWMxDC = 26, PR2=12) = 38461 Hz
    PWM5DCL = (BYTE)(26 << 6); // Lowest 2 bits
    PWM5DCH = 26 >> 2;      // Highest 8 bits
    PWM5CON = 0b10000000;   // Enable PWM5, active high
    PIR1bits.TMR2IF = 0;    // Clear ISR flag
    T2CON = 0b00000100;     // 1:1 postscaler, enabled, 1:1 prescaler
    TMR2 = 0x00;            // Reset counter
    
    // Power down unused peripherals
    PMD0 = 0b01000111;      // Disable IOC, CLK, NVM, FVR modules
    PMD1 = 0b10000011;      // Enable TMR2, disable TMR1, TMR0, NCO modules
    PMD2 = 0b01100110;      // Disable CMP, ADC, DAC modules
    PMD3 = 0b01100011;      // PWM5 enabled, disable CCP, PWM6, CWG modules
    PMD4 = 0b00100010;      // Disable MSSP, UART modules
    PMD5 = 0b00000111;      // Disable DSM, CLC modules
    CPUDOZE = 0b00000000;   // Full sleep, no doze
    VREGCON = 0b00000010;   // Low power sleep mode (LDO off)
    
    // Enable interrupts
    PIE0 = 0b00000001;      // Enable external interrupt (A2)
    PIE1 = 0b00000000;      // Disable TMR2 interrupt
    //PIE1 = 0b00000010;      // Enable TMR2 interrupt
    INTCONbits.PEIE = 1;    // Enable peripheral interrupts
    INTCONbits.GIE = 1;     // Enable interrupts
    //  </editor-fold>
    
    while (1)
    {
        // This is required due to wakeup, ISR, and signalCamera delays
        // If this were a simple SLEEP(), a button press only works every other time
        if (!signalCamera)
            SLEEP();
        
        if (!signalCamera)
            continue;
        
        signalCamera = false;
        for (BYTE i=0;i<2;i++)
        {
            IR_PLUS = 1;
            __delay_us(2000);
            IR_PLUS = 0;
            __delay_ms(28);
            IR_PLUS = 1;
            __delay_us(400);
            IR_PLUS = 0;
            __delay_us(1580);
            IR_PLUS = 1;
            __delay_us(400);
            IR_PLUS = 0;
            __delay_us(3580);
            IR_PLUS = 1;
            __delay_us(400);
            IR_PLUS = 0;
            __delay_ms(63);
            __delay_us(200);
        }
    }
    return;
}

void __interrupt() ISR(void)
{
    if (PIR0bits.INTF && PIE0bits.INTE) // See page 88
    {
        PIR0bits.INTF = 0;
        signalCamera = true;
    }
    
    if(PIR1bits.TMR2IF && PIE1bits.TMR2IE) // See page 174 for PWM
    {
       PIR1bits.TMR2IF = 0;
    }
    
    /*if(PIR0bits.TMR0IF && PIE0bits.TMR0IE)
    {
       PIR0bits.TMR0IF = 0;
       TMR0L = 0;   // Reset for 8-bit mode
       counter++;
    }*/
}

/* Todo 
 * 
 * 1) Allow for repeated shutter releases while button is pressed
 * 2) Look for further sleep power reductions
 * 3) Implement intervalometer functionality
 */

