/******************************************************************************
 *
 *  Copyright 2016 The Android Open Source Project
 *  Copyright 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_a2dp"

#include <stdbool.h>

#include "audio_a2dp_hw/include/audio_a2dp_hw.h"
#include "bt_common.h"
#include "bta_av_api.h"
#include "btif_a2dp.h"
#include "btif_a2dp_audio_interface.h"
#include "btif_a2dp_control.h"
#include "btif_a2dp_sink.h"
#include "btif_a2dp_source.h"
#include "btif_av.h"
#include "btif_av_co.h"
#include "btif_hf.h"
#include "btif_util.h"
#include "osi/include/log.h"

void btif_a2dp_on_idle(void) {
  APPL_TRACE_WARNING("## ON A2DP IDLE ## peer_sep = %d",
                     btif_av_get_peer_sep());
  if (btif_av_get_peer_sep() == AVDT_TSEP_SNK) {
    btif_a2dp_source_on_idle();
  } else if (btif_av_get_peer_sep() == AVDT_TSEP_SRC) {
    btif_a2dp_sink_on_idle();
  }
}

bool btif_a2dp_on_started(const RawAddress& peer_addr,
                          tBTA_AV_START* p_av_start, bool pending_start) {
  bool ack = false;

  APPL_TRACE_WARNING("## ON A2DP STARTED ##");

  if (p_av_start == NULL) {
    /* ack back a local start request */

    if (!btif_av_is_a2dp_offload_enabled()) {
      btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
      return true;
    } else if (bluetooth::headset::IsCallIdle()) {
      btif_av_stream_start_offload();
    } else {
      APPL_TRACE_ERROR("%s: call in progress, do not start offload", __func__);
      btif_a2dp_audio_on_started(A2DP_CTRL_ACK_INCALL_FAILURE);
    }
    return true;
  }

  APPL_TRACE_WARNING(
      "%s: pending_start = %d status = %d suspending = %d initiator = %d",
      __func__, pending_start, p_av_start->status, p_av_start->suspending,
      p_av_start->initiator);

  if (p_av_start->status == BTA_AV_SUCCESS) {
    if (!p_av_start->suspending) {
      if (p_av_start->initiator) {
        if (pending_start) {
          if (btif_av_is_a2dp_offload_enabled()) {
            btif_av_stream_start_offload();
          } else {
            btif_a2dp_command_ack(A2DP_CTRL_ACK_SUCCESS);
          }
          ack = true;
        }
      } else {
        /* We were remotely started, make sure codec
         * is setup before datapath is started.
         */
        btif_a2dp_source_setup_codec(peer_addr);
        if (btif_av_is_a2dp_offload_enabled()) {
          btif_av_stream_start_offload();
        }
      }

      /* media task is autostarted upon a2dp audiopath connection */
    }
  } else if (pending_start) {
    APPL_TRACE_WARNING("%s: A2DP start request failed: status = %d", __func__,
                       p_av_start->status);
    btif_a2dp_command_ack(A2DP_CTRL_ACK_FAILURE);
    ack = true;
  }
  return ack;
}

void btif_a2dp_on_stopped(tBTA_AV_SUSPEND* p_av_suspend) {
  APPL_TRACE_WARNING("## ON A2DP STOPPED ##");

  if (btif_av_get_peer_sep() == AVDT_TSEP_SRC) {
    btif_a2dp_sink_on_stopped(p_av_suspend);
    return;
  }
  if (!btif_av_is_a2dp_offload_enabled()) {
    btif_a2dp_source_on_stopped(p_av_suspend);
  } else if (p_av_suspend != NULL) {
    btif_a2dp_audio_on_stopped(p_av_suspend->status);
  }
}

void btif_a2dp_on_suspended(tBTA_AV_SUSPEND* p_av_suspend) {
  APPL_TRACE_EVENT("## ON A2DP SUSPENDED ##");
  if (!btif_av_is_a2dp_offload_enabled()) {
    if (btif_av_get_peer_sep() == AVDT_TSEP_SRC) {
      btif_a2dp_sink_on_suspended(p_av_suspend);
    } else {
      btif_a2dp_source_on_suspended(p_av_suspend);
    }
  } else {
    btif_a2dp_audio_on_suspended(p_av_suspend->status);
  }
}

void btif_a2dp_on_offload_started(const RawAddress& peer_addr,
                                  tBTA_AV_STATUS status) {
  tA2DP_CTRL_ACK ack;
  APPL_TRACE_EVENT("%s status %d", __func__, status);

  switch (status) {
    case BTA_AV_SUCCESS:
      ack = A2DP_CTRL_ACK_SUCCESS;
      break;
    case BTA_AV_FAIL_RESOURCES:
      APPL_TRACE_ERROR("%s FAILED UNSUPPORTED", __func__);
      ack = A2DP_CTRL_ACK_UNSUPPORTED;
      break;
    default:
      APPL_TRACE_ERROR("%s FAILED: status = %d", __func__, status);
      ack = A2DP_CTRL_ACK_FAILURE;
      break;
  }
  if (btif_av_is_a2dp_offload_enabled()) {
    btif_a2dp_audio_on_started(status);
    if (ack != BTA_AV_SUCCESS && btif_av_stream_started_ready()) {
      // Offload request will return with failure from btif_av sm if
      // suspend is triggered for remote start. Disconnect only if SoC
      // returned failure for offload VSC
      APPL_TRACE_ERROR("%s: offload start failed", __func__);
      btif_av_src_disconnect_sink(peer_addr);
    }
  } else {
    btif_a2dp_command_ack(ack);
  }
}

void btif_debug_a2dp_dump(int fd) {
  btif_a2dp_source_debug_dump(fd);
  btif_a2dp_sink_debug_dump(fd);
  btif_a2dp_codec_debug_dump(fd);
}
