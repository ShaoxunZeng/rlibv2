// XD: change this file name to bench_client.cc ?

#include <gflags/gflags.h>

#include <vector>
#include <sys/queue.h>

#include "../../core/lib.hh"
#include "../../tests/random.hh"
#include "../reporter.hh"
#include "../thread.hh"
#include "../bench_op.hh"
#include "../bench.hh"

using namespace rdmaio;  // warning: should not use it in a global space often
using namespace rdmaio::qp;
using namespace rdmaio::rmem;

using Thread_t = bench::Thread<usize>;

DEFINE_string(addr, "10.0.2.161:8888", "Server address to connect to.");
DEFINE_int64(threads, CLIENT_THREAD_NUM, "#Threads used.");
DEFINE_int64(payload, REQUEST_SIZE, "Payload of each req");
DEFINE_string(client_name, "localhost", "Unique name to identify machine.");
DEFINE_int64(op_type, 0, "RDMA_READ(0) RDMA_WRITE(1) ATOMIC_CAS(2) ATOMIC_FAA(3)");
DEFINE_bool(random, false, "Offset random.");

usize worker_fn(const usize &worker_id, Statics *s);

bool volatile running = true;

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  worker_fn(0, nullptr);

  // std::vector<Thread_t *> workers;
  // std::vector<Statics> worker_statics(FLAGS_threads);

  // for (uint i = 0; i < FLAGS_threads; ++i) {
  //   workers.push_back(
  //       new Thread_t(std::bind(worker_fn, i, &(worker_statics[i]))));
  // }

  // // start the workers
  // for (auto w : workers) {
  //   w->start();
  // }

  // //Reporter::report_thpt(worker_statics, 10);  // report for 10 seconds
  // Reporter::report_bandwidth(worker_statics, TEST_TIME_SEC, FLAGS_payload);  // report for 10 seconds
  // running = false;                            // stop workers

  // // wait for workers to join
  // for (auto w : workers) {
  //   w->join();
  // }

  // RDMA_LOG(4) << "done";
}

usize worker_fn(const usize &worker_id, Statics *s) {
  Statics &ss = *s;

 // 1. create a local QP to use
  // FIXME: hard coded the nic selection to worker_id % 2
  auto nic =
    RNic::create(RNicInfo::query_dev_names().at(LOCAL_NIC(worker_id))).value();
  
  std::shared_ptr<rdmaio::qp::RC> qps[QP_NUM];
  BenchOp<1> ops[QP_NUM];
  Arc<RMem> local_mems[QP_NUM];
  Arc<RegHandler> local_mrs[QP_NUM];

  for(int i = 0; i < QP_NUM; i++){
    qps[i] = rdmaio::qp::RC::create(nic, QPConfig()).value();
    ops[i] = BenchOp<1>(FLAGS_op_type, FLAGS_random);
    local_mems[i] = Arc<RMem>(new RMem(REQUEST_SIZE * QUEUE_DEPTH));  // 20M
    local_mrs[i] = RegHandler::create(local_mems[i], nic).value();
  }

  // 2. create the pair QP at server using CM
  ConnectManager cm(FLAGS_addr);
  if (cm.wait_ready(1000000, 2) ==
      IOCode::Timeout)  // wait 1 second for server to ready, retry 2 times
    RDMA_ASSERT(false) << "cm connect to server timeout";

  for(int i = 0; i < QP_NUM; i++){
    // FIXME: hard coded the remote nic selection to worker_id % 2
    auto qp_res = cm.cc_rc_recv(FLAGS_client_name + " thread-qp" + std::to_string(worker_id + i), qps[i],
                          REMOTE_NIC(worker_id), QPConfig());
    RDMA_ASSERT(qp_res == IOCode::Ok) << std::get<0>(qp_res.desc);

    auto key = std::get<1>(qp_res.desc);
    RDMA_LOG(4) << "t-" << worker_id << " fetch QP authentical key: " << key;

    qps[i]->bind_local_mr(local_mrs[i]->get_reg_attr().value());

    RDMA_LOG(4) << "t-" << worker_id << " started";
  }

  std::shared_ptr<rdmaio::qp::RC> qp = qps[0];
  u64 *buf = (u64 *)(qp->local_mr.value().buf);

  std::string msg = "hello world";
  memset(buf, 0, msg.size() + 1);
  memcpy(buf, msg.data(), msg.size());

  struct ibv_sge sge;
  struct ibv_send_wr wr;
  struct ibv_send_wr *bad_wr;

  memset(&sge, 0 , sizeof(struct ibv_sge));
  memset(&wr, 0 , sizeof(struct ibv_send_wr));

  sge.length = (u32) msg.size() + 1;
  sge.lkey = qp->local_mr->lkey;
  sge.addr = (uint64_t)buf;

  wr.opcode = IBV_WR_SEND;
  wr.num_sge = 1;
  wr.sg_list = &sge;
  wr.next = nullptr;
  wr.send_flags = IBV_SEND_SIGNALED;

  auto res_s = ibv_post_send(qp->qp, &wr, &bad_wr);
  RDMA_ASSERT(res_s == IOCode::Ok) << res_s;

  RDMA_ASSERT(res_s == IOCode::Ok);
  auto res_p = qp->wait_one_comp();
  RDMA_ASSERT(res_p == IOCode::Ok) << res_p.desc.status;

  return 0;
}

