/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config/config.h>
#include <drivers/serial.h>

int bcm283x_mu_putc(char c);
char bcm283x_mu_getc(void);
int bcm283x_mu_init(void *base, int clk, int baud);

int mvebu_serial_probe(void *addr);
void serial_mvebu_putc(char ch);
char serial_mvebu_getc(void);

int pl011_init(void *addr, int clock, int baud);
void serial_pl011_putc(char ch);
char serial_pl011_getc(void);

int serial_init(void)
{
#ifdef CONFIG_PLATFORM_ARMADA
	return mvebu_serial_probe((void *)0xd0012000);
#endif

#ifdef CONFIG_PLATFORM_FVP
	return  pl011_init((void *)0x1c090000, 24000000, 115200);
#endif

#ifdef CONFIG_PLATFORM_RASPBERRY3
	return bcm283x_mu_init((void *)0x3f215040, 250000000, 115200);
#endif

#ifdef CONFIG_PLATFORM_RASPBERRY4
	return bcm283x_mu_init((void *)0xfe215040, 250000000, 115200);
#endif

	return 0;
}

void serial_putc(char ch)
{
#ifdef CONFIG_PLATFORM_ARMADA
	serial_mvebu_putc(ch);
#endif

#ifdef CONFIG_PLATFORM_FVP
	serial_pl011_putc(ch);
#endif

#if defined(CONFIG_PLATFORM_RASPBERRY3) || defined(CONFIG_PLATFORM_RASPBERRY4)
	bcm283x_mu_putc(ch);
#endif
}

char serial_getc(void)
{
#ifdef CONFIG_PLATFORM_ARMADA
	return serial_mvebu_getc();
#endif

#ifdef CONFIG_PLATFORM_FVP
	return serial_pl011_getc();
#endif

#if defined(CONFIG_PLATFORM_RASPBERRY3) || defined(CONFIG_PLATFORM_RASPBERRY4)
	return bcm283x_mu_getc();
#endif
	return 0;
}
