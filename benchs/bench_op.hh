#pragma once

 #include <sys/queue.h>

#include "../core/qps/op.hh"
#include "../tests/random.hh"

namespace rdmaio {

using namespace rdmaio;
using namespace rdmaio::qp;

template <usize NSGE = 1>
struct BenchOp : Op<NSGE> {
  op_type type;
  u64 lbuf_base;
  u64 rbuf_base;

  u64 rbuf_mod;
  u64 lbuf_mod;

  test::FastRandom rand;

  bool is_random;
  u64 current_roff; // for sequantial access
  u64 current_loff;

  u32 length;
  u32 lkey;
  u32 rkey;

 public:
  BenchOp<NSGE>(int type = 0, bool random = false) : Op<NSGE>() {
    lbuf_base = 0;
    rbuf_base = 0;
    rand = test::FastRandom();
    this->set_type(type);
    is_random = random;
  }

  inline auto valid() -> bool {
    return (lbuf_base != 0) && (rbuf_base != 0);
  }

  inline BenchOp &set_type(int type) {
    this->type = (op_type)type;
    switch (this->type) {
      case RDMA_READ:
        this->set_read();
        break;

      case RDMA_WRITE:
        this->set_write();
        break;

      case ATOMIC_CAS:
        this->set_cas(0, 0);
        break;
      case ATOMIC_FAA:
        this->set_fetch_add(0);
        break;

      default:
        break;
    }
    return *this;
  }

  inline BenchOp &init_rbuf(u64 *ra, const u32 &rk, u64 mod = 10) {
    this->rbuf_base = (u64)ra;
    this->rbuf_mod = mod;
    this->current_roff = 0;
    this->rkey = rk;
    switch (type) {
      case RDMA_READ:
      case RDMA_WRITE:
        this->set_rdma_rbuf(ra, rk);
        break;

      case ATOMIC_CAS:
      case ATOMIC_FAA:
        this->set_atomic_rbuf(ra, rk);
        break;

      default:
        break;
    }
    return *this;
  }

  inline BenchOp &init_lbuf(u64 *la, const u32 &length, const u32 &lk,
                            u64 mod = 10) {
    this->lbuf_base = (u64)la;
    this->lbuf_mod = mod;
    this->current_loff = 0;
    this->length = length;
    this->set_payload(la, length, lk);
    this->lkey = lk;
    return *this;
  }

  inline auto refresh(){
    if(!valid()) return;
    u64 ra;
    u64 la;
    if(this->is_random){
      ra = rbuf_base + (this->rand.next() % rbuf_mod);
      la = lbuf_base + (this->rand.next() % lbuf_mod);
    } else {
      this->current_roff = (this->current_roff + this->length) % rbuf_mod;
      this->current_loff = (this->current_loff + this->length) % lbuf_mod;
      ra = this->rbuf_base + this->current_roff;
      la = this->lbuf_base + this->current_loff;
    }
    switch (type) {
      case RDMA_READ:
        this->wr.wr.rdma.remote_addr = ra;
        break;
      case RDMA_WRITE:
        this->wr.wr.rdma.remote_addr = ra;
        this->sges[0].addr = la;
        break;
      case ATOMIC_CAS:
        this->wr.wr.atomic.remote_addr = ra;
        this->sges[0].addr = la;
        this->set_cas(this->rand.next() % 100, this->rand.next() % 100);
        break;
      case ATOMIC_FAA:
        this->wr.wr.atomic.remote_addr = ra;
        this->set_fetch_add(this->rand.next() % 100);
        this->sges[0].addr = la;
        break;
      default:
        break;
    }
  }
};  // namespace rdma

}  // namespace rdmaio
