// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <string.h>
#include <stdbool.h>
#include "esp_partition.h"
#include "sdkconfig.h"
#include "esp_core_dump_priv.h"
#include "core_dump_elf.h"

const static DRAM_ATTR char TAG[] __attribute__((unused)) = "esp_core_dump_common";

#if CONFIG_ESP32_COREDUMP_DATA_FORMAT_BIN

static inline uint32_t esp_core_dump_get_tcb_len()
{
    if (COREDUMP_TCB_SIZE % sizeof(uint32_t)) {
        return ((COREDUMP_TCB_SIZE / sizeof(uint32_t) + 1) * sizeof(uint32_t));
    }
    return COREDUMP_TCB_SIZE;
}

static inline uint32_t esp_core_dump_get_stack_len(uint32_t stack_start, uint32_t stack_end)
{
    uint32_t len = stack_end - stack_start;
    // Take stack padding into account
    return (len + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
}

static esp_err_t esp_core_dump_save_task(core_dump_write_config_t *write_cfg,
                                                core_dump_task_header_t *task)
{
    esp_err_t err = ESP_FAIL;
    uint32_t stk_vaddr, stk_len;
    uint32_t stk_paddr = esp_core_dump_get_stack(task, &stk_vaddr, &stk_len);

    stk_len = esp_core_dump_get_stack_len(stk_vaddr, stk_vaddr+stk_len);

    // Save TCB address, stack base and stack top addr
    err = write_cfg->write(write_cfg->priv, (void*)task, sizeof(core_dump_task_header_t));
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("Failed to write task header, error=%d!", err);
        return err;
    }
    // Save TCB block
    err = write_cfg->write(write_cfg->priv, task->tcb_addr, esp_core_dump_get_tcb_len());
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("Failed to write TCB, error=%d!", err);
        return err;
    }
    // Save task stack
    err = write_cfg->write(write_cfg->priv, (void*)stk_paddr, stk_len);
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("Failed to write stack for task (TCB:%x), stack_start=%x, error=%d!",
                                task->tcb_addr,
                                stk_vaddr,
                                err);
        return err;
    }

    ESP_COREDUMP_LOG_PROCESS("Task (TCB:%x) dump is saved.",
                                    task->tcb_addr);
    return ESP_OK;
}

static esp_err_t esp_core_dump_save_mem_segment(core_dump_write_config_t* write_cfg,
                                                core_dump_mem_seg_header_t* seg)
{
    esp_err_t err = ESP_FAIL;

    if (!esp_core_dump_mem_seg_is_sane(seg->start, seg->size)) {
        ESP_COREDUMP_LOGE("Failed to write memory segment, (%x, %lu)!",
                                seg->start, seg->size);
        return ESP_FAIL;
    }
    // Save TCB address, stack base and stack top addr
    err = write_cfg->write(write_cfg->priv, (void*)seg, sizeof(core_dump_mem_seg_header_t));
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("Failed to write memory segment header, error=%d!", err);
        return err;
    }
    // Save memory contents
    err = write_cfg->write(write_cfg->priv, (void*)seg->start, seg->size);
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("Failed to write memory segment, (%x, %lu), error=%d!",
                                seg->start, seg->size, err);
        return err;
    }
    ESP_COREDUMP_LOG_PROCESS("Memory segment (%x, %lu) is saved.",
                                seg->start, seg->size);
    return ESP_OK;
}

static esp_err_t esp_core_dump_write_binary(void *frame, core_dump_write_config_t *write_cfg)
{
    esp_err_t err;
    static core_dump_task_header_t tasks[CONFIG_ESP32_CORE_DUMP_MAX_TASKS_NUM];
    uint32_t task_num, tcb_sz = esp_core_dump_get_tcb_len();
    uint32_t data_len = 0, task_id;
    int curr_task_index = COREDUMP_CURR_TASK_NOT_FOUND;
    core_dump_header_t hdr;
    core_dump_mem_seg_header_t interrupted_task_stack;

    task_num = esp_core_dump_get_tasks_snapshot(tasks, CONFIG_ESP32_CORE_DUMP_MAX_TASKS_NUM);
    ESP_COREDUMP_LOGI("Found tasks: %d!", task_num);

    // Verifies all tasks in the snapshot
    for (task_id = 0; task_id < task_num; task_id++) {
        bool is_current_task = false, stack_is_valid = false;
        bool tcb_is_valid = esp_core_dump_check_task(frame, &tasks[task_id], &is_current_task, &stack_is_valid);
        // Check if task tcb or stack is corrupted
        if (!tcb_is_valid || !stack_is_valid) {
            // If tcb or stack for task is corrupted count task as broken
            write_cfg->bad_tasks_num++;
        }
        if (is_current_task) {
            curr_task_index = task_id; // save current crashed task index in the snapshot
            ESP_COREDUMP_LOG_PROCESS("Task #%d (TCB:%x) is first crashed task.",
                                        task_id,
                                        tasks[task_id].tcb_addr);
        }
        // Increase core dump size by task stack size
        uint32_t stk_vaddr, stk_len;
        esp_core_dump_get_stack(&tasks[task_id], &stk_vaddr, &stk_len);
        data_len += esp_core_dump_get_stack_len(stk_vaddr, stk_vaddr+stk_len);
        // Add tcb size
        data_len += (tcb_sz + sizeof(core_dump_task_header_t));
    }

    if (esp_core_dump_in_isr_context()) {
        interrupted_task_stack.start = tasks[curr_task_index].stack_start;
        interrupted_task_stack.size = esp_core_dump_get_stack_len(tasks[curr_task_index].stack_start, tasks[curr_task_index].stack_end);
        // size of the task's stack has been already taken into account, also addresses have also been checked
        data_len += sizeof(core_dump_mem_seg_header_t);
        tasks[curr_task_index].stack_start = (uint32_t)frame;
        tasks[curr_task_index].stack_end = esp_core_dump_get_isr_stack_end();
        ESP_COREDUMP_LOG_PROCESS("Add ISR stack %lu to %lu", tasks[curr_task_index].stack_end - tasks[curr_task_index].stack_start, data_len);
        // take into account size of the ISR stack
        data_len += esp_core_dump_get_stack_len(tasks[curr_task_index].stack_start, tasks[curr_task_index].stack_end);
    }

    // Check if current task TCB is broken
    if (curr_task_index == COREDUMP_CURR_TASK_NOT_FOUND) {
         ESP_COREDUMP_LOG_PROCESS("The current crashed task is broken.");
         curr_task_index = 0;
    }

    // Add core dump header size
    data_len += sizeof(core_dump_header_t);

    ESP_COREDUMP_LOG_PROCESS("Core dump length=%lu, tasks processed: %d, broken tasks: %d",
                                data_len, task_num, write_cfg->bad_tasks_num);
    // Prepare write
    if (write_cfg->prepare) {
        err = write_cfg->prepare(write_cfg->priv, &data_len);
        if (err != ESP_OK) {
            ESP_COREDUMP_LOGE("Failed to prepare core dump, error=%d!", err);
            return err;
        }
    }

    // Write start
    if (write_cfg->start) {
        err = write_cfg->start(write_cfg->priv);
        if (err != ESP_OK) {
            ESP_COREDUMP_LOGE("Failed to start core dump, error=%d!", err);
            return err;
        }
    }

    // Write header
    hdr.data_len  = data_len;
    hdr.version   = COREDUMP_VERSION;
    hdr.tasks_num = task_num; // save all the tasks in snapshot even broken
    hdr.mem_segs_num = 0;
    if (xPortInterruptedFromISRContext()) {
        hdr.mem_segs_num++; // stack of interrupted task
    }
    hdr.tcb_sz    = tcb_sz;
    err = write_cfg->write(write_cfg->priv, &hdr, sizeof(core_dump_header_t));
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("Failed to write core dump header error=%d!", err);
        return err;
    }

    // Write first crashed task data first (not always first task in the snapshot)
    err = esp_core_dump_save_task(write_cfg, &tasks[curr_task_index]);
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("Failed to save first crashed task #%d (TCB:%x), error=%d!",
                            curr_task_index, tasks[curr_task_index].tcb_addr, err);
        return err;
    }

    // Write all other tasks in the snapshot
    for (task_id = 0; task_id < task_num; task_id++) {
        // Skip first crashed task
        if (task_id == curr_task_index) {
            continue;
        }
        err = esp_core_dump_save_task(write_cfg, &tasks[task_id]);
        if (err != ESP_OK) {
            ESP_COREDUMP_LOGE("Failed to save core dump task #%d (TCB:%x), error=%d!",
                                    task_id, tasks[curr_task_index].tcb_addr, err);
            return err;
        }
    }
    if (xPortInterruptedFromISRContext()) {
        err = esp_core_dump_save_mem_segment(write_cfg, &interrupted_task_stack);
        if (err != ESP_OK) {
            ESP_COREDUMP_LOGE("Failed to save interrupted task stack, error=%d!", err);
            return err;
        }
    }

    // Write end
    if (write_cfg->end) {
        err = write_cfg->end(write_cfg->priv);
        if (err != ESP_OK) {
            ESP_COREDUMP_LOGE("Failed to end core dump error=%d!", err);
            return err;
        }
    }
    if (write_cfg->bad_tasks_num) {
        ESP_COREDUMP_LOGE("Found %d broken tasks!", write_cfg->bad_tasks_num);
    }
    return err;
}
#endif

inline void esp_core_dump_write(void *frame, core_dump_write_config_t *write_cfg)
{
    esp_core_dump_setup_stack();

#ifndef CONFIG_ESP32_ENABLE_COREDUMP_TO_NONE
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_ESP32_COREDUMP_DATA_FORMAT_BIN
    err = esp_core_dump_write_binary(frame, write_cfg);
#elif CONFIG_ESP32_COREDUMP_DATA_FORMAT_ELF
    err = esp_core_dump_write_elf(frame, write_cfg);
#endif
    if (err != ESP_OK) {
        ESP_COREDUMP_LOGE("Core dump write binary failed with error=%d", err);
    }
#endif

    esp_core_dump_report_stack_usage();
}

void esp_core_dump_init(void)
{
#if CONFIG_ESP32_ENABLE_COREDUMP_TO_FLASH
    esp_core_dump_flash_init();
#endif
#if CONFIG_ESP32_ENABLE_COREDUMP_TO_UART
    ESP_COREDUMP_LOGI("Init core dump to UART");
#endif
}

esp_err_t esp_core_dump_image_get(size_t* out_addr, size_t *out_size)
{
    esp_err_t err;
    const void *core_data;
    spi_flash_mmap_handle_t core_data_handle;

    if (out_addr == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *core_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                                                NULL);
    if (!core_part) {
        ESP_LOGE(TAG, "No core dump partition found!");
        return ESP_ERR_NOT_FOUND;
    }
    if (core_part->size < sizeof(uint32_t)) {
        ESP_LOGE(TAG, "Too small core dump partition!");
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_partition_mmap(core_part, 0,  sizeof(uint32_t),
                             SPI_FLASH_MMAP_DATA, &core_data, &core_data_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap core dump data (%d)!", err);
        return err;
    }

    uint32_t *dw = (uint32_t *)core_data;
    *out_size = *dw;
    spi_flash_munmap(core_data_handle);
    if ((*out_size < sizeof(uint32_t)) || (*out_size > core_part->size)) {
        ESP_LOGE(TAG, "Incorrect size of core dump image: %d", *out_size);
        return ESP_ERR_INVALID_SIZE;
    }

    // remap full core dump with CRC
    err = esp_partition_mmap(core_part, 0, *out_size,
                             SPI_FLASH_MMAP_DATA, &core_data, &core_data_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap core dump data (%d)!", err);
        return err;
    }
#if CONFIG_ESP32_COREDUMP_CHECKSUM_CRC32
    uint32_t *crc = (uint32_t *)(((uint8_t *)core_data) + *out_size);
    crc--; // Point to CRC field

    // Calculate CRC over core dump data except for CRC field
    core_dump_crc_t cur_crc = crc32_le(0, (uint8_t const *)core_data, *out_size - sizeof(core_dump_crc_t));
    if (*crc != cur_crc) {
        ESP_LOGD(TAG, "Core dump CRC offset 0x%x, data size: %u",
                (uint32_t)((uint32_t)crc - (uint32_t)core_data), *out_size);
        ESP_LOGE(TAG, "Core dump data CRC check failed: 0x%x -> 0x%x!", *crc, cur_crc);
        spi_flash_munmap(core_data_handle);
        return ESP_ERR_INVALID_CRC;
    }
#elif CONFIG_ESP32_COREDUMP_CHECKSUM_SHA256
    uint8_t* sha256_ptr = (uint8_t*)(((uint8_t *)core_data) + *out_size);
    sha256_ptr -= COREDUMP_SHA256_LEN;
    ESP_LOGD(TAG, "Core dump data offset, size: %d, %u!",
                    (uint32_t)((uint32_t)sha256_ptr - (uint32_t)core_data), *out_size);
    unsigned char sha_output[COREDUMP_SHA256_LEN];
    mbedtls_sha256_context ctx;
    ESP_LOGI(TAG, "Calculate SHA256 for coredump:");
    (void)esp_core_dump_sha(&ctx, core_data, *out_size - COREDUMP_SHA256_LEN, sha_output);
    if (memcmp((uint8_t*)sha256_ptr, (uint8_t*)sha_output, COREDUMP_SHA256_LEN) != 0) {
        ESP_LOGE(TAG, "Core dump data SHA256 check failed:");
        esp_core_dump_print_sha256("Calculated SHA256", (uint8_t*)sha_output);
        esp_core_dump_print_sha256("Image SHA256",(uint8_t*)sha256_ptr);
        spi_flash_munmap(core_data_handle);
        return ESP_ERR_INVALID_CRC;
    } else {
        ESP_LOGI(TAG, "Core dump data SHA256 is correct");
    }
#endif
    spi_flash_munmap(core_data_handle);

    *out_addr = core_part->address;
    return ESP_OK;
}