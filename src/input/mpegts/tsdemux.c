/*
 *  tvheadend, MPEG transport stream demuxer
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE
#include <stdlib.h>

#include <pthread.h>

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "tvheadend.h"
#include "subscriptions.h"
#include "parsers.h"
#include "streaming.h"
#include "input.h"
#include "parsers/parser_teletext.h"
#include "tsdemux.h"

#define TS_REMUX_BUFSIZE (188 * 100)

static void ts_remux(mpegts_service_t *t, const uint8_t *tsb, int error);

/**
 * Continue processing of transport stream packets
 */
static void
ts_recv_packet0
  (mpegts_service_t *t, elementary_stream_t *st, const uint8_t *tsb)
{
  int off, pusi, cc, error;

  service_set_streaming_status_flags((service_t*)t, TSS_MUX_PACKETS);

  if (!st) {
    if(streaming_pad_probe_type(&t->s_streaming_pad, SMT_MPEGTS))
      ts_remux(t, tsb, 0);
    return;
  }

  error = !!(tsb[1] & 0x80);
  pusi  = !!(tsb[1] & 0x40);

  /* Check CC */

  if(tsb[3] & 0x10) {
    cc = tsb[3] & 0xf;
    if(st->es_cc != -1 && cc != st->es_cc) {
      /* Let the hardware to stabilize and don't flood the log */
      if (t->s_start_time + 1 < dispatch_clock &&
          tvhlog_limit(&st->es_cc_log, 10))
        tvhwarn("TS", "%s Continuity counter error (total %zi)",
                      service_component_nicename(st), st->es_cc_log.count);
      avgstat_add(&t->s_cc_errors, 1, dispatch_clock);
      avgstat_add(&st->es_cc_errors, 1, dispatch_clock);
      error |= 2;
    }
    st->es_cc = (cc + 1) & 0xf;
  }

  if(streaming_pad_probe_type(&t->s_streaming_pad, SMT_MPEGTS))
    ts_remux(t, tsb, error);

  off = tsb[3] & 0x20 ? tsb[4] + 5 : 4;

  switch(st->es_type) {

  case SCT_CA:
    break;

  default:
    if(!streaming_pad_probe_type(&t->s_streaming_pad, SMT_PACKET))
      break;

    if(st->es_type == SCT_TELETEXT)
      teletext_input(t, st, tsb);
    if(off > 188)
      break;

    if(t->s_status == SERVICE_RUNNING)
      parse_mpeg_ts((service_t*)t, st, tsb + off, 188 - off, pusi, error);
    break;
  }
}

/**
 * Process service stream packets, extract PCR and optionally descramble
 */
int
ts_recv_packet1
  (mpegts_service_t *t, const uint8_t *tsb, int64_t *pcrp, int table)
{
  elementary_stream_t *st;
  int pid, r;
  int error = 0;
  int64_t pcr = PTS_UNSET;
  
  /* Error */
  if (tsb[1] & 0x80)
    error = 1;

#if 0
  printf("%02X %02X %02X %02X %02X %02X\n",
         tsb[0], tsb[1], tsb[2], tsb[3], tsb[4], tsb[5]);
#endif

  /* Extract PCR (do this early for tsfile) */
  if((tsb[3] & 0x20) && (tsb[4] > 5) && (tsb[5] & 0x10) && !error) {
    pcr  = (uint64_t)tsb[6] << 25;
    pcr |= (uint64_t)tsb[7] << 17;
    pcr |= (uint64_t)tsb[8] << 9;
    pcr |= (uint64_t)tsb[9] << 1;
    pcr |= ((uint64_t)tsb[10] >> 7) & 0x01;
    if (pcrp) *pcrp = pcr;
  }

  /* Nothing - special case for tsfile to get PCR */
  if (!t) return 0;

  /* Service inactive - ignore */
  if(t->s_status != SERVICE_RUNNING)
    return 0;

  pthread_mutex_lock(&t->s_stream_mutex);

  service_set_streaming_status_flags((service_t*)t, TSS_INPUT_HARDWARE);

  if(error) {
    /* Transport Error Indicator */
    if (tvhlog_limit(&t->s_tei_log, 10))
      tvhwarn("TS", "%s Transport error indicator (total %zi)",
              service_nicename((service_t*)t), t->s_tei_log.count);
  }

  pid = (tsb[1] & 0x1f) << 8 | tsb[2];

  st = service_stream_find((service_t*)t, pid);

  if((st == NULL) && (pid != t->s_pcr_pid) && !table) {
    pthread_mutex_unlock(&t->s_stream_mutex);
    return 0;
  }

  if(!error)
    service_set_streaming_status_flags((service_t*)t, TSS_INPUT_SERVICE);

  avgstat_add(&t->s_rate, 188, dispatch_clock);

  if((tsb[3] & 0xc0) ||
      (t->s_scrambled_seen && st && st->es_type != SCT_CA)) {

    /**
     * Lock for descrambling, but only if packet was not in error
     */
    if(!error)
      t->s_scrambled_seen |= service_is_encrypted((service_t*)t);

    /* scrambled stream */
    r = descrambler_descramble((service_t *)t, st, tsb);
    if(r > 0) {
      pthread_mutex_unlock(&t->s_stream_mutex);
      return 1;
    }

    if(!error && service_is_encrypted((service_t*)t)) {
      if(r == 0) {
        service_set_streaming_status_flags((service_t*)t, TSS_NO_DESCRAMBLER);
      } else {
        service_set_streaming_status_flags((service_t*)t, TSS_NO_ACCESS);
      }
    }

  } else {
    ts_recv_packet0(t, st, tsb);
  }
  pthread_mutex_unlock(&t->s_stream_mutex);
  return 1;
}


/*
 * Process transport stream packets, simple version
 */
void
ts_recv_packet2(mpegts_service_t *t, const uint8_t *tsb)
{
  elementary_stream_t *st;
  int pid = (tsb[1] & 0x1f) << 8 | tsb[2];

  if((st = service_stream_find((service_t*)t, pid)) != NULL)
    ts_recv_packet0(t, st, tsb);
}

/*
 *
 */
void
ts_recv_raw(mpegts_service_t *t, const uint8_t *tsb)
{
  elementary_stream_t *st = NULL;
  int pid;

  pthread_mutex_lock(&t->s_stream_mutex);
  if (t->s_parent) {
    /* If PID is owned by parent, let parent service to
     * deliver this PID (decrambling)
     */
    pid = (tsb[1] & 0x1f) << 8 | tsb[2];
    st = service_stream_find(t->s_parent, pid);
  }
  if(st == NULL)
    if (streaming_pad_probe_type(&t->s_streaming_pad, SMT_MPEGTS))
      ts_remux(t, tsb, 0);
  pthread_mutex_unlock(&t->s_stream_mutex);
}

/**
 *
 */
static void
ts_remux(mpegts_service_t *t, const uint8_t *src, int error)
{
  streaming_message_t sm;
  pktbuf_t *pb;
  sbuf_t *sb = &t->s_tsbuf;

  if (sb->sb_data == NULL)
    sbuf_init_fixed(sb, TS_REMUX_BUFSIZE);

  sbuf_append(sb, src, 188);

  if (error)
    sb->sb_err++;

  if(sb->sb_ptr < TS_REMUX_BUFSIZE) 
    return;

  pb = pktbuf_alloc(sb->sb_data, sb->sb_ptr);
  pb->pb_err = sb->sb_err;

  sm.sm_type = SMT_MPEGTS;
  sm.sm_data = pb;
  streaming_pad_deliver(&t->s_streaming_pad, streaming_msg_clone(&sm));

  pktbuf_ref_dec(pb);

  service_set_streaming_status_flags((service_t*)t, TSS_PACKETS);
  t->s_streaming_live |= TSS_LIVE;

  sbuf_reset(sb, TS_REMUX_BUFSIZE);
}

/*
 * Attempt to re-sync a ts stream (3 valid sync's in a row)
 */
int
ts_resync ( const uint8_t *tsb, int *len, int *idx )
{
  int err = 1;
  while (err && (*len > 376)) {
    (*idx)++; (*len)--;
    err = (tsb[*idx] != 0x47) || (tsb[*idx+188] != 0x47) || 
          (tsb[*idx+376] != 0x47);
  }
  return err;
}
