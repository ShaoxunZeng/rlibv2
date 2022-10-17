#include <gflags/gflags.h>

#include <utility>

#include "../../core/lib.hh"
#include "../bench.hh"
#include "../../core/qps/impl.hh"
#include "../../core/nordma.hh"
#include "../../core/qps/wrappers.hh"

DEFINE_int64(port, 8888, "Server listener (UDP) port.");
DEFINE_int64(use_nic_idx, 0, "Which NIC to create QP");
DEFINE_int64(reg_nic_name, 73, "The name to register an opened NIC at rctrl.");
DEFINE_int64(reg_mem_name, 73, "The name to register an MR at rctrl.");
DEFINE_uint64(magic_num, 0xdeadbeaf, "The magic number read by the client");

using namespace rdmaio;
using namespace rdmaio::rmem;

int main(int argc, char **argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // start a controler, so that others may access it using UDP based channel
    RCtrl ctrl(FLAGS_port);
    RDMA_LOG(4) << "Pingping server listenes at localhost:" << FLAGS_port;

    // first we open the NIC
    {
      for (uint i = 0; i < RNicInfo::query_dev_names().size(); ++i) {
        auto nic =
            RNic::create(RNicInfo::query_dev_names().at(i))
                .value();

        // register the nic with name 0 to the ctrl
        RDMA_ASSERT(ctrl.opened_nics.reg(i, nic));
      }
    }

    /**
     * create srq and cq
     * srq and cq would be used in create qp later when client connect
     * all qp share the same srq and cq per nic
    */

    // create cq for all recvs
    // one nic one srq
    {
      for (uint i = 0; i < ctrl.opened_nics.reg_entries(); ++i) {
        auto nic = ctrl.opened_nics.query(i);

        auto cq_res = qp::Impl::create_cq(nic.value(), CQ_SIZE);
        if (cq_res != IOCode::Ok) {
          RDMA_LOG(4) << "Error on creating SRQ: " << std::get<1>(cq_res.desc);
          return -1;
        }
        auto cq = qp::ibv_cq_wrapper::create(std::get<0>(cq_res.desc));
        ctrl.recv_cqs.reg(i, cq.value());
      }
    }

    // create srq for all recvs
    // one nic one srq
    {
      for (uint i = 0; i < ctrl.opened_nics.reg_entries(); ++i) {
        auto nic = ctrl.opened_nics.query(i);

        auto srq_res = qp::Impl::create_srq(nic.value(), SRQ_SIZE, 1);
        if (srq_res != IOCode::Ok) {
          RDMA_LOG(4) << "Error on creating SRQ: " << std::get<1>(srq_res.desc);
          return -1;
        }
        auto srq = qp::ibv_srq_wrapper::create(std::get<0>(srq_res.desc));
        ctrl.registered_srqs.reg(i, srq.value());
      }
    }

    // create nordma_resource
    // one nic one nordma_resource
    {
      for (uint i = 0; i < ctrl.opened_nics.reg_entries(); ++i) {
        auto nic = ctrl.opened_nics.query(i);

        struct nordma::nordma_resource_opts opts;
        opts.pd = nic.value()->get_pd();
        opts.srq = ctrl.registered_srqs.query(i).value().get()->srq;

        ctrl.create_and_reg_nordma_resources(i, &opts);
      }
    }

    // start the listener thread so that client can communicate w it
    ctrl.start_daemon();

    RDMA_LOG(2) << "thpt bench server started!";
    // run for 20 sec
    // for (uint i = 0; i < 600; ++i) {
    //     // server does nothing because it is RDMA
    //     // client will read the reg_mem using RDMA
    //     sleep(1);
    // }

    ibv_cq *cq = ctrl.recv_cqs.query(0).value()->cq;
    ibv_wc wc;
    while(1){
      int num = ibv_poll_cq(cq, 1, &wc);
      if(num == 1){
        RDMA_LOG(4) << "poll one";
      } else {
        RDMA_LOG(4) << "poll nothing";
      }
      sleep(1);
    }

    RDMA_LOG(4) << "server exit!";
}
