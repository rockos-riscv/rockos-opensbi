/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi_utils/serial/uart8250.h>

/* clang-format off */

#define UART_RBR_OFFSET		0	/* In:  Recieve Buffer Register */
#define UART_THR_OFFSET		0	/* Out: Transmitter Holding Register */
#define UART_DLL_OFFSET		0	/* Out: Divisor Latch Low */
#define UART_IER_OFFSET		1	/* I/O: Interrupt Enable Register */
#define UART_DLM_OFFSET		1	/* Out: Divisor Latch High */
#define UART_FCR_OFFSET		2	/* Out: FIFO Control Register */
#define UART_IIR_OFFSET		2	/* I/O: Interrupt Identification Register */
#define UART_LCR_OFFSET		3	/* Out: Line Control Register */
#define UART_MCR_OFFSET		4	/* Out: Modem Control Register */
#define UART_LSR_OFFSET		5	/* In:  Line Status Register */
#define UART_MSR_OFFSET		6	/* In:  Modem Status Register */
#define UART_SCR_OFFSET		7	/* I/O: Scratch Register */
#define UART_MDR1_OFFSET	8	/* I/O:  Mode Register */

#define UART_LSR_FIFOE		0x80	/* Fifo error */
#define UART_LSR_TEMT		0x40	/* Transmitter empty */
#define UART_LSR_THRE		0x20	/* Transmit-hold-register empty */
#define UART_LSR_BI		0x10	/* Break interrupt indicator */
#define UART_LSR_FE		0x08	/* Frame error indicator */
#define UART_LSR_PE		0x04	/* Parity error indicator */
#define UART_LSR_OE		0x02	/* Overrun error indicator */
#define UART_LSR_DR		0x01	/* Receiver data ready */
#define UART_LSR_BRK_ERROR_BITS	0x1E	/* BI, FE, PE, OE bits */

/* clang-format on */

static struct uart8250_device console_dev;

static u32 get_reg(struct uart8250_device *dev, u32 num)
{
	u32 offset = num << dev->reg_shift;

	if (dev->reg_width == 1)
		return readb(dev->base + offset);
	else if (dev->reg_width == 2)
		return readw(dev->base + offset);
	else
		return readl(dev->base + offset);
}

static void set_reg(struct uart8250_device *dev, u32 num, u32 val)
{
	u32 offset = num << dev->reg_shift;

	if (dev->reg_width == 1)
		writeb(val, dev->base + offset);
	else if (dev->reg_width == 2)
		writew(val, dev->base + offset);
	else
		writel(val, dev->base + offset);
}

void uart8250_putc(struct uart8250_device *dev, char ch)
{
	while ((get_reg(dev, UART_LSR_OFFSET) & UART_LSR_THRE) == 0)
		;

	set_reg(dev, UART_THR_OFFSET, ch);
}

int uart8250_getc(struct uart8250_device *dev)
{
	if (get_reg(dev, UART_LSR_OFFSET) & UART_LSR_DR)
		return get_reg(dev, UART_RBR_OFFSET);
	return -1;
}

static void uart8250_console_putc(char ch)
{
	uart8250_putc(&console_dev, ch);
}

static int uart8250_console_getc(void)
{
	return uart8250_getc(&console_dev);
}

static struct sbi_console_device uart8250_console = {
	.name = "uart8250",
	.console_putc = uart8250_console_putc,
	.console_getc = uart8250_console_getc
};

int uart8250_init(struct uart8250_device * dev, unsigned long base, u32 in_freq,
		  u32 baudrate, u32 reg_shift, u32 reg_width, u32 reg_offset)
{
	u16 bdiv = 0;

	dev->base      = (volatile char *)base + reg_offset;
	dev->reg_shift = reg_shift;
	dev->reg_width = reg_width;
	dev->in_freq   = in_freq;
	dev->baudrate  = baudrate;

	if (baudrate)
		bdiv = (in_freq + 8 * baudrate) / (16 * baudrate);

	/* Disable all interrupts */
	set_reg(dev, UART_IER_OFFSET, 0x00);
	/* Enable DLAB */
	set_reg(dev, UART_LCR_OFFSET, 0x80);

	if (bdiv) {
		/* Set divisor low byte */
		set_reg(dev, UART_DLL_OFFSET, bdiv & 0xff);
		/* Set divisor high byte */
		set_reg(dev, UART_DLM_OFFSET, (bdiv >> 8) & 0xff);
	}

	/* 8 bits, no parity, one stop bit */
	set_reg(dev, UART_LCR_OFFSET, 0x03);
	/* Enable FIFO */
	set_reg(dev, UART_FCR_OFFSET, 0x01);
	/* No modem control DTR RTS */
	set_reg(dev, UART_MCR_OFFSET, 0x00);
	/* Clear line status */
	get_reg(dev, UART_LSR_OFFSET);
	/* Read receive buffer */
	get_reg(dev, UART_RBR_OFFSET);
	/* Set scratchpad */
	set_reg(dev, UART_SCR_OFFSET, 0x00);

	return 0;
}

int uart8250_console_init(unsigned long base, u32 in_freq, u32 baudrate, u32 reg_shift,
		  u32 reg_width, u32 reg_offset)
{
	uart8250_init(&console_dev, base, in_freq, baudrate, reg_shift, reg_width, reg_offset);

	sbi_console_set_device(&uart8250_console);

	return sbi_domain_root_add_memrange(base, PAGE_SIZE, PAGE_SIZE,
					    (SBI_DOMAIN_MEMREGION_MMIO |
					    SBI_DOMAIN_MEMREGION_SHARED_SURW_MRW));
}
