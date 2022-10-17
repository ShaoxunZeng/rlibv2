#pragma once

#include "../nic.hh"
#include "./config.hh"

namespace rdmaio {

namespace qp {

struct ibv_cq_wrapper {
  ibv_cq *cq;

  ibv_cq_wrapper(ibv_cq *cq)
      : cq(cq) {}

  static Option<Arc<ibv_cq_wrapper>> create(ibv_cq *cq) {
    return std::make_shared<ibv_cq_wrapper>(cq);
  }
};

struct ibv_srq_wrapper {
  ibv_srq *srq;

  ibv_srq_wrapper(ibv_srq *srq)
      : srq(srq) {}

  static Option<Arc<ibv_srq_wrapper>> create(ibv_srq *srq) {
    return std::make_shared<ibv_srq_wrapper>(srq);
  }
};

}
}