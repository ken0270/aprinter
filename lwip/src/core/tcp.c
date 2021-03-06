/**
 * @file
 * Transmission Control Protocol for IP
 *
 * This file contains common functions for the TCP implementation, such as functinos
 * for manipulating the data structures and the TCP timer functions. TCP functions
 * related to input and output is found in tcp_in.c and tcp_out.c respectively.
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 * 
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

#include "lwip/opt.h"

#if LWIP_TCP /* don't build if not configured for use in lwipopts.h */

#include "lwip/def.h"
#include "lwip/memp.h"
#include "lwip/tcp.h"
#include "lwip/tcp_impl.h"
#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/nd6.h"

#include <string.h>

#ifndef TCP_LOCAL_PORT_RANGE_START
/* From http://www.iana.org/assignments/port-numbers:
   "The Dynamic and/or Private Ports are those from 49152 through 65535" */
#define TCP_LOCAL_PORT_RANGE_START        0xc000
#define TCP_LOCAL_PORT_RANGE_END          0xffff
#define TCP_ENSURE_LOCAL_PORT_RANGE(port) ((u16_t)(((port) & ~TCP_LOCAL_PORT_RANGE_START) + TCP_LOCAL_PORT_RANGE_START))
#endif

#if LWIP_TCP_KEEPALIVE
#define TCP_KEEP_DUR(pcb)   ((pcb)->keep_cnt * (pcb)->keep_intvl)
#define TCP_KEEP_INTVL(pcb) ((pcb)->keep_intvl)
#else /* LWIP_TCP_KEEPALIVE */
#define TCP_KEEP_DUR(pcb)   TCP_MAXIDLE
#define TCP_KEEP_INTVL(pcb) TCP_KEEPINTVL_DEFAULT
#endif /* LWIP_TCP_KEEPALIVE */

/* As initial send MSS, we use TCP_MSS but limit it to 536. */
#if TCP_MSS > 536
#define INITIAL_MSS 536
#else
#define INITIAL_MSS TCP_MSS
#endif

const char * const tcp_state_str[] = {
  "CLOSED",
  "LISTEN",
  "SYN_SENT",
  "SYN_RCVD",
  "ESTABLISHED",
  "FIN_WAIT_1",
  "FIN_WAIT_2",
  "CLOSE_WAIT",
  "CLOSING",
  "LAST_ACK",
  "TIME_WAIT",
  "LISTEN_CLOS"
};

/* last local TCP port */
static u16_t tcp_port = TCP_LOCAL_PORT_RANGE_START;

/* Incremented every coarse grained timer shot (typically every 500 ms). */
u32_t tcp_ticks;
const u8_t tcp_backoff[13] =
    { 1, 2, 3, 4, 5, 6, 7, 7, 7, 7, 7, 7, 7};
 /* Times per slowtmr hits */
const u8_t tcp_persist_backoff[7] = { 3, 6, 12, 24, 48, 96, 120 };

/* The TCP PCB lists. */

/** List of all TCP PCBs bound but not yet (connected || listening) */
struct tcp_pcb_base *tcp_bound_pcbs;
/** List of all TCP PCBs in LISTEN state */
struct tcp_pcb_listen *tcp_listen_pcbs;
/** List of all TCP PCBs that are in a state in which
 * they accept or send data. */
struct tcp_pcb *tcp_active_pcbs;
/** List of all TCP PCBs in TIME-WAIT state */
struct tcp_pcb *tcp_tw_pcbs;

#define NUM_TCP_PCB_LISTS               4
#define NUM_TCP_PCB_LISTS_NO_TIME_WAIT  3
#define START_TCP_PCB_LISTS_CONNECTION  2

/** An array with all (non-temporary) PCB lists, mainly used for smaller code size */
struct tcp_pcb_base ** const tcp_pcb_lists[] = {
  (struct tcp_pcb_base **)&tcp_listen_pcbs,
  &tcp_bound_pcbs,
  (struct tcp_pcb_base **)&tcp_active_pcbs,
  (struct tcp_pcb_base **)&tcp_tw_pcbs  
};

/* This represents a posible current PCB iteration. When a PCB is
 * about to be added to or removed from tcp_active_pcbs or tcp_tw_pcbs,
 * tcp_iter_will_prepend() or tcp_iter_will_remove() is called to
 * make the iteration aware of the change. */
struct tcp_iter tcp_conn_iter;

/** Timer counter to handle calling slow-timer from tcp_tmr() */
static u8_t tcp_timer;
static u8_t tcp_timer_ctr;
static u16_t tcp_new_port(void);

/**
 * Initialize this module.
 */
void
tcp_init(void)
{
#if LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS && defined(LWIP_RAND)
  tcp_port = TCP_ENSURE_LOCAL_PORT_RANGE(LWIP_RAND());
#endif /* LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS && defined(LWIP_RAND) */
}

/**
 * Called periodically to dispatch TCP timers.
 */
void
tcp_tmr(void)
{
  /* Call tcp_fasttmr() every 250 ms */
  tcp_fasttmr();

  if (++tcp_timer & 1) {
    /* Call tcp_tmr() every 500 ms, i.e., every other timer
       tcp_tmr() is called. */
    tcp_slowtmr();
  }
}

/**
 * Closes the TX side of a connection held by the PCB.
 * For tcp_close(), a RST is sent if the application didn't receive all data
 * (tcp_recved() not called for all data passed to recv callback).
 *
 * Listening pcbs are freed and may not be referenced any more.
 * Connection pcbs are freed if not yet connected and may not be referenced
 * any more. If a connection is established (at least SYN received or in
 * a closing state), the connection is closed, and put in a closing state.
 * The pcb is then automatically freed in tcp_slowtmr(). It is therefore
 * unsafe to reference it.
 *
 * @param pcb the tcp_pcb to close
 * @return ERR_OK if connection has been closed
 *         another err_t if closing failed and pcb is not freed
 */
static err_t
tcp_close_shutdown(struct tcp_pcb *pcb, u8_t rst_on_unacked_data)
{
  err_t err;
  
  LWIP_ASSERT("tcp_close_shutdown on listen-pcb", !tcp_pcb_is_listen(pcb));

  if (rst_on_unacked_data && (pcb->state == ESTABLISHED || pcb->state == CLOSE_WAIT)) {
    if (pcb->rcv_wnd != TCP_WND_MAX(pcb)) {
      /* Not all data received by application, send RST to tell the remote
         side about this. */
      LWIP_ASSERT("pcb->flags & TF_NOUSER", (pcb->flags & TF_NOUSER));

      tcp_rst(pcb->snd_nxt, pcb->rcv_nxt, &pcb->local_ip, &pcb->remote_ip,
               pcb->local_port, pcb->remote_port);
      
      if (pcb->state == ESTABLISHED) {
        /* move to TIME_WAIT since we close actively */
        tcp_move_to_time_wait(pcb);
      } else {
        pcb->flags &= ~TF_ACK_DELAY;
        tcp_pcb_free(pcb, 0, NULL);
      }
      return ERR_OK;
    }
  }

  switch (pcb->state) {
  case CLOSED:
    /* Closing a pcb in the CLOSED state might seem erroneous,
     * however, it is in this state once allocated and as yet unused
     * and the user needs some way to free it should the need arise.
     * Calling tcp_close() with a pcb that has already been closed, (i.e. twice)
     * or for a pcb that has been used and then entered the CLOSED state
     * is erroneous, but this should never happen as the pcb has in those cases
     * been freed, and so any remaining handles are bogus. */
    err = ERR_OK;
    tcp_pcb_free(pcb, 0, NULL);
    pcb = NULL;
    break;
  case SYN_SENT:
    err = ERR_OK;
    tcp_pcb_free(pcb, 0, NULL);
    pcb = NULL;
    MIB2_STATS_INC(mib2.tcpattemptfails);
    break;
  case SYN_RCVD:
    err = tcp_send_fin(pcb);
    if (err == ERR_OK) {
      tcp_backlog_accepted_internal(pcb);
      MIB2_STATS_INC(mib2.tcpattemptfails);
      pcb->state = FIN_WAIT_1;
    }
    break;
  case ESTABLISHED:
    err = tcp_send_fin(pcb);
    if (err == ERR_OK) {
      MIB2_STATS_INC(mib2.tcpestabresets);
      pcb->state = FIN_WAIT_1;
    }
    break;
  case CLOSE_WAIT:
    err = tcp_send_fin(pcb);
    if (err == ERR_OK) {
      MIB2_STATS_INC(mib2.tcpestabresets);
      pcb->state = LAST_ACK;
    }
    break;
  default:
    /* Has already been closed, do nothing. */
    err = ERR_OK;
    pcb = NULL;
    break;
  }

  if (pcb != NULL && err == ERR_OK) {
    /* To ensure all data has been sent when tcp_close returns, we have
       to make sure tcp_output doesn't fail.
       Since we don't really have to ensure all data has been sent when tcp_close
       returns (unsent data is sent from tcp timer functions, also), we don't care
       for the return value of tcp_output for now. */
    tcp_output(pcb);
  }
  
  return err;
}

/**
 * Releases the application reference to the PCB.
 * 
 * Don't use this with listen PCBs (use tcp_close_listen).
 * After this returns, the PCB is deemed to no longer be referenced
 * by the application, and the application must not use it in any
 * way (and it may have been freed).
 * 
 * It is guaranteed that after tcp_close is called, none of the
 * PCB callbacks will be called again (including within tcp_close).
 * 
 * If possible, this will automatically start active close as if
 * tcp_shut_tx was called.
 *
 * @param pcb the tcp_pcb to close
 */
void tcp_close (struct tcp_pcb *pcb)
{
  err_t err;
  
  LWIP_ASSERT("tcp_close on listen-pcb", !tcp_pcb_is_listen(pcb));
  LWIP_ASSERT("tcp_close without user reference", !(pcb->flags & TF_NOUSER));
  
  /* Remember that we no longer have a user reference. */
  pcb->flags |= TF_NOUSER;
  
  /* Try to do an orderly close. */
  err = tcp_close_shutdown(pcb, 1);
  if (err != ERR_OK) {
    /* Just RST and free the PCB. */
    tcp_pcb_free(pcb, 1, NULL);
  }
}

err_t tcp_shut_tx (struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_shut_tx on listen-pcb", !tcp_pcb_is_listen(pcb));
  LWIP_ASSERT("tcp_shut_tx without user reference", !(pcb->flags & TF_NOUSER));
  
  switch (pcb->state) {
    case SYN_RCVD:
    case ESTABLISHED:
    case CLOSE_WAIT:
      return tcp_close_shutdown(pcb, 0);
    default:
      /* Not yet connected or already shut. Will not proceed because
       * tcp_close_shutdown might then free the PCB. */
      return ERR_CONN;
  }
}

/**
 * Close a listen PCB (which may or may not be listening).
 * After this it is freed and may not be referenced any more.
 * 
 * @param lpcb the listen PCB
 */
void tcp_close_listen(struct tcp_pcb_listen *lpcb)
{
  LWIP_ASSERT("tcp_close_listen on non-listen-pcb", tcp_pcb_is_listen(lpcb));
  
  LWIP_DEBUGF(TCP_DEBUG, ("tcp_close_listen: closing in "));
  tcp_debug_print_state(lpcb->state);
  
  if (lpcb->state == LISTEN) {
    /* Remove reference to listener from any connection PCBs. */
    size_t i;
    for (i = START_TCP_PCB_LISTS_CONNECTION; i < NUM_TCP_PCB_LISTS; i++) {
      struct tcp_pcb *pcb;
      for (pcb = (struct tcp_pcb *)*tcp_pcb_lists[i]; pcb != NULL; pcb = pcb->next) {
        if (pcb->listener == lpcb) {
          pcb->listener = NULL;
        }
      }
    }
    tcp_rmv((struct tcp_pcb_base **)&tcp_listen_pcbs, to_tcp_pcb_base(lpcb));
  }
  else {
    if (lpcb->local_port != 0) {
      tcp_rmv(&tcp_bound_pcbs, to_tcp_pcb_base(lpcb));
    }
  }
  
  memp_free(MEMP_TCP_PCB_LISTEN, lpcb);
  
  LWIP_ASSERT("tcp_close_listen: tcp_pcbs_sane()", tcp_pcbs_sane());
}

/**
 * Aborts the connection by sending a RST (reset) segment to the remote
 * host. The pcb is deallocated. This function never fails.
 *
 * @param pcb the tcp pcb to abort
 */
void
tcp_abort(struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_abort on listen-pcb", !tcp_pcb_is_listen(pcb));
  LWIP_ASSERT("tcp_abort without user reference", !(pcb->flags & TF_NOUSER));
  
  tcp_pcb_free(pcb, 1, NULL);
}

/**
 * Binds the connection to a local port number and IP address. If the
 * IP address is not given (i.e., ipaddr == NULL), the IP address of
 * the outgoing network interface is used instead.
 *
 * @param pcb the tcp_pcb to bind (no check is done whether this pcb is
 *        already bound!)
 * @param ipaddr the local ip address to bind to (use IP_ADDR_ANY to bind
 *        to any local address
 * @param port the local port to bind to
 * @return ERR_USE if the port is already in use
 *         ERR_VAL if bind failed because the PCB is not in a valid state
 *         ERR_OK if bound
 */
err_t
tcp_bind(struct tcp_pcb_base *pcb, const ip_addr_t *ipaddr, u16_t port)
{
  int i;
  int max_pcb_list = NUM_TCP_PCB_LISTS;
  struct tcp_pcb_base *cpcb;
  
  LWIP_ASSERT("tcp_bind: not in CLOSED/LISTEN_CLOS",
              pcb->state == CLOSED || pcb->state == LISTEN_CLOS);
  
  if (!IP_ADDR_PCB_VERSION_MATCH(pcb, ipaddr)) {
    return ERR_VAL;
  }

#if SO_REUSE
  /* Unless the REUSEADDR flag is set,
     we have to check the pcbs in TIME-WAIT state, also.
     We do not dump TIME_WAIT pcb's; they can still be matched by incoming
     packets using both local and remote IP addresses and ports to distinguish.
   */
  if (ip_get_option(pcb, SOF_REUSEADDR)) {
    max_pcb_list = NUM_TCP_PCB_LISTS_NO_TIME_WAIT;
  }
#endif /* SO_REUSE */

  if (port == 0) {
    port = tcp_new_port();
    if (port == 0) {
      return ERR_BUF;
    }
  } else {
    /* Check if the address already is in use (on all lists) */
    for (i = 0; i < max_pcb_list; i++) {
      for (cpcb = *tcp_pcb_lists[i]; cpcb != NULL; cpcb = cpcb->next) {
        if (cpcb->local_port == port) {
#if SO_REUSE
          /* Omit checking for the same port if both pcbs have REUSEADDR set.
             For SO_REUSEADDR, the duplicate-check for a 5-tuple is done in
             tcp_connect. */
          if (!ip_get_option(pcb, SOF_REUSEADDR) ||
              !ip_get_option(cpcb, SOF_REUSEADDR))
#endif /* SO_REUSE */
          {
            /* @todo: check accept_any_ip_version */
            if (IP_PCB_IPVER_EQ(pcb, cpcb) &&
                (ip_addr_isany(&cpcb->local_ip) ||
                ip_addr_isany(ipaddr) ||
                ip_addr_cmp(&cpcb->local_ip, ipaddr))) {
              return ERR_USE;
            }
          }
        }
      }
    }
  }

  if (!ip_addr_isany(ipaddr)) {
    ip_addr_set(&pcb->local_ip, ipaddr);
  }
  pcb->local_port = port;
  tcp_reg(&tcp_bound_pcbs, pcb);
  LWIP_DEBUGF(TCP_DEBUG, ("tcp_bind: bind to port %"U16_F"\n", port));
  return ERR_OK;
}


/**
 * Set the state of the connection to be LISTEN, which means that it
 * is able to accept incoming connections. Setting the connection to LISTEN
 * is an irreversible process.
 *
 * @param lpcb the tcp_pcb_listen
 * @param backlog the incoming connections queue limit
 * @return ERR_OK on success, ERR_USE if address is already in use
 */
err_t tcp_listen_with_backlog(struct tcp_pcb_listen *lpcb, u8_t backlog)
{
  struct tcp_pcb_listen *other_lpcb;
  LWIP_UNUSED_ARG(backlog);
  
  LWIP_ASSERT("tcp_listen: not in LISTEN_CLOS", lpcb->state == LISTEN_CLOS);

#if SO_REUSE
  if (ip_get_option(lpcb, SOF_REUSEADDR)) {
    /* Since SOF_REUSEADDR allows reusing a local address before the pcb's usage
       is declared (listen-/connection-pcb), we have to make sure now that
       this port is only used once for every local IP. */
    for (other_lpcb = tcp_listen_pcbs; other_lpcb != NULL; other_lpcb = other_lpcb->next) {
      if (IP_PCB_IPVER_EQ(other_lpcb, lpcb) &&
          other_lpcb->local_port == lpcb->local_port &&
          ip_addr_cmp(&other_lpcb->local_ip, &lpcb->local_ip)) {
        /* this address/port is already used */
        return ERR_USE;
      }
    }
  }
#endif /* SO_REUSE */
  lpcb->state = LISTEN;
#if LWIP_IPV4 && LWIP_IPV6
  lpcb->accept_any_ip_version = 0;
#endif /* LWIP_IPV4 && LWIP_IPV6 */
  if (lpcb->local_port != 0) {
    tcp_rmv(&tcp_bound_pcbs, to_tcp_pcb_base(lpcb));
  }
  lpcb->accepts_pending = 0;
  tcp_backlog_set(lpcb, backlog);
  lpcb->initial_rcv_wnd = TCPWND_MIN16(TCP_WND);
  tcp_reg((struct tcp_pcb_base **)&tcp_listen_pcbs, to_tcp_pcb_base(lpcb));
  return ERR_OK;
}

void tcp_backlog_set(struct tcp_pcb_listen *lpcb, u8_t new_backlog)
{
  LWIP_ASSERT("tcp_backlog_set: not in LISTEN", lpcb->state == LISTEN);
  
  lpcb->backlog = ((new_backlog) ? (new_backlog) : 1);
}

/** Delay accepting a connection in respect to the listen backlog:
 * the number of outstanding connections is increased until
 * tcp_backlog_accepted() is called.
 * 
 * You can use the backlog function to limit the maximum number
 * of connections on a listener (SYN_RCVD and established),
 * if you call this in the accept callback for each new connection,
 * and never call tcp_backlog_accepted.
 *
 * @param pcb the connection pcb which is not fully accepted yet
 */
void
tcp_backlog_delayed(struct tcp_pcb* pcb)
{
  LWIP_ASSERT("tcp_backlog_delayed without user reference", !(pcb->flags & TF_NOUSER));
  
  tcp_backlog_delayed_internal(pcb);
}

void tcp_backlog_delayed_internal (struct tcp_pcb* pcb)
{
  if ((pcb->flags & TF_BACKLOGPEND) == 0 && pcb->listener != NULL) {
    pcb->listener->accepts_pending++;
    LWIP_ASSERT("accepts_pending != 0", pcb->listener->accepts_pending != 0);
    pcb->flags |= TF_BACKLOGPEND;
  }
}

/** A delayed-accept a connection is accepted (or closed/aborted): decreases
 * the number of outstanding connections after calling tcp_backlog_delayed().
 *
 * @param pcb the connection pcb which is now fully accepted (or closed/aborted)
 */
void
tcp_backlog_accepted(struct tcp_pcb* pcb)
{
  LWIP_ASSERT("tcp_backlog_accepted without user reference", !(pcb->flags & TF_NOUSER));
  
  tcp_backlog_accepted_internal(pcb);
}

void tcp_backlog_accepted_internal(struct tcp_pcb* pcb)
{
  if ((pcb->flags & TF_BACKLOGPEND) != 0 && pcb->listener != NULL) {
    LWIP_ASSERT("accepts_pending != 0", pcb->listener->accepts_pending != 0);
    pcb->listener->accepts_pending--;
    pcb->flags &= ~TF_BACKLOGPEND;
  }
}

#if LWIP_IPV4 && LWIP_IPV6
/**
 * Same as tcp_listen_with_backlog, but allows to accept IPv4 and IPv6
 * connections, if the pcb's local address is set to ANY.
 */
err_t
tcp_listen_dual_with_backlog(struct tcp_pcb_listen *lpcb, u8_t backlog)
{
  struct tcp_pcb_listen *l;
  err_t err;

  if (lpcb->local_port != 0) {
    /* Check that there's noone listening on this port already
       (don't check the IP address since we'll set it to ANY */
    for (l = tcp_listen_pcbs; l != NULL; l = l->next) {
      if (l->local_port == lpcb->local_port) {
        /* this port is already used */
        return NULL;
      }
    }
  }

  err = tcp_listen_with_backlog(lpcb, backlog);
  if (err == ERR_OK && ip_addr_isany(&lpcb->local_ip)) {
    /* The default behavior is to accept connections on either
     * IPv4 or IPv6, if not bound. */
    /* @see NETCONN_FLAG_IPV6_V6ONLY for changing this behavior */
    lpcb->accept_any_ip_version = 1;
  }
  return lpcb;
}
#endif /* LWIP_IPV4 && LWIP_IPV6 */

/**
 * Update the state that tracks the available window space to advertise.
 *
 * Returns how much extra window would be advertised if we sent an
 * update now.
 */
u32_t tcp_update_rcv_ann_wnd(struct tcp_pcb *pcb)
{
  u32_t new_right_edge = pcb->rcv_nxt + pcb->rcv_wnd;

  if (TCP_SEQ_GEQ(new_right_edge, pcb->rcv_ann_right_edge + LWIP_MIN((TCP_WND / 2), pcb->mss))) {
    /* we can advertise more window */
    pcb->rcv_ann_wnd = pcb->rcv_wnd;
    return new_right_edge - pcb->rcv_ann_right_edge;
  } else {
    if (TCP_SEQ_GT(pcb->rcv_nxt, pcb->rcv_ann_right_edge)) {
      /* Can happen due to other end sending out of advertised window,
       * but within actual available (but not yet advertised) window */
      pcb->rcv_ann_wnd = 0;
    } else {
      /* keep the right edge of window constant */
      u32_t new_rcv_ann_wnd = pcb->rcv_ann_right_edge - pcb->rcv_nxt;
      LWIP_ASSERT("new_rcv_ann_wnd <= 0xffff", new_rcv_ann_wnd <= 0xffff);
      pcb->rcv_ann_wnd = (tcpwnd_size_t)new_rcv_ann_wnd;
    }
    return 0;
  }
}

/**
 * This function should be called by the application when it has
 * processed the data. The purpose is to advertise a larger window
 * when the data has been processed.
 *
 * @param pcb the tcp_pcb for which data is read
 * @param len the amount of bytes that have been read by the application
 */
void
tcp_recved(struct tcp_pcb *pcb, u16_t len)
{
  LWIP_ASSERT("tcp_recved on listen-pcb", !tcp_pcb_is_listen(pcb));
  LWIP_ASSERT("tcp_recved without user reference", !(pcb->flags & TF_NOUSER));

  tcp_recved_internal(pcb, len);
}

void tcp_recved_internal (struct tcp_pcb *pcb, u16_t len)
{
  int wnd_inflation;

  pcb->rcv_wnd += len;
  if (pcb->rcv_wnd > TCP_WND_MAX(pcb)) {
    pcb->rcv_wnd = TCP_WND_MAX(pcb);
  } else if (pcb->rcv_wnd == 0) {
    /* rcv_wnd overflowed */
    if ((pcb->state == CLOSE_WAIT) || (pcb->state == LAST_ACK)) {
      /* In passive close, we allow this, since the FIN bit is added to rcv_wnd
         by the stack itself, since it is not mandatory for an application
         to call tcp_recved() for the FIN bit, but e.g. the netconn API does so. */
      pcb->rcv_wnd = TCP_WND_MAX(pcb);
    } else {
      LWIP_ASSERT("tcp_recved: len wrapped rcv_wnd\n", 0);
    }
  }

  wnd_inflation = tcp_update_rcv_ann_wnd(pcb);

  /* If the change in the right edge of window is significant (default
   * watermark is TCP_WND/4), then send an explicit update now.
   * Otherwise wait for a packet to be sent in the normal course of
   * events (or more window to be available later) */
  if (wnd_inflation >= TCP_WND_UPDATE_THRESHOLD) {
    tcp_ack_now(pcb);
    tcp_output(pcb);
  }

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_recved: received %"U16_F" bytes, wnd %"TCPWNDSIZE_F" (%"TCPWNDSIZE_F").\n",
         len, pcb->rcv_wnd, (u16_t)(TCP_WND_MAX(pcb) - pcb->rcv_wnd)));
}

/**
 * Allocate a new local TCP port.
 *
 * @return a new (free) local TCP port number
 */
static u16_t
tcp_new_port(void)
{
  u8_t i;
  u16_t n = 0;
  struct tcp_pcb_base *pcb;

again:
  if (tcp_port++ == TCP_LOCAL_PORT_RANGE_END) {
    tcp_port = TCP_LOCAL_PORT_RANGE_START;
  }
  /* Check all PCB lists. */
  for (i = 0; i < NUM_TCP_PCB_LISTS; i++) {
    for (pcb = *tcp_pcb_lists[i]; pcb != NULL; pcb = pcb->next) {
      if (pcb->local_port == tcp_port) {
        if (++n > (TCP_LOCAL_PORT_RANGE_END - TCP_LOCAL_PORT_RANGE_START)) {
          return 0;
        }
        goto again;
      }
    }
  }
  return tcp_port;
}

/**
 * Connects to another host. The function given as the "connected"
 * argument will be called when the connection has been established.
 *
 * @param pcb the tcp_pcb used to establish the connection
 * @param ipaddr the remote ip address to connect to
 * @param port the remote tcp port to connect to
 * @param connected callback function to call when connected (on error,
                    the err calback will be called)
 * @return ERR_VAL if invalid arguments are given
 *         ERR_OK if connect request has been sent
 *         other err_t values if connect request couldn't be sent
 */
err_t
tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port,
      tcp_connected_fn connected)
{
  err_t ret;
  u32_t iss;
  u16_t old_local_port;

  LWIP_ASSERT("tcp_connect: not in CLOSED", pcb->state == CLOSED);

  if (!IP_ADDR_PCB_VERSION_MATCH(pcb, ipaddr)) {
    return ERR_VAL;
  }

  LWIP_DEBUGF(TCP_DEBUG, ("tcp_connect to port %"U16_F"\n", port));
  if (ipaddr != NULL) {
    ip_addr_set(&pcb->remote_ip, ipaddr);
  } else {
    return ERR_VAL;
  }
  pcb->remote_port = port;

  /* check if we have a route to the remote host */
  if (ip_addr_isany(&pcb->local_ip)) {
    /* no local IP address set, yet. */
    struct netif *netif;
    const ip_addr_t *local_ip;
    ip_route_get_local_ip(PCB_ISIPV6(pcb), &pcb->local_ip, &pcb->remote_ip, netif, local_ip);
    if ((netif == NULL) || (local_ip == NULL)) {
      /* Don't even try to send a SYN packet if we have no route
         since that will fail. */
      return ERR_RTE;
    }
    /* Use the address as local address of the pcb. */
    ip_addr_copy(pcb->local_ip, *local_ip);
  }

  old_local_port = pcb->local_port;
  if (pcb->local_port == 0) {
    pcb->local_port = tcp_new_port();
    if (pcb->local_port == 0) {
      return ERR_BUF;
    }
  } else {
#if SO_REUSE
    if (ip_get_option(pcb, SOF_REUSEADDR)) {
      /* Since SOF_REUSEADDR allows reusing a local address, we have to make sure
         now that the 5-tuple is unique. */
      struct tcp_pcb *cpcb;
      int i;
      /* Don't check listen- and bound-PCBs, check active- and TIME-WAIT PCBs. */
      for (i = START_TCP_PCB_LISTS_CONNECTION; i < NUM_TCP_PCB_LISTS; i++) {
        for (cpcb = *tcp_pcb_lists[i]; cpcb != NULL; cpcb = cpcb->next) {
          if ((cpcb->local_port == pcb->local_port) &&
              (cpcb->remote_port == port) &&
              IP_PCB_IPVER_EQ(cpcb, pcb) &&
              ip_addr_cmp(&cpcb->local_ip, &pcb->local_ip) &&
              ip_addr_cmp(&cpcb->remote_ip, ipaddr)) {
            /* linux returns EISCONN here, but ERR_USE should be OK for us */
            return ERR_USE;
          }
        }
      }
    }
#endif /* SO_REUSE */
  }

  iss = tcp_next_iss();
  pcb->rcv_nxt = 0;
  pcb->snd_nxt = iss;
  pcb->lastack = iss - 1;
  pcb->snd_lbb = iss - 1;
  /* Start with a window that does not need scaling. When window scaling is
     enabled and used, the window is enlarged when both sides agree on scaling. */
  pcb->rcv_wnd = pcb->rcv_ann_wnd = TCPWND_MIN16(TCP_WND);
  pcb->rcv_ann_right_edge = pcb->rcv_nxt;
  pcb->snd_wnd = TCP_WND;
  /* As initial send MSS, we use TCP_MSS but limit it to 536.
     The send MSS is updated when an MSS option is received. */
  pcb->mss = INITIAL_MSS;
#if TCP_CALCULATE_EFF_SEND_MSS
  pcb->mss = tcp_eff_send_mss(pcb->mss, &pcb->local_ip, &pcb->remote_ip, PCB_ISIPV6(pcb));
#endif /* TCP_CALCULATE_EFF_SEND_MSS */
  pcb->cwnd = 1;
  pcb->ssthresh = TCP_WND;
  pcb->connected = connected;

  /* Send a SYN together with the MSS option. */
  ret = tcp_enqueue_flags(pcb, TCP_SYN);
  if (ret == ERR_OK) {
    /* SYN segment was enqueued, changed the pcbs state now */
    pcb->state = SYN_SENT;
    if (old_local_port != 0) {
      tcp_rmv(&tcp_bound_pcbs, to_tcp_pcb_base(pcb));
    }
    tcp_iter_will_prepend(&tcp_conn_iter, pcb, tcp_active_pcbs);
    tcp_reg((struct tcp_pcb_base **)&tcp_active_pcbs, to_tcp_pcb_base(pcb));
    MIB2_STATS_INC(mib2.tcpactiveopens);

    tcp_output(pcb);
  }
  return ret;
}

/**
 * Called every 500 ms and implements the retransmission timer and the timer that
 * removes PCBs that have been in TIME-WAIT for enough time. It also increments
 * various timers such as the inactivity timer in each PCB.
 *
 * Automatically called from tcp_tmr().
 */
void
tcp_slowtmr(void)
{
  struct tcp_pcb *pcb;
  tcpwnd_size_t eff_wnd;
  u8_t pcb_remove;      /* flag if a PCB should be removed */
  u8_t pcb_reset;       /* flag if a RST should be sent when removing */
  err_t err;

  err = ERR_OK;

  ++tcp_ticks;
  ++tcp_timer_ctr;

  /* Steps through all of the active PCBs. */
  if (tcp_active_pcbs == NULL) {
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_slowtmr: no active pcbs\n"));
  }
  tcp_iter_start(&tcp_conn_iter, tcp_active_pcbs);
  
  while ((pcb = tcp_iter_next(&tcp_conn_iter))) {
    LWIP_ASSERT("tcp_slowtmr: active pcb->state\n", tcp_state_is_active(pcb->state));
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_slowtmr: processing active pcb\n"));
    
    if (pcb->last_timer == tcp_timer_ctr) {
      /* skip this pcb, we have already processed it */
      continue;
    }
    pcb->last_timer = tcp_timer_ctr;

    pcb_remove = 0;
    pcb_reset = 0;

    if (pcb->state == SYN_SENT && pcb->nrtx == TCP_SYNMAXRTX) {
      ++pcb_remove;
      LWIP_DEBUGF(TCP_DEBUG, ("tcp_slowtmr: max SYN retries reached\n"));
    }
    else if (pcb->nrtx == TCP_MAXRTX) {
      ++pcb_remove;
      LWIP_DEBUGF(TCP_DEBUG, ("tcp_slowtmr: max DATA retries reached\n"));
    } else {
      if (pcb->persist_backoff > 0) {
        /* If snd_wnd is zero, use persist timer to send 1 byte probes
         * instead of using the standard retransmission mechanism. */
        u8_t backoff_cnt = tcp_persist_backoff[pcb->persist_backoff-1];
        if (pcb->persist_cnt < backoff_cnt) {
          pcb->persist_cnt++;
        }
        if (pcb->persist_cnt >= backoff_cnt) {
          if (tcp_zero_window_probe(pcb) == ERR_OK) {
            pcb->persist_cnt = 0;
            if (pcb->persist_backoff < sizeof(tcp_persist_backoff)) {
              pcb->persist_backoff++;
            }
          }
        }
      } else {
        /* Increase the retransmission timer if it is running */
        if (pcb->rtime >= 0) {
          ++pcb->rtime;
        }

        if (pcb->sndq != NULL && pcb->rtime >= pcb->rto) {
          /* Time for a retransmission. */
          LWIP_DEBUGF(TCP_RTO_DEBUG, ("tcp_slowtmr: rtime %"S16_F
                                      " pcb->rto %"S16_F"\n",
                                      pcb->rtime, pcb->rto));

          /* Double retransmission time-out unless we are trying to
           * connect to somebody (i.e., we are in SYN_SENT). */
          if (pcb->state != SYN_SENT) {
            pcb->rto = ((pcb->sa >> 3) + pcb->sv) << tcp_backoff[pcb->nrtx];
          }

          /* Reset the retransmission timer. */
          pcb->rtime = 0;

          /* Reduce congestion window and ssthresh. */
          eff_wnd = LWIP_MIN(pcb->cwnd, pcb->snd_wnd);
          pcb->ssthresh = eff_wnd >> 1;
          if (pcb->ssthresh < (tcpwnd_size_t)(pcb->mss << 1)) {
            pcb->ssthresh = (pcb->mss << 1);
          }
          pcb->cwnd = pcb->mss;
          LWIP_DEBUGF(TCP_CWND_DEBUG, ("tcp_slowtmr: cwnd %"TCPWNDSIZE_F
                                       " ssthresh %"TCPWNDSIZE_F"\n",
                                       pcb->cwnd, pcb->ssthresh));

          /* The following needs to be called AFTER cwnd is set to one
             mss - STJ */
          tcp_rexmit_rto(pcb);
        }
      }
    }
    /* Check if this PCB has stayed too long in FIN-WAIT-2 */
    if (pcb->state == FIN_WAIT_2) {
      /* If this PCB is in FIN_WAIT_2 because of tcp_shut_tx don't let it time out. */
      if ((pcb->flags & TF_NOUSER)) {
        /* PCB was fully closed (either through close() or SHUT_RDWR):
           normal FIN-WAIT timeout handling. */
        if ((u32_t)(tcp_ticks - pcb->tmr) > TCP_FIN_WAIT_TIMEOUT / TCP_SLOW_INTERVAL) {
          ++pcb_remove;
          LWIP_DEBUGF(TCP_DEBUG, ("tcp_slowtmr: removing pcb stuck in FIN-WAIT-2\n"));
        }
      }
    }

    /* Check if KEEPALIVE should be sent */
    if (ip_get_option(pcb, SOF_KEEPALIVE) &&
       ((pcb->state == ESTABLISHED) ||
        (pcb->state == CLOSE_WAIT))) {
      if ((u32_t)(tcp_ticks - pcb->tmr) >
         (pcb->keep_idle + TCP_KEEP_DUR(pcb)) / TCP_SLOW_INTERVAL)
      {
        LWIP_DEBUGF(TCP_DEBUG, ("tcp_slowtmr: KEEPALIVE timeout. Aborting connection to "));
        ip_addr_debug_print(TCP_DEBUG, &pcb->remote_ip);
        LWIP_DEBUGF(TCP_DEBUG, ("\n"));

        ++pcb_remove;
        ++pcb_reset;
      }
      else if((u32_t)(tcp_ticks - pcb->tmr) >
              (pcb->keep_idle + pcb->keep_cnt_sent * TCP_KEEP_INTVL(pcb))
              / TCP_SLOW_INTERVAL)
      {
        err = tcp_keepalive(pcb);
        if (err == ERR_OK) {
          pcb->keep_cnt_sent++;
        }
      }
    }

    /* Check if this PCB has stayed too long in SYN-RCVD */
    if (pcb->state == SYN_RCVD) {
      if ((u32_t)(tcp_ticks - pcb->tmr) >
          TCP_SYN_RCVD_TIMEOUT / TCP_SLOW_INTERVAL) {
        ++pcb_remove;
        LWIP_DEBUGF(TCP_DEBUG, ("tcp_slowtmr: removing pcb stuck in SYN-RCVD\n"));
      }
    }

    /* Check if this PCB has stayed too long in LAST-ACK */
    if (pcb->state == LAST_ACK) {
      if ((u32_t)(tcp_ticks - pcb->tmr) > 2 * TCP_MSL / TCP_SLOW_INTERVAL) {
        ++pcb_remove;
        LWIP_DEBUGF(TCP_DEBUG, ("tcp_slowtmr: removing pcb stuck in LAST-ACK\n"));
      }
    }
    
    /* If the PCB should be removed, do it. */
    if (pcb_remove) {
      pcb->flags &= ~TF_ACK_DELAY;
      tcp_report_err(pcb, ERR_ABRT);
      tcp_pcb_free(pcb, pcb_reset, tcp_conn_iter.prev);
    } else {
      /* Try to output. */
      tcp_output(pcb);
    }
  }


  /* Steps through all of the TIME-WAIT PCBs. */
  tcp_iter_start(&tcp_conn_iter, tcp_tw_pcbs);
  
  while ((pcb = tcp_iter_next(&tcp_conn_iter))) {
    LWIP_ASSERT("tcp_slowtmr: TIME-WAIT pcb->state == TIME-WAIT", pcb->state == TIME_WAIT);
    pcb_remove = 0;

    /* Check if this PCB has stayed long enough in TIME-WAIT */
    if ((u32_t)(tcp_ticks - pcb->tmr) > 2 * TCP_MSL / TCP_SLOW_INTERVAL) {
      ++pcb_remove;
    }
    
    /* If the PCB should be removed, do it. */
    if (pcb_remove) {
      tcp_pcb_free(pcb, 0, tcp_conn_iter.prev);
    }
  }
}

/**
 * Is called every TCP_FAST_INTERVAL (250 ms) and sends delayed ACKs.
 *
 * Automatically called from tcp_tmr().
 */
void
tcp_fasttmr(void)
{
  struct tcp_pcb *pcb;

  ++tcp_timer_ctr;

  for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_ASSERT("tcp_fasttmr: pcb->state active", tcp_state_is_active(pcb->state));
    if (pcb->last_timer != tcp_timer_ctr) {
      pcb->last_timer = tcp_timer_ctr;
      /* send delayed ACKs */
      if (pcb->flags & TF_ACK_DELAY) {
        LWIP_DEBUGF(TCP_DEBUG, ("tcp_fasttmr: delayed ACK\n"));
        tcp_ack_now(pcb);
        tcp_output(pcb);
        pcb->flags &= ~(TF_ACK_DELAY | TF_ACK_NOW);
      }
    }
  }
}

/** Call tcp_output for all active pcbs that have TF_NAGLEMEMERR set */
void
tcp_txnow(void)
{
  struct tcp_pcb *pcb;

  for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_ASSERT("tcp_txnow: pcb->state active", tcp_state_is_active(pcb->state));
    if (pcb->flags & TF_NAGLEMEMERR) {
      tcp_output(pcb);
    }
  }
}

/**
 * Deallocates a list of TCP segments (tcp_seg structures).
 *
 * @param seg tcp_seg list of TCP segments to free
 */
void
tcp_segs_free(struct tcp_seg *seg)
{
  while (seg != NULL) {
    struct tcp_seg *next = seg->next;
    tcp_seg_free(seg);
    seg = next;
  }
}

/**
 * Frees a TCP segment (tcp_seg structure).
 *
 * @param seg single tcp_seg to free
 */
void
tcp_seg_free(struct tcp_seg *seg)
{
  if (seg != NULL) {
    if (seg->p != NULL) {
      pbuf_free(seg->p);
#if TCP_DEBUG
      seg->p = NULL;
#endif /* TCP_DEBUG */
    }
    memp_free(MEMP_TCP_SEG, seg);
  }
}

/**
 * Sets the priority of a connection.
 *
 * @param pcb the pcb to manipulate
 * @param prio new priority
 */
void
tcp_setprio(struct tcp_pcb_base *pcb, u8_t prio)
{
  LWIP_ASSERT("tcp_setprio without user reference", tcp_pcb_has_user_ref(pcb));
  
  pcb->prio = prio;
}

/**
 * Kills the oldest active connection that has the same or lower priority than
 * 'prio'.
 *
 * @param prio minimum priority
 */
static void
tcp_kill_prio(u8_t prio)
{
  struct tcp_pcb *pcb, *inactive;
  u32_t inactivity;
  u8_t mprio;

  mprio = LWIP_MIN(TCP_PRIO_MAX, prio);

  /* We kill the oldest active connection that has lower priority than prio. */
  inactivity = 0;
  inactive = NULL;
  for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_ASSERT("tcp_kill_prio: pcb->state active", tcp_state_is_active(pcb->state));
    if (pcb->prio <= mprio &&
       (u32_t)(tcp_ticks - pcb->tmr) >= inactivity) {
      inactivity = tcp_ticks - pcb->tmr;
      inactive = pcb;
      mprio = pcb->prio;
    }
  }
  if (inactive != NULL) {
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_kill_prio: killing oldest PCB %p (%"S32_F")\n",
           (void *)inactive, inactivity));
    tcp_report_err(inactive, ERR_ABRT);
    tcp_pcb_free(inactive, 1, NULL);
  }
}

/**
 * Kills the oldest connection that is in specific state.
 * Called from tcp_alloc() for LAST_ACK and CLOSING if no more connections are available.
 */
static void
tcp_kill_state(enum tcp_state state)
{
  struct tcp_pcb *pcb, *inactive;
  u32_t inactivity;

  LWIP_ASSERT("invalid state", (state == CLOSING) || (state == LAST_ACK));

  inactivity = 0;
  inactive = NULL;
  /* Go through the list of active pcbs and get the oldest pcb that is in state
     CLOSING/LAST_ACK. */
  for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_ASSERT("tcp_kill_state: pcb->state active", tcp_state_is_active(pcb->state));
    if (pcb->state == state) {
      if ((u32_t)(tcp_ticks - pcb->tmr) >= inactivity) {
        inactivity = tcp_ticks - pcb->tmr;
        inactive = pcb;
      }
    }
  }
  if (inactive != NULL) {
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_kill_closing: killing oldest %s PCB %p (%"S32_F")\n",
           tcp_state_str[state], (void *)inactive, inactivity));
    /* Don't send a RST, since no data is lost. */
    tcp_report_err(inactive, ERR_ABRT);
    tcp_pcb_free(inactive, 0, NULL);
  }
}

/**
 * Kills the oldest connection that is in TIME_WAIT state.
 * Called from tcp_alloc() if no more connections are available.
 */
static void
tcp_kill_timewait(void)
{
  struct tcp_pcb *pcb, *inactive;
  u32_t inactivity;

  inactivity = 0;
  inactive = NULL;
  /* Go through the list of TIME_WAIT pcbs and get the oldest pcb. */
  for (pcb = tcp_tw_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_ASSERT("tcp_kill_timewait: pcb->state == TIME_WAIT", pcb->state == TIME_WAIT);
    if ((u32_t)(tcp_ticks - pcb->tmr) >= inactivity) {
      inactivity = tcp_ticks - pcb->tmr;
      inactive = pcb;
    }
  }
  if (inactive != NULL) {
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_kill_timewait: killing oldest TIME-WAIT PCB %p (%"S32_F")\n",
           (void *)inactive, inactivity));
    tcp_pcb_free(inactive, 0, NULL);
  }
}

/**
 * Allocate a new tcp_pcb structure.
 *
 * @param prio priority for the new pcb
 * @return a new tcp_pcb that initially is in state CLOSED
 */
struct tcp_pcb *
tcp_alloc(u8_t prio)
{
  struct tcp_pcb *pcb;
  u32_t iss;

  pcb = (struct tcp_pcb *)memp_malloc(MEMP_TCP_PCB);
  if (pcb == NULL) {
    /* Try killing oldest connection in TIME-WAIT. */
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_alloc: killing off oldest TIME-WAIT connection\n"));
    tcp_kill_timewait();
    /* Try to allocate a tcp_pcb again. */
    pcb = (struct tcp_pcb *)memp_malloc(MEMP_TCP_PCB);
    if (pcb == NULL) {
      /* Try killing oldest connection in LAST-ACK (these wouldn't go to TIME-WAIT). */
      LWIP_DEBUGF(TCP_DEBUG, ("tcp_alloc: killing off oldest LAST-ACK connection\n"));
      tcp_kill_state(LAST_ACK);
      /* Try to allocate a tcp_pcb again. */
      pcb = (struct tcp_pcb *)memp_malloc(MEMP_TCP_PCB);
      if (pcb == NULL) {
        /* Try killing oldest connection in CLOSING. */
        LWIP_DEBUGF(TCP_DEBUG, ("tcp_alloc: killing off oldest CLOSING connection\n"));
        tcp_kill_state(CLOSING);
        /* Try to allocate a tcp_pcb again. */
        pcb = (struct tcp_pcb *)memp_malloc(MEMP_TCP_PCB);
        if (pcb == NULL) {
          /* Try killing active connections with lower priority than the new one. */
          LWIP_DEBUGF(TCP_DEBUG, ("tcp_alloc: killing connection with prio lower than %d\n", prio));
          tcp_kill_prio(prio);
          /* Try to allocate a tcp_pcb again. */
          pcb = (struct tcp_pcb *)memp_malloc(MEMP_TCP_PCB);
          if (pcb != NULL) {
            /* adjust err stats: memp_malloc failed multiple times before */
            MEMP_STATS_DEC(err, MEMP_TCP_PCB);
          }
        }
        if (pcb != NULL) {
          /* adjust err stats: memp_malloc failed multiple times before */
          MEMP_STATS_DEC(err, MEMP_TCP_PCB);
        }
      }
      if (pcb != NULL) {
        /* adjust err stats: memp_malloc failed multiple times before */
        MEMP_STATS_DEC(err, MEMP_TCP_PCB);
      }
    }
    if (pcb != NULL) {
      /* adjust err stats: memp_malloc failed above */
      MEMP_STATS_DEC(err, MEMP_TCP_PCB);
    }
  }
  if (pcb != NULL) {
    /* zero out the whole pcb, so there is no need to initialize members to zero */
    memset(pcb, 0, sizeof(struct tcp_pcb));
    pcb->prio = prio;
    pcb->snd_buf = TCP_SND_BUF;
    /* Start with a window that does not need scaling. When window scaling is
       enabled and used, the window is enlarged when both sides agree on scaling. */
    pcb->rcv_wnd = pcb->rcv_ann_wnd = TCPWND_MIN16(TCP_WND);
    pcb->ttl = TCP_TTL;
    /* As initial send MSS, we use TCP_MSS but limit it to 536.
       The send MSS is updated when an MSS option is received. */
    pcb->mss = INITIAL_MSS;
    pcb->rto = 3000 / TCP_SLOW_INTERVAL;
    pcb->sv = 3000 / TCP_SLOW_INTERVAL;
    pcb->rtime = -1;
    pcb->cwnd = 1;
    iss = tcp_next_iss();
    pcb->snd_wl2 = iss;
    pcb->snd_nxt = iss;
    pcb->lastack = iss;
    pcb->snd_lbb = iss;
    pcb->tmr = tcp_ticks;
    pcb->last_timer = tcp_timer_ctr;

    /* Init KEEPALIVE timer */
    pcb->keep_idle  = TCP_KEEPIDLE_DEFAULT;

#if LWIP_TCP_KEEPALIVE
    pcb->keep_intvl = TCP_KEEPINTVL_DEFAULT;
    pcb->keep_cnt   = TCP_KEEPCNT_DEFAULT;
#endif /* LWIP_TCP_KEEPALIVE */
  }
  return pcb;
}

/**
 * Creates a new TCP protocol control block but doesn't place it on
 * any of the TCP PCB lists.
 * The pcb is not put on any list until binding using tcp_bind().
 *
 * @internal: Maybe there should be a idle TCP PCB list where these
 * PCBs are put on. Port reservation using tcp_bind() is implemented but
 * allocated pcbs that are not bound can't be killed automatically if wanting
 * to allocate a pcb with higher prio (@see tcp_kill_prio())
 *
 * @return a new tcp_pcb that initially is in state CLOSED
 */
struct tcp_pcb *
tcp_new(void)
{
  return tcp_alloc(TCP_PRIO_NORMAL);
}

/**
 * Create a new TCP protocol control block for listening.
 * 
 * @return the listen PCB, or null if out of memory
 */
struct tcp_pcb_listen * tcp_new_listen (void)
{
  struct tcp_pcb_listen *lpcb = (struct tcp_pcb_listen *)memp_malloc(MEMP_TCP_PCB_LISTEN);
  if (lpcb != NULL) {
    memset(lpcb, 0, sizeof(*lpcb));
    lpcb->prio = TCP_PRIO_NORMAL;
    lpcb->ttl = TCP_TTL;
    lpcb->state = LISTEN_CLOS;
  }
  return lpcb;
}

#if LWIP_IPV6
/**
 * Creates a new TCP-over-IPv6 protocol control block but doesn't
 * place it on any of the TCP PCB lists.
 * The pcb is not put on any list until binding using tcp_bind().
 *
 * @return a new tcp_pcb that initially is in state CLOSED
 */
struct tcp_pcb *
tcp_new_ip6(void)
{
  struct tcp_pcb * pcb;
  pcb = tcp_alloc(TCP_PRIO_NORMAL);
#if LWIP_IPV4
  ip_set_v6(pcb, 1);
#endif /* LWIP_IPV4 */
  return pcb;
}

struct tcp_pcb_listen *
tcp_new_listen_ip6(void)
{
  struct tcp_pcb_listen * pcb;
  pcb = tcp_new_listen();
#if LWIP_IPV4
  ip_set_v6(pcb, 1);
#endif /* LWIP_IPV4 */
  return pcb;
}
#endif /* LWIP_IPV6 */

/**
 * Used to specify the argument that should be passed callback
 * functions. It is used for both listen and connection PCBs.
 *
 * @param pcb tcp_pcb to set the callback argument
 * @param arg void pointer argument to pass to callback functions
 */
void
tcp_arg(struct tcp_pcb_base *pcb, void *arg)
{
  LWIP_ASSERT("tcp_arg without user reference", tcp_pcb_has_user_ref(pcb));
  
  pcb->callback_arg = arg;
}

/**
 * Used to specify the function that should be called when a TCP
 * connection receives data.
 *
 * @param pcb tcp_pcb to set the recv callback
 * @param recv callback function to call for this pcb when data is received
 */
void
tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv)
{
  LWIP_ASSERT("tcp_recv on listen-pcb", !tcp_pcb_is_listen(pcb));
  LWIP_ASSERT("tcp_recv without user reference", !(pcb->flags & TF_NOUSER));
  
  pcb->recv = recv;
}

/**
 * Used to specify the function that should be called when TCP data
 * has been successfully delivered to the remote host.
 *
 * @param pcb tcp_pcb to set the sent callback
 * @param sent callback function to call for this pcb when data is successfully sent
 */
void
tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent)
{
  LWIP_ASSERT("tcp_sent on listen-pcb", !tcp_pcb_is_listen(pcb));
  LWIP_ASSERT("tcp_sent without user reference", !(pcb->flags & TF_NOUSER));
  
  pcb->sent = sent;
}

/**
 * Used to specify the function that should be called when a fatal error
 * has occurred on the connection.
 *
 * @param pcb tcp_pcb to set the err callback
 * @param err callback function to call for this pcb when a fatal error
 *        has occurred on the connection
 */
void
tcp_err(struct tcp_pcb *pcb, tcp_err_fn err)
{
  LWIP_ASSERT("tcp_err on listen-pcb", !tcp_pcb_is_listen(pcb));
  LWIP_ASSERT("tcp_err without user reference", !(pcb->flags & TF_NOUSER));
  
  pcb->errf = err;
}

/**
 * Used for specifying the function that should be called when a
 * LISTENing connection has been connected to another host.
 *
 * @param lpcb tcp_pcb_listen to set the accept callback
 * @param accept callback function to call for this pcb when LISTENing
 *        connection has been connected to another host
 */
void
tcp_accept(struct tcp_pcb_listen *lpcb, tcp_accept_fn accept)
{
  LWIP_ASSERT("tcp_accept on non-listen-pcb", tcp_pcb_is_listen(lpcb));
  
  lpcb->accept = accept;
}

/**
 * Determines whether a state is considered active, i.e.
 * whether PCBs in this state belong into tcp_active_pcbs.
 */
u8_t tcp_state_is_active(enum tcp_state state)
{
  switch (state) {
    case SYN_SENT:
    case SYN_RCVD:
    case ESTABLISHED:
    case FIN_WAIT_1:
    case FIN_WAIT_2:
    case CLOSE_WAIT:
    case CLOSING:
    case LAST_ACK:
      return 1;
    default:
      return 0;
  }
}

/* Axioms about the above lists:
   1) Every TCP PCB that is not CLOSED is in one of the lists.
   2) A PCB is only in one of the lists.
   3) All PCBs in the tcp_listen_pcbs list is in LISTEN state.
   4) All PCBs in the tcp_tw_pcbs list is in TIME-WAIT state.
*/
/* Define two functions, tcp_reg and tcp_rmv that registers a TCP PCB
   with a PCB list or removes a PCB from a list, respectively. */

void tcp_reg(struct tcp_pcb_base **pcbs, struct tcp_pcb_base *npcb)
{
#if TCP_DEBUG_PCB_LISTS
  struct tcp_pcb_base *tcp_tmp_pcb;
  LWIP_DEBUGF(TCP_DEBUG, ("TCP_REG %p local port %d\n", npcb, npcb->local_port));
  for (tcp_tmp_pcb = *pcbs; tcp_tmp_pcb != NULL; tcp_tmp_pcb = tcp_tmp_pcb->next) {
    LWIP_ASSERT("TCP_REG: already registered\n", tcp_tmp_pcb != npcb);
  }
  LWIP_ASSERT("TCP_REG: pcb->state != CLOSED", (pcbs == &tcp_bound_pcbs) || (npcb->state != CLOSED));
  npcb->next = *pcbs;
  LWIP_ASSERT("TCP_REG: npcb->next != npcb", npcb->next != npcb);
  *pcbs = npcb;
  LWIP_ASSERT("TCP_RMV: tcp_pcbs sane", tcp_pcbs_sane());
#else
  npcb->next = *pcbs;
  *pcbs = npcb;
#endif
  tcp_timer_needed();
}

void tcp_rmv(struct tcp_pcb_base **pcbs, struct tcp_pcb_base *npcb)
{
  struct tcp_pcb_base *tcp_tmp_pcb;
#if TCP_DEBUG_PCB_LISTS
  LWIP_ASSERT("TCP_RMV: pcbs != NULL", *pcbs != NULL);
  LWIP_DEBUGF(TCP_DEBUG, ("TCP_RMV: removing %p from %p\n", npcb, *pcbs));
  if (*pcbs == npcb) {
    *pcbs = (*pcbs)->next;
  } else {
    for (tcp_tmp_pcb = *pcbs; tcp_tmp_pcb != NULL; tcp_tmp_pcb = tcp_tmp_pcb->next) {
      if (tcp_tmp_pcb->next == npcb) {
        tcp_tmp_pcb->next = npcb->next;
        break;
      }
    }
  }
  npcb->next = NULL;
  LWIP_ASSERT("TCP_RMV: tcp_pcbs sane", tcp_pcbs_sane());
  LWIP_DEBUGF(TCP_DEBUG, ("TCP_RMV: removed %p from %p\n", npcb, *pcbs));
#else
  if (*pcbs == npcb) {
    *pcbs = (*pcbs)->next;
  } else {
    for (tcp_tmp_pcb = *pcbs; tcp_tmp_pcb != NULL; tcp_tmp_pcb = tcp_tmp_pcb->next) {
      if (tcp_tmp_pcb->next == npcb) {
        tcp_tmp_pcb->next = npcb->next;
        break;
      }
    }
  }
  npcb->next = NULL;
#endif
}

void tcp_iter_start (struct tcp_iter *it, struct tcp_pcb *pcblist)
{
  it->current = pcblist;
  it->prev = NULL;
  it->next_is_current = 1;
}

struct tcp_pcb * tcp_iter_next (struct tcp_iter *it)
{
  if (it->next_is_current) {
    /* Returning current as the next. This happens at start
     * of iteration and after the current has been removed. */
    it->next_is_current = 0;
  } else {
    /* Advancing current. */
    LWIP_ASSERT("tcp_iter_next: current != NULL", it->current != NULL);
    it->prev = it->current;
    it->current = it->current->next;
  }
  return it->current;
}

void tcp_iter_will_remove (struct tcp_iter *it, struct tcp_pcb *pcb, struct tcp_pcb *pcblist)
{
  LWIP_ASSERT("tcp_iter_will_remove: pcb != NULL", pcb != NULL);
  LWIP_ASSERT("tcp_iter_will_remove: pcblist != NULL", pcblist != NULL); /* pcb is in pcblist */
  
  if (it->current != NULL) {
    if (pcb == it->current) {
      /* Removing current - advance it and set next_is_current so
       * tcp_iter_next will return that one next time it's called. */
      it->current = it->current->next;
      it->next_is_current = 1;
    }
    else if (pcb == it->prev) {
      /* Removing prev - fixup prev to the predecessor of prev. */
      struct tcp_pcb *prev_prev;
      struct tcp_pcb *ipcb;
      
      LWIP_ASSERT("tcp_iter_unref: pcb->next inconsistent", pcb->next == it->current);
      
      prev_prev = NULL;
      for (ipcb = pcblist; ipcb != NULL; ipcb = ipcb->next) {
        if (ipcb == pcb) {
          break;
        }
        prev_prev = ipcb;
      }
      LWIP_ASSERT("tcp_iter_unref: prev not found", ipcb != NULL);
      
      it->prev = prev_prev;
    }
  }
}

void tcp_iter_will_prepend (struct tcp_iter *it, struct tcp_pcb *pcb, struct tcp_pcb *pcblist)
{
  LWIP_ASSERT("tcp_iter_will_prepend: pcb != NULL", pcb != NULL);
  
  if (pcblist != NULL && pcblist == it->current) {
    /* Inserting just before current - fixup prev. */
    LWIP_ASSERT("tcp_iter_will_prepend: prev == NULL", it->prev == NULL);
    it->prev = pcb;
  }
}

/**
 * Purges a TCP PCB. Removes any buffered data and frees the buffer memory.
 *
 * @param pcb tcp_pcb to purge. The pcb itself is not deallocated!
 */
void
tcp_pcb_purge(struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_pcb_purge not active", tcp_state_is_active(pcb->state));
  
  LWIP_DEBUGF(TCP_DEBUG, ("tcp_pcb_purge\n"));

  tcp_backlog_accepted_internal(pcb);

  if (pcb->sndq != NULL) {
    LWIP_DEBUGF(TCP_DEBUG, ("tcp_pcb_purge: data left on send queue\n"));
  }

  /* Stop the retransmission timer as it will expect data on sndq if it fires */
  pcb->rtime = -1;

  tcp_segs_free(pcb->sndq);
  pcb->sndq = NULL;
  pcb->sndq_last = NULL;
  pcb->sndq_next = NULL;
}

void tcp_pcb_free(struct tcp_pcb *pcb, u8_t send_rst, struct tcp_pcb *prev)
{
  LWIP_ASSERT("tcp_pcb_free on listen-pcb", !tcp_pcb_is_listen(pcb));
  
  /* Remove any tcp_input_pcb reference so callers can see that the PCB is gone. */
  if (tcp_input_pcb == pcb) {
    tcp_input_pcb = NULL;
  }
  
  if (pcb->state == CLOSED) {
    if (pcb->local_port != 0) {
      tcp_rmv(&tcp_bound_pcbs, to_tcp_pcb_base(pcb));
    }
  }
  else if (tcp_state_is_active(pcb->state)) {
    if ((pcb->flags & TF_ACK_DELAY)) {
      pcb->flags |= TF_ACK_NOW;
      tcp_output(pcb);
    }
    
    if (send_rst) {
      tcp_rst(pcb->snd_nxt, pcb->rcv_nxt, &pcb->local_ip, &pcb->remote_ip, pcb->local_port, pcb->remote_port);
    }
    
    tcp_iter_will_remove(&tcp_conn_iter, pcb, tcp_active_pcbs);
    if (prev != NULL) { /* optimization if the caller knows the previous PCB */
      LWIP_ASSERT("prev->next == pcb", prev->next == pcb);
      prev->next = pcb->next;
    } else {
      tcp_rmv((struct tcp_pcb_base **)&tcp_active_pcbs, to_tcp_pcb_base(pcb));
    }
    
    // NOTE: This must be after any tcp_output above, because tcp_output
    // may add segments to the queues. Otherwise we could leak segments.
    tcp_pcb_purge(pcb);
  }
  else if (pcb->state == TIME_WAIT) {
    tcp_iter_will_remove(&tcp_conn_iter, pcb, tcp_tw_pcbs);
    tcp_rmv((struct tcp_pcb_base **)&tcp_tw_pcbs, to_tcp_pcb_base(pcb));
    
    /* tcp_pcb_purge has been done already in tcp_move_to_time_wait */
  }
  
  LWIP_ASSERT("send queue segments leaking", pcb->sndq == NULL);
  
  memp_free(MEMP_TCP_PCB, pcb);
  
  LWIP_ASSERT("tcp_pcb_free: tcp_pcbs_sane()", tcp_pcbs_sane());
}

void tcp_move_to_time_wait(struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_move_to_time_wait active state", tcp_state_is_active(pcb->state));
  
  tcp_iter_will_remove(&tcp_conn_iter, pcb, tcp_active_pcbs);
  tcp_rmv((struct tcp_pcb_base **)&tcp_active_pcbs, to_tcp_pcb_base(pcb));
  
  tcp_pcb_purge(pcb);
  
  pcb->state = TIME_WAIT;
  
  tcp_iter_will_prepend(&tcp_conn_iter, pcb, tcp_tw_pcbs);
  tcp_reg((struct tcp_pcb_base **)&tcp_tw_pcbs, to_tcp_pcb_base(pcb));
  
  LWIP_ASSERT("tcp_move_to_time_wait: tcp_pcbs_sane()", tcp_pcbs_sane());
}

void tcp_report_err(struct tcp_pcb *pcb, err_t err)
{
  /* Only report error if we have a user reference. */
  if (!(pcb->flags & TF_NOUSER)) {
    /* Remember that we no longer have a user reference. */
    pcb->flags |= TF_NOUSER;
    
    /* Call the error function. */
    if (pcb->errf != NULL) {
      pcb->errf(pcb->callback_arg, err);
    }
  }
}

struct tcp_seg * tcp_sndq_pop(struct tcp_pcb *pcb)
{
  LWIP_ASSERT("tcp_sndq_pop: pcb->sndq != NULL", pcb->sndq != NULL);
  
  struct tcp_seg *seg = pcb->sndq;
  pcb->sndq = seg->next;
  if (pcb->sndq == NULL) {
    pcb->sndq_last = NULL;
  }
  if (pcb->sndq_next == seg) {
    pcb->sndq_next = pcb->sndq;
  }
  
  u8_t clen = pbuf_clen(seg->p);
  LWIP_ASSERT("tcp_sndq_pop: pcb->snd_queuelen >= clen", pcb->snd_queuelen >= clen);
  pcb->snd_queuelen -= clen;
  
  return seg;
}

/**
 * Calculates a new initial sequence number for new connections.
 *
 * @return u32_t pseudo random sequence number
 */
u32_t
tcp_next_iss(void)
{
  static u32_t iss = 6510;

  iss += tcp_ticks;       /* XXX */
  return iss;
}

#if TCP_CALCULATE_EFF_SEND_MSS
/**
 * Calculates the effective send mss that can be used for a specific IP address
 * by using ip_route to determine the netif used to send to the address and
 * calculating the minimum of TCP_MSS and that netif's mtu (if set).
 */
u16_t
tcp_eff_send_mss_impl(u16_t sendmss, const ip_addr_t *dest
#if LWIP_IPV6 || LWIP_IPV4_SRC_ROUTING
                     , const ip_addr_t *src
#endif /* LWIP_IPV6 || LWIP_IPV4_SRC_ROUTING */
#if LWIP_IPV6 && LWIP_IPV4
                     , u8_t isipv6
#endif /* LWIP_IPV6 && LWIP_IPV4 */
                     )
{
  u16_t mss_s;
  struct netif *outif;
  s16_t mtu;

  outif = ip_route(isipv6, src, dest);
#if LWIP_IPV6
#if LWIP_IPV4
  if (isipv6)
#endif /* LWIP_IPV4 */
  {
    /* First look in destination cache, to see if there is a Path MTU. */
    mtu = nd6_get_destination_mtu(ip_2_ip6(dest), outif);
  }
#if LWIP_IPV4
  else
#endif /* LWIP_IPV4 */
#endif /* LWIP_IPV6 */
#if LWIP_IPV4
  {
    if (outif == NULL) {
      return sendmss;
    }
    mtu = outif->mtu;
  }
#endif /* LWIP_IPV4 */

  if (mtu != 0) {
#if LWIP_IPV6
#if LWIP_IPV4
    if (isipv6)
#endif /* LWIP_IPV4 */
    {
      mss_s = mtu - IP6_HLEN - TCP_HLEN;
    }
#if LWIP_IPV4
    else
#endif /* LWIP_IPV4 */
#endif /* LWIP_IPV6 */
#if LWIP_IPV4
    {
      mss_s = mtu - IP_HLEN - TCP_HLEN;
    }
#endif /* LWIP_IPV4 */
    /* RFC 1122, chap 4.2.2.6:
     * Eff.snd.MSS = min(SendMSS+20, MMS_S) - TCPhdrsize - IPoptionsize
     * We correct for TCP options in tcp_write(), and don't support IP options.
     */
    sendmss = LWIP_MIN(sendmss, mss_s);
  }
  return sendmss;
}
#endif /* TCP_CALCULATE_EFF_SEND_MSS */

#if LWIP_IPV4
/** Helper function for tcp_netif_ipv4_addr_changed() that iterates a pcb list */
static void
tcp_netif_ipv4_addr_changed_pcblist(const ip4_addr_t* old_addr, struct tcp_pcb_base *pcb_list)
{
  struct tcp_pcb_base *pcb;
  pcb = pcb_list;
  while (pcb != NULL) {
    /* PCB bound to current local interface address? */
    if (!tcp_pcb_is_listen(pcb) && !IP_IS_V6_VAL(pcb->local_ip) && ip4_addr_cmp(ip_2_ip4(&pcb->local_ip), old_addr)) {
      /* this connection must be aborted */
      struct tcp_pcb_base *next = pcb->next;
      struct tcp_pcb *tpcb = (struct tcp_pcb *)pcb;
      LWIP_DEBUGF(NETIF_DEBUG | LWIP_DBG_STATE, ("netif_set_ipaddr: aborting TCP pcb %p\n", (void *)tpcb));
      tcp_report_err(tpcb, ERR_ABRT);
      tcp_pcb_free(tpcb, 1, NULL);
      pcb = next;
    } else {
      pcb = pcb->next;
    }
  }
}

/** This function is called from netif.c when address is changed or netif is removed
 *
 * @param old_addr IPv4 address of the netif before change
 * @param new_addr IPv4 address of the netif after change or NULL if netif has been removed
 */
void tcp_netif_ipv4_addr_changed(const ip4_addr_t* old_addr, const ip4_addr_t* new_addr)
{
  struct tcp_pcb_listen *lpcb, *next;

  tcp_netif_ipv4_addr_changed_pcblist(old_addr, to_tcp_pcb_base(tcp_active_pcbs));
  tcp_netif_ipv4_addr_changed_pcblist(old_addr, tcp_bound_pcbs);

  if (!ip4_addr_isany(new_addr)) {
    /* PCB bound to current local interface address? */
    for (lpcb = tcp_listen_pcbs; lpcb != NULL; lpcb = next) {
      next = lpcb->next;
      /* Is this an IPv4 pcb? */
      if (!IP_IS_V6_VAL(lpcb->local_ip)) {
        /* PCB bound to current local interface address? */
        if ((!(ip4_addr_isany(ip_2_ip4(&lpcb->local_ip)))) &&
            (ip4_addr_cmp(ip_2_ip4(&lpcb->local_ip), old_addr))) {
          /* The PCB is listening to the old ipaddr and
           * is set to listen to the new one instead */
              ip_addr_copy_from_ip4(lpcb->local_ip, *new_addr);
        }
      }
    }
  }
}
#endif /* LWIP_IPV4 */

const char*
tcp_debug_state_str(enum tcp_state s)
{
  return tcp_state_str[s];
}

#if TCP_DEBUG || TCP_INPUT_DEBUG || TCP_OUTPUT_DEBUG
/**
 * Print a tcp header for debugging purposes.
 *
 * @param tcphdr pointer to a struct tcp_hdr
 */
void
tcp_debug_print(struct tcp_hdr *tcphdr)
{
  LWIP_DEBUGF(TCP_DEBUG, ("TCP header:\n"));
  LWIP_DEBUGF(TCP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(TCP_DEBUG, ("|    %5"U16_F"      |    %5"U16_F"      | (src port, dest port)\n",
         lwip_ntohs(tcphdr->src), lwip_ntohs(tcphdr->dest)));
  LWIP_DEBUGF(TCP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(TCP_DEBUG, ("|           %010"U32_F"          | (seq no)\n",
          lwip_ntohl(tcphdr->seqno)));
  LWIP_DEBUGF(TCP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(TCP_DEBUG, ("|           %010"U32_F"          | (ack no)\n",
         lwip_ntohl(tcphdr->ackno)));
  LWIP_DEBUGF(TCP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(TCP_DEBUG, ("| %2"U16_F" |   |%"U16_F"%"U16_F"%"U16_F"%"U16_F"%"U16_F"%"U16_F"|     %5"U16_F"     | (hdrlen, flags (",
       TCPH_HDRLEN(tcphdr),
         (u16_t)(TCPH_FLAGS(tcphdr) >> 5 & 1),
         (u16_t)(TCPH_FLAGS(tcphdr) >> 4 & 1),
         (u16_t)(TCPH_FLAGS(tcphdr) >> 3 & 1),
         (u16_t)(TCPH_FLAGS(tcphdr) >> 2 & 1),
         (u16_t)(TCPH_FLAGS(tcphdr) >> 1 & 1),
         (u16_t)(TCPH_FLAGS(tcphdr)      & 1),
         lwip_ntohs(tcphdr->wnd)));
  tcp_debug_print_flags(TCPH_FLAGS(tcphdr));
  LWIP_DEBUGF(TCP_DEBUG, ("), win)\n"));
  LWIP_DEBUGF(TCP_DEBUG, ("+-------------------------------+\n"));
  LWIP_DEBUGF(TCP_DEBUG, ("|    0x%04"X16_F"     |     %5"U16_F"     | (chksum, urgp)\n",
         lwip_ntohs(tcphdr->chksum), lwip_ntohs(tcphdr->urgp)));
  LWIP_DEBUGF(TCP_DEBUG, ("+-------------------------------+\n"));
}

/**
 * Print a tcp state for debugging purposes.
 *
 * @param s enum tcp_state to print
 */
void
tcp_debug_print_state(enum tcp_state s)
{
  LWIP_DEBUGF(TCP_DEBUG, ("State: %s\n", tcp_state_str[s]));
}

/**
 * Print tcp flags for debugging purposes.
 *
 * @param flags tcp flags, all active flags are printed
 */
void
tcp_debug_print_flags(u8_t flags)
{
  if (flags & TCP_FIN) {
    LWIP_DEBUGF(TCP_DEBUG, ("FIN "));
  }
  if (flags & TCP_SYN) {
    LWIP_DEBUGF(TCP_DEBUG, ("SYN "));
  }
  if (flags & TCP_RST) {
    LWIP_DEBUGF(TCP_DEBUG, ("RST "));
  }
  if (flags & TCP_PSH) {
    LWIP_DEBUGF(TCP_DEBUG, ("PSH "));
  }
  if (flags & TCP_ACK) {
    LWIP_DEBUGF(TCP_DEBUG, ("ACK "));
  }
  if (flags & TCP_URG) {
    LWIP_DEBUGF(TCP_DEBUG, ("URG "));
  }
  if (flags & TCP_ECE) {
    LWIP_DEBUGF(TCP_DEBUG, ("ECE "));
  }
  if (flags & TCP_CWR) {
    LWIP_DEBUGF(TCP_DEBUG, ("CWR "));
  }
  LWIP_DEBUGF(TCP_DEBUG, ("\n"));
}

/**
 * Print all tcp_pcbs in every list for debugging purposes.
 */
void
tcp_debug_print_pcbs(void)
{
  struct tcp_pcb *pcb;
  struct tcp_pcb_listen *lpcb;
  LWIP_DEBUGF(TCP_DEBUG, ("Active PCB states:\n"));
  for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_DEBUGF(TCP_DEBUG, ("Local port %"U16_F", foreign port %"U16_F" snd_nxt %"U32_F" rcv_nxt %"U32_F" ",
                       pcb->local_port, pcb->remote_port,
                       pcb->snd_nxt, pcb->rcv_nxt));
    tcp_debug_print_state(pcb->state);
  }    
  LWIP_DEBUGF(TCP_DEBUG, ("Listen PCB states:\n"));
  for (lpcb = tcp_listen_pcbs; lpcb != NULL; lpcb = lpcb->next) {
    LWIP_DEBUGF(TCP_DEBUG, ("Local port %"U16_F" ", lpcb->local_port));
    tcp_debug_print_state(lpcb->state);
  }
  LWIP_DEBUGF(TCP_DEBUG, ("TIME-WAIT PCB states:\n"));
  for (pcb = tcp_tw_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_DEBUGF(TCP_DEBUG, ("Local port %"U16_F", foreign port %"U16_F" snd_nxt %"U32_F" rcv_nxt %"U32_F" ",
                       pcb->local_port, pcb->remote_port,
                       pcb->snd_nxt, pcb->rcv_nxt));
    tcp_debug_print_state(pcb->state);
  }
}

/**
 * Check state consistency of the tcp_pcb lists.
 */
s16_t
tcp_pcbs_sane(void)
{
  struct tcp_pcb *pcb;
  for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_ASSERT("tcp_pcbs_sane: active pcb->state", tcp_state_is_active(pcb->state));
  }
  for (pcb = tcp_tw_pcbs; pcb != NULL; pcb = pcb->next) {
    LWIP_ASSERT("tcp_pcbs_sane: tw pcb->state == TIME-WAIT", pcb->state == TIME_WAIT);
  }
  return 1;
}
#endif /* TCP_DEBUG */

#endif /* LWIP_TCP */
