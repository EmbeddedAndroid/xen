/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Driver for the UART function of a Qualcomm Generic Interface (GENI)
 * serial engine, as found on Qualcomm SoCs with a QUP v3 wrapper.
 *
 * Based on Linux drivers/tty/serial/qcom_geni_serial.c (v6.18) and U-Boot
 * drivers/serial/serial_msm_geni.c (v2025.10). No public manual exists.
 *
 * The driver runs the engine in FIFO mode with one character per FIFO word
 * and one TX transfer per character: a GENI TX transfer has a fixed length,
 * so characters cannot be added to a running transfer as with other UARTs.
 * The firmware is expected to have set up the clock, baud rate and word
 * length of the engine.
 *
 * There is no vUART support: the engine cannot be emulated with the
 * single status/data register model of vuart.c.
 */

#include <xen/console.h>
#include <xen/delay.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/mm.h>
#include <xen/serial.h>
#include <asm/device.h>
#include <asm/io.h>

/*
 * Registers, from Linux include/linux/soc/qcom/geni-se.h and
 * drivers/tty/serial/qcom_geni_serial.c (v6.18).
 */
#define GENI_FORCE_DEFAULT_REG          0x020
#define   FORCE_DEFAULT                 BIT(0, U)
#define SE_GENI_STATUS                  0x040
#define   M_GENI_CMD_ACTIVE             BIT(0, U)
#define SE_GENI_TX_PACKING_CFG0         0x260
#define SE_GENI_TX_PACKING_CFG1         0x264
#define SE_UART_TX_TRANS_LEN            0x270
#define SE_GENI_RX_PACKING_CFG0         0x284
#define SE_GENI_RX_PACKING_CFG1         0x288
#define SE_GENI_M_CMD0                  0x600
#define   M_OPCODE_SHIFT                27
#define   UART_START_TX                 0x1
#define SE_GENI_M_CMD_CTRL_REG          0x604
#define   M_GENI_CMD_ABORT              BIT(1, U)
#define SE_GENI_M_IRQ_STATUS            0x610
#define SE_GENI_M_IRQ_EN                0x614
#define SE_GENI_M_IRQ_CLEAR             0x618
#define   M_CMD_DONE_EN                 BIT(0, U)
#define   M_CMD_ABORT_EN                BIT(5, U)
#define   M_RX_FIFO_WATERMARK_EN        BIT(26, U)
#define   M_RX_FIFO_LAST_EN             BIT(27, U)
#define   M_TX_FIFO_WATERMARK_EN        BIT(30, U)
#define SE_GENI_S_CMD0                  0x630
#define   S_OPCODE_SHIFT                27
#define   UART_START_READ               0x1
#define SE_GENI_S_CMD_CTRL_REG          0x634
#define   S_GENI_CMD_ABORT              BIT(1, U)
#define SE_GENI_S_IRQ_STATUS            0x640
#define SE_GENI_S_IRQ_EN                0x644
#define SE_GENI_S_IRQ_CLEAR             0x648
#define   S_CMD_DONE_EN                 BIT(0, U)
#define   S_CMD_ABORT_EN                BIT(5, U)
#define   S_RX_FIFO_WATERMARK_EN        BIT(26, U)
#define   S_RX_FIFO_LAST_EN             BIT(27, U)
#define SE_GENI_TX_FIFOn                0x700
#define SE_GENI_RX_FIFOn                0x780
#define SE_GENI_RX_FIFO_STATUS          0x804
#define   RX_FIFO_WC                    0x01ffffff
#define SE_GENI_TX_WATERMARK_REG        0x80c

/* geni_se_config_packing(): 8 bits, one character per FIFO word, LSB first */
#define GENI_PACKING_CFG0_8BIT          0xf
/* TX watermark in FIFO words (Linux and U-Boot DEF_TX_WM) */
#define GENI_TX_WATERMARK               2

#define GENI_POLL_TIMEOUT_US    20000U
#define GENI_POLL_STEP_US       10U

static struct geni_uart {
    unsigned int irq;
    void __iomem *regs;
    struct irqaction irqaction;
} geni_com;

static uint32_t geni_read(const struct geni_uart *uart, unsigned int off)
{
    return readl(uart->regs + off);
}

static void geni_write(const struct geni_uart *uart, unsigned int off,
                       uint32_t val)
{
    writel(val, uart->regs + off);
}

static bool geni_poll_bit(const struct geni_uart *uart, unsigned int off,
                          uint32_t mask, bool set)
{
    unsigned int timeout_us = GENI_POLL_TIMEOUT_US;

    for ( ; ; )
    {
        if ( !!(geni_read(uart, off) & mask) == set )
            return true;
        if ( !timeout_us )
            return false;
        udelay(GENI_POLL_STEP_US);
        timeout_us -= GENI_POLL_STEP_US;
    }
}

static void geni_abort_tx(const struct geni_uart *uart)
{
    geni_write(uart, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
    geni_poll_bit(uart, SE_GENI_M_IRQ_STATUS, M_CMD_ABORT_EN, true);
    geni_write(uart, SE_GENI_M_IRQ_CLEAR, M_CMD_ABORT_EN | M_CMD_DONE_EN);
}

static void geni_arm_tx(const struct geni_uart *uart)
{
    geni_write(uart, SE_UART_TX_TRANS_LEN, 1);
    geni_write(uart, SE_GENI_M_CMD0, UART_START_TX << M_OPCODE_SHIFT);
}

static void geni_abort_rx(const struct geni_uart *uart)
{
    geni_write(uart, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_ABORT);
    geni_poll_bit(uart, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_ABORT, false);
    geni_write(uart, SE_GENI_S_IRQ_CLEAR, S_CMD_DONE_EN | S_CMD_ABORT_EN);
    geni_write(uart, GENI_FORCE_DEFAULT_REG, FORCE_DEFAULT);
}

static void geni_start_rx(const struct geni_uart *uart)
{
    geni_write(uart, SE_GENI_S_CMD0, UART_START_READ << S_OPCODE_SHIFT);
}

static void __init geni_uart_init_preirq(struct serial_port *port)
{
    struct geni_uart *uart = port->uart;

    /* Stop a transfer the firmware left running. */
    if ( geni_read(uart, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE )
        geni_abort_tx(uart);
    geni_write(uart, SE_GENI_M_IRQ_CLEAR,
               M_CMD_DONE_EN | M_CMD_ABORT_EN | M_TX_FIFO_WATERMARK_EN);

    geni_write(uart, SE_GENI_TX_PACKING_CFG0, GENI_PACKING_CFG0_8BIT);
    geni_write(uart, SE_GENI_TX_PACKING_CFG1, 0);
    geni_write(uart, SE_GENI_RX_PACKING_CFG0, GENI_PACKING_CFG0_8BIT);
    geni_write(uart, SE_GENI_RX_PACKING_CFG1, 0);
    geni_write(uart, SE_GENI_TX_WATERMARK_REG, GENI_TX_WATERMARK);

    geni_abort_rx(uart);
}

static void geni_uart_interrupt(int irq, void *data)
{
    struct serial_port *port = data;
    struct geni_uart *uart = port->uart;
    uint32_t m_status = geni_read(uart, SE_GENI_M_IRQ_STATUS);
    uint32_t s_status = geni_read(uart, SE_GENI_S_IRQ_STATUS);

    geni_write(uart, SE_GENI_M_IRQ_CLEAR, m_status);
    geni_write(uart, SE_GENI_S_IRQ_CLEAR, s_status);

    if ( s_status & (S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN) )
        serial_rx_interrupt(port);
}

static void __init geni_uart_init_postirq(struct serial_port *port)
{
    struct geni_uart *uart = port->uart;

    uart->irqaction.handler = geni_uart_interrupt;
    uart->irqaction.name = "geni-uart";
    uart->irqaction.dev_id = port;

    if ( setup_irq(uart->irq, 0, &uart->irqaction) )
    {
        printk(XENLOG_ERR "geni-uart: failed to request IRQ %u\n", uart->irq);
        return;
    }

    geni_abort_rx(uart);
    geni_write(uart, SE_GENI_S_IRQ_EN,
               S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN);
    geni_write(uart, SE_GENI_M_IRQ_EN,
               M_RX_FIFO_WATERMARK_EN | M_RX_FIFO_LAST_EN);
    geni_start_rx(uart);
}

/* One transfer per character, as U-Boot's msm_serial_putc() */
static void geni_uart_putc(struct serial_port *port, char c)
{
    struct geni_uart *uart = port->uart;

    geni_arm_tx(uart);
    geni_poll_bit(uart, SE_GENI_M_IRQ_STATUS, M_TX_FIFO_WATERMARK_EN, true);
    geni_write(uart, SE_GENI_TX_FIFOn, (unsigned char)c);
    geni_write(uart, SE_GENI_M_IRQ_CLEAR, M_TX_FIFO_WATERMARK_EN);

    if ( !geni_poll_bit(uart, SE_GENI_M_IRQ_STATUS, M_CMD_DONE_EN, true) )
        geni_abort_tx(uart);
    geni_write(uart, SE_GENI_M_IRQ_CLEAR,
               M_CMD_DONE_EN | M_TX_FIFO_WATERMARK_EN);
}

static int geni_uart_getc(struct serial_port *port, char *pc)
{
    struct geni_uart *uart = port->uart;

    if ( !(geni_read(uart, SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC) )
        return 0;

    /* One character per FIFO word; the read command stays active. */
    *pc = geni_read(uart, SE_GENI_RX_FIFOn) & 0xff;

    return 1;
}

static int __init geni_uart_irq(struct serial_port *port)
{
    const struct geni_uart *uart = port->uart;

    return uart->irq;
}

static struct uart_driver __read_mostly geni_uart_driver = {
    .init_preirq = geni_uart_init_preirq,
    .init_postirq = geni_uart_init_postirq,
    .putc = geni_uart_putc,
    .getc = geni_uart_getc,
    .irq = geni_uart_irq,
};

static int __init geni_uart_init(struct dt_device_node *dev, const void *data)
{
    const char *config = data;
    struct geni_uart *uart = &geni_com;
    const struct dt_device_node *parent;
    struct dt_device_node *np;
    paddr_t addr, size;
    int res;

    if ( strcmp(config, "") )
        printk("geni-uart: UART configuration is not supported\n");

    res = dt_device_get_paddr(dev, 0, &addr, &size);
    if ( res )
    {
        printk("geni-uart: unable to retrieve the base address\n");
        return res;
    }

    res = platform_get_irq(dev, 0);
    if ( res < 0 )
    {
        printk("geni-uart: unable to retrieve the IRQ\n");
        return res;
    }
    uart->irq = res;

    uart->regs = ioremap_nocache(addr, size);
    if ( !uart->regs )
    {
        printk("geni-uart: unable to map the UART memory\n");
        return -ENOMEM;
    }

    serial_register_uart(SERHND_DTUART, &geni_uart_driver, uart);

    dt_device_set_used_by(dev, DOMID_XEN);

    /*
     * The other protocol nodes of this serial engine (I2C, SPI), usually
     * disabled, describe the same registers and interrupt: keep them away
     * from the domains as well.
     */
    parent = dt_get_parent(dev);
    if ( parent )
    {
        dt_for_each_child_node ( parent, np )
        {
            paddr_t np_addr, np_size;

            if ( np != dev &&
                 !dt_device_get_paddr(np, 0, &np_addr, &np_size) &&
                 np_addr == addr )
                dt_device_set_used_by(np, DOMID_XEN);
        }
    }

    return 0;
}

static const struct dt_device_match geni_uart_dt_match[] __initconst =
{
    DT_MATCH_COMPATIBLE("qcom,geni-debug-uart"),
    { /* sentinel */ },
};

DT_DEVICE_START(geni_uart, "Qualcomm GENI UART", DEVICE_SERIAL)
    .dt_match = geni_uart_dt_match,
    .init = geni_uart_init,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
