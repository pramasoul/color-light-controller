/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.

.

Authors: Daniele Lacamera, Philippe Mariman
*********************************************************************/

#include "pico_tcp.h"
#include "pico_config.h"
#include "pico_eth.h"
#include "pico_socket.h"
#include "pico_stack.h"
#include "pico_socket.h"
#include "pico_queue.h"
#include "pico_tree.h"

#define TCP_SOCK(s) ((struct pico_socket_tcp *)s)
#define SEQN(f) (f?(long_be(((struct pico_tcp_hdr *)(f->transport_hdr))->seq)):0)
#define ACKN(f) (f?(long_be(((struct pico_tcp_hdr *)(f->transport_hdr))->ack)):0)

#define PICO_TCP_RTO_MIN 10
#define PICO_TCP_RTO_MAX 120000
#define PICO_TCP_IW          2
#define PICO_TCP_SYN_TO  1000

#define PICO_TCP_MAX_CONNECT_RETRIES 7

#define PICO_TCP_LOOKAHEAD      0x00
#define PICO_TCP_FIRST_DUPACK   0x01
#define PICO_TCP_SECOND_DUPACK  0x02
#define PICO_TCP_RECOVER        0x03
#define PICO_TCP_BLACKOUT       0x04
#define PICO_TCP_UNREACHABLE    0x05
#define PICO_TCP_WINDOW_FULL    0x06

/* check if the Nagle algorithm is enabled on the socket */
#define IS_NAGLE_ENABLED(s)     (!(!(!(s->opt_flags & (1 << PICO_SOCKET_OPT_TCPNODELAY)))))
/* check if tcp connection is "idle" according to Nagle (RFC 896) */
#define IS_TCP_IDLE(t)          ((t->in_flight == 0) && (t->tcpq_out.size == 0))
/* check if the hold queue contains data (again Nagle) */
#define IS_TCP_HOLDQ_EMPTY(t)   (t->tcpq_hold.size == 0)

#ifdef PICO_SUPPORT_TCP
#define tcp_dbg_nagle(...) do{}while(0)
#define tcp_dbg_options(...) do{}while(0)


#define tcp_dbg(...) do{}while(0)
//#define tcp_dbg dbg


#ifdef PICO_SUPPORT_MUTEX
static void * Mutex = NULL;
#define LOCK(x) {\
  if (x == NULL) \
    x = pico_mutex_init(); \
  pico_mutex_lock(x); \
}
#define UNLOCK(x) pico_mutex_unlock(x);

#else
#define LOCK(x) do{}while(0)
#define UNLOCK(x) do{}while(0)
#endif


static inline int seq_compare(uint32_t a, uint32_t b)
{
  uint32_t thresh = ((uint32_t)(-1))>>1;
  if (((a > thresh) && (b > thresh)) || ((a <= thresh) && (b <= thresh))) {
    if (a > b)
      return 1;
    if (b > a)
      return -1;
  } else {
    if (a > b)
      return -2;
    if (b > a)
      return 2;
  }
  return 0;
}

static int segment_compare(void * ka, void * kb)
{
  struct pico_frame *a = ka, *b = kb;
  return seq_compare(SEQN(a), SEQN(b));
}

struct pico_tcp_queue
{
  struct pico_tree pool;
  uint32_t max_size;
  uint32_t size;
  uint32_t frames;
};

static struct pico_frame *peek_segment(struct pico_tcp_queue *tq, uint32_t seq)
{
  struct pico_tcp_hdr H;
  struct pico_frame f = {};
  f.transport_hdr = (uint8_t *) (&H);
  H.seq = long_be(seq);

  return pico_tree_findKey(&tq->pool,&f);
}

static struct pico_frame *first_segment(struct pico_tcp_queue *tq)
{
  return pico_tree_first(&tq->pool);
}

static struct pico_frame *next_segment(struct pico_tcp_queue *tq, struct pico_frame *cur)
{
  if (!cur)
    return NULL;
  return peek_segment(tq, SEQN(cur) + cur->payload_len);
}

static int pico_enqueue_segment(struct pico_tcp_queue *tq, struct pico_frame *f)
{
    int ret = -1;
  if (f->payload_len <= 0) {
    tcp_dbg("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! TRIED TO ENQUEUE INVALID SEGMENT!\n");
    //abort();
    return -1;
  }
    LOCK(Mutex);
  if ((tq->size + f->payload_len) > tq->max_size)
  {
    ret = 0;
    goto out;
  }
  if (pico_tree_insert(&tq->pool,f) != 0)
  {
    ret = 0;
    goto out;
  }
  tq->size += f->payload_len;
  if (f->payload_len > 0)
    tq->frames++;
  ret = f->payload_len;

out :
  UNLOCK(Mutex);
  return ret;
}

static void pico_discard_segment(struct pico_tcp_queue *tq, struct pico_frame *f)
{
  struct pico_frame *f1;
  LOCK(Mutex);
  f1 = pico_tree_delete(&tq->pool,f);
  if (f1) {
    tq->size -= f->payload_len;
    if (f->payload_len > 0)
      tq->frames--;
  }
  pico_frame_discard(f);
  UNLOCK(Mutex);
}

/* Structure for TCP socket */
struct tcp_sack_block {
  uint32_t left;
  uint32_t right;
  struct tcp_sack_block *next;
};

struct pico_socket_tcp {
  struct pico_socket sock;

  /* Tree/queues */
  struct pico_tcp_queue tcpq_in;
  struct pico_tcp_queue tcpq_out;
  struct pico_tcp_queue tcpq_hold;  /* buffer to hold delayed frames according to Nagle */

  /* tcp_output */
  uint32_t snd_nxt;
  uint32_t snd_last;
  uint32_t snd_old_ack;
  uint32_t snd_retry;
  uint32_t snd_last_out;

  /* congestion control */
  uint32_t avg_rtt;
  uint32_t rttvar;
  uint32_t rto;
  uint32_t in_flight;
  uint8_t  timer_running;
  uint8_t  keepalive_timer_running;
  uint16_t cwnd_counter;
  uint16_t cwnd;
  uint16_t ssthresh;
  uint16_t recv_wnd;
  uint16_t recv_wnd_scale;

  /* tcp_input */
  uint32_t rcv_nxt;
  uint32_t rcv_ackd;
  uint32_t rcv_processed;
  uint16_t wnd;
  uint16_t wnd_scale;

  /* options */
  uint32_t ts_nxt;
  uint16_t mss;
  uint8_t sack_ok;
  uint8_t ts_ok;
  uint8_t mss_ok;
  uint8_t scale_ok;
  struct tcp_sack_block *sacks;
  uint8_t jumbo;

  /* Transmission */
  uint8_t  x_mode;
  uint8_t  dupacks;
  uint8_t  backoff;

};

/* Queues */
static struct pico_queue tcp_in = {};
static struct pico_queue tcp_out = {};

/* If Nagle enabled, this function can make 1 new segment from smaller segments in hold queue */
static struct pico_frame * pico_hold_segment_make(struct pico_socket_tcp *t);

/* checks if tcpq_in is empty */
int pico_tcp_queue_in_is_empty(struct pico_socket *s)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) s;

  if (t->tcpq_in.frames == 0)
    return 1;
  else
    return 0;
}

/* Useful for getting rid of the beginning of the buffer (read() op) */
static int release_until(struct pico_tcp_queue *q, uint32_t seq)
{
  struct pico_frame *head = first_segment(q);
  int ret = 0;
  while (head && (seq_compare(SEQN(head) + head->payload_len, seq) <= 0)) {
    struct pico_frame *cur = head;
    head = next_segment(q, cur);
    tcp_dbg("Releasing %p\n", q);
    pico_discard_segment(q, cur);
    ret++;
  }
  return ret;
}

static int release_all_until(struct pico_tcp_queue *q, uint32_t seq)
{
  struct pico_frame *f = NULL, *tmp __attribute__((unused));
  struct pico_tree_node * idx, * temp;
  int ret = 0;

  pico_tree_foreach_safe(idx,&q->pool,temp){
  f = idx->keyValue;
    if (seq_compare(SEQN(f) + f->payload_len, seq) <= 0) {
      tcp_dbg("Releasing %p\n", f);
      pico_discard_segment(q, f);
      ret++;
    } else
      return ret;
  }
  return ret;
}

/* API calls */

uint16_t pico_tcp_checksum_ipv4(struct pico_frame *f)
{
  struct pico_ipv4_hdr *hdr = (struct pico_ipv4_hdr *) f->net_hdr;
  struct pico_tcp_hdr *tcp_hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  struct pico_socket *s = f->sock;
  struct pico_ipv4_pseudo_hdr pseudo;

  if (s) {
    /* Case of outgoing frame */
    //dbg("TCP CRC: on outgoing frame\n");
    pseudo.src.addr = s->local_addr.ip4.addr;
    pseudo.dst.addr = s->remote_addr.ip4.addr;
  } else {
    /* Case of incomming frame */
    //dbg("TCP CRC: on incomming frame\n");
    pseudo.src.addr = hdr->src.addr;
    pseudo.dst.addr = hdr->dst.addr;
  }
  pseudo.zeros = 0;
  pseudo.proto = PICO_PROTO_TCP;
  pseudo.len = short_be(f->transport_len);

  return pico_dualbuffer_checksum(&pseudo, sizeof(struct pico_ipv4_pseudo_hdr), tcp_hdr, f->transport_len);
}

static void tcp_send_fin(struct pico_socket_tcp *t);
static int pico_tcp_process_out(struct pico_protocol *self, struct pico_frame *f)
{
  struct pico_tcp_hdr *hdr;
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)f->sock;
  hdr = (struct pico_tcp_hdr *)f->transport_hdr;

  if (f->payload_len > 0) {
    tcp_dbg("Process out: sending %p (%d bytes)\n",f, f->payload_len);
  } else {
    tcp_dbg("Sending empty packet\n");
  }

  if (f->payload_len > 0) {
    if (seq_compare(SEQN(f) + f->payload_len, t->snd_nxt) > 0) {
      t->snd_nxt = SEQN(f) + f->payload_len;
      tcp_dbg("%s: snd_nxt is now %08x\n", __FUNCTION__, t->snd_nxt);
    }
  } else if (hdr->flags == PICO_TCP_ACK) { /* pure ack */
    //hdr->seq = long_be(t->snd_nxt);   /* XXX disabled this to not to mess with seq nrs of ACKs anymore */
  } else {
    tcp_dbg("%s: non-pure ACK with len=0, fl:%04x\n", __FUNCTION__, hdr->flags);
  }

  pico_network_send(f);
  return 0;
}

int pico_tcp_push(struct pico_protocol *self, struct pico_frame *data);

/* Interface: protocol definition */
struct pico_protocol pico_proto_tcp = {
  .name = "tcp",
  .proto_number = PICO_PROTO_TCP,
  .layer = PICO_LAYER_TRANSPORT,
  .process_in = pico_transport_process_in,
  .process_out = pico_tcp_process_out,
  .push = pico_tcp_push,
  .q_in = &tcp_in,
  .q_out = &tcp_out,
};

static uint32_t pico_paws(void)
{
  static unsigned long _paws = 0;
  _paws = pico_rand();
  return long_be(_paws); /*XXX: implement paws */
}

static void tcp_add_options(struct pico_socket_tcp *ts, struct pico_frame *f, uint16_t flags, int optsiz)
{
  uint32_t tsval = long_be(pico_tick);
  uint32_t tsecr = long_be(ts->ts_nxt);
  int i = 0;
  f->start = f->transport_hdr + PICO_SIZE_TCPHDR;

  memset(f->start, PICO_TCP_OPTION_NOOP, optsiz); /* fill blanks with noop */

  if (flags & PICO_TCP_SYN) {
    f->start[i++] = PICO_TCP_OPTION_MSS;
    f->start[i++] = PICO_TCPOPTLEN_MSS;
    f->start[i++] = (ts->mss >> 8) & 0xFF;
    f->start[i++] = ts->mss & 0xFF;
    f->start[i++] = PICO_TCP_OPTION_SACK_OK;
    f->start[i++] = PICO_TCPOPTLEN_SACK_OK;
  }

  f->start[i++] = PICO_TCP_OPTION_WS;
  f->start[i++] = PICO_TCPOPTLEN_WS;
  f->start[i++] = ts->wnd_scale;

  if (optsiz >= 12) {
    f->start[i++] = PICO_TCP_OPTION_TIMESTAMP;
    f->start[i++] = PICO_TCPOPTLEN_TIMESTAMP;
    memcpy(f->start + i, &tsval, 4);
    i += 4;
    memcpy(f->start + i, &tsecr, 4);
    i += 4;
  }

  if (flags & PICO_TCP_ACK) {
    struct tcp_sack_block *sb;
    int len_off;

    if (ts->sack_ok && ts->sacks) {
      f->start[i++] = PICO_TCP_OPTION_SACK;
      len_off = i;
      f->start[i++] = PICO_TCPOPTLEN_SACK;
      while(ts->sacks) {
        sb = ts->sacks;
        ts->sacks = sb->next;
        memcpy(f->start + i, sb, 2 * sizeof(uint32_t));
        i += (2 * sizeof(uint32_t));
        f->start[len_off] += (2 * sizeof(uint32_t));
        pico_free(sb);
      }
    }
  }
  if (i < optsiz)
    f->start[ optsiz - 1 ] = PICO_TCP_OPTION_END;
}

static void tcp_send_ack(struct pico_socket_tcp *t);

static void tcp_set_space(struct pico_socket_tcp *t)
{
  int mtu, space;
  int shift = 0;

  mtu = t->mss + PICO_SIZE_TCPHDR + PICO_SIZE_TCPOPT_SYN ;
  if (t->tcpq_in.max_size == 0) {
    space = 1024 * 1024 * 1024; /* One Gigabyte, for unlimited sockets. */
  } else {
    space = ((t->tcpq_in.max_size - t->tcpq_in.size) / mtu) * t->mss;
  }
  if (space < 0)
    space = 0;
  while(space > 0xFFFF) {
    space >>= 1;
    shift++;
  }
  if ((space != t->wnd) || (shift != t->wnd_scale) || ((space - t->wnd) > (space>>2))) {
    t->wnd = space;
    t->wnd_scale = shift;
  }
}

/* Return 32-bit aligned option size */
static int tcp_options_size(struct pico_socket_tcp *t, uint16_t flags)
{
  int size = 0;
  struct tcp_sack_block *sb = t->sacks;

  if (flags & PICO_TCP_SYN) {  /* Full options */
    size = PICO_TCPOPTLEN_MSS + PICO_TCP_OPTION_SACK_OK + PICO_TCPOPTLEN_WS + PICO_TCPOPTLEN_TIMESTAMP;
  } else {

   /* Always update window scale. */
    size += PICO_TCPOPTLEN_WS;

    if (t->ts_ok)
      size += PICO_TCPOPTLEN_TIMESTAMP;

    size+= PICO_TCPOPTLEN_END;
  }
  if ((flags & PICO_TCP_ACK) && (t->sack_ok && sb)) {
    size += 2;
    while(sb) {
      size += (2 * sizeof(uint32_t));
      sb = sb->next;
    }
  }
  size = (((size + 3) >> 2) << 2);
  return size;
}

int pico_tcp_overhead(struct pico_socket *s)
{
  if (!s)
    return 0;

  return PICO_SIZE_TCPHDR + tcp_options_size((struct pico_socket_tcp *)s, 0); /* hdr + Options size for data pkt */

}

static void tcp_process_sack(struct pico_socket_tcp *t, uint32_t start, uint32_t end)
{
  struct pico_frame *f;
  struct pico_tree_node * index, * temp;
  int cmp;
  int count = 0;

  pico_tree_foreach_safe(index,&t->tcpq_out.pool,temp){
    f = index->keyValue;
    cmp = seq_compare(SEQN(f), start);
    if (cmp > 0)
      goto done;

    if (cmp == 0) {
      cmp = seq_compare(SEQN(f) + f->payload_len, end);
      if (cmp > 0) {
        tcp_dbg("Invalid SACK: ignoring.\n");
      }

      tcp_dbg("Marking (by SACK) segment %08x BLK:[%08x::%08x]\n", SEQN(f), start, end);
      f->flags |= PICO_FRAME_FLAG_SACKED;
      count++;

      if (cmp == 0) {
        /* that was last segment sacked. Job done */
        goto done;
      }
    }
  }

done:
  if (t->x_mode > PICO_TCP_LOOKAHEAD) {
    if (t->in_flight > (count))
      t->in_flight -= (count);
    else
      t->in_flight = 0;
  }
}

static void tcp_rcv_sack(struct pico_socket_tcp *t, uint8_t *opt, int len)
{
  uint32_t start, end;
  int i = 0;
  if (len % 8) {
    tcp_dbg("SACK: Invalid len.\n");
    return;
  }
  while (i < len) {
    start = long_from(opt + i);
    i += 4;
    end = long_from(opt + i);
    i += 4;
    tcp_process_sack(t, long_be(start), long_be(end));
  }
}

static void tcp_parse_options(struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)f->sock;
  uint8_t *opt = f->transport_hdr + PICO_SIZE_TCPHDR;
  int i = 0;
  f->timestamp = 0;
  while (i < (f->transport_len - PICO_SIZE_TCPHDR)) {
    uint8_t type =  opt[i++];
    uint8_t len;
    if(i < (f->transport_len - PICO_SIZE_TCPHDR) && (type > 1))
      len =  opt[i++];
    else
      len = 1;
    if (f->payload && ((opt + i) > f->payload))
      break;
    tcp_dbg_options("Received option '%d', len = %d \n", type, len);
    switch (type) {
      case PICO_TCP_OPTION_NOOP:
      case PICO_TCP_OPTION_END:
        break;
      case PICO_TCP_OPTION_WS:
        if (len != PICO_TCPOPTLEN_WS) {
          tcp_dbg_options("TCP Window scale: bad len received (%d).\n", len);
          i += len - 2;
          break;
        }
        t->recv_wnd_scale = opt[i++];
        tcp_dbg_options("TCP Window scale: received %d\n", t->recv_wnd_scale);
        break;
      case PICO_TCP_OPTION_SACK_OK:
        if (len != PICO_TCPOPTLEN_SACK_OK) {
          tcp_dbg_options("TCP option sack: bad len received.\n");
          i += len - 2;
          break;
        }
        t->sack_ok = 1;
        break;
      case PICO_TCP_OPTION_MSS: {
        uint16_t mss;
        if (len != PICO_TCPOPTLEN_MSS) {
          tcp_dbg_options("TCP option mss: bad len received.\n");
          i += len - 2;
          break;
        }
        t->mss_ok = 1;
        mss = short_from(opt + i);
        i += sizeof(uint16_t);
        if (t->mss > short_be(mss))
          t->mss = short_be(mss);
        break;
      }
      case PICO_TCP_OPTION_TIMESTAMP: {
        uint32_t tsval, tsecr;
        if (len != PICO_TCPOPTLEN_TIMESTAMP) {
          tcp_dbg_options("TCP option timestamp: bad len received.\n");
          i += len - 2;
          break;
        }
        t->ts_ok = 1;
        tsval = long_from(opt + i);
        i += sizeof(uint32_t);
        tsecr = long_from(opt + i);
        f->timestamp = long_be(tsecr);
        i += sizeof(uint32_t);
        t->ts_nxt = long_be(tsval);
        break;
      }
      case PICO_TCP_OPTION_SACK:
      {
        tcp_rcv_sack(t, opt + i, len - 2);
        i += len - 2;
        break;
      }
      default:
        tcp_dbg_options("TCP: received unsupported option %u\n", type);
        i += len - 2;
    }
  }
}

static int tcp_send(struct pico_socket_tcp *ts, struct pico_frame *f)
{
  struct pico_tcp_hdr *hdr= (struct pico_tcp_hdr *) f->transport_hdr;
  struct pico_frame *cpy;
  hdr->trans.sport = ts->sock.local_port;
  hdr->trans.dport = ts->sock.remote_port;
  if (!hdr->seq)
    hdr->seq = long_be(ts->snd_nxt);

  if (ts->rcv_nxt != 0) {
    if ( (ts->rcv_ackd == 0) || (seq_compare(ts->rcv_ackd, ts->rcv_nxt) != 0) || (hdr->flags & PICO_TCP_ACK)) {
      hdr->flags |= PICO_TCP_ACK;
      hdr->ack = long_be(ts->rcv_nxt);
      ts->rcv_ackd = ts->rcv_nxt;
    }
  }

  if (hdr->flags & PICO_TCP_SYN) {
    ts->snd_nxt++;
  }
  if (f->payload_len > 0) {
    hdr->flags |= PICO_TCP_PSH | PICO_TCP_ACK;
    hdr->ack = long_be(ts->rcv_nxt);
    ts->rcv_ackd = ts->rcv_nxt;
    ts->keepalive_timer_running = 2;    /* XXX TODO check fix: added 1 to counter to postpone sending keepalive, ACK is in data segments */
  }

  f->start = f->transport_hdr + PICO_SIZE_TCPHDR;
  hdr->rwnd = short_be(ts->wnd);
  hdr->crc = 0;
  hdr->crc = short_be(pico_tcp_checksum_ipv4(f));

  /* TCP: ENQUEUE to PROTO ( Transmit ) */
  cpy = pico_frame_copy(f);
  if ((pico_enqueue(&tcp_out, cpy) > 0)) {
    if (f->payload_len > 0) {
      ts->in_flight++;
      ts->snd_nxt += f->payload_len;  /* update next pointer here to prevent sending same segment twice when called twice in same tick */
    }
    tcp_dbg("DBG> [tcp output] state: %02x --> local port:%d remote port: %d seq: %08x ack: %08x flags: %02x = t_len: %d, hdr: %u payload: %d\n",
      TCPSTATE(&ts->sock) >> 8, short_be(hdr->trans.sport), short_be(hdr->trans.dport), SEQN(f), ACKN(f), hdr->flags, f->transport_len, (hdr->len & 0xf0) >> 2 , f->payload_len );
  } else {
    pico_frame_discard(cpy);
  }
  return 0;
}

//#define PICO_TCP_SUPPORT_SOCKET_STATS

#ifdef PICO_TCP_SUPPORT_SOCKET_STATS
static void sock_stats(unsigned long when, void *arg)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)arg;
  tcp_dbg("STATISTIC> [%lu] socket state: %02x --> local port:%d remote port: %d queue size: %d snd_una: %08x snd_nxt: %08x timer: %d cwnd: %d\n",
    when, t->sock.state, short_be(t->sock.local_port), short_be(t->sock.remote_port), t->tcpq_out.size, SEQN(first_segment(&t->tcpq_out)), t->snd_nxt, t->timer_running, t->cwnd);
  pico_timer_add(2000, sock_stats, t);
}
#endif

struct pico_socket *pico_tcp_open(void)
{
  struct pico_socket_tcp *t = pico_zalloc(sizeof(struct pico_socket_tcp));
  if (!t)
    return NULL;
  t->mss = PICO_TCP_DEFAULT_MSS;

  t->tcpq_in.pool.root = t->tcpq_hold.pool.root = t->tcpq_out.pool.root = &LEAF;
  t->tcpq_hold.pool.compare = t->tcpq_in.pool.compare = t->tcpq_out.pool.compare = segment_compare;

  t->tcpq_in.max_size = PICO_DEFAULT_SOCKETQ;
  t->tcpq_out.max_size = PICO_DEFAULT_SOCKETQ;
  t->tcpq_hold.max_size = 2*PICO_TCP_DEFAULT_MSS;

  /* disable Nagle by default */
  t->sock.opt_flags |= (1 << PICO_SOCKET_OPT_TCPNODELAY);

#ifdef PICO_TCP_SUPPORT_SOCKET_STATS
  pico_timer_add(2000, sock_stats, t);
#endif
  tcp_set_space(t);

  return &t->sock;
}

int pico_tcp_read(struct pico_socket *s, void *buf, int len)
{
  struct pico_socket_tcp *t = TCP_SOCK(s);
  struct pico_frame *f;
  uint32_t in_frame_off, in_frame_len;
  int tot_rd_len = 0;

  while (tot_rd_len < len) {
    /* To be sure we don't have garbage at the beginning */
    release_until(&t->tcpq_in, t->rcv_processed);
    f = first_segment(&t->tcpq_in);
    if (!f) {
      tcp_set_space(t);
      goto out;
    }

    /* Hole at the beginning of data, awaiting retransmissions. */
    if (seq_compare(t->rcv_processed, SEQN(f)) < 0) {
      tcp_dbg("TCP> read hole beginning of data, %08x - %08x. rcv_nxt is %08x\n",t->rcv_processed, SEQN(f), t->rcv_nxt);
      goto out;
    }

    if(seq_compare(t->rcv_processed, SEQN(f)) > 0) {
      in_frame_off = t->rcv_processed - SEQN(f);
      in_frame_len = f->payload_len - in_frame_off;
    } else {
      in_frame_off = 0;
      in_frame_len = f->payload_len;
    }
    if ((in_frame_len + tot_rd_len) > len) {
      in_frame_len = len - tot_rd_len;
    }

    if (in_frame_len > f->payload_len - in_frame_off)
      in_frame_len = f->payload_len - in_frame_off;

    memcpy((uint8_t *)buf + tot_rd_len, f->payload + in_frame_off, in_frame_len);
    tot_rd_len += in_frame_len;
    t->rcv_processed += in_frame_len;

    if ((in_frame_len == 0) || (in_frame_len == f->payload_len)) {
      pico_discard_segment(&t->tcpq_in, f);
    }
  }

out:
  tcp_set_space(t);
  if (t->tcpq_in.size == 0) {
    s->ev_pending &= (~PICO_SOCK_EV_RD);
  }
  return tot_rd_len;
}

int pico_tcp_initconn(struct pico_socket *s);
static void initconn_retry(unsigned long when, void *arg)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)arg;
  if (TCPSTATE(&t->sock) == PICO_SOCKET_STATE_TCP_SYN_SENT) {
    if (t->backoff > PICO_TCP_MAX_CONNECT_RETRIES) {
      tcp_dbg("TCP> Connection timeout. \n");
      if (t->sock.wakeup)
        t->sock.wakeup(PICO_SOCK_EV_ERR, &t->sock);
      return;
    }
    tcp_dbg("TCP> SYN retry %d...\n", t->backoff);
    t->backoff++;
    pico_tcp_initconn(&t->sock);
  } else {
    tcp_dbg("TCP> Connection is already established: no retry needed. good.\n");
  }
}

int pico_tcp_initconn(struct pico_socket *s)
{
  struct pico_socket_tcp *ts = TCP_SOCK(s);
  struct pico_frame *syn;
  struct pico_tcp_hdr *hdr;
  int opt_len = tcp_options_size(ts, PICO_TCP_SYN);

  syn = s->net->alloc(s->net, PICO_SIZE_TCPHDR + opt_len);
  if (!syn)
    return -1;
  hdr = (struct pico_tcp_hdr *) syn->transport_hdr;

  if (!ts->snd_nxt)
    ts->snd_nxt = long_be(pico_paws());
  ts->snd_last = ts->snd_nxt;
  ts->cwnd = PICO_TCP_IW;
  ts->ssthresh = 40;
  syn->sock = s;
  hdr->seq = long_be(ts->snd_nxt);
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2 | ts->jumbo;
  hdr->flags = PICO_TCP_SYN;
  tcp_set_space(ts);
  hdr->rwnd = short_be(ts->wnd);
  tcp_add_options(ts,syn, PICO_TCP_SYN, opt_len);
  hdr->trans.sport = ts->sock.local_port;
  hdr->trans.dport = ts->sock.remote_port;

  hdr->crc = 0;
  hdr->crc = short_be(pico_tcp_checksum_ipv4(syn));

  /* TCP: ENQUEUE to PROTO ( SYN ) */
  tcp_dbg("Sending SYN... (ports: %d - %d) size: %d\n", short_be(ts->sock.local_port), short_be(ts->sock.remote_port), syn->buffer_len);
  pico_enqueue(&tcp_out, syn);
  pico_timer_add(PICO_TCP_SYN_TO << ts->backoff, initconn_retry, ts);
  return 0;
}

static int tcp_send_synack(struct pico_socket *s)
{
  struct pico_socket_tcp *ts = TCP_SOCK(s);
  struct pico_frame *synack;
  struct pico_tcp_hdr *hdr;
  int opt_len = tcp_options_size(ts, PICO_TCP_SYN | PICO_TCP_ACK);

  synack = s->net->alloc(s->net, PICO_SIZE_TCPHDR + opt_len);
  if (!synack)
    return -1;
  hdr = (struct pico_tcp_hdr *) synack->transport_hdr;

  synack->sock = s;
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2 | ts->jumbo;
  hdr->flags = PICO_TCP_SYN | PICO_TCP_ACK;
  hdr->rwnd = short_be(ts->wnd);
  hdr->seq = long_be(ts->snd_nxt);
  ts->rcv_processed = long_be(hdr->seq);
  ts->snd_last = ts->snd_nxt;
  tcp_set_space(ts);
  tcp_add_options(ts,synack, hdr->flags, opt_len);
  synack->payload_len = 0;
  synack->timestamp = pico_tick;
  tcp_send(ts, synack);
  pico_frame_discard(synack);
  return 0;
}

static void tcp_send_empty(struct pico_socket_tcp *t, uint16_t flags)
{
  struct pico_frame *f;
  struct pico_tcp_hdr *hdr;
  int opt_len = tcp_options_size(t, flags);
  f = t->sock.net->alloc(t->sock.net, PICO_SIZE_TCPHDR + opt_len);
  if (!f) {
    return;
  }
  f->sock = &t->sock;
  hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2 | t->jumbo;
  hdr->flags = flags;
  hdr->rwnd = short_be(t->wnd);
  tcp_set_space(t);
  tcp_add_options(t,f, flags, opt_len);
  hdr->trans.sport = t->sock.local_port;
  hdr->trans.dport = t->sock.remote_port;
  hdr->seq = long_be(t->snd_nxt);
  if ((flags & PICO_TCP_ACK) != 0)
    hdr->ack = long_be(t->rcv_nxt);
  t->rcv_ackd = t->rcv_nxt;

  f->start = f->transport_hdr + PICO_SIZE_TCPHDR;
  hdr->rwnd = short_be(t->wnd);
  hdr->crc = 0;
  hdr->crc = short_be(pico_tcp_checksum_ipv4(f));

  /* TCP: ENQUEUE to PROTO */
  pico_enqueue(&tcp_out, f);
}

static void tcp_send_ack(struct pico_socket_tcp *t)
{
  return tcp_send_empty(t, PICO_TCP_ACK);
}

static int tcp_send_rst(struct pico_socket *s, struct pico_frame *fr)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) s;
  struct pico_frame *f;
  struct pico_tcp_hdr *hdr, *hdr_rcv;
  int opt_len = tcp_options_size(t, PICO_TCP_RST);
  int close;

  tcp_dbg("TCP SEND_RST >>>>>>>>>>>>>>> START\n");

  f = t->sock.net->alloc(t->sock.net, PICO_SIZE_TCPHDR + opt_len);

  if (!f) {
    return -1;
  }

  hdr_rcv = (struct pico_tcp_hdr *) fr->transport_hdr;

  f->sock = &t->sock;
  hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2 | t->jumbo;
  hdr->flags = PICO_TCP_RST;
  hdr->rwnd = short_be(t->wnd);
  tcp_set_space(t);
  tcp_add_options(t,f, PICO_TCP_RST, opt_len);
  hdr->trans.sport = t->sock.local_port;
  hdr->trans.dport = t->sock.remote_port;
  hdr->seq = long_be(t->snd_nxt);

  /* check if state is synchronized */
  if (((s->state & PICO_SOCKET_STATE_TCP) > PICO_SOCKET_STATE_TCP_SYN_RECV)) {
    /* in synchronized state: send RST with seq = ack from previous segment */
    hdr->seq = hdr_rcv->ack;
    close = 0;
  } else {
    /* non-synchronized state */
    /* go to CLOSED here to prevent timer callback to go on after timeout */
    (t->sock).state &= 0x00FFU;
    (t->sock).state |= PICO_SOCKET_STATE_TCP_CLOSED;
    close = 1;
  }

  hdr->ack = long_be(t->rcv_nxt);
  t->rcv_ackd = t->rcv_nxt;
  f->start = f->transport_hdr + PICO_SIZE_TCPHDR;
  hdr->rwnd = short_be(t->wnd);
  hdr->crc = 0;
  hdr->crc = short_be(pico_tcp_checksum_ipv4(f));

  /* TCP: ENQUEUE to PROTO */
  pico_enqueue(&tcp_out, f);

  /* goto CLOSED */
  if (close) {
    (t->sock).state &= 0xFF00U;
    (t->sock).state |= PICO_SOCKET_STATE_CLOSED;

    /* call EV_FIN wakeup before deleting */
    if ((t->sock).wakeup)
      (t->sock).wakeup(PICO_SOCK_EV_FIN, &(t->sock));

    /* delete socket */
      pico_socket_del(&t->sock);

    tcp_dbg("TCP SEND_RST >>>>>>>>>>>>>>> DONE, deleted socket\n");
  }

  return 0;
}

int pico_tcp_reply_rst(struct pico_frame *fr)
{
  struct pico_tcp_hdr *hdr;
  struct pico_frame *f;
  int size = PICO_SIZE_TCPHDR;

  tcp_dbg("TCP>>>>>>>>>>>>>>>> sending RST ... <<<<<<<<<<<<<<<<<<\n");

  f = fr->sock->net->alloc(fr->sock->net, size);

  /* fill in IP data from original frame */
  // TODO if IPv4
  ((struct pico_ipv4_hdr *)(f->net_hdr))->dst.addr = ((struct pico_ipv4_hdr *)(fr->net_hdr))->src.addr;
  ((struct pico_ipv4_hdr *)(f->net_hdr))->src.addr = ((struct pico_ipv4_hdr *)(fr->net_hdr))->dst.addr;

  /* fill in TCP data from original frame */
  ((struct pico_tcp_hdr *)(f->transport_hdr))->trans.dport = ((struct pico_tcp_hdr *)(fr->transport_hdr))->trans.sport;
  ((struct pico_tcp_hdr *)(f->transport_hdr))->trans.sport = ((struct pico_tcp_hdr *)(fr->transport_hdr))->trans.dport;
  hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  hdr->len   = size << 2;
  hdr->flags = PICO_TCP_RST | PICO_TCP_ACK;
  hdr->rwnd  = 0;
  if (((struct pico_tcp_hdr *)(fr->transport_hdr))->flags & PICO_TCP_ACK) {
    hdr->seq = ((struct pico_tcp_hdr *)(fr->transport_hdr))->ack;
  } else {
    hdr->seq = 0U;
  }

  hdr->ack = ((struct pico_tcp_hdr *)(fr->transport_hdr))->seq + short_be(fr->payload_len);

  /* enqueue for transmission */
  pico_ipv4_frame_push(f,&(((struct pico_ipv4_hdr *)(f->net_hdr))->dst),PICO_PROTO_TCP);

  return 0;
}

static int tcp_nosync_rst(struct pico_socket *s, struct pico_frame *fr)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) s;
  struct pico_frame *f;
  struct pico_tcp_hdr *hdr, *hdr_rcv;
  int opt_len = tcp_options_size(t, PICO_TCP_RST | PICO_TCP_ACK);

  tcp_dbg("TCP SEND RST (NON-SYNC) >>>>>>>>>>>>>>>>>> state %x\n",(s->state & PICO_SOCKET_STATE_TCP));
  if (((s->state & PICO_SOCKET_STATE_TCP) ==  PICO_SOCKET_STATE_TCP_LISTEN)) {
    /* XXX TODO NOTE: to prevent the parent socket from trying to send, because this socket has no knowledge of dst IP !!! */
    return pico_tcp_reply_rst(fr);
  }

  /***************************************************************************/
  /* sending RST */
  f = t->sock.net->alloc(t->sock.net, PICO_SIZE_TCPHDR + opt_len);

  if (!f) {
    return -1;
  }

  hdr_rcv = (struct pico_tcp_hdr *) fr->transport_hdr;

  f->sock = &t->sock;
  hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2 | t->jumbo;
  hdr->flags = PICO_TCP_RST | PICO_TCP_ACK;
  hdr->rwnd = short_be(t->wnd);
  tcp_set_space(t);
  tcp_add_options(t,f, PICO_TCP_RST | PICO_TCP_ACK, opt_len);
  hdr->trans.sport = t->sock.local_port;
  hdr->trans.dport = t->sock.remote_port;

  /* non-synchronized state */
  if (hdr_rcv->flags & PICO_TCP_ACK) {
    hdr->seq = hdr_rcv->ack;
  } else {
    hdr->seq = 0U;
  }

  hdr->ack = hdr_rcv->seq + short_be(fr->payload_len);

  t->rcv_ackd = t->rcv_nxt;
  f->start = f->transport_hdr + PICO_SIZE_TCPHDR;
  hdr->rwnd = short_be(t->wnd);
  hdr->crc = 0;
  hdr->crc = short_be(pico_tcp_checksum_ipv4(f));

  /* TCP: ENQUEUE to PROTO */
  pico_enqueue(&tcp_out, f);

  /***************************************************************************/

  tcp_dbg("TCP SEND_RST (NON_SYNC) >>>>>>>>>>>>>>> DONE, ...\n");

  return 0;
}

static void tcp_send_fin(struct pico_socket_tcp *t)
{
  struct pico_frame *f;
  struct pico_tcp_hdr *hdr;
  int opt_len = tcp_options_size(t, PICO_TCP_FIN);
  f = t->sock.net->alloc(t->sock.net, PICO_SIZE_TCPHDR + opt_len);
  if (!f) {
    return;
  }
  f->sock = &t->sock;
  hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2 | t->jumbo;
  hdr->flags = PICO_TCP_FIN | PICO_TCP_ACK;
  hdr->ack = long_be(t->rcv_nxt);
  t->rcv_ackd = t->rcv_nxt;
  hdr->rwnd = short_be(t->wnd);
  tcp_set_space(t);
  tcp_add_options(t,f, PICO_TCP_FIN, opt_len);
  hdr->trans.sport = t->sock.local_port;
  hdr->trans.dport = t->sock.remote_port;
  hdr->seq = long_be(t->snd_nxt);   /* XXX TODO check correct ?? --> snd_last? otherwise maybe data after FIN */

  f->start = f->transport_hdr + PICO_SIZE_TCPHDR;
  hdr->rwnd = short_be(t->wnd);
  hdr->crc = 0;
  hdr->crc = short_be(pico_tcp_checksum_ipv4(f));
  //tcp_dbg("SENDING FIN...\n");
  /* TCP: ENQUEUE to PROTO ( Pure ACK ) */
  pico_enqueue(&tcp_out, f);
  t->snd_nxt++;
}

static void tcp_sack_prepare(struct pico_socket_tcp *t)
{
  struct pico_frame *pkt;
  uint32_t left=0, right=0;
  struct tcp_sack_block *sb;
  int n = 0;
  if (t->sacks) /* previous sacks are pending */
    return;

  pkt = first_segment(&t->tcpq_in);
  while(n < 3) {
    if (!pkt) {
      if(left) {
        sb = pico_zalloc(sizeof(struct tcp_sack_block));
        if (!sb)
          break;
        sb->left = long_be(left);
        sb->right = long_be(right);
        n++;
        sb->next = t->sacks;
        t->sacks = sb;
        left = 0;
        right = 0;
      }
      break;
    }
    if ((SEQN(pkt) < t->rcv_nxt)) {
      pkt = next_segment(&t->tcpq_in, pkt);
      continue;
    }
    if (!left) {
      left = SEQN(pkt);
      right = SEQN(pkt) + pkt->payload_len;
      pkt = next_segment(&t->tcpq_in, pkt);
      continue;
    }
    if(SEQN(pkt) == (right + 1)) {
      right += pkt->payload_len;
      pkt = next_segment(&t->tcpq_in, pkt);
      continue;
    } else {
      sb = pico_zalloc(sizeof(struct tcp_sack_block));
      if (!sb)
        break;
      sb->left = long_be(left);
      sb->right = long_be(right);
      n++;
      sb->next = t->sacks;
      t->sacks = sb;
      left = 0;
      right = 0;
      pkt = next_segment(&t->tcpq_in, pkt);
    }
  }
}

static int tcp_data_in(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) f->transport_hdr;

  if (((hdr->len & 0xf0) >> 2) <= f->transport_len) {
    tcp_parse_options(f);
    f->payload = f->transport_hdr + ((hdr->len & 0xf0) >>2);
    f->payload_len = f->transport_len - ((hdr->len & 0xf0) >>2);
    tcp_dbg("TCP> Received segment. (exp: %x got: %x)\n", t->rcv_nxt, SEQN(f));

    if (seq_compare(SEQN(f), t->rcv_nxt) <= 0) {
      struct pico_frame *nxt;
      if (seq_compare(SEQN(f), t->rcv_nxt) == 0) { /* Exactly what we expected */
        struct pico_frame *cpy = pico_frame_copy(f);
        /* Enqueue: try to put into RCV buffer */
        if(pico_enqueue_segment(&t->tcpq_in, cpy) <= 0) {
          pico_frame_discard(cpy);
        }
        t->rcv_nxt = SEQN(f) + f->payload_len;
        nxt = peek_segment(&t->tcpq_in, t->rcv_nxt);
        while(nxt) {
          tcp_dbg("scrolling rcv_nxt...%08x\n", t->rcv_nxt);
          t->rcv_nxt += f->payload_len;
          nxt = peek_segment(&t->tcpq_in, t->rcv_nxt);
        }
        t->sock.ev_pending |= PICO_SOCK_EV_RD;
        t->rcv_nxt = SEQN(f) + f->payload_len;
      } else {
        tcp_dbg("TCP> lo segment. Uninteresting retransmission. (exp: %x got: %x)\n", t->rcv_nxt, SEQN(f));
      }
    } else {
      tcp_dbg("TCP> hi segment. Possible packet loss. I'll dupack this. (exp: %x got: %x)\n", t->rcv_nxt, SEQN(f));
      if (t->sack_ok) {
        tcp_sack_prepare(t);
      }
    }
    /* In either case, ack til recv_nxt. */
    if ( ((t->sock.state & PICO_SOCKET_STATE_TCP) != PICO_SOCKET_STATE_TCP_CLOSE_WAIT) && ((t->sock.state & PICO_SOCKET_STATE_TCP) != PICO_SOCKET_STATE_TCP_SYN_SENT) && ((t->sock.state & PICO_SOCKET_STATE_TCP) != PICO_SOCKET_STATE_TCP_SYN_RECV)) {
      //tcp_dbg("SENDACK CALLED FROM OUTSIDE tcp_synack, state %x\n",t->sock.state);
      tcp_send_ack(t);
    } else {
      //tcp_dbg("SENDACK PREVENTED IN SYNSENT STATE\n");
    }
    return 0;
  } else {
    tcp_dbg("TCP: invalid data in pkt len, exp: %d, got %d\n", (hdr->len & 0xf0) >> 2, f->transport_len);
    return -1;
  }
}

static int tcp_ack_advance_una(struct pico_socket_tcp *t, struct pico_frame *f)
{
  int ret =  release_all_until(&t->tcpq_out, ACKN(f));
  if (ret > 0)
    t->sock.ev_pending |= PICO_SOCK_EV_WR;
  return ret;
}

static uint16_t time_diff(unsigned long a, unsigned long b)
{
  if (a >= b)
    return (a - b);
  else
    return (b - a);
}

static void tcp_rtt(struct pico_socket_tcp *t, uint32_t rtt)
{

  uint32_t avg = t->avg_rtt;
  uint32_t rvar = t->rttvar;
  if (!avg) {
    /* This follows RFC2988
     * (2.2) When the first RTT measurement R is made, the host MUST set
     *
     * SRTT <- R
     * RTTVAR <- R/2
     * RTO <- SRTT + max (G, K*RTTVAR)
     */
    t->avg_rtt = rtt;
    t->rttvar = rtt >> 1;
    t->rto = t->avg_rtt + (t->rttvar << 4);
  } else {
    int var = (t->avg_rtt - rtt);
    if (var < 0)
      var = 0-var;
    /* RFC2988, section (2.3). Alpha and beta are the ones suggested. */

    /* First, evaluate a new value for the rttvar */
    t->rttvar <<= 2;
    t->rttvar -= rvar;
    t->rttvar += var;
    t->rttvar >>= 2;

    /* Then, calculate the new avg_rtt */
    t->avg_rtt <<= 3;
    t->avg_rtt -= avg;
    t->avg_rtt += rtt;
    t->avg_rtt >>= 3;

    /* Finally, assign a new value for the RTO, as specified in the RFC, with K=4 */
    t->rto = t->avg_rtt + (t->rttvar << 2);
  }
  tcp_dbg(" -----=============== RTT CUR: %u AVG: %u RTTVAR: %u RTO: %u ======================----\n", rtt, t->avg_rtt, t->rttvar, t->rto);
}

static void tcp_congestion_control(struct pico_socket_tcp *t)
{
  if (t->x_mode > PICO_TCP_LOOKAHEAD)
    return;
  if (t->cwnd > t->tcpq_out.frames) {
    tcp_dbg("Limited by app: %d\n", t->cwnd);
    if (t->sock.wakeup)
      t->sock.wakeup(PICO_SOCK_EV_WR, &t->sock);
    return;
  }
  tcp_dbg("Doing congestion control\n");
  if (t->cwnd < t->ssthresh) {
    t->cwnd++;
  } else {
    t->cwnd_counter++;
    if (t->cwnd_counter >= t->cwnd) {
      t->cwnd++;
      t->cwnd_counter = 0;
    }
  }
  tcp_dbg("TCP_CWND, %lu, %u, %u, %u\n", pico_tick, t->cwnd, t->ssthresh, t->in_flight);
}

static void add_retransmission_timer(struct pico_socket_tcp *t, unsigned long next_ts);
static void tcp_retrans_timeout(unsigned long val, void *sock)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) sock;
  struct pico_frame *f = NULL;
  unsigned long limit = val - t->rto;
  struct pico_tcp_hdr *hdr;
  if( t->sock.net && ((t->sock.state & 0xFF00) == PICO_SOCKET_STATE_TCP_ESTABLISHED
        || (t->sock.state & 0xFF00) == PICO_SOCKET_STATE_TCP_CLOSE_WAIT) )
  {
        tcp_dbg("\n\nTIMEOUT! backoff = %d\n", t->backoff);
        /* was timer cancelled? */
        if (t->timer_running == 0) {
            add_retransmission_timer(t, 0);
            return;
        }
        t->timer_running--;

        f = first_segment(&t->tcpq_out);
        while (f) {
            if ((t->x_mode == PICO_TCP_WINDOW_FULL) ||
                    ((f->timestamp != 0) && (f->timestamp <= limit))) {
                struct pico_frame *cpy;
                hdr = (struct pico_tcp_hdr *)f->transport_hdr;
                tcp_dbg("TCP BLACKOUT> TIMED OUT (output) frame %08x, len= %d rto=%d Win full: %d frame flags: %04x\n", SEQN(f), f->payload_len, t->rto, t->x_mode == PICO_TCP_WINDOW_FULL, f->flags);
                if ((t->x_mode != PICO_TCP_WINDOW_FULL) ) {
                    t->x_mode = PICO_TCP_BLACKOUT;
                    tcp_dbg("Mode: Blackout.\n");
                    t->cwnd = PICO_TCP_IW;
                    t->in_flight = 0;
                }
                f->timestamp = pico_tick;
                tcp_add_options(t, f, 0, f->transport_len - f->payload_len - PICO_SIZE_TCPHDR);
                hdr->rwnd = short_be(t->wnd);
                hdr->flags |= PICO_TCP_PSH;
                hdr->ack = long_be(t->rcv_nxt);
                hdr->crc = 0;
                hdr->crc = short_be(pico_tcp_checksum_ipv4(f));
                /* TCP: ENQUEUE to PROTO ( retransmit )*/
                cpy = pico_frame_copy(f);
                if (pico_enqueue(&tcp_out, cpy) > 0) {
                    t->backoff++;
                    add_retransmission_timer(t, (t->rto << t->backoff) + pico_tick);
                    tcp_dbg("TCP_CWND, %lu, %u, %u, %u\n", pico_tick, t->cwnd, t->ssthresh, t->in_flight);
                    return;
                } else {
                    add_retransmission_timer(t, (t->rto << t->backoff) + pico_tick);
                    pico_frame_discard(cpy);
                }
            }
            f = next_segment(&t->tcpq_out, f);
        }
        t->backoff = 0;
        add_retransmission_timer(t, 0);
        if (t->tcpq_out.size < t->tcpq_out.max_size)
             t->sock.ev_pending |= PICO_SOCK_EV_WR;
        return;
    }
}

static void add_retransmission_timer(struct pico_socket_tcp *t, unsigned long next_ts)
{
  struct pico_tree_node * index;

  if (t->timer_running > 0)
    return;

  if (next_ts == 0) {
    struct pico_frame *f;

    pico_tree_foreach(index,&t->tcpq_out.pool){
      f = index->keyValue;
      if (((next_ts == 0) || (f->timestamp < next_ts)) && (f->timestamp > 0)) {
        next_ts = f->timestamp;
      }
    }
  }
  if (next_ts > 0) {
    if ((next_ts + t->rto) > pico_tick) {
      pico_timer_add(next_ts + t->rto - pico_tick, tcp_retrans_timeout, t);
    } else {
      pico_timer_add(1, tcp_retrans_timeout, t);
    }
    t->timer_running++;
  }
}

static int tcp_retrans(struct pico_socket_tcp *t, struct pico_frame *f)
{
  struct pico_frame *cpy;
  struct pico_tcp_hdr *hdr;
  if (f) {
    hdr = (struct pico_tcp_hdr *)f->transport_hdr;
    tcp_dbg("TCP> RETRANS (by dupack) frame %08x, len= %d\n", SEQN(f), f->payload_len);
    f->timestamp = pico_tick;
    tcp_add_options(t, f, 0, f->transport_len - f->payload_len - PICO_SIZE_TCPHDR);
    hdr->rwnd = short_be(t->wnd);
    hdr->flags |= PICO_TCP_PSH;
    hdr->ack = long_be(t->rcv_nxt);
    hdr->crc = 0;
    hdr->crc = short_be(pico_tcp_checksum_ipv4(f));
    /* TCP: ENQUEUE to PROTO ( retransmit )*/
    cpy = pico_frame_copy(f);
    if (pico_enqueue(&tcp_out, cpy) > 0) {
      t->in_flight++;
      t->snd_last_out = SEQN(cpy);
      add_retransmission_timer(t, pico_tick + t->rto);
    } else {
      pico_frame_discard(cpy);
    }
    return(f->payload_len);
  }
  return 0;
}

#ifdef TCP_ACK_DBG
static void tcp_ack_dbg(struct pico_socket *s, struct pico_frame *f)
{
  uint32_t una, nxt, ack, cur;
  struct pico_frame *una_f = NULL, *cur_f;
  struct pico_tree_node *idx;
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  char info[64];
  char tmp[64];
  ack = ACKN(f);
  nxt = t->snd_nxt;
  tcp_dbg("===================================\n");
  tcp_dbg("Queue out (%d/%d). ACKED=%08x\n", t->tcpq_out.size, t->tcpq_out.max_size, ack);

  pico_tree_foreach(idx, &t->tcpq_out.pool) {
    info[0] = 0;
    cur_f = idx->keyValue;
    cur = SEQN(cur_f);
    if (!una_f) {
      una_f = cur_f;
      una = SEQN(una_f);
    }

    if (cur == nxt) {
      strncpy(tmp, info, strlen(info));
      snprintf(info,64, "%s SND_NXT", tmp);
    }
    if (cur == ack) {
      strncpy(tmp, info, strlen(info));
      snprintf(info,64, "%s ACK", tmp);
    }
    if (cur == una) {
      strncpy(tmp, info, strlen(info));
      snprintf(info,64, "%s SND_UNA", tmp);
    }
    if (cur == t->snd_last) {
      strncpy(tmp, info, strlen(info));
      snprintf(info,64, "%s SND_LAST", tmp);
    }
    tcp_dbg("%08x %d%s\n", cur, cur_f->payload_len, info);

  }
  tcp_dbg("SND_NXT is %08x, snd_LAST is %08x", nxt, t->snd_last);
  tcp_dbg("===================================\n");
  tcp_dbg("\n\n");
}
#endif

static int tcp_ack(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_frame *f_new; /* use with Nagle to push to out queue */
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  uint32_t rtt = 0;
  int acked = 0;
  struct pico_frame *una = NULL;
  if ((hdr->flags & PICO_TCP_ACK) == 0)
    return -1;

#ifdef TCP_ACK_DBG
  tcp_ack_dbg(s,f);
#endif

  tcp_parse_options(f);
  t->recv_wnd = short_be(hdr->rwnd);

  acked = tcp_ack_advance_una(t, f);
  una = first_segment(&t->tcpq_out);

  if ((t->x_mode == PICO_TCP_BLACKOUT) || 
    ((t->x_mode == PICO_TCP_WINDOW_FULL) && ((t->recv_wnd << t->recv_wnd_scale) > t->mss))) {
    tcp_dbg("Re-entering look-ahead...\n\n\n");
    t->x_mode = PICO_TCP_LOOKAHEAD;
    t->backoff = 0;
  }

  /* One should be acked. */
//  if ((acked == 0) && (t->in_flight > 0))
  if ((acked == 0) && (f->payload_len  == 0) && (t->in_flight > 0))
    t->in_flight--;
  if (!una || acked > 0) {
    t->x_mode = PICO_TCP_LOOKAHEAD;
    tcp_dbg("Mode: Look-ahead. In flight: %d/%d buf: %d\n", t->in_flight, t->cwnd, t->tcpq_out.frames);
    t->backoff = 0;

    /* Do rtt/rttvar/rto calculations */
    /* First, try with timestamps, using the value from options */
    if(f && (f->timestamp != 0)) {
      rtt = time_diff(pico_tick, f->timestamp);
      if (rtt)
        tcp_rtt(t, rtt);
    } else if(una && (una->timestamp != 0)) {
      /* If no timestamps are there, use conservatve estimation on the una */
        rtt = time_diff(pico_tick, una->timestamp);
        if (rtt)
          tcp_rtt(t, rtt);
    }

    tcp_dbg("TCP ACK> FRESH ACK %08x (acked %d) Queue size: %u/%u frames: %u cwnd: %u in_flight: %u snd_una: %u\n", ACKN(f), acked, t->tcpq_out.size, t->tcpq_out.max_size, t->tcpq_out.frames, t->cwnd, t->in_flight, SEQN(una));
    if (acked > t->in_flight) {
      tcp_dbg("WARNING: in flight < 0\n");
      t->in_flight = 0;
    } else
      t->in_flight -= (acked);

  } else if ((t->snd_old_ack == ACKN(f)) && /* We've just seen this ack, and... */
      ((0 == (hdr->flags & (PICO_TCP_PSH | PICO_TCP_SYN))) &&
          (f->payload_len == 0)) && /* This is a pure ack, and... */
      (ACKN(f) != t->snd_nxt)) /* There is something in flight awaiting to be acked... */
  {
    /* Process incoming duplicate ack. */
    if (t->x_mode < PICO_TCP_RECOVER) {
      t->x_mode++;
      tcp_dbg("Mode: DUPACK %d, due to PURE ACK %0x, len = %d\n", t->x_mode, SEQN(f), f->payload_len);
      tcp_dbg("ACK: %x - QUEUE: %x\n",ACKN(f), SEQN(first_segment(&t->tcpq_out)));
      if (t->x_mode == PICO_TCP_RECOVER) { /* Switching mode */
        t->snd_retry = SEQN(first_segment(&t->tcpq_out));
        if (t->ssthresh > t->cwnd)
          t->ssthresh >>=2;
        else
          t->ssthresh = (t->cwnd >> 1);
        if (t->ssthresh < 2)
          t->ssthresh = 2;
      }
    } else if (t->x_mode == PICO_TCP_RECOVER) {
     tcp_dbg("TCP RECOVER> DUPACK! snd_una: %08x, snd_nxt: %08x, acked now: %08x\n", SEQN(first_segment(&t->tcpq_out)), t->snd_nxt, ACKN(f));
      if (t->in_flight <= t->cwnd) {
        struct pico_frame *nxt = peek_segment(&t->tcpq_out, t->snd_retry);
        if (!nxt)
          nxt = first_segment(&t->tcpq_out);

        while (nxt && (nxt->flags & PICO_FRAME_FLAG_SACKED) && (nxt != first_segment(&t->tcpq_out))) {
          tcp_dbg("Skipping %08x because it is sacked.\n", SEQN(nxt));
          nxt = next_segment(&t->tcpq_out, nxt);
        }

        if (nxt && (seq_compare(SEQN(nxt), t->snd_nxt)) > 0)
          nxt = NULL;
        if (nxt && (seq_compare(SEQN(nxt), SEQN(first_segment(&t->tcpq_out))) > (t->recv_wnd << t->recv_wnd_scale)))
          nxt = NULL;

        if(!nxt)
          nxt = first_segment(&t->tcpq_out);
        if (nxt) {
          tcp_retrans(t, peek_segment(&t->tcpq_out, t->snd_retry));
          t->snd_retry = SEQN(nxt);
        }
      }

      if (++t->cwnd_counter > 1) {
        t->cwnd--;
        if (t->cwnd < 2)
          t->cwnd = 2;
        t->cwnd_counter = 0;
      }
    } else {
      tcp_dbg("DUPACK in mode %d \n", t->x_mode);

    }
  } /* End case duplicate ack detection */

  /* Do congestion control */
  tcp_congestion_control(t);
  if ((acked > 0) && t->sock.wakeup) {
    if (t->tcpq_out.size < t->tcpq_out.max_size)
      t->sock.wakeup(PICO_SOCK_EV_WR, &(t->sock));
      //t->sock.ev_pending |= PICO_SOCK_EV_WR;
  }

  /* if Nagle enabled, check if no unack'ed data and fill out queue (till window) */
  if (IS_NAGLE_ENABLED((&(t->sock)))) {
    while (!IS_TCP_HOLDQ_EMPTY(t) && ((t->tcpq_out.max_size - t->tcpq_out.size) >= PICO_TCP_DEFAULT_MSS)) {
      tcp_dbg_nagle("TCP_ACK - NAGLE add new segment\n");
      f_new = pico_hold_segment_make(t);
      if (f_new == NULL)
        break;            /* XXX corrupt !!! (or no memory) */
      if (pico_enqueue_segment(&t->tcpq_out,f_new) <= 0)
        // handle error
        tcp_dbg_nagle("TCP_ACK - NAGLE FAILED to enqueue in out\n");
    }
  }

  /* If some space was created, put a few segments out. */
  tcp_dbg("TCP_CWND, %lu, %u, %u, %u\n", pico_tick, t->cwnd, t->ssthresh, t->in_flight);
  if (t->x_mode ==  PICO_TCP_LOOKAHEAD) {
    if ((t->cwnd >= t->in_flight) && (t->snd_nxt > t->snd_last_out)) {
      pico_tcp_output(&t->sock, t->cwnd - t->in_flight);
    }
  }

  t->snd_old_ack = ACKN(f);
  return 0;
}

static int tcp_finwaitack(struct pico_socket *s, struct pico_frame *f)
{
  tcp_dbg("RECEIVED ACK IN FIN_WAIT1\nTCP> IN STATE FIN_WAIT2\n");

  /* acking part */
  tcp_ack(s,f);
  /* update TCP state */
  s->state &= 0x00FFU;
  s->state |= PICO_SOCKET_STATE_TCP_FIN_WAIT2;

  return 0;
}

static void tcp_deltcb(unsigned long when, void *arg)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)arg;
  if (TCPSTATE(&t->sock) == PICO_SOCKET_STATE_TCP_TIME_WAIT) {
    tcp_dbg("TCP> state: time_wait, final timer expired, going to closed state\n");
    /* update state */
    (t->sock).state &= 0x00FFU;
    (t->sock).state |= PICO_SOCKET_STATE_TCP_CLOSED;
    (t->sock).state &= 0xFF00U;
    (t->sock).state |= PICO_SOCKET_STATE_CLOSED;
    /* call EV_FIN wakeup before deleting */
    if (t->sock.wakeup) {
      (t->sock).wakeup(PICO_SOCK_EV_FIN, &(t->sock));
    }
    /* delete socket */
    pico_socket_del(&t->sock);
  } else {
    tcp_dbg("TCP> trying to go to closed, wrong state\n");
  }
}

static int tcp_finwaitfin(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  struct pico_tcp_hdr *hdr  = (struct pico_tcp_hdr *) (f->transport_hdr);
  tcp_dbg("TCP> received fin in FIN_WAIT2\n");
  /* received FIN, increase ACK nr */
  t->rcv_nxt = long_be(hdr->seq) + 1;
  s->state &= 0x00FFU;
  s->state |= PICO_SOCKET_STATE_TCP_TIME_WAIT;
  /* set SHUT_REMOTE */
  s->state |= PICO_SOCKET_STATE_SHUT_REMOTE;
  if (s->wakeup)
    s->wakeup(PICO_SOCK_EV_CLOSE, s);
  if (f->payload_len > 0) /* needed?? */
    tcp_data_in(s,f);
  /* send ACK */
  tcp_send_ack(t);
  /* set timer */
  pico_timer_add(200, tcp_deltcb, t);
  return 0;
}

static int tcp_closewaitack(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  tcp_dbg("TCP> received ack in CLOSING\n");
  /* acking part */
  tcp_ack(s,f);
  /* update TCP state */
  s->state &= 0x00FFU;
  s->state |= PICO_SOCKET_STATE_TCP_TIME_WAIT;
  /* set timer */
  pico_timer_add(200, tcp_deltcb, t);
  return 0;
}

static int tcp_lastackwait(struct pico_socket *s, struct pico_frame *f)
{
  tcp_dbg("TCP> state: last_ack, received ack, to closed\n");
  s->state &= 0x00FFU;
  s->state |= PICO_SOCKET_STATE_TCP_CLOSED;
  s->state &= 0xFF00U;
  s->state |= PICO_SOCKET_STATE_CLOSED;
  /* call socket wakeup with EV_FIN */
  if (s->wakeup)
    s->wakeup(PICO_SOCK_EV_FIN, s);
  /* delete socket */
  pico_socket_del(s);
  return 0;
}

static int tcp_syn(struct pico_socket *s, struct pico_frame *f)
{
  /* TODO: Check against backlog length */
  struct pico_socket_tcp *new = (struct pico_socket_tcp *)pico_socket_clone(s);
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *)f->transport_hdr;
  if (!new)
    return -1;

#ifdef PICO_TCP_SUPPORT_SOCKET_STATS
  pico_timer_add(2000, sock_stats, s);
#endif

  new->sock.remote_port = ((struct pico_trans *)f->transport_hdr)->sport;
#ifdef PICO_SUPPORT_IPV4
  if (IS_IPV4(f)) {
    new->sock.remote_addr.ip4.addr = ((struct pico_ipv4_hdr *)(f->net_hdr))->src.addr;
    new->sock.local_addr.ip4.addr = ((struct pico_ipv4_hdr *)(f->net_hdr))->dst.addr;
  }
#endif
#ifdef PICO_SUPPORT_IPV6
  if (IS_IPV6(f)) {
    new->sock.remote_addr.ip6 = ((struct pico_ipv6_hdr *)(f->net_hdr))->src;
    new->sock.local_addr.ip6 = ((struct pico_ipv6_hdr *)(f->net_hdr))->dst;
  }
#endif

  /* Set socket limits */
  new->tcpq_in.max_size = PICO_DEFAULT_SOCKETQ;
  new->tcpq_out.max_size = PICO_DEFAULT_SOCKETQ;
  new->tcpq_hold.max_size = 2*PICO_TCP_DEFAULT_MSS;

  f->sock = &new->sock;
  tcp_parse_options(f);
  new->mss = PICO_TCP_DEFAULT_MSS;
  new->rcv_nxt = long_be(hdr->seq) + 1;
  new->snd_nxt = long_be(pico_paws());
  new->snd_last = new->snd_nxt;
  new->cwnd = PICO_TCP_IW;
  new->ssthresh = 40;
  new->recv_wnd = short_be(hdr->rwnd);
  new->jumbo = hdr->len & 0x07;
  new->sock.parent = s;
  new->sock.wakeup = s->wakeup;
  /* Initialize timestamp values */
  new->sock.state = PICO_SOCKET_STATE_BOUND | PICO_SOCKET_STATE_CONNECTED | PICO_SOCKET_STATE_TCP_SYN_RECV;
  pico_socket_add(&new->sock);
  tcp_send_synack(&new->sock);
  tcp_dbg("SYNACK sent, socket added. snd_nxt is %08x\n", new->snd_nxt);
  return 0;
}

static void tcp_set_init_point(struct pico_socket *s)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  t->rcv_processed = t->rcv_nxt;
}

static int tcp_synack(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) s;
  struct pico_tcp_hdr *hdr  = (struct pico_tcp_hdr *)f->transport_hdr;

  if (ACKN(f) ==  (1 + t->snd_nxt)) {
    t->rcv_nxt = long_be(hdr->seq);
    t->rcv_processed = t->rcv_nxt + 1;
    tcp_ack(s, f);

    s->state &= 0x00FFU;
    s->state |= PICO_SOCKET_STATE_TCP_ESTABLISHED;
    tcp_dbg("TCP> Established. State: %x\n", s->state);

    if (s->wakeup)
      s->wakeup(PICO_SOCK_EV_CONN, s);
    s->ev_pending |= PICO_SOCK_EV_WR;

    t->rcv_nxt++;
    t->snd_nxt++;
    tcp_send_ack(t);  /* return ACK */

    return 0;

  } else {
    tcp_dbg("TCP> Not established, RST sent.\n");
    tcp_nosync_rst(s,f);
    return 0;
  }
}

static int tcp_first_ack(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  tcp_dbg("ACK in SYN_RECV: expecting %08x got %08x\n", t->snd_nxt, ACKN(f));
  if (t->snd_nxt == ACKN(f)) {
    tcp_set_init_point(s);
    tcp_ack(s, f);
    s->state &= 0x00FFU;
    s->state |= PICO_SOCKET_STATE_TCP_ESTABLISHED;
    tcp_dbg("TCP: Established. State now: %04x\n", s->state);
    if( !s->parent && s->wakeup) { /* If the socket has no parent, -> sending socket that has a sim_open */
        tcp_dbg("FIRST ACK - No parent found -> sending socket\n");
        s->wakeup(PICO_SOCK_EV_CONN,  s);
    }
    if (s->parent && s->parent->wakeup) {
      tcp_dbg("FIRST ACK - Parent found -> listening socket\n");
      s->wakeup = s->parent->wakeup;
      s->parent->wakeup(PICO_SOCK_EV_CONN, s->parent);
    }
    s->ev_pending |= PICO_SOCK_EV_WR;
    tcp_dbg("%s: snd_nxt is now %08x\n", __FUNCTION__, t->snd_nxt);
    return 0;
  } else {
    tcp_nosync_rst(s,f);
    return 0;
  }
}

static int tcp_closewait(struct pico_socket *s, struct pico_frame *f)
{

  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  struct pico_tcp_hdr *hdr  = (struct pico_tcp_hdr *) (f->transport_hdr);


  if (f->payload_len > 0)
    tcp_data_in(s,f);
  if (f->flags & PICO_TCP_ACK)
    tcp_ack(s,f);
  if (seq_compare(SEQN(f), t->rcv_nxt) == 0) {
    /* received FIN, increase ACK nr */
    t->rcv_nxt = long_be(hdr->seq) + 1;
        s->state &= 0x00FFU;
        s->state |= PICO_SOCKET_STATE_TCP_CLOSE_WAIT;
        /* set SHUT_REMOTE */
    s->state |= PICO_SOCKET_STATE_SHUT_REMOTE;
      tcp_dbg("TCP> Close-wait\n");

    if (s->wakeup){
      if(f->payload_len>0){
        struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
        t->sock.ev_pending |=PICO_SOCK_EV_CLOSE;
      }else
        s->wakeup(PICO_SOCK_EV_CLOSE, s);
    }
  } else {
    tcp_send_ack(t);  /* return ACK */
  }
  return 0;
}

/*static int tcp_fin(struct pico_socket *s, struct pico_frame *f)
{
  return 0;
}*/

static int tcp_rcvfin(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  tcp_dbg("TCP> Received FIN in FIN_WAIT1\n");
  s->state &= 0x00FFU;
  s->state |= PICO_SOCKET_STATE_TCP_CLOSING;
  t->rcv_processed = t->rcv_nxt + 1;
  t->rcv_nxt++;
  /* send ACK */
  tcp_send_ack(t);
  return 0;
}

static int tcp_finack(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  tcp_dbg("TCP> ENTERED finack\n");
  t->rcv_nxt++;
  /* send ACK */
  tcp_send_ack(t);

  /* call socket wakeup with EV_FIN */
  if (s->wakeup)
    s->wakeup(PICO_SOCK_EV_FIN, s);
  s->state &= 0x00FFU;
  s->state |= PICO_SOCKET_STATE_TCP_TIME_WAIT;
  /* set SHUT_REMOTE */
  s->state |= PICO_SOCKET_STATE_SHUT_REMOTE;
  pico_timer_add(2000, tcp_deltcb, t);

  return 0;
}

static int tcp_rst(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) s;
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) (f->transport_hdr);

  tcp_dbg("TCP >>>>>>>>>>>>>> received RST <<<<<<<<<<<<<<<<<<<<\n");
  if ((s->state & PICO_SOCKET_STATE_TCP) == PICO_SOCKET_STATE_TCP_SYN_SENT) {
    /* the RST is acceptable if the ACK field acknowledges the SYN */
    if ((t->snd_nxt + 1) == ACKN(f)) {  /* valid, got to closed state */
      /* update state */
      (t->sock).state &= 0x00FFU;
      (t->sock).state |= PICO_SOCKET_STATE_TCP_CLOSED;
      (t->sock).state &= 0xFF00U;
      (t->sock).state |= PICO_SOCKET_STATE_CLOSED;

      /* call EV_FIN wakeup before deleting */
      if ((t->sock).wakeup)
        (t->sock).wakeup(PICO_SOCK_EV_FIN, &(t->sock));

      /* call EV_ERR wakeup before deleting */
      pico_err = PICO_ERR_ECONNRESET;
      if ((t->sock).wakeup)
        (t->sock).wakeup(PICO_SOCK_EV_ERR, &(t->sock));

      /* delete socket */
      pico_socket_del(&t->sock);
    } else {                      /* not valid, ignore */
      tcp_dbg("TCP RST> IGNORE\n");
      return 0;
    }
  } else {  /* all other states */
    /* all reset (RST) segments are validated by checking their SEQ-fields,
    a reset is valid if its sequence number is in the window */
    if ((long_be(hdr->seq) >= t->rcv_ackd) && (long_be(hdr->seq) <= ((short_be(hdr->rwnd)<<(t->wnd_scale)) + t->rcv_ackd))) {
      if ((s->state & PICO_SOCKET_STATE_TCP) == PICO_SOCKET_STATE_TCP_SYN_RECV) {
        /* go to closed */
        (t->sock).state &= 0x00FFU;
        (t->sock).state |= PICO_SOCKET_STATE_TCP_CLOSED;
        (t->sock).state &= 0xFF00U;
        (t->sock).state |= PICO_SOCKET_STATE_CLOSED;
        /* call EV_ERR wakeup */
        pico_err = PICO_ERR_ECONNRESET;
        if ((t->sock).wakeup)
          (t->sock).wakeup(PICO_SOCK_EV_ERR, &(t->sock));
        tcp_dbg("TCP RST> SOCKET BACK TO LISTEN\n");
        pico_socket_del(s);
      } else {
        /* go to closed */
        (t->sock).state &= 0x00FFU;
        (t->sock).state |= PICO_SOCKET_STATE_TCP_CLOSED;
        (t->sock).state &= 0xFF00U;
        (t->sock).state |= PICO_SOCKET_STATE_CLOSED;

        /* call EV_FIN wakeup before deleting */
        if ((t->sock).wakeup)
          (t->sock).wakeup(PICO_SOCK_EV_FIN, &(t->sock));
        /* call EV_ERR wakeup before deleting */
        pico_err = PICO_ERR_ECONNRESET;
        if ((t->sock).wakeup)
          (t->sock).wakeup(PICO_SOCK_EV_ERR, &(t->sock));

        /* delete socket */
        pico_socket_del(&t->sock);
      }
    } else {                      /* not valid, ignore */
      tcp_dbg("TCP RST> IGNORE\n");
      return 0;
    }
  }

  return 0;
}

struct tcp_action_entry {
  uint16_t tcpstate;
  int (*syn)(struct pico_socket *s, struct pico_frame *f);
  int (*synack)(struct pico_socket *s, struct pico_frame *f);
  int (*ack)(struct pico_socket *s, struct pico_frame *f);
  int (*data)(struct pico_socket *s, struct pico_frame *f);
  int (*fin)(struct pico_socket *s, struct pico_frame *f);
  int (*finack)(struct pico_socket *s, struct pico_frame *f);
  int (*rst)(struct pico_socket *s, struct pico_frame *f);
};

static struct tcp_action_entry tcp_fsm[] = {
    /* State                            syn              synack             ack                data             fin              finack           rst*/
  { PICO_SOCKET_STATE_TCP_UNDEF,        NULL,            NULL,              NULL,              NULL,            NULL,            NULL,            NULL     },
  { PICO_SOCKET_STATE_TCP_CLOSED,       NULL,            NULL,              NULL,              NULL,            NULL,            NULL,            NULL     },
  { PICO_SOCKET_STATE_TCP_LISTEN,       &tcp_syn,        &tcp_nosync_rst,   &tcp_nosync_rst,   &tcp_nosync_rst, &tcp_nosync_rst, &tcp_nosync_rst, NULL     },
  { PICO_SOCKET_STATE_TCP_SYN_SENT,     &tcp_nosync_rst, &tcp_synack,       &tcp_nosync_rst,   &tcp_nosync_rst, &tcp_nosync_rst, &tcp_nosync_rst, &tcp_rst },
  { PICO_SOCKET_STATE_TCP_SYN_RECV,     NULL,            &tcp_nosync_rst,   &tcp_first_ack,    &tcp_nosync_rst, &tcp_nosync_rst, &tcp_nosync_rst, &tcp_rst },
  { PICO_SOCKET_STATE_TCP_ESTABLISHED,  NULL,            &tcp_ack,          &tcp_ack,          &tcp_data_in,    &tcp_closewait,  &tcp_closewait,  &tcp_rst },
  { PICO_SOCKET_STATE_TCP_CLOSE_WAIT,   NULL,            &tcp_ack,          &tcp_ack,          &tcp_send_rst,   &tcp_closewait,  &tcp_closewait,  &tcp_rst },
  { PICO_SOCKET_STATE_TCP_LAST_ACK,     NULL,            &tcp_ack,          &tcp_lastackwait,  &tcp_send_rst,   &tcp_send_rst,   &tcp_send_rst,   &tcp_rst },
  { PICO_SOCKET_STATE_TCP_FIN_WAIT1,    NULL,            &tcp_ack,          &tcp_finwaitack,   &tcp_data_in,    &tcp_rcvfin,     &tcp_finack,     &tcp_rst },
  { PICO_SOCKET_STATE_TCP_FIN_WAIT2,    NULL,            &tcp_ack,          &tcp_ack,          &tcp_data_in,    &tcp_finwaitfin, &tcp_finack,     &tcp_rst },
  { PICO_SOCKET_STATE_TCP_CLOSING,      NULL,            &tcp_ack,          &tcp_closewaitack, &tcp_send_rst,   &tcp_send_rst,   &tcp_send_rst,   &tcp_rst },
  { PICO_SOCKET_STATE_TCP_TIME_WAIT,    NULL,            &tcp_ack,          &tcp_send_rst,     &tcp_send_rst,   &tcp_send_rst,   &tcp_send_rst,   &tcp_rst }
};

/*
   NOTE: in SYN-RECV receiving syn when cloned by default (see yellow pos-it), should send reset.
*/

int pico_tcp_input(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) (f->transport_hdr);
  int ret = 0;
  uint8_t flags = hdr->flags;
  struct tcp_action_entry *action = &tcp_fsm[s->state >> 8];

  f->payload = (f->transport_hdr + ((hdr->len & 0xf0) >> 2));
  f->payload_len = f->transport_len - ((hdr->len & 0xf0) >> 2);

  tcp_dbg("[%lu] TCP> [tcp input] socket: %p state: %d <-- local port:%d remote port: %d seq: %08x ack: %08x flags: %02x = t_len: %d, hdr: %u payload: %d\n", pico_tick,
      s, s->state >> 8, short_be(hdr->trans.dport), short_be(hdr->trans.sport), SEQN(f), ACKN(f), hdr->flags, f->transport_len, (hdr->len & 0xf0) >> 2, f->payload_len );

  /* This copy of the frame has the current socket as owner */
  f->sock = s;

  /* Those are not supported at this time. */
  flags &= ~(PICO_TCP_CWR | PICO_TCP_URG | PICO_TCP_ECN);
  if (flags == PICO_TCP_SYN) {
    if (action->syn)
      action->syn(s,f);
  } else if (flags == (PICO_TCP_SYN | PICO_TCP_ACK)) {
    if (action->synack)
      action->synack(s,f);
  } else {
    if ((flags == PICO_TCP_ACK) || (flags == (PICO_TCP_ACK | PICO_TCP_PSH))) {
      if (action->ack) {
        action->ack(s,f);
      }
    }
    if (f->payload_len > 0) {
      ret = f->payload_len;
      if (action->data)
        action->data(s,f);
    }
    if (flags == PICO_TCP_FIN) {
      if (action->fin)
        action->fin(s,f);
    }
    if ((flags == (PICO_TCP_FIN | PICO_TCP_ACK)) || (flags == (PICO_TCP_FIN | PICO_TCP_ACK | PICO_TCP_PSH))) {
      if (action->finack)
        action->finack(s,f);
    }
    if (flags & PICO_TCP_RST) {
      if (action->rst)
        action->rst(s,f);
    }
  }

//discard:
  pico_frame_discard(f);
  return ret;
}

static void tcp_send_keepalive(unsigned long when, void *_t)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)_t;
  tcp_dbg("\n\nSending keepalive (%d), [State = %d]...\n", t->backoff,t->sock.state );
  if( t->sock.net && ((t->sock.state & 0xFF00) == PICO_SOCKET_STATE_TCP_ESTABLISHED) )
  {
        tcp_send_ack(t);

        if (t->keepalive_timer_running > 0) {
            t->keepalive_timer_running--;
        }

        if (t->keepalive_timer_running == 0) {
            t->keepalive_timer_running++;
            tcp_dbg("[Self] Adding timer(retransmit keepalive)\n");
            pico_timer_add(t->rto << (++t->backoff), tcp_send_keepalive, t);
        }
  }
}

int pico_tcp_output(struct pico_socket *s, int loop_score)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  struct pico_frame *f, *una;
  struct pico_tcp_hdr *hdr;
  int sent = 0;

  una = first_segment(&t->tcpq_out);

  f = peek_segment(&t->tcpq_out, t->snd_nxt);
  while((f) && (t->cwnd >= t->in_flight)) {
    hdr = (struct pico_tcp_hdr *)f->transport_hdr;
    f->timestamp = pico_tick;
    tcp_add_options(t, f, hdr->flags, tcp_options_size(t, hdr->flags));
    if (seq_compare(SEQN(f) + f->payload_len, SEQN(una) + (t->recv_wnd << t->recv_wnd_scale)) > 0) {
      t->cwnd = t->in_flight;
      if (t->cwnd < 1)
        t->cwnd = 1;
      if (t->x_mode != PICO_TCP_WINDOW_FULL) {
        tcp_dbg("TCP> RIGHT SIZING (rwnd: %d, frame len: %d\n",t->recv_wnd << t->recv_wnd_scale, f->payload_len);
        tcp_dbg("In window full...\n");
        t->snd_nxt = SEQN(una);   /* XXX prevent out-of-order-packets ! */ /*DLA re-enabled.*/
        t->snd_retry = SEQN(una);   /* XXX replace by retry pointer? */

        /* Alternative to the line above:  (better performance, but seems to lock anyway with larger buffers)
        if (seq_compare(t->snd_nxt, SEQN(una)) > 0)
          t->snd_nxt -= f->payload_len;
        */

        t->x_mode = PICO_TCP_WINDOW_FULL;
        if (t->keepalive_timer_running == 0) {
          tcp_dbg("[Window full] Adding timer(send keepalive)\n");
          tcp_send_keepalive(0, t);
        }
      }
      break;
    }
    tcp_dbg("TCP> DEQUEUED (for output) frame %08x, acks %08x len= %d, remaining frames %d\n", SEQN(f), ACKN(f), f->payload_len,t->tcpq_out.frames);
    tcp_send(t, f);
    sent++;
    loop_score--;
    t->snd_last_out = SEQN(f);
    if (loop_score < 1)
      break;
    if (f->payload_len > 0) {
      f = next_segment(&t->tcpq_out, f);
    } else {
      f = NULL;
    }
  }
  if (sent > 0) {
    if (t->rto < PICO_TCP_RTO_MIN)
      t->rto = PICO_TCP_RTO_MIN;
    //if (s->wakeup)
    //  t->sock.wakeup(PICO_SOCK_EV_WR, &t->sock);
    add_retransmission_timer(t, pico_tick + t->rto);
  } else {
    /* Nothing to transmit. */
  }

  if ((t->tcpq_out.frames == 0) && (s->state & PICO_SOCKET_STATE_SHUT_LOCAL)) {    /* if no more packets in queue, XXX replacled !f by tcpq check */
    if ((s->state & PICO_SOCKET_STATE_TCP) == PICO_SOCKET_STATE_TCP_ESTABLISHED) {
      tcp_dbg("TCP> buffer empty, shutdown established ...\n");
      /* send fin if queue empty and in state shut local (write) */
      tcp_send_fin(t);
      /* change tcp state to FIN_WAIT1 */
      s->state &= 0x00FFU;
      s->state |= PICO_SOCKET_STATE_TCP_FIN_WAIT1;
    } else if ((s->state & PICO_SOCKET_STATE_TCP) == PICO_SOCKET_STATE_TCP_CLOSE_WAIT) {
      /* send fin if queue empty and in state shut local (write) */
      tcp_send_fin(t);
      /* change tcp state to LAST_ACK */
      s->state &= 0x00FFU;
      s->state |= PICO_SOCKET_STATE_TCP_LAST_ACK;
      tcp_dbg("TCP> STATE: LAST_ACK.\n");
    }
  }
  return loop_score;
}

/* function to make new segment from hold queue with specific size (mss) */
static struct pico_frame * pico_hold_segment_make(struct pico_socket_tcp *t)
{
  struct pico_frame *f_temp,*f_new;
  struct pico_socket *s = (struct pico_socket *) &t->sock;
  struct pico_tcp_hdr *hdr;
  int total_len = 0, total_payload_len = 0;
  int off = 0, test = 0;

  off = pico_tcp_overhead(s);

  /* init with first frame in hold queue */
  f_temp = first_segment(&t->tcpq_hold);
  total_len = f_temp->payload_len;
  f_temp = next_segment(&t->tcpq_hold, f_temp);

  /* check till total_len <= MSS */
  while ((f_temp != NULL) && ((total_len+f_temp->payload_len) <= PICO_TCP_DEFAULT_MSS)) {
    total_len += f_temp->payload_len;
    f_temp = next_segment(&t->tcpq_hold, f_temp);
    if (f_temp == NULL)
      break;
  }
  /* alloc new frame with payload size = off + total_len */
  f_new = pico_socket_frame_alloc(s, off + total_len);
  if (!f_new) {
    pico_err = PICO_ERR_ENOMEM;
    return f_new;
  }

  hdr = (struct pico_tcp_hdr *) f_new->transport_hdr;
  /* init new frame */
  f_new->payload += off;
  f_new->payload_len -= off;
  f_new->sock = s;

  f_temp = first_segment(&t->tcpq_hold);
  hdr->seq = ((struct pico_tcp_hdr *)(f_temp->transport_hdr))->seq;  /* get sequence number of first frame */
  hdr->trans.sport = t->sock.local_port;
  hdr->trans.dport = t->sock.remote_port;

  /* check till total_payload_len <= MSS */
  while ((f_temp != NULL) && ((total_payload_len + f_temp->payload_len) <= PICO_TCP_DEFAULT_MSS)) {
    /* cpy data and discard frame */
    test++;
    memcpy(f_new->payload + total_payload_len, f_temp->payload, f_temp->payload_len);
    total_payload_len += f_temp->payload_len;
    pico_discard_segment(&t->tcpq_hold, f_temp);
    f_temp = first_segment(&t->tcpq_hold);
  }

  hdr->len = (f_new->payload - f_new->transport_hdr) << 2 | t->jumbo;

  tcp_dbg_nagle("NAGLE make - joined %d segments, len %d bytes\n",test,total_payload_len);

  return f_new;
}

/* original behavior kept when Nagle disabled;
   Nagle algorithm added here, keeping hold frame queue instead of eg linked list of data */
int pico_tcp_push(struct pico_protocol *self, struct pico_frame *f)
{
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *)f->transport_hdr;
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) f->sock;
  struct pico_frame *f_new;
  int total_len = 0;

  hdr->trans.sport = t->sock.local_port;
  hdr->trans.dport = t->sock.remote_port;
  hdr->seq = long_be(t->snd_last + 1);
  hdr->len = (f->payload - f->transport_hdr) << 2 | t->jumbo;

  if (f->payload_len > (t->tcpq_out.max_size - t->tcpq_out.size))
    t->sock.ev_pending &= (~PICO_SOCK_EV_WR);

  /***************************************************************************/

  if (!IS_NAGLE_ENABLED((&(t->sock)))) {
    /* TCP_NODELAY enabled, original behavior */
    if (pico_enqueue_segment(&t->tcpq_out,f) > 0) {
      tcp_dbg_nagle("TCP_PUSH - NO NAGLE - Pushing segment %08x, len %08x to socket %p\n", t->snd_last + 1, f->payload_len, t);
      t->snd_last += f->payload_len;
      return f->payload_len;
    } else {
      tcp_dbg("Enqueue failed.\n");
      return 0;
    }
  }
  /***************************************************************************/
  else {
    /* Nagle's algorithm enabled, check if ready to send, or put frame in hold queue */
    if (IS_TCP_IDLE(t) && IS_TCP_HOLDQ_EMPTY(t)) {  /* opt 1. send frame */
      if (pico_enqueue_segment(&t->tcpq_out,f) > 0) {
        tcp_dbg_nagle("TCP_PUSH - NAGLE - Pushing segment %08x, len %08x to socket %p\n", t->snd_last + 1, f->payload_len, t);
        t->snd_last += f->payload_len;
        return f->payload_len;
      } else {
        tcp_dbg("Enqueue failed.\n");
        return 0;
      }
    } else {                                        /* opt 2. hold data back */
      total_len = f->payload_len + t->tcpq_hold.size;
      if ((total_len >= PICO_TCP_DEFAULT_MSS) && ((t->tcpq_out.max_size - t->tcpq_out.size) >= PICO_TCP_DEFAULT_MSS)) {/* TODO check mss socket */
        /* IF enough data in hold (>mss) AND space in out queue (>mss) */
        /* add current frame in hold and make new segment */
        if (pico_enqueue_segment(&t->tcpq_hold,f) > 0 ) {
          tcp_dbg_nagle("TCP_PUSH - NAGLE - Pushed into hold, make new (enqueued frames out %d)\n",t->tcpq_out.frames);
          t->snd_last += f->payload_len;    /* XXX  WATCH OUT */
          f_new = pico_hold_segment_make(t);
        } else {
          tcp_dbg_nagle("TCP_PUSH - NAGLE - enqueue hold failed 1\n");
          return 0;
        }
        /* and put new frame in out queue */
        if ((f_new != NULL) && (pico_enqueue_segment(&t->tcpq_out,f_new) > 0)) {
          return f_new->payload_len;
        } else {
          tcp_dbg_nagle("TCP_PUSH - NAGLE - enqueue out failed, f_new = %p\n",f_new);
          return -1;                        /* XXX something seriously wrong */
        }
      } else {
        /* ELSE put frame in hold queue */
        if (pico_enqueue_segment(&t->tcpq_hold,f) > 0) {
          tcp_dbg_nagle("TCP_PUSH - NAGLE - Pushed into hold (enqueued frames out %d)\n",t->tcpq_out.frames);
          t->snd_last += f->payload_len;    /* XXX  WATCH OUT */
          return f->payload_len;
        } else {
          tcp_dbg_nagle("TCP_PUSH - NAGLE - enqueue hold failed 2\n");
          return 0;
        }
      }
    }
  }
  /***************************************************************************/
}
#endif //PICO_SUPPORT_TCP
