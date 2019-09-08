#pragma once

#include "../common.hh"

namespace rdmaio {

class Dummy {
 public:
  bool valid() const
  {
    return qp_ != nullptr && cq_ != nullptr;
  }
  struct ibv_qp *qp_ = nullptr;
  struct ibv_cq *cq_ = nullptr;
  struct ibv_cq *recv_cq_ = nullptr;
};

/*!
  Below structures are make packed to allow communicating
  between servers
 */
struct __attribute__ ((packed)) QPAddress
{
  u64 subnet_prefix;
  u64 interface_id;
  u32 local_id;
};

struct __attribute__ ((packed)) QPAttr {
  qp_address_t addr;
  u64 lid;
  u64 psn;
  u64 port_id;
  u64 qpn;
  u64 qkey;
};

}

#include "rc.hh"
#include "ud.hh"
