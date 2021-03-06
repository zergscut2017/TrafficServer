/** @file

    A brief file description

    @section license License

    Licensed to the Apache Software Foundation (ASF) under one
    or more contributor license agreements.  See the NOTICE file
    distributed with this work for additional information
    regarding copyright ownership.  The ASF licenses this file
    to you under the Apache License, Version 2.0 (the
    "License"); you may not use this file except in compliance
    with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "P_Net.h"

#define STATE_VIO_OFFSET ((uintptr_t)&((NetState*)0)->vio)
#define STATE_FROM_VIO(_x) ((NetState*)(((char*)(_x)) - STATE_VIO_OFFSET))

#define disable_read(_vc) (_vc)->read.enabled = 0
#define disable_write(_vc) (_vc)->write.enabled = 0
#define enable_read(_vc) (_vc)->read.enabled = 1
#define enable_write(_vc) (_vc)->write.enabled = 1

typedef struct iovec IOVec;
#ifndef UIO_MAXIOV
#define NET_MAX_IOV 16          // UIO_MAXIOV shall be at least 16 1003.1g (5.4.1.1)
#else
#define NET_MAX_IOV UIO_MAXIOV
#endif

struct SpdyProberCont:public Continuation
{
  MIOBuffer buf;
  unsigned char data;
  SpdyProberCont(): data(0)
  {
    SET_HANDLER(&SpdyProberCont::mainEvent);
  }

  int mainEvent(int event, void *e);
};

static ClassAllocator<SpdyProberCont> spdyProberContAllocator("spdyProberContAllocator");

SpdyProberCont *
new_SpdyProberCont(UnixNetVConnection *vc)
{
  SpdyProberCont *c = spdyProberContAllocator.alloc();
  c->buf.clear();
  c->buf.set(&c->data, sizeof c->data);
  c->buf._writer->fill(-(sizeof c->data));
  c->mutex = vc->mutex;
  return c;
}
void
free_SpdyProberCont(SpdyProberCont *c)
{
  c->mutex.clear();
  c->buf.clear();
  spdyProberContAllocator.free(c);
}

inline int
SpdyProberCont::mainEvent(int event, void *e) {
  UnixNetVConnection *vc = (UnixNetVConnection *) ((VIO *) e)->vc_server;
  ProbeType probe_type = vc->pt;
  vc->pt = PROBE_END;

  switch (event) {
  case VC_EVENT_EOS:
  case VC_EVENT_ERROR:
  case VC_EVENT_INACTIVITY_TIMEOUT:
  case VC_EVENT_ACTIVE_TIMEOUT:
    vc->do_io_close();
    free_SpdyProberCont(this);
    return EVENT_DONE;
  case VC_EVENT_READ_COMPLETE:
    if ((data & 0x80) != 0) {
      free_SpdyProberCont(this);
      vc->proto_type = NET_PROTO_HTTP_SPDY;
      if (probe_type == PROBE_SPDY_FOR_CORE) {
        vc->action_.continuation->handleEvent(NET_EVENT_ACCEPT, vc);
      } else {
        ink_assert(probe_type == PROBE_SPDY_FOR_PLUGIN);
        spdy_accept(NET_EVENT_ACCEPT, vc);
      }
      return EVENT_DONE;
    } else {
      // normal http request
      free_SpdyProberCont(this);
      vc->action_.continuation->handleEvent(NET_EVENT_ACCEPT, vc);
      return EVENT_DONE;
    }
  default:
    ink_release_assert(!"unexpected event");
  }
  return EVENT_CONT;
}

int SpdyProbe(UnixNetVConnection *vc)
{
  SpdyProberCont *spdyProber = new_SpdyProberCont(vc);
  vc->set_inactivity_timeout(HRTIME_SECONDS(30));
  vc->do_io_read(spdyProber, 1, &spdyProber->buf);
  return EVENT_CONT;
}
// Global
ClassAllocator<UnixNetVConnection> netVCAllocator("netVCAllocator");

//
// Reschedule a UnixNetVConnection by moving it
// onto or off of the ready_list
//
static inline void
read_reschedule(NetHandler *nh, UnixNetVConnection *vc)
{
  vc->ep.refresh(EVENTIO_READ);
  if (vc->read.triggered && vc->read.enabled) {
    nh->read_ready_list.in_or_enqueue(vc);
  } else
    nh->read_ready_list.remove(vc);
}

static inline void
write_reschedule(NetHandler *nh, UnixNetVConnection *vc)
{
  vc->ep.refresh(EVENTIO_WRITE);
  if (vc->write.triggered && vc->write.enabled) {
    nh->write_ready_list.in_or_enqueue(vc);
  } else
    nh->write_ready_list.remove(vc);
}

void
net_activity(UnixNetVConnection *vc, EThread *thread)
{
  (void) thread;
#ifdef INACTIVITY_TIMEOUT
  if (vc->inactivity_timeout && vc->inactivity_timeout_in && vc->inactivity_timeout->ethread == thread)
    vc->inactivity_timeout->schedule_in(vc->inactivity_timeout_in);
  else {
    if (vc->inactivity_timeout)
      vc->inactivity_timeout->cancel_action();
    if (vc->inactivity_timeout_in) {
      vc->inactivity_timeout = vc->thread->schedule_in_local(vc, vc->inactivity_timeout_in);
    } else
      vc->inactivity_timeout = 0;
  }
#else
  if (vc->inactivity_timeout_in)
    vc->next_inactivity_timeout_at = ink_get_hrtime() + vc->inactivity_timeout_in;
  else
    vc->next_inactivity_timeout_at = 0;
#endif

}

//
// Function used to close a UnixNetVConnection and free the vc
//
void
close_UnixNetVConnection(UnixNetVConnection *vc, EThread *t)
{
  NetHandler *nh = vc->nh;
  vc->cancel_OOB();
  vc->ep.stop();
  vc->con.close();
#ifdef INACTIVITY_TIMEOUT
  if (vc->inactivity_timeout) {
    vc->inactivity_timeout->cancel_action(vc);
    vc->inactivity_timeout = NULL;
  }
#else
  vc->next_inactivity_timeout_at = 0;
#endif
  vc->inactivity_timeout_in = 0;
  if (vc->active_timeout) {
    vc->active_timeout->cancel_action(vc);
    vc->active_timeout = NULL;
  }
  vc->active_timeout_in = 0;
  nh->open_list.remove(vc);
  nh->cop_list.remove(vc);
  nh->read_ready_list.remove(vc);
  nh->write_ready_list.remove(vc);
  if (vc->read.in_enabled_list) {
    nh->read_enable_list.remove(vc);
    vc->read.in_enabled_list = 0;
  }
  if (vc->write.in_enabled_list) {
    nh->write_enable_list.remove(vc);
    vc->write.in_enabled_list = 0;
  }
  vc->read_fct.reset();
  vc->write_fct.reset();
  vc->free(t);
}

//
// Signal an event
//
static inline int
read_signal_and_update(int event, UnixNetVConnection *vc)
{
  vc->recursion++;
  vc->read.vio._cont->handleEvent(event, &vc->read.vio);
  if (!--vc->recursion && vc->closed) {
    /* BZ  31932 */
    ink_debug_assert(vc->thread == this_ethread());
    close_UnixNetVConnection(vc, vc->thread);
    return EVENT_DONE;
  } else {
    return EVENT_CONT;
  }
}

static inline int
write_signal_and_update(int event, UnixNetVConnection *vc)
{
  vc->recursion++;
  vc->write.vio._cont->handleEvent(event, &vc->write.vio);
  if (!--vc->recursion && vc->closed) {
    /* BZ  31932 */
    ink_debug_assert(vc->thread == this_ethread());
    close_UnixNetVConnection(vc, vc->thread);
    return EVENT_DONE;
  } else {
    return EVENT_CONT;
  }
}

static inline int
read_signal_done(int event, NetHandler *nh, UnixNetVConnection *vc)
{
  vc->read.enabled = 0;
  if (read_signal_and_update(event, vc) == EVENT_DONE) {
    return EVENT_DONE;
  } else {
    read_reschedule(nh, vc);
    return EVENT_CONT;
  }
}

static inline int
write_signal_done(int event, NetHandler *nh, UnixNetVConnection *vc)
{
  vc->write.enabled = 0;
  if (write_signal_and_update(event, vc) == EVENT_DONE) {
    return EVENT_DONE;
  } else {
    write_reschedule(nh, vc);
    return EVENT_CONT;
  }
}

static inline int
read_signal_error(NetHandler *nh, UnixNetVConnection *vc, int lerrno)
{
  vc->lerrno = lerrno;
  return read_signal_done(VC_EVENT_ERROR, nh, vc);
}

static inline int
write_signal_error(NetHandler *nh, UnixNetVConnection *vc, int lerrno)
{
  vc->lerrno = lerrno;
  return write_signal_done(VC_EVENT_ERROR, nh, vc);
}

// Read the data for a UnixNetVConnection.
// Rescheduling the UnixNetVConnection by moving the VC
// onto or off of the ready_list.
// Had to wrap this function with net_read_io for SSL.
static void
read_from_net(NetHandler *nh, UnixNetVConnection *vc, EThread *thread)
{
  NetState *s = &vc->read;
  ProxyMutex *mutex = thread->mutex;
  MIOBufferAccessor & buf = s->vio.buffer;
  int64_t r = 0;

  MUTEX_TRY_LOCK_FOR(lock, s->vio.mutex, thread, s->vio._cont);

  if (!lock || lock.m.m_ptr != s->vio.mutex.m_ptr) {
    read_reschedule(nh, vc);
    return;
  }
  // if it is not enabled.
  if (!s->enabled || s->vio.op != VIO::READ || vc->read_fct.e_flowctl) {
    read_disable(nh, vc);
    return;
  }

  ink_debug_assert(buf.writer());

  // if there is nothing to do, disable connection
  int64_t ntodo = s->vio.ntodo();
  if (ntodo <= 0) {
    read_disable(nh, vc);
    return;
  }
  int64_t toread = buf.writer()->write_avail();
  if (toread > ntodo)
    toread = ntodo;

  // read data
  int64_t rattempted = 0, total_read = 0;
  int niov = 0;
  IOVec tiovec[NET_MAX_IOV];
  if (toread) {
    IOBufferBlock *b = buf.mbuf->_writer;
    do {
      niov = 0;
      rattempted = 0;
      while (b && niov < NET_MAX_IOV) {
        int64_t a = b->write_avail();
        if (a > 0) {
          tiovec[niov].iov_base = b->_end;
          int64_t togo = toread - total_read - rattempted;
          if (a > togo)
            a = togo;
          tiovec[niov].iov_len = a;
          rattempted += a;
          niov++;
          if (a >= togo)
            break;
        }
        b = b->next;
      }
      ink_assert(vc->pt != PROBE_SPDY_FOR_CORE || vc->pt != PROBE_SPDY_FOR_PLUGIN || niov == 1);
      if (niov == 1) {
        if (vc->pt == PROBE_SPDY_FOR_CORE || vc->pt == PROBE_SPDY_FOR_PLUGIN) {
          r = recv(vc->con.fd, tiovec[0].iov_base, tiovec[0].iov_len, MSG_PEEK);
        } else
          r = socketManager.read(vc->con.fd, tiovec[0].iov_base, tiovec[0].iov_len);
      } else {
        r = socketManager.readv(vc->con.fd, &tiovec[0], niov);
      }
      NET_DEBUG_COUNT_DYN_STAT(net_calls_to_read_stat, 1);
      total_read += rattempted;
    } while (r == rattempted && total_read < toread);

    // if we have already moved some bytes successfully, summarize in r
    if (total_read != rattempted) {
      if (r <= 0)
        r = total_read - rattempted;
      else
        r = total_read - rattempted + r;
    }
    // check for errors
    if (r <= 0) {

      if (r == -EAGAIN || r == -ENOTCONN) {
        NET_DEBUG_COUNT_DYN_STAT(net_calls_to_read_nodata_stat, 1);
        vc->read.triggered = 0;
        nh->read_ready_list.remove(vc);
        return;
      }

      if (!r || r == -ECONNRESET) {
        vc->read.triggered = 0;
        nh->read_ready_list.remove(vc);
        read_signal_done(VC_EVENT_EOS, nh, vc);
        return;
      }
      vc->read.triggered = 0;
      read_signal_error(nh, vc, (int)-r);
      return;
    }
    NET_SUM_DYN_STAT(net_read_bytes_stat, r);

    // Add data to buffer and signal continuation.
    buf.writer()->fill(r);
#ifdef DEBUG
    if (buf.writer()->write_avail() <= 0)
      Debug("iocore_net", "read_from_net, read buffer full");
#endif
    s->vio.ndone += r;
    net_activity(vc, thread);
  } else
    r = 0;

  // Signal read ready, check if user is not done
  if (r) {
    // If there are no more bytes to read, signal read complete
    ink_assert(ntodo >= 0);
    if (s->vio.ntodo() <= 0) {
      read_signal_done(VC_EVENT_READ_COMPLETE, nh, vc);
      Debug("iocore_net", "read_from_net, read finished - signal done");
      return;
    } else {
      if (read_signal_and_update(VC_EVENT_READ_READY, vc) != EVENT_CONT)
        return;
      // change of lock... don't look at shared variables!
      if (lock.m.m_ptr != s->vio.mutex.m_ptr) {
        read_reschedule(nh, vc);
        return;
      }
    }
  }
  // If here are is no more room, or nothing to do, disable the connection
  if (s->vio.ntodo() <= 0 || !s->enabled || !buf.writer()->write_avail()) {
    read_disable(nh, vc);
    return;
  }
  // re caculate the flow control
  if (vc->read_fct.bps_max) {
    ink_debug_assert(!vc->read_fct.e_flowctl);
    ink_hrtime now = ink_get_hrtime();
    ink_hrtime next_time = vc->read_fct.t_start + 8 * s->vio.ndone * HRTIME_SECONDS(1) / vc->read_fct.bps_max - now;
    if (next_time > HRTIME_MSECONDS(50)) {
      vc->read_fct.e_flowctl = vc->thread->schedule_in_local(vc, next_time);
      read_disable(nh, vc);
      return;
    }
  }

  read_reschedule(nh, vc);
}


//
// Write the data for a UnixNetVConnection.
// Rescheduling the UnixNetVConnection when necessary.
//
void
write_to_net(NetHandler *nh, UnixNetVConnection *vc, PollDescriptor *pd, EThread *thread)
{
  NOWARN_UNUSED(pd);
  ProxyMutex *mutex = thread->mutex;

  NET_DEBUG_COUNT_DYN_STAT(net_calls_to_writetonet_stat, 1);
  NET_DEBUG_COUNT_DYN_STAT(net_calls_to_writetonet_afterpoll_stat, 1);

  write_to_net_io(nh, vc, thread);
}


void
write_to_net_io(NetHandler *nh, UnixNetVConnection *vc, EThread *thread)
{
  NetState *s = &vc->write;
  ProxyMutex *mutex = thread->mutex;

  MUTEX_TRY_LOCK_FOR(lock, s->vio.mutex, thread, s->vio._cont);

  if (!lock || lock.m.m_ptr != s->vio.mutex.m_ptr) {
    write_reschedule(nh, vc);
    return;
  }

  // This function will always return true unless
  // vc is an SSLNetVConnection.
  if (!vc->getSSLHandShakeComplete()) {
    int err, ret;

    if (vc->getSSLClientConnection())
      ret = vc->sslStartHandShake(SSL_EVENT_CLIENT, err);
    else
      ret = vc->sslStartHandShake(SSL_EVENT_SERVER, err);

    if (ret == EVENT_ERROR) {
      vc->write.triggered = 0;
      write_signal_error(nh, vc, err);
    } else if (ret == SSL_HANDSHAKE_WANT_READ || ret == SSL_HANDSHAKE_WANT_ACCEPT || ret == SSL_HANDSHAKE_WANT_CONNECT
               || ret == SSL_HANDSHAKE_WANT_WRITE) {
      vc->read.triggered = 0;
      nh->read_ready_list.remove(vc);
      vc->write.triggered = 0;
      nh->write_ready_list.remove(vc);
    } else if (ret == EVENT_DONE) {
      vc->write.triggered = 1;
      if (vc->write.enabled)
        nh->write_ready_list.in_or_enqueue(vc);
    } else
      write_reschedule(nh, vc);
    return;
  }
  // If it is not enabled,add to WaitList.
  if (!s->enabled || s->vio.op != VIO::WRITE || vc->write_fct.e_flowctl) {
    write_disable(nh, vc);
    return;
  }
  // If there is nothing to do, disable
  int64_t ntodo = s->vio.ntodo();
  if (ntodo <= 0) {
    write_disable(nh, vc);
    return;
  }

  MIOBufferAccessor & buf = s->vio.buffer;
  ink_debug_assert(buf.writer());

  // Calculate amount to write
  int64_t towrite = buf.reader()->read_avail();
  if (towrite > ntodo)
    towrite = ntodo;
  int signalled = 0;

  // signal write ready to allow user to fill the buffer
  if (towrite != ntodo && buf.writer()->write_avail()) {
    if (write_signal_and_update(VC_EVENT_WRITE_READY, vc) != EVENT_CONT) {
      return;
    }
    ntodo = s->vio.ntodo();
    if (ntodo <= 0) {
      write_disable(nh, vc);
      return;
    }
    signalled = 1;
    // Recalculate amount to write
    towrite = buf.reader()->read_avail();
    if (towrite > ntodo)
      towrite = ntodo;
  }
  // if there is nothing to do, disable
  ink_assert(towrite >= 0);
  if (towrite <= 0) {
    write_disable(nh, vc);
    return;
  }

  int64_t total_wrote = 0, wattempted = 0;
  int64_t r = vc->load_buffer_and_write(towrite, wattempted, total_wrote, buf);

  // if we have already moved some bytes successfully, summarize in r
  if (total_wrote != wattempted) {
    if (r <= 0)
      r = total_wrote - wattempted;
    else
      r = total_wrote - wattempted + r;
  }
  // check for errors
  if (r <= 0) {                 // if the socket was not ready,add to WaitList
    if (r == -EAGAIN || r == -ENOTCONN) {
      NET_DEBUG_COUNT_DYN_STAT(net_calls_to_write_nodata_stat, 1);
      vc->write.triggered = 0;
      nh->write_ready_list.remove(vc);
      return;
    }
    if (!r || r == -ECONNRESET) {
      vc->write.triggered = 0;
      write_signal_done(VC_EVENT_EOS, nh, vc);
      return;
    }
    vc->write.triggered = 0;
    write_signal_error(nh, vc, (int)-r);
    return;
  } else {
    NET_SUM_DYN_STAT(net_write_bytes_stat, r);

    // Remove data from the buffer and signal continuation.
    ink_debug_assert(buf.reader()->read_avail() >= r);
    buf.reader()->consume(r);
    ink_debug_assert(buf.reader()->read_avail() >= 0);
    s->vio.ndone += r;

    net_activity(vc, thread);
    // If there are no more bytes to write, signal write complete,
    ink_assert(ntodo >= 0);
    if (s->vio.ntodo() <= 0) {
      write_signal_done(VC_EVENT_WRITE_COMPLETE, nh, vc);
      return;
    } else if (!signalled) {
      if (write_signal_and_update(VC_EVENT_WRITE_READY, vc) != EVENT_CONT) {
        return;
      }
      // change of lock... don't look at shared variables!
      if (lock.m.m_ptr != s->vio.mutex.m_ptr) {
        write_reschedule(nh, vc);
        return;
      }
      if (s->vio.ntodo() <= 0 || !buf.reader()->read_avail()) {
        write_disable(nh, vc);
        return;
      }
    }
    // re caculate the flow control
    if (vc->write_fct.bps_max) {
      ink_debug_assert(!vc->write_fct.e_flowctl);
      ink_hrtime now = ink_get_hrtime();
      ink_hrtime next_time = vc->write_fct.t_start + 8 * s->vio.ndone * HRTIME_SECONDS(1) / vc->write_fct.bps_max - now;
      if (next_time > HRTIME_MSECONDS(50)) {
        vc->write_fct.e_flowctl = vc->thread->schedule_in_local(vc, next_time);
        write_disable(nh, vc);
        return;
      }
    }

    write_reschedule(nh, vc);
    return;
  }
}

bool
UnixNetVConnection::get_data(int id, void *data)
{
  union {
    TSVIO * vio;
    void * data;
  } ptr;

  ptr.data = data;

  switch (id) {
  case TS_API_DATA_READ_VIO:
    *ptr.vio = (TSVIO)&this->read.vio;
    return true;
  case TS_API_DATA_WRITE_VIO:
    *ptr.vio = (TSVIO)&this->write.vio;
    return true;
  default:
    return false;
  }
}

VIO *
UnixNetVConnection::do_io_read(Continuation *c, int64_t nbytes, MIOBuffer *buf)
{
  ink_assert(!closed);
  read.vio.op = VIO::READ;
  read.vio.mutex = c->mutex;
  read.vio._cont = c;
  read.vio.nbytes = nbytes;
  read.vio.ndone = 0;
  read.vio.vc_server = (VConnection *) this;
  if (buf) {
    read.vio.buffer.writer_for(buf);
    if (!read.enabled)
      read.vio.reenable();
  } else {
    read.vio.buffer.clear();
    disable_read(this);
  }
  return &read.vio;
}

VIO *
UnixNetVConnection::do_io_write(Continuation *c, int64_t nbytes, IOBufferReader *reader, bool owner)
{
  ink_assert(!closed);
  write.vio.op = VIO::WRITE;
  write.vio.mutex = c->mutex;
  write.vio._cont = c;
  write.vio.nbytes = nbytes;
  write.vio.ndone = 0;
  write.vio.vc_server = (VConnection *) this;
  if (reader) {
    ink_assert(!owner);
    write.vio.buffer.reader_for(reader);
    if (nbytes && !write.enabled)
      write.vio.reenable();
  } else {
    disable_write(this);
  }
  return &write.vio;
}

void
UnixNetVConnection::do_io_close(int alerrno /* = -1 */ )
{
  disable_read(this);
  disable_write(this);
  read.vio.buffer.clear();
  read.vio.nbytes = 0;
  read.vio.op = VIO::NONE;
  write.vio.buffer.clear();
  write.vio.nbytes = 0;
  write.vio.op = VIO::NONE;

  EThread *t = this_ethread();
  bool close_inline = !recursion && nh->mutex->thread_holding == t;

  INK_WRITE_MEMORY_BARRIER;
  if (alerrno && alerrno != -1)
    this->lerrno = alerrno;
  if (alerrno == -1)
    closed = 1;
  else
    closed = -1;

  if (close_inline)
    close_UnixNetVConnection(this, t);
}

void
UnixNetVConnection::do_io_shutdown(ShutdownHowTo_t howto)
{
  switch (howto) {
  case IO_SHUTDOWN_READ:
    socketManager.shutdown(((UnixNetVConnection *) this)->con.fd, 0);
    read_fct.reset();
    disable_read(this);
    read.vio.buffer.clear();
    read.vio.nbytes = 0;
    f.shutdown = NET_VC_SHUTDOWN_READ;
    break;
  case IO_SHUTDOWN_WRITE:
    socketManager.shutdown(((UnixNetVConnection *) this)->con.fd, 1);
    write_fct.reset();
    disable_write(this);
    write.vio.buffer.clear();
    write.vio.nbytes = 0;
    f.shutdown = NET_VC_SHUTDOWN_WRITE;
    break;
  case IO_SHUTDOWN_READWRITE:
    socketManager.shutdown(((UnixNetVConnection *) this)->con.fd, 2);
    read_fct.reset();
    write_fct.reset();
    disable_read(this);
    disable_write(this);
    read.vio.buffer.clear();
    read.vio.nbytes = 0;
    write.vio.buffer.clear();
    write.vio.nbytes = 0;
    f.shutdown = NET_VC_SHUTDOWN_READ | NET_VC_SHUTDOWN_WRITE;
    break;
  default:
    ink_assert(!"not reached");
  }
}

int
OOB_callback::retry_OOB_send(int event, Event *e)
{
  (void) event;
  (void) e;
  ink_debug_assert(mutex->thread_holding == this_ethread());
  // the NetVC and the OOB_callback share a mutex
  server_vc->oob_ptr = NULL;
  server_vc->send_OOB(server_cont, data, length);
  delete this;
  return EVENT_DONE;
}

void
UnixNetVConnection::cancel_OOB()
{
  UnixNetVConnection *u = (UnixNetVConnection *) this;
  if (u->oob_ptr) {
    if (u->oob_ptr->trigger) {
      u->oob_ptr->trigger->cancel_action();
      u->oob_ptr->trigger = NULL;
    }
    delete u->oob_ptr;
    u->oob_ptr = NULL;
  }
}

Action *
UnixNetVConnection::send_OOB(Continuation *cont, char *buf, int len)
{
  UnixNetVConnection *u = (UnixNetVConnection *) this;
  ink_debug_assert(len > 0);
  ink_debug_assert(buf);
  ink_debug_assert(!u->oob_ptr);
  int written;
  ink_debug_assert(cont->mutex->thread_holding == this_ethread());
  written = socketManager.send(u->con.fd, buf, len, MSG_OOB);
  if (written == len) {
    cont->handleEvent(VC_EVENT_OOB_COMPLETE, NULL);
    return ACTION_RESULT_DONE;
  } else if (!written) {
    cont->handleEvent(VC_EVENT_EOS, NULL);
    return ACTION_RESULT_DONE;
  }
  if (written > 0 && written < len) {
    u->oob_ptr = NEW(new OOB_callback(mutex, this, cont, buf + written, len - written));
    u->oob_ptr->trigger = mutex->thread_holding->schedule_in_local(u->oob_ptr, HRTIME_MSECONDS(10));
    return u->oob_ptr->trigger;
  } else {
    // should be a rare case : taking a new continuation should not be
    // expensive for this
    written = -errno;
    ink_assert(written == -EAGAIN || written == -ENOTCONN);
    u->oob_ptr = NEW(new OOB_callback(mutex, this, cont, buf, len));
    u->oob_ptr->trigger = mutex->thread_holding->schedule_in_local(u->oob_ptr, HRTIME_MSECONDS(10));
    return u->oob_ptr->trigger;
  }
}

//
// Function used to reenable the VC for reading or
// writing.
//
void
UnixNetVConnection::reenable(VIO *vio)
{
  if (STATE_FROM_VIO(vio)->enabled)
    return;
  set_enabled(vio);
  if (!thread)
    return;
  EThread *t = vio->mutex->thread_holding;
  ink_debug_assert(t == this_ethread());
  ink_debug_assert(!closed);
  if (nh->mutex->thread_holding == t) {
    if (vio == &read.vio) {
      ep.modify(EVENTIO_READ);
      ep.refresh(EVENTIO_READ);
      if (read.triggered)
        nh->read_ready_list.in_or_enqueue(this);
      else
        nh->read_ready_list.remove(this);
    } else {
      ep.modify(EVENTIO_WRITE);
      ep.refresh(EVENTIO_WRITE);
      if (write.triggered)
        nh->write_ready_list.in_or_enqueue(this);
      else
        nh->write_ready_list.remove(this);
    }
  } else {
    MUTEX_TRY_LOCK(lock, nh->mutex, t);
    if (!lock) {
      if (vio == &read.vio) {
        if (!read.in_enabled_list) {
          read.in_enabled_list = 1;
          nh->read_enable_list.push(this);
        }
      } else {
        if (!write.in_enabled_list) {
          write.in_enabled_list = 1;
          nh->write_enable_list.push(this);
        }
      }
      if (nh->trigger_event && nh->trigger_event->ethread->signal_hook)
        nh->trigger_event->ethread->signal_hook(nh->trigger_event->ethread);
    } else {
      if (vio == &read.vio) {
        ep.modify(EVENTIO_READ);
        ep.refresh(EVENTIO_READ);
        if (read.triggered)
          nh->read_ready_list.in_or_enqueue(this);
        else
          nh->read_ready_list.remove(this);
      } else {
        ep.modify(EVENTIO_WRITE);
        ep.refresh(EVENTIO_WRITE);
        if (write.triggered)
          nh->write_ready_list.in_or_enqueue(this);
        else
          nh->write_ready_list.remove(this);
      }
    }
  }
}

void
UnixNetVConnection::reenable_re(VIO *vio)
{
  if (!thread)
    return;
  EThread *t = vio->mutex->thread_holding;
  ink_debug_assert(t == this_ethread());
  if (nh->mutex->thread_holding == t) {
    set_enabled(vio);
    if (vio == &read.vio) {
      ep.modify(EVENTIO_READ);
      ep.refresh(EVENTIO_READ);
      if (read.triggered)
        net_read_io(nh, t);
      else
        nh->read_ready_list.remove(this);
    } else {
      ep.modify(EVENTIO_WRITE);
      ep.refresh(EVENTIO_WRITE);
      if (write.triggered)
        write_to_net(nh, this, NULL, t);
      else
        nh->write_ready_list.remove(this);
    }
  } else
    reenable(vio);
}


UnixNetVConnection::UnixNetVConnection()
  : closed(0), inactivity_timeout_in(0), active_timeout_in(0),
#ifdef INACTIVITY_TIMEOUT
    inactivity_timeout(NULL),
#else
    next_inactivity_timeout_at(0),
#endif
    active_timeout(NULL), nh(NULL),
    id(0), flags(0), recursion(0), submit_time(0), oob_ptr(0),
    from_accept_thread(false), pt(PROBE_NONE)
{
  memset(&local_addr, 0, sizeof local_addr);
  memset(&server_addr, 0, sizeof server_addr);
  SET_HANDLER((NetVConnHandler) & UnixNetVConnection::startEvent);
}

// Private methods

void
UnixNetVConnection::set_enabled(VIO *vio)
{
  ink_debug_assert(vio->mutex->thread_holding == this_ethread() && thread);
  ink_assert(!closed);
  STATE_FROM_VIO(vio)->enabled = 1;
#ifdef INACTIVITY_TIMEOUT
  if (!inactivity_timeout && inactivity_timeout_in) {
    if (vio->mutex->thread_holding == thread)
      inactivity_timeout = thread->schedule_in_local(this, inactivity_timeout_in);
    else
      inactivity_timeout = thread->schedule_in(this, inactivity_timeout_in);
  }
#else
  if (!next_inactivity_timeout_at && inactivity_timeout_in)
    next_inactivity_timeout_at = ink_get_hrtime() + inactivity_timeout_in;
#endif
}

void
UnixNetVConnection::net_read_io(NetHandler *nh, EThread *lthread)
{
  read_from_net(nh, this, lthread);
}

// This code was pulled out of write_to_net so
// I could overwrite it for the SSL implementation
// (SSL read does not support overlapped i/o)
// without duplicating all the code in write_to_net.
int64_t
UnixNetVConnection::load_buffer_and_write(int64_t towrite, int64_t &wattempted, int64_t &total_wrote, MIOBufferAccessor & buf)
{
  int64_t r = 0;
  int64_t offset = buf.entry->start_offset;
  IOBufferBlock *b = buf.entry->block;
  do {
    IOVec tiovec[NET_MAX_IOV];
    int niov = 0;
    int64_t total_wrote_last = total_wrote;
    while (b && niov < NET_MAX_IOV) {
      // check if we have done this block
      int64_t l = b->read_avail();
      l -= offset;
      if (l <= 0) {
        offset = -l;
        b = b->next;
        continue;
      }
      // check if to amount to write exceeds that in this buffer
      int64_t wavail = towrite - total_wrote;
      if (l > wavail)
        l = wavail;
      if (!l)
        break;
      total_wrote += l;
      // build an iov entry
      tiovec[niov].iov_len = l;
      tiovec[niov].iov_base = b->start() + offset;
      niov++;
      // on to the next block
      offset = 0;
      b = b->next;
    }
    wattempted = total_wrote - total_wrote_last;
    if (niov == 1)
      r = socketManager.write(con.fd, tiovec[0].iov_base, tiovec[0].iov_len);
    else
      r = socketManager.writev(con.fd, &tiovec[0], niov);
    ProxyMutex *mutex = thread->mutex;
    NET_DEBUG_COUNT_DYN_STAT(net_calls_to_write_stat, 1);
  } while (r == wattempted && total_wrote < towrite);

  return (r);
}

void
UnixNetVConnection::readDisable(NetHandler *nh)
{
  read_disable(nh, this);
}

void
UnixNetVConnection::readSignalError(NetHandler *nh, int err)
{
  read_signal_error(nh, this, err);
}

int
UnixNetVConnection::readSignalDone(int event, NetHandler *nh)
{
  return (read_signal_done(event, nh, this));
}


int
UnixNetVConnection::readSignalAndUpdate(int event)
{
  return (read_signal_and_update(event, this));
}

// Interface so SSL inherited class can call some static in-line functions
// without affecting regular net stuff or copying a bunch of code into
// the header files.
void
UnixNetVConnection::readReschedule(NetHandler *nh)
{
  read_reschedule(nh, this);
}

void
UnixNetVConnection::writeReschedule(NetHandler *nh)
{
  write_reschedule(nh, this);
}

void
UnixNetVConnection::netActivity(EThread *lthread)
{
  net_activity(this, lthread);
}

int
UnixNetVConnection::startEvent(int event, Event *e)
{
  (void) event;

  MUTEX_TRY_LOCK(lock, get_NetHandler(e->ethread)->mutex, e->ethread);
  if (!lock) {
    e->schedule_in(NET_RETRY_DELAY);
    return EVENT_CONT;
  }
  if (!action_.cancelled)
    connectUp(e->ethread);
  else
    free(e->ethread);
  return EVENT_DONE;
}

int
UnixNetVConnection::acceptEvent(int event, Event *e)
{
  (void) event;
  thread = e->ethread;

  MUTEX_TRY_LOCK(lock, get_NetHandler(thread)->mutex, e->ethread);
  if (!lock) {
    if (event == EVENT_NONE) {
      thread->schedule_in(this, NET_RETRY_DELAY);
      return EVENT_DONE;
    } else {
      e->schedule_in(NET_RETRY_DELAY);
      return EVENT_CONT;
    }
  }

  if (action_.cancelled) {
    free(thread);
    return EVENT_DONE;
  }

  SET_HANDLER((NetVConnHandler) & UnixNetVConnection::mainEvent);

  nh = get_NetHandler(thread);
  PollDescriptor *pd = get_PollDescriptor(thread);
  if (ep.start(pd, this, EVENTIO_READ|EVENTIO_WRITE) < 0) {
    Debug("iocore_net", "acceptEvent : failed EventIO::start\n");
    close_UnixNetVConnection(this, e->ethread);
    return EVENT_DONE;
  }

  nh->open_list.enqueue(this);

  if (inactivity_timeout_in)
    UnixNetVConnection::set_inactivity_timeout(inactivity_timeout_in);
  if (active_timeout_in)
    UnixNetVConnection::set_active_timeout(active_timeout_in);
  if (pt == PROBE_NONE)
    action_.continuation->handleEvent(NET_EVENT_ACCEPT, this);
  else {
    ink_assert(pt == PROBE_SPDY_FOR_PLUGIN || pt == PROBE_SPDY_FOR_CORE);
    SpdyProbe(this);
  }

  return EVENT_DONE;
}

//
// The main event for UnixNetVConnections.
// This is called by the Event subsystem to initialize the UnixNetVConnection
// and for active and inactivity timeouts.
//
int
UnixNetVConnection::mainEvent(int event, Event *e)
{
  ink_debug_assert(event == EVENT_IMMEDIATE || event == EVENT_INTERVAL);
  ink_debug_assert(thread == this_ethread());

  MUTEX_TRY_LOCK(hlock, get_NetHandler(thread)->mutex, e->ethread);
  MUTEX_TRY_LOCK(rlock, read.vio.mutex ? (ProxyMutex *) read.vio.mutex : (ProxyMutex *) e->ethread->mutex, e->ethread);
  MUTEX_TRY_LOCK(wlock, write.vio.mutex ? (ProxyMutex *) write.vio.mutex :
                 (ProxyMutex *) e->ethread->mutex, e->ethread);
  if (!hlock || !rlock || !wlock ||
      (read.vio.mutex.m_ptr && rlock.m.m_ptr != read.vio.mutex.m_ptr) ||
      (write.vio.mutex.m_ptr && wlock.m.m_ptr != write.vio.mutex.m_ptr)) {
#ifndef INACTIVITY_TIMEOUT
    if (e == active_timeout)
#endif
      e->schedule_in(NET_RETRY_DELAY);
    return EVENT_CONT;
  }
  if (e->cancelled)
    return EVENT_DONE;

  if (closed) {
    close_UnixNetVConnection(this, thread);
    return EVENT_DONE;
  }

  int signal_event;
  Event **signal_timeout;
  Continuation *reader_cont = NULL;
  Continuation *writer_cont = NULL;
  ink_hrtime next_activity_timeout_at = 0;
  ink_hrtime *signal_timeout_at = &next_activity_timeout_at;
  Event *t = NULL;
  signal_timeout = &t;

#ifdef INACTIVITY_TIMEOUT
  if (e == inactivity_timeout) {
    signal_event = VC_EVENT_INACTIVITY_TIMEOUT;
    signal_timeout = &inactivity_timeout;
  }
#else
  if (event == EVENT_IMMEDIATE) {
    /* BZ 49408 */
    //ink_debug_assert(inactivity_timeout_in);
    //ink_debug_assert(next_inactivity_timeout_at < ink_get_hrtime());
    if (!inactivity_timeout_in || next_inactivity_timeout_at > ink_get_hrtime())
      return EVENT_CONT;
    signal_event = VC_EVENT_INACTIVITY_TIMEOUT;
    signal_timeout_at = &next_inactivity_timeout_at;
  }
#endif
  else {
    if (e == active_timeout) {
      signal_event = VC_EVENT_ACTIVE_TIMEOUT;
      signal_timeout = &active_timeout;
    } else if (e == read_fct.e_flowctl) {
      read_fct.e_flowctl = NULL;
      reenable(&read.vio);
      return EVENT_CONT;
    } else {
      ink_debug_assert(e == write_fct.e_flowctl);
      write_fct.e_flowctl = NULL;
      reenable(&write.vio);
      return EVENT_CONT;
    }
  }
  *signal_timeout = 0;
  *signal_timeout_at = 0;
  writer_cont = write.vio._cont;

  if (read.vio.op == VIO::READ && !(f.shutdown & NET_VC_SHUTDOWN_READ)) {
    reader_cont = read.vio._cont;
    if (read_signal_and_update(signal_event, this) == EVENT_DONE)
      return EVENT_DONE;
  }

  if (!*signal_timeout &&
      !*signal_timeout_at &&
      !closed && write.vio.op == VIO::WRITE &&
      !(f.shutdown & NET_VC_SHUTDOWN_WRITE) && reader_cont != write.vio._cont && writer_cont == write.vio._cont)
    if (write_signal_and_update(signal_event, this) == EVENT_DONE)
      return EVENT_DONE;
  return EVENT_DONE;
}


int
UnixNetVConnection::connectUp(EThread *t)
{
  thread = t;
  if (check_net_throttle(CONNECT, submit_time)) {
    check_throttle_warning();
    action_.continuation->handleEvent(NET_EVENT_OPEN_FAILED, (void *) -ENET_THROTTLING);
    free(t);
    return CONNECT_FAILURE;
  }

  // Force family to agree with remote (server) address.
  options.ip_family = server_addr.sa.sa_family;

  //
  // Initialize this UnixNetVConnection
  //
  if (is_debug_tag_set("iocore_net")) {
    char addrbuf[INET6_ADDRSTRLEN];
    Debug("iocore_net", "connectUp:: local_addr=%s:%d [%s]\n",
      options.local_ip.isValid()
      ? options.local_ip.toString(addrbuf, sizeof(addrbuf))
      : "*",
      options.local_port,
      NetVCOptions::toString(options.addr_binding)
    );
  }


  int res = con.open(options);
  if (0 == res) {
    // Must connect after EventIO::Start() to avoid a race condition
    // when edge triggering is used.
    if (ep.start(get_PollDescriptor(t), this, EVENTIO_READ|EVENTIO_WRITE) < 0) {
      lerrno = errno;
      Debug("iocore_net", "connectUp : Failed to add to epoll list\n");
      action_.continuation->handleEvent(NET_EVENT_OPEN_FAILED, (void *)0); // 0 == res
      free(t);
      return CONNECT_FAILURE;
    }
    res = con.connect(&server_addr.sa, options);
  }

  if (res) {
    lerrno = errno;
    action_.continuation->handleEvent(NET_EVENT_OPEN_FAILED, (void *)(intptr_t)res);
    free(t);
    return CONNECT_FAILURE;
  }
  check_emergency_throttle(con);

  // start up next round immediately

  SET_HANDLER(&UnixNetVConnection::mainEvent);
  // This function is empty for regular UnixNetVConnection, it has code
  // in it for the inherited SSLUnixNetVConnection.  Allows the connectUp
  // function code not to be duplicated in the inherited SSL class.
  //  sslStartHandShake (SSL_EVENT_CLIENT, err);

  nh = get_NetHandler(t);
  nh->open_list.enqueue(this);

  ink_assert(!inactivity_timeout_in);
  ink_assert(!active_timeout_in);
  action_.continuation->handleEvent(NET_EVENT_OPEN, this);
  return CONNECT_SUCCESS;
}


void
UnixNetVConnection::free(EThread *t)
{
  NET_SUM_GLOBAL_DYN_STAT(net_connections_currently_open_stat, -1);
  // clear variables for reuse
  this->mutex.clear();
  action_.mutex.clear();
  got_remote_addr = 0;
  got_local_addr = 0;
  read.vio.mutex.clear();
  write.vio.mutex.clear();
  flags = 0;
  SET_CONTINUATION_HANDLER(this, (NetVConnHandler) & UnixNetVConnection::startEvent);
  nh = NULL;
  read.triggered = 0;
  write.triggered = 0;
  options.reset();
  closed = 0;
  ink_debug_assert(!read.ready_link.prev && !read.ready_link.next);
  ink_debug_assert(!read.enable_link.next);
  ink_debug_assert(!write.ready_link.prev && !write.ready_link.next);
  ink_debug_assert(!write.enable_link.next);
  ink_debug_assert(!link.next && !link.prev);
  ink_debug_assert(!active_timeout);
  ink_debug_assert(con.fd == NO_FD);
  ink_debug_assert(t == this_ethread());

  if (from_accept_thread) {
    netVCAllocator.free(this);
  } else {
    THREAD_FREE(this, netVCAllocator, t);
  }
}

void
UnixNetVConnection::apply_options()
{
  con.apply_options(options);
}
