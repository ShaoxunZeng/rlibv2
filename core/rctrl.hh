#pragma once

#include "./rmem/handler.hh"
#include "qps/mod.hh"

#include "./bootstrap/srpc.hh"
#include "../core/nordma.hh"
#include "qps/wrappers.hh"

#include <atomic>

#include <pthread.h>

namespace rdmaio {

/*!
  RCtrl is a control path daemon, that handles all RDMA bootstrap to this
  machine. RCtrl is **thread-safe**.
 */
class RCtrl {

  std::atomic<bool> running;

  pthread_t handler_tid;

  /*!
    The two factory which allow user to **register** the QP, MR so that others
    can establish communication with them.
   */
public:
  rmem::MRFactory registered_mrs;
  qp::QPFactory registered_qps;
  qp::DCFactory registered_dcs;
  Factory<nic_id_t, RNic> opened_nics;

  Factory<nic_id_t, qp::ibv_cq_wrapper> recv_cqs; // one nic one cq among all qps
  Factory<nic_id_t, qp::ibv_srq_wrapper> registered_srqs; // one nic one srq among all qps
  Factory<nic_id_t, nordma::nordma_resources> nordma_resources; // one nic one nordma_resource among all qps

  bootstrap::SRpcHandler rpc;

public:
  explicit RCtrl(const usize &port, const std::string &h = "localhost")
      : running(false), rpc(port, h) {
    RDMA_ASSERT(rpc.register_handler(
        proto::FetchMr,
        std::bind(&RCtrl::fetch_mr_handler, this, std::placeholders::_1)));

    RDMA_ASSERT(rpc.register_handler(
        proto::CreateRC,
        std::bind(&RCtrl::rc_handler, this, std::placeholders::_1)));

    RDMA_ASSERT(rpc.register_handler(
        proto::DeleteRC,
        std::bind(&RCtrl::delete_rc, this, std::placeholders::_1)));

    RDMA_ASSERT(rpc.register_handler(
        proto::FetchQPAttr,
        std::bind(&RCtrl::fetch_qp_attr_wrapper, this, std::placeholders::_1)));
#ifdef DCT
    RDMA_ASSERT(rpc.register_handler(
        proto::FetchDCAttr,
        std::bind(&RCtrl::fetch_dc_attr_wrapper, this, std::placeholders::_1)));
#endif
    RDMA_ASSERT(rpc.register_handler(
        proto::CreateRCRecv,
        std::bind(&RCtrl::rc_recv_handler, this, std::placeholders::_1)));
  }

  ~RCtrl() {
    this->stop_daemon();
  }

  /*!
    Start the daemon thread for handling RDMA connection requests
   */
  bool start_daemon() {
    running = true;
    asm volatile("" ::: "memory");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    return (pthread_create(&handler_tid, &attr, &RCtrl::daemon, this) == 0);
  }

  /*!
    Stop the daemon thread for handling RDMA connection requests
   */
  void stop_daemon() {
    if (running) {
      running = false;

      asm volatile("" ::: "memory");
      pthread_join(handler_tid, nullptr);
    }
  }

  static void *daemon(void *ctx) {
    RCtrl &ctrl = *((RCtrl *)ctx);
    u64 total_reqs = 0;
    while (ctrl.running) {
      total_reqs += ctrl.rpc.run_one_event_loop();
      continue;
    }
    RDMA_LOG(INFO) << "stop with :" << total_reqs << " processed.";
    return nullptr; // nothing should return
  }

  void create_and_reg_nordma_resources(nic_id_t nic_id, struct nordma::nordma_resource_opts *opts) {
    nordma::nordma_resources *nordma_resource = new nordma::nordma_resources(opts);

    nordma_resources.reg(nic_id, Arc<nordma::nordma_resources>(nordma_resource));
  }

  // handlers of the dameon call
private:
  ByteBuffer fetch_mr_handler(const ByteBuffer &b) {
    auto o_id = ::rdmaio::Marshal::dedump<proto::MRReq>(b);
    if (o_id) {
      auto req_id = o_id.value();
      auto o_mr = registered_mrs.query(req_id.id);
      if (o_mr) {
        return ::rdmaio::Marshal::dump<proto::MRReply>(
            {.status = proto::CallbackStatus::Ok,
             .attr = o_mr.value()->get_reg_attr().value()});

      } else {
        return ::rdmaio::Marshal::dump<proto::MRReply>(
            {.status = proto::CallbackStatus::NotFound});
      }
    }
    return ::rdmaio::Marshal::dump<proto::MRReply>(
        {.status = proto::CallbackStatus::WrongArg});
  }

  ByteBuffer delete_rc(const ByteBuffer &b) {
    auto rc_req_o = ::rdmaio::Marshal::dedump<proto::DelRCReq>(b);
    if (!rc_req_o)
      goto WA;
    {
      auto del_res =
          registered_qps.dereg(rc_req_o.value().name, rc_req_o.value().key);
      if (!del_res) {
        goto WA;
      }
      if (del_res.value() == nullptr)
        return ::rdmaio::Marshal::dump<proto::RCReply>(
            {.status = proto::CallbackStatus::AuthErr});
      return ::rdmaio::Marshal::dump<proto::RCReply>(
          {.status = proto::CallbackStatus::Ok});
    }
  WA:
    return ::rdmaio::Marshal::dump<proto::RCReply>(
        {.status = proto::CallbackStatus::WrongArg});
  }

  ByteBuffer fetch_qp_attr_wrapper(const ByteBuffer &b) {
    auto req_o = ::rdmaio::Marshal::dedump<proto::QPReq>(b);
    if (!req_o)
      return ::rdmaio::Marshal::dump<proto::RCReply>(
          {.status = proto::CallbackStatus::WrongArg});
    proto::RCReq req = {};
    memcpy(req.name, req_o.value().name, ::rdmaio::qp::kMaxQPNameLen + 1);
    return fetch_qp_attr(req, 0);
  }
#ifdef DCT
  ByteBuffer fetch_dc_attr_wrapper(const ByteBuffer &b) {
    auto req_o = ::rdmaio::Marshal::dedump<proto::QPReq>(b);
    if (!req_o)
      return ::rdmaio::Marshal::dump<proto::DCReply>(
          {.status = proto::CallbackStatus::WrongArg});
    proto::RCReq req = {};
    memcpy(req.name, req_o.value().name, ::rdmaio::qp::kMaxQPNameLen + 1);
    return fetch_dc_attr(req, 0);
  }
#endif

  /*!
    Given a RCReq, query its attribute from the QPs
    \ret: Marshalling RCReply to a Bytebuffer
   */
public:
  ByteBuffer fetch_qp_attr(const proto::RCReq &req, const u64 &key) {
    auto rc = registered_qps.query(req.name);
    if (rc) {
      return ::rdmaio::Marshal::dump<proto::RCReply>(
          {.status = proto::CallbackStatus::Ok,
           .attr = rc.value()->my_attr(),
           .key = key});
    }
    return ::rdmaio::Marshal::dump<proto::RCReply>(
        {.status = proto::CallbackStatus::NotFound});
  }

#ifdef DCT
  ByteBuffer fetch_dc_attr(const proto::RCReq &req, const u64 &key) {
    auto dc = registered_dcs.query(req.name);
    if (dc) {
      return ::rdmaio::Marshal::dump<proto::DCReply>(
          {.status = proto::CallbackStatus::Ok,
           .attr = dc.value()->remote_dct_attr,
           .key = key});
    }
    return ::rdmaio::Marshal::dump<proto::DCReply>(
        {.status = proto::CallbackStatus::NotFound});
  }
#endif

private:
  /*!
    Handling the RC request
    The process has two steps:
    1. check whether user wants to create a QP
    2. if so, create it using the provided parameters
    3. query the RC attribute and returns to the user
   */
  ByteBuffer rc_handler(const ByteBuffer &b) {

    auto rc_req_o = ::rdmaio::Marshal::dedump<proto::RCReq>(b);
    if (!rc_req_o)
      goto WA;
    {
      auto rc_req = rc_req_o.value();

      // 1. sanity check the request
      if (!(rc_req.whether_create == static_cast<u8>(1) ||
            rc_req.whether_create != static_cast<u8>(0)))
        goto WA;

      // 1. check whether we need to create the QP
      u64 key = 0;
      if (rc_req.whether_create == 1) {
        // 1.0 find the Nic to create this QP
        auto nic = opened_nics.query(rc_req.nic_id);
        if (!nic)
          goto WA; // failed to find Nic

        // 1.0 check whether we are able to use the registered recv_cq
        ibv_cq *recv_cq = nullptr;
#if 0 // we move this to a separte class
        if (rc_req.whether_recv == 1) {
          recv_cq = rc_recv_cqs.query_or_default(rc_req.name_recv,nullptr).get();
        }
#endif

        // 1.1 try to create and register this QP
        auto rc = qp::RC::create(nic.value(), rc_req.config, recv_cq).value();
        auto rc_status = registered_qps.reg(rc_req.name, rc);

        if (!rc_status) {
          // clean up
          goto WA;
        }

        // 1.2 finally we connect the QP
        if (rc->connect(rc_req.attr) != IOCode::Ok) {
          // in connect error
          registered_qps.dereg(rc_req.name, rc_status.value());
          goto WA;
        }
        key = rc_status.value();
      }

      // 2. fetch the QP result
      return fetch_qp_attr(rc_req, key);
    }
    // Error handling cases:
  WA: // wrong arg
    return ::rdmaio::Marshal::dump<proto::RCReply>(
        {.status = proto::CallbackStatus::WrongArg});
  Err:
    return ::rdmaio::Marshal::dump<proto::RCReply>(
        {.status = proto::CallbackStatus::ConnectErr});
  }


  /*!
    The handler for creating a QP which is ready for recv.
    This handler should register with RCtrl (defined in ../rctrl.hh).
    \note: the implementation is similar to the RCtrl's rc_handler,
    but additionally allocate a RecvEntries<R> for the QP.
  */
   ByteBuffer rc_recv_handler(const ByteBuffer &b) {

    auto rc_req_o = ::rdmaio::Marshal::dedump<proto::RCReq>(b);
    if (!rc_req_o)
      goto WA;
    {
      auto rc_req = rc_req_o.value();

      // 1. sanity check the request
      if (!(rc_req.whether_create == static_cast<u8>(1) ||
            rc_req.whether_create != static_cast<u8>(0)))
        goto WA;

      // 1. check whether we need to create the QP
      u64 key = 0;
      if (rc_req.whether_create == 1) {
        // 1.0 find the Nic to create this QP
        auto nic = opened_nics.query(rc_req.nic_id);
        if (!nic)
          goto WA; // failed to find Nic

        // 1.1 check whether we are able to use the registered cq
        ibv_cq *cq = nullptr;
        if (rc_req.whether_cq == 1) {
          auto recv_c_res = recv_cqs.query(rc_req.nic_id);
          if (!recv_c_res)
            goto CQNotFound;
          else
            cq = recv_c_res.value().get()->cq;
        } else {
          goto WA;
        }

        ibv_srq *srq = nullptr;
        if (rc_req.whether_srq == 1) {
          auto srq_c_res = registered_srqs.query(rc_req.nic_id);
          if (!srq_c_res)
            goto SRQNotFound;
          else
            srq = srq_c_res.value().get()->srq;
        } else {
          goto WA;
        }

        // 1.2 try to create and register this QP
        auto rc = qp::RC::create(nic.value(), rc_req.config, cq, true, srq).value();
        auto rc_status = registered_qps.reg(rc_req.name, rc);

        if (!rc_status) {
          // clean up
          goto WA;
        }

        // 1.3 finally we connect the QP
        if (rc->connect(rc_req.attr) != IOCode::Ok) {
          // in connect error
          registered_qps.dereg(rc_req.name, rc_status.value());
          goto WA;
        }
        key = rc_status.value();

        // 1.4 this QP is done, alloc the recv entries would be done in the init srq stage
      }

      // 2. fetch the QP result
      return fetch_qp_attr(rc_req, key);
    }
    // Error handling cases:
    WA: // wrong arg
        return ::rdmaio::Marshal::dump<proto::RCReply>(
            {.status = proto::CallbackStatus::WrongArg});
    Err:
        return ::rdmaio::Marshal::dump<proto::RCReply>(
            {.status = proto::CallbackStatus::ConnectErr});
    CQNotFound:
        return ::rdmaio::Marshal::dump<proto::RCReply>(
            {.status = proto::CallbackStatus::CQNotFound});
    SRQNotFound:
        return ::rdmaio::Marshal::dump<proto::RCReply>(
            {.status = proto::CallbackStatus::SRQNotFound});
  }
};

} // namespace rdmaio
