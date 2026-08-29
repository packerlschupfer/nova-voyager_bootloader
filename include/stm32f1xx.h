/**
 * @file stm32f1xx.h
 * @brief Minimal STM32F1/GD32F1 register definitions for bootloader
 */

#ifndef STM32F1XX_H
#define STM32F1XX_H

#include <stdint.h>
#include <stddef.h>  // for NULL

/*===========================================================================*/
/* Core Registers                                                            */
/*===========================================================================*/

#define __IO volatile

// SCB - System Control Block
typedef struct {
    __IO uint32_t CPUID;
    __IO uint32_t ICSR;
    __IO uint32_t VTOR;
    __IO uint32_t AIRCR;
    __IO uint32_t SCR;
    __IO uint32_t CCR;
    __IO uint32_t SHPR[3];
    __IO uint32_t SHCSR;
    __IO uint32_t CFSR;
    __IO uint32_t HFSR;
    __IO uint32_t DFSR;
    __IO uint32_t MMFAR;
    __IO uint32_t BFAR;
    __IO uint32_t AFSR;
} SCB_Type;

#define SCB_BASE            0xE000ED00
#define SCB                 ((SCB_Type*)SCB_BASE)
#define SCB_ICSR_PENDSTCLR_Msk  (1 << 25)

// SysTick
typedef struct {
    __IO uint32_t CTRL;
    __IO uint32_t LOAD;
    __IO uint32_t VAL;
    __IO uint32_t CALIB;
} SysTick_Type;

#define SysTick_BASE        0xE000E010
#define SysTick             ((SysTick_Type*)SysTick_BASE)

// NVIC
typedef struct {
    __IO uint32_t ISER[8];
    uint32_t RESERVED0[24];
    __IO uint32_t ICER[8];
    uint32_t RESERVED1[24];
    __IO uint32_t ISPR[8];
    uint32_t RESERVED2[24];
    __IO uint32_t ICPR[8];
    uint32_t RESERVED3[24];
    __IO uint32_t IABR[8];
    uint32_t RESERVED4[56];
    __IO uint8_t  IP[240];
    uint32_t RESERVED5[644];
    __IO uint32_t STIR;
} NVIC_Type;

#define NVIC_BASE           0xE000E100
#define NVIC                ((NVIC_Type*)NVIC_BASE)

static inline void NVIC_SystemReset(void) {
    __asm volatile ("dsb");
    SCB->AIRCR = (0x5FA << 16) | (1 << 2);
    __asm volatile ("dsb");
    while(1);
}

/*===========================================================================*/
/* RCC - Reset and Clock Control                                             */
/*===========================================================================*/

typedef struct {
    __IO uint32_t CR;
    __IO uint32_t CFGR;
    __IO uint32_t CIR;
    __IO uint32_t APB2RSTR;
    __IO uint32_t APB1RSTR;
    __IO uint32_t AHBENR;
    __IO uint32_t APB2ENR;
    __IO uint32_t APB1ENR;
    __IO uint32_t BDCR;
    __IO uint32_t CSR;
} RCC_TypeDef;

#define RCC_BASE            0x40021000
#define RCC                 ((RCC_TypeDef*)RCC_BASE)

// CR bits
#define RCC_CR_HSION        (1 << 0)
#define RCC_CR_HSIRDY       (1 << 1)
#define RCC_CR_HSEON        (1 << 16)
#define RCC_CR_HSERDY       (1 << 17)
#define RCC_CR_PLLON        (1 << 24)
#define RCC_CR_PLLRDY       (1 << 25)

// CFGR bits
#define RCC_CFGR_SW_HSI     0
#define RCC_CFGR_SW_HSE     1
#define RCC_CFGR_SW_PLL     2
#define RCC_CFGR_SWS        (3 << 2)
#define RCC_CFGR_SWS_HSI    (0 << 2)
#define RCC_CFGR_SWS_HSE    (1 << 2)
#define RCC_CFGR_SWS_PLL    (2 << 2)
#define RCC_CFGR_PLLSRC_HSI_DIV2    0
#define RCC_CFGR_PLLSRC_HSE         (1 << 16)
#define RCC_CFGR_PLLMULL12  (10 << 18)

// APB2ENR bits
#define RCC_APB2ENR_AFIOEN  (1 << 0)
#define RCC_APB2ENR_IOPAEN  (1 << 2)
#define RCC_APB2ENR_IOPBEN  (1 << 3)
#define RCC_APB2ENR_IOPCEN  (1 << 4)
#define RCC_APB2ENR_IOPDEN  (1 << 5)

// APB1ENR bits
#define RCC_APB1ENR_USBEN   (1 << 23)

/*===========================================================================*/
/* GPIO                                                                      */
/*===========================================================================*/

typedef struct {
    __IO uint32_t CRL;
    __IO uint32_t CRH;
    __IO uint32_t IDR;
    __IO uint32_t ODR;
    __IO uint32_t BSRR;
    __IO uint32_t BRR;
    __IO uint32_t LCKR;
} GPIO_TypeDef;

#define GPIOA_BASE          0x40010800
#define GPIOB_BASE          0x40010C00
#define GPIOC_BASE          0x40011000
#define GPIOD_BASE          0x40011400

#define GPIOA               ((GPIO_TypeDef*)GPIOA_BASE)
#define GPIOB               ((GPIO_TypeDef*)GPIOB_BASE)
#define GPIOC               ((GPIO_TypeDef*)GPIOC_BASE)
#define GPIOD               ((GPIO_TypeDef*)GPIOD_BASE)

/*===========================================================================*/
/* FLASH                                                                     */
/*===========================================================================*/

typedef struct {
    __IO uint32_t ACR;
    __IO uint32_t KEYR;
    __IO uint32_t OPTKEYR;
    __IO uint32_t SR;
    __IO uint32_t CR;
    __IO uint32_t AR;
    __IO uint32_t RESERVED;
    __IO uint32_t OBR;
    __IO uint32_t WRPR;
} FLASH_TypeDef;

#define FLASH_BASE          0x40022000
#define FLASH               ((FLASH_TypeDef*)FLASH_BASE)

// ACR bits
#define FLASH_ACR_LATENCY_0 0
#define FLASH_ACR_LATENCY_1 1
#define FLASH_ACR_LATENCY_2 2
#define FLASH_ACR_PRFTBE    (1 << 4)

// SR bits
#define FLASH_SR_BSY        (1 << 0)
#define FLASH_SR_PGERR      (1 << 2)
#define FLASH_SR_WRPRTERR   (1 << 4)
#define FLASH_SR_EOP        (1 << 5)

// CR bits
#define FLASH_CR_PG         (1 << 0)
#define FLASH_CR_PER        (1 << 1)
#define FLASH_CR_MER        (1 << 2)
#define FLASH_CR_STRT       (1 << 6)
#define FLASH_CR_LOCK       (1 << 7)

/*===========================================================================*/
/* USB                                                                       */
/*===========================================================================*/

typedef struct {
    __IO uint32_t EP0R;
    __IO uint32_t EP1R;
    __IO uint32_t EP2R;
    __IO uint32_t EP3R;
    __IO uint32_t EP4R;
    __IO uint32_t EP5R;
    __IO uint32_t EP6R;
    __IO uint32_t EP7R;
    uint32_t RESERVED[8];
    __IO uint32_t CNTR;
    __IO uint32_t ISTR;
    __IO uint32_t FNR;
    __IO uint32_t DADDR;
    __IO uint32_t BTABLE;
} USB_TypeDef;

#define USB_BASE            0x40005C00
#define USB                 ((USB_TypeDef*)USB_BASE)

// CNTR bits
#define USB_CNTR_FRES       (1 << 0)
#define USB_CNTR_PDWN       (1 << 1)
#define USB_CNTR_LPMODE     (1 << 2)
#define USB_CNTR_FSUSP      (1 << 3)
#define USB_CNTR_RESUME     (1 << 4)
#define USB_CNTR_RESETM     (1 << 10)
#define USB_CNTR_CTRM       (1 << 15)

// ISTR bits
#define USB_ISTR_EP_ID      0x0F
#define USB_ISTR_DIR        (1 << 4)
#define USB_ISTR_RESET      (1 << 10)
#define USB_ISTR_CTR        (1 << 15)

// DADDR bits
#define USB_DADDR_EF        (1 << 7)

// EPnR bits
#define USB_EP_CTR_RX       (1 << 15)
#define USB_EP_DTOG_RX      (1 << 14)
#define USB_EP_STAT_RX      (3 << 12)
#define USB_EP_SETUP        (1 << 11)
#define USB_EP_TYPE         (3 << 9)
#define USB_EP_KIND         (1 << 8)
#define USB_EP_CTR_TX       (1 << 7)
#define USB_EP_DTOG_TX      (1 << 6)
#define USB_EP_STAT_TX      (3 << 4)
#define USB_EP_EA           (0xF << 0)

#define USB_EP_CONTROL      (1 << 9)
#define USB_EP_BULK         (0 << 9)
#define USB_EP_INTERRUPT    (3 << 9)

#define USB_EP_TX_DIS       (0 << 4)
#define USB_EP_TX_STALL     (1 << 4)
#define USB_EP_TX_NAK       (2 << 4)
#define USB_EP_TX_VALID     (3 << 4)

#define USB_EP_RX_DIS       (0 << 12)
#define USB_EP_RX_STALL     (1 << 12)
#define USB_EP_RX_NAK       (2 << 12)
#define USB_EP_RX_VALID     (3 << 12)

/*===========================================================================*/
/* Intrinsics                                                                */
/*===========================================================================*/

static inline void __disable_irq(void) {
    __asm volatile ("cpsid i" ::: "memory");
}

static inline void __enable_irq(void) {
    __asm volatile ("cpsie i" ::: "memory");
}

static inline void __set_MSP(uint32_t topOfMainStack) {
    __asm volatile ("MSR msp, %0" : : "r" (topOfMainStack) : );
}

static inline uint32_t __get_MSP(void) {
    uint32_t result;
    __asm volatile ("MRS %0, msp" : "=r" (result));
    return result;
}

#endif /* STM32F1XX_H */
