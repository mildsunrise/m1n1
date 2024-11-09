/* SPDX-License-Identifier: MIT */

#include "tps6598x.h"
#include "adt.h"
#include "i2c.h"
#include "spmi.h"
#include "iodev.h"
#include "malloc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define TPS_REG_MODE        0x03
#define TPS_REG_CMD1        0x08
#define TPS_REG_DATA1       0x09
#define TPS_REG_INT_EVENT1  0x14
#define TPS_REG_INT_MASK1   0x16
#define TPS_REG_INT_CLEAR1  0x18
#define TPS_REG_POWER_STATE 0x20
#define TPS_CMD_INVALID     0x444d4321 // !CMD as LE u32
#define TPS_MODE_DBMA       ((u32)'D' | ((u32)'B' << 8) | ((u32)'M' << 16) | ((u32)'a' << 24))

struct tps6598x_dev {
    bool is_spmi;
    union {
        i2c_dev_t *i2c;
        spmi_dev_t *spmi;
    };
    u8 addr;
};

static int tps6598x_spmi_select(tps6598x_dev_t *dev, u8 reg)
{
    int status;

    for (size_t attempt = 0; attempt < 5; attempt++) {
        if ((status = spmi_reg0_write(dev->spmi, dev->addr, reg)) == -SPMI_ERR_BUS_IO)
            continue;
        if (status < 0)
            return status;

        for (size_t i = 0; i < 50; i++) {
            u8 got;
            if ((status = spmi_ext_read(dev->spmi, dev->addr, 0, &got, 1)) == -SPMI_ERR_BUS_IO)
                continue;
            if (status < 0)
                return status;

            if ((got & MASK(7)) != reg) {
                printf("tps6598x: reg0 write succeeded but read returned 0x%x, retrying\n", got);
                break;
            }
            if (got == reg)
                return 0;
            udelay(100);
        }
    }

    return -1;
}

static int tps6598x_spmi_select_checked(tps6598x_dev_t *dev, u8 reg, size_t len)
{
    if (tps6598x_spmi_select(dev, reg) < 0)
        return -1;

    u8 reg_size;
    if (spmi_ext_read(dev->spmi, dev->addr, 0x1F, &reg_size, 1) < 0)
        return -1;

    if (len > reg_size) {
        printf("tps6598x: length of register 0x%x is %u (expected at least %lu)\n", reg, reg_size, len);
        return -1;
    }
    return 0;
}

static int tps6598x_read(tps6598x_dev_t *dev, u8 reg, u8 *bfr, size_t len)
{
    if (!dev->is_spmi)
        return i2c_smbus_read(dev->i2c, dev->addr, reg, bfr, len) == (int)len ? -1 : 0;

    if (tps6598x_spmi_select_checked(dev, reg, len) < 0)
        return -1;

    u8 addr = 0x20;
    while (len) {
        size_t block = min(len, 16);
        if (spmi_ext_read(dev->spmi, dev->addr, addr, bfr, block) < 0)
            return -1;
        addr += block, bfr += block, len -= block;
    }

    return 0;
}

static int tps6598x_write(tps6598x_dev_t *dev, u8 reg, const u8 *bfr, size_t len)
{
    if (!dev->is_spmi)
        return i2c_smbus_write(dev->i2c, dev->addr, reg, bfr, len) == (int)len ? -1 : 0;

    if (tps6598x_spmi_select_checked(dev, reg, len) < 0)
        return -1;

    u8 addr = 0xa0;
    while (len) {
        size_t block = min(len, 16);
        if (spmi_ext_write(dev->spmi, dev->addr, addr, bfr, block) < 0)
            return -1;
        addr += block, bfr += block, len -= block;
    }

    // re-select the register to issue the write
    if (tps6598x_spmi_select(dev, reg) < 0)
        return -1;
    return 0;
}

static int tps6598x_wakeup(tps6598x_dev_t *dev)
{
    int status;

    if (!dev->is_spmi)
        // not implemented, but so far we haven't seen any device
        // with an I2C HPM supporting wake/sleep
        return -1;

    if (spmi_send_wakeup(dev->spmi, dev->addr) < 0)
        return -1;

    // wait for it to wake up by testing that it responds to writes
    u8 target;
    if (spmi_ext_read(dev->spmi, dev->addr, 0, &target, 1) < 0)
        return -1;
    target ^= 1;
    for (size_t attempt = 0; attempt < 50; attempt++) {
        if ((status = spmi_ext_write(dev->spmi, dev->addr, 0, &target, 1)) == -SPMI_ERR_BUS_IO)
            continue;
        if (status < 0)
            return status;
        u8 got;
        if ((status = spmi_ext_read(dev->spmi, dev->addr, 0, &got, 1)) == -SPMI_ERR_BUS_IO)
            continue;
        if (status < 0)
            return status;
        if (got == target)
            return 0;
        mdelay(1);
    }

    printf("tps6598x: Timeout waiting for device to wake up\n");
    return -1;
}

tps6598x_dev_t *tps6598x_init_i2c(const char *adt_node, i2c_dev_t *i2c)
{
    int adt_offset;
    adt_offset = adt_path_offset(adt, adt_node);
    if (adt_offset < 0) {
        printf("tps6598x: Error getting %s node\n", adt_node);
        return NULL;
    }

    const u8 *iic_addr = adt_getprop(adt, adt_offset, "hpm-iic-addr", NULL);
    if (iic_addr == NULL) {
        printf("tps6598x: Error getting %s hpm-iic-addr\n.", adt_node);
        return NULL;
    }

    tps6598x_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;

    dev->is_spmi = false;
    dev->i2c = i2c;
    dev->addr = *iic_addr;
    return dev;
}

tps6598x_dev_t *tps6598x_init_spmi(const char *adt_node, spmi_dev_t *spmi)
{
    int adt_offset;
    adt_offset = adt_path_offset(adt, adt_node);
    if (adt_offset < 0) {
        printf("tps6598x: Error getting %s node\n", adt_node);
        return NULL;
    }

    u32 spmi_addr_len;
    const u8 *spmi_addr = adt_getprop(adt, adt_offset, "reg", &spmi_addr_len);
    if (spmi_addr == NULL || spmi_addr_len < 1) {
        printf("tps6598x: Error getting %s spmi address\n", adt_node);
        return NULL;
    }

    tps6598x_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;

    dev->is_spmi = true;
    dev->spmi = spmi;
    dev->addr = *spmi_addr;

    if (tps6598x_wakeup(dev) < 0) {
        printf("tps6598x: Failed to wake up SPMI device %s\n", adt_node);
        tps6598x_shutdown(dev);
        return NULL;
    }

    return dev;
}

void tps6598x_shutdown(tps6598x_dev_t *dev)
{
    free(dev);
}

int tps6598x_command(tps6598x_dev_t *dev, const char *cmd, const u8 *data_in, size_t len_in,
                     u8 *data_out, size_t len_out)
{
    if (len_in) {
        if (tps6598x_write(dev, TPS_REG_DATA1, data_in, len_in) < 0)
            return -1;
    }

    if (tps6598x_write(dev, TPS_REG_CMD1, (const u8 *)cmd, 4) < 0)
        return -1;

    u32 cmd_status;
    do {
        if (tps6598x_read(dev, TPS_REG_CMD1, (u8 *)&cmd_status, 4) < 0)
            return -1;
        if (cmd_status == TPS_CMD_INVALID)
            return -1;
        udelay(100);
    } while (cmd_status != 0);

    if (len_out) {
        if (tps6598x_read(dev, TPS_REG_DATA1, data_out, len_out) < 0)
            return -1;
    }

    return 0;
}

int tps6598x_cmd_status(tps6598x_dev_t *dev, const char *cmd)
{
    u32 cmd_status;

    if (tps6598x_read(dev, TPS_REG_CMD1, (u8 *)&cmd_status, 4)) {
        printf("tps6598x: tps6598x_read cmd: %s failed\n", cmd);
        return -1;
    }
    if (cmd_status == TPS_CMD_INVALID) {
        printf("tps6598x: tps6598x_read cmd: %s status invalid\n", cmd);
        return -1;
    }
    if (cmd_status) {
        printf("tps6598x: tps6598x_read cmd: %s status 0x%x\n", cmd, cmd_status);
        return -1;
    }

    return 0;
}

int tps6598x_disable_irqs(tps6598x_dev_t *dev, tps6598x_irq_state_t *state)
{
    static const u8 zeros[CD3218B12_IRQ_WIDTH] = {0x00};
    static const u8 ones[CD3218B12_IRQ_WIDTH] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                 0xFF, 0xFF, 0xFF, 0xFF};

    // store IntEvent 1 to restore it later
    if (tps6598x_read(dev, TPS_REG_INT_MASK1, state->int_mask1, sizeof(state->int_mask1)) < 0) {
        printf("tps6598x: reading TPS_REG_INT_MASK1 failed\n");
        return -1;
    }
    state->valid = 1;

    // mask interrupts and ack all interrupt flags
    if (tps6598x_write(dev, TPS_REG_INT_CLEAR1, ones, sizeof(ones)) < 0) {
        printf("tps6598x: writing TPS_REG_INT_CLEAR1 failed\n");
        return -1;
    }
    if (tps6598x_write(dev, TPS_REG_INT_MASK1, zeros, sizeof(zeros)) < 0) {
        printf("tps6598x: writing TPS_REG_INT_MASK1 failed\n");
        return -1;
    }

#ifdef DEBUG
    u8 tmp[CD3218B12_IRQ_WIDTH] = {0x00};
    if (tps6598x_read(dev, TPS_REG_INT_MASK1, tmp, CD3218B12_IRQ_WIDTH) < 0)
        printf("tps6598x: failed verification, can't read TPS_REG_INT_MASK1\n");
    else {
        printf("tps6598x: verify: TPS_REG_INT_MASK1 vs. saved IntMask1\n");
        hexdump(tmp, sizeof(tmp));
        hexdump(state->int_mask1, sizeof(state->int_mask1));
    }
#endif
    return 0;
}

int tps6598x_restore_irqs(tps6598x_dev_t *dev, tps6598x_irq_state_t *state)
{
    if (tps6598x_write(dev, TPS_REG_INT_MASK1, state->int_mask1, sizeof(state->int_mask1)) < 0) {
        printf("tps6598x: restoring TPS_REG_INT_MASK1 failed\n");
        return -1;
    }

#ifdef DEBUG
    u8 tmp[CD3218B12_IRQ_WIDTH];
    if (tps6598x_read(dev, TPS_REG_INT_MASK1, tmp, sizeof(tmp)) < 0)
        printf("tps6598x: failed verification, can't read TPS_REG_INT_MASK1\n");
    else {
        printf("tps6598x: verify saved IntMask1 vs. TPS_REG_INT_MASK1:\n");
        hexdump(state->int_mask1, sizeof(state->int_mask1));
        hexdump(tmp, sizeof(tmp));
    }
#endif

    return 0;
}

int tps6598x_powerup(tps6598x_dev_t *dev)
{
    u8 power_state;

    if (tps6598x_read(dev, TPS_REG_POWER_STATE, &power_state, 1) < 0)
        return -1;

    if (power_state == 0)
        return 0;

    const u8 data = 0;
    tps6598x_command(dev, "SSPS", &data, 1, NULL, 0);

    if (tps6598x_read(dev, TPS_REG_POWER_STATE, &power_state, 1) < 0)
        return -1;

    if (power_state != 0)
        return -1;

    return 0;
}

int tps6598x_enter_kis(tps6598x_dev_t *dev)
{
    u32 target_len = 0;
    const u8 *target = adt_getprop(adt, 0, "target-type", &target_len);
    u8 key[4] = {0};
    const u8 key_null[4] = {0, 0, 0, 0};
    const u8 vdm[] = {0x06, 0x46, 0x82, 0x01};
    u32 mode = 0;
    u8 out = 0;
    u8 en = 1;
    int ret;

    if (target_len < 4)
        return -1;

    // reverse the key
    for (int i = 0; i < 4; i++)
        key[i] = target[3 - i];

    // check status and soft reset if it fails
    if (tps6598x_cmd_status(dev, "LOCK")) {
        tps6598x_command(dev, "Gaid", NULL, 0, NULL, 0);
        mdelay(20);
    }

    ret = tps6598x_command(dev, "LOCK", key, 4, (u8 *)&out, 1);
    if (ret || (out & 0xf)) {
        printf("tps6598x_enter_kis: Failed to unlock using '%.4s': ret=%d result=0x%hhx\n", key,
               ret, out & 0xf);
        return -1;
    }

    ret = tps6598x_command(dev, "DBMa", &en, 1, &out, 1);
    if (ret || (out & 0xf)) {
        printf("tps6598x_enter_kis: DBMa cmd failed: ret=%d result=0x%hhx\n", ret, out & 0xf);
        return -1;
    }

    ret = tps6598x_read(dev, TPS_REG_MODE, (u8 *)&mode, 4);
    if (mode != TPS_MODE_DBMA) {
        printf("tps6598x_enter_kis: Failed to enter DBMa mode, mode=0x%08x\n", mode);
        return -1;
    }

    ret = tps6598x_command(dev, "DVEn", vdm, sizeof(vdm), &out, 1);
    if (ret || (out & 0xf)) {
        printf("tps6598x_enter_kis: DVEn cmd failed: ret=%d result=0x%hhx\n", ret, out & 0xf);
        return -1;
    }

    en = 0;
    tps6598x_command(dev, "DBMa", &en, 1, NULL, 0);
    tps6598x_command(dev, "LOCK", key_null, 4, NULL, 0);

    return ret;
}

int tps6598x_enable_debugusb(void)
{
    char hpm_path[64] = {0};
    char i2c_path[64] = {0};
    bool found = false;
    int node;
    int ret;

    node = adt_path_offset(adt, "/arm-io");
    if (node < 0)
        return -1;

    ADT_FOREACH_CHILD(adt, node)
    {
        int mngr_node;

        if (!adt_is_compatible(adt, node, "i2c,s5l8940x"))
            continue;

        mngr_node = adt_first_child_offset(adt, node);
        if (mngr_node < 0 || !adt_is_compatible(adt, mngr_node, "usbc,manager"))
            continue;

        int it = mngr_node;
        ADT_FOREACH_CHILD(adt, it)
        {
            if (!adt_is_compatible(adt, it, "usbc,cd3217"))
                continue;

            const char *name = adt_get_name(adt, it);
            if (strcmp(name, "hpm0"))
                continue;

            ret = snprintf(i2c_path, sizeof(i2c_path), "/arm-io/%s", adt_get_name(adt, node));
            if (ret < 0 || (size_t)ret >= sizeof(i2c_path))
                continue;
            ret = snprintf(hpm_path, sizeof(hpm_path), "/arm-io/%s/%s/%s", adt_get_name(adt, node),
                           adt_get_name(adt, mngr_node), name);
            if (ret < 0 || (size_t)ret >= sizeof(hpm_path))
                continue;

            found = true;
        }
        if (found)
            break;
    }
    if (!found) {
        printf("tps6598x_enable_debugusb: i2c / hpm node not found\n");
        return -1;
    }

    printf("tps6598x: enable debugusb for %s\n", hpm_path);

    i2c_dev_t *i2c = i2c_init(i2c_path);
    if (!i2c) {
        printf("tps6598x_enable_debugusb: i2c_init failed for %s.\n", i2c_path);
        return -1;
    }

    tps6598x_dev_t *tps = tps6598x_init_i2c(hpm_path, i2c);
    if (!tps) {
        printf("tps6598x_enable_debugusb: tps6598x_init failed for %s.\n", hpm_path);
        return -1;
    }

    if (tps6598x_powerup(tps) < 0) {
        printf("tps6598x_enable_debugusb: tps6598x_powerup failed for %s.\n", hpm_path);
        tps6598x_shutdown(tps);
        return -1;
    }

    tps6598x_enter_kis(tps);

    tps6598x_shutdown(tps);

    i2c_shutdown(i2c);

    return 0;
}
