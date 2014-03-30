/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of The Linux Foundation nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <fs_mgr.h>
#include "bootselect.h"
#include "bootloader.h"
#include "common.h"
#include "roots.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <sys/ioctl.h>

#include <linux/major.h>
#include <linux/mmc/core.h>
#include <linux/mmc/ioctl.h>
#include <linux/mmc/mmc.h>


static int set_bootselect_info_rw(const struct boot_selection_info *in);

int get_bootselect_info(struct boot_selection_info *out) {
    Volume* v = volume_for_path("/bootselect");
    if (v == NULL) {
      LOGE("Cannot load volume /bootselect!\n");
      return -1;
    }
    if (strcmp(v->fs_type, "emmc") == 0) {
        wait_for_device(v->blk_device);
        FILE* f = fopen(v->blk_device, "rb");
        if (f == NULL) {
            LOGE("Can't open %s\n(%s)\n", v->blk_device, strerror(errno));
            return -1;
        }
        int count = fread(out, sizeof(*out), 1, f);
        fclose(f);
        if (count != 1) {
            LOGE("Failed reading %s\n(%s)\n", v->blk_device, strerror(errno));
            return -1;
        }
        return 0;
    }
    LOGE("unknown /bootselect fs_type \"%s\"\n", v->fs_type);
    return -1;
}

int clear_format_bit() {
    struct boot_selection_info in;
    if(get_bootselect_info(&in)) {
        LOGE("Failed to get the message from /bootselect!\n");
        return -1;
    }
    /* Get the 31st bit out of the StateInfo which actually
     * indicates whether to format or not
     */
    in.state_info = ((in.state_info) & (uint32_t)(~(1 << 31)));
    return set_bootselect_info_rw(&in);
}

static int get_partition_offset(const Volume *v, uint32_t *offset) {
    const char *parent_path = "/sys/block/mmcblk0";
    char *path, *dev_path;
    char *partition = NULL;
    char *save_ptr = NULL;
    uint32_t partn_addr = 0;
    FILE *f;
    /* Read the actual device path from the symbolic link
     * realpath() internally allocates memory which we
     * will free after use
     */
    if ((dev_path = realpath(v->blk_device, NULL)) == NULL) {
        LOGD("Failed to get the sysfs node of %s\n", blk_device);
        return -1;
    }
    // Get the partition from the device path
    if (strtok_r(dev_path, "/", &save_ptr) != NULL) {
        while ((partition = strtok_r(NULL, "/", &save_ptr)) != NULL)
            if (!save_ptr)
                break;
    }
    // get the starting physical address from the sysfs file i.e.start
    if (partition) {
        path = (char *) malloc(MAX_PATH_LEN);
        if (path == NULL) {
            LOGE("Failed to get allocation.\n");
            return -1;
        }
        snprintf(path, MAX_PATH_LEN, "%s/%s/%s", parent_path, partition, "start");
        LOGI("partition path is: %s\n", path);
        free(dev_path);
        f = fopen(path, "rb");
        free(path);
        if (!f) {
            LOGE("Failed opening %s\n(%s)\n", path, strerror(errno));
            return -1;
        }
        if (!fscanf(f, "%u", offset)) {
            LOGE("Failed to get the content of the %s\n", path);
            fclose(f);
            return -1;
        }
        fclose(f);
        return 0;
    }
    LOGE("Failed to get the partition path out of the sysfs entry\n");
    return -1;

}

static int set_bootselect_info_rw(const struct boot_selection_info *in) {
    int fd, ret;
    uint32_t offset;
    struct mmc_ioc_cmd idata;
    const uint32_t BLK_SIZE = 512;
    char *data;
    const char* DEVFS = "/dev/block/mmcblk0";
    Volume* v = volume_for_path("/bootselect");
    if (v == NULL) {
      LOGE("Cannot load volume /bootselect!\n");
      return -1;
    }
    data = (char *) malloc(BLK_SIZE);
    if (data == NULL) {
        LOGE("Failed to get allocation for data to be written on bootselect!\n");
        return -1;
    }
    // Making block zeroed
    memset(data, 0, BLK_SIZE);
    memset(&idata, 0, sizeof(idata));
    if (sizeof(*in) > BLK_SIZE) {
        LOGE("Invalid structure passed to clear the format bit\n");
        return -1;
    }
    // Copy the structure to the zeroed block
    memcpy(data, in, sizeof(*in));

    wait_for_device(v->blk_device);

    // Get the physical starting address of the partition
    if (get_partition_offset(v, &offset)) {
        LOGE("Error in getting offset of the partition\n");
        free(data);
        return -1;
    }

    fd = open(DEVFS, O_RDWR);

    if (fd < 0) {
        LOGE("Error in opening file\n");
        free(data);
        return -1;
    }
    // Prepare to set reliable write bit
    idata.opcode = MMC_SET_BLOCK_COUNT;
    idata.arg = 1 | (1 << 31);      // For reliable write
    idata.flags = MMC_RSP_R1 | MMC_CMD_AC;

    ret = ioctl(fd, MMC_IOC_CMD, &idata);
    if (ret) {
        LOGE("Error sending ioctl (error no: %d) for MMC_SET_BLOCK_COUNT (response: %d)\n",
                errno, idata.response[0]);
        close(fd);
        free(data);
        return -1;
    }

    // Prepare to write actual data
    idata.write_flag = true;
    idata.opcode = MMC_WRITE_BLOCK;
    idata.arg = offset;
    idata.flags = MMC_RSP_R1 | MMC_CMD_ADTC;
    idata.blksz = BLK_SIZE;
    idata.blocks = 1;       //Suggested by MMC team

    mmc_ioc_cmd_set_data(idata, data);

    ret = ioctl(fd, MMC_IOC_CMD, &idata);
    close(fd);
    free(data);
    if (ret) {
        LOGE("Error sending ioctl (error no: %d) for MMC_WRITE_BLOCK (response: %d)\n",
                errno, idata.response[0]);
        return -1;
    }
    return 0;
}
