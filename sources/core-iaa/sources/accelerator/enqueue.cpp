/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 03/23/2020
 * @brief Internal HW API functions for @ref hw_enqueue_descriptor API implementation
 */

#include "hw_definitions.h"
#include "hw_descriptors_api.h"
#include "dispatcher/hw_dispatcher.hpp"
#include "dispatcher/numa.hpp"


#ifdef DWQ_SUPPORT
extern "C" hw_accelerator_status hw_enqueue_descriptor(void *desc_ptr, int32_t device_numa_id, void **sem) {
#else
extern "C" hw_accelerator_status hw_enqueue_descriptor(void *desc_ptr, int32_t device_numa_id) {
#endif
    auto result = HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
    auto enqueue_failed = false;


#if defined( linux )
    auto                               &dispatcher   = qpl::ml::dispatcher::hw_dispatcher::get_instance();
    const auto                          device_count = dispatcher.device_count();
    static thread_local std::uint32_t   device_idx   = 0;

    if (device_count == 0) {
        return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
    }

    int32_t numa_id = (device_numa_id == -1) ? qpl::ml::util::get_numa_id()
                                             : device_numa_id;

    for (uint64_t try_count = 0u; try_count < device_count; ++try_count) {
        const auto &device = dispatcher.device(device_idx);
        device_idx = (device_idx+1) % device_count;

        if (device.numa_id() != (uint64_t)numa_id) {
            continue;
        }

        hw_iaa_descriptor_hint_cpu_cache_as_destination((hw_descriptor *) desc_ptr, device.get_cache_write_available());

#ifdef DWQ_SUPPORT
        enqueue_failed = device.enqueue_descriptor(desc_ptr, sem);
#else
        enqueue_failed = device.enqueue_descriptor(desc_ptr);
#endif
        if (enqueue_failed) {
            result = HW_ACCELERATOR_WQ_IS_BUSY;
        } else {
            result = HW_ACCELERATOR_STATUS_OK;
            break;
        }


    }
#else
    // Not supported on Windows yet
    return HW_ACCELERATOR_SUPPORT_ERR;
#endif

    return result;
}

extern "C" hw_accelerator_status hw_accelerator_submit_descriptor(hw_accelerator_context *const UNREFERENCED_PARAMETER(accel_context_ptr),
                                                                  const hw_descriptor *const descriptor_ptr,
                                                                  hw_accelerator_submit_options *const submit_options) {
#ifdef DWQ_SUPPORT
    return hw_enqueue_descriptor((void *) descriptor_ptr, submit_options->numa_id, NULL);
#else
    return hw_enqueue_descriptor((void *) descriptor_ptr, submit_options->numa_id);
#endif
}
