// XD: change this file name to bench_client.cc ?

#include <gflags/gflags.h>

#include <vector>
#include <sys/queue.h>

#include "../core/lib.hh"
#include "../tests/random.hh"
#include "./reporter.hh"
#include "./thread.hh"
#include "./bench_op.hh"
#include "./bench.hh"

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

  std::vector<Thread_t *> workers;
  std::vector<Statics> worker_statics(FLAGS_threads);

  for (uint i = 0; i < FLAGS_threads; ++i) {
    workers.push_back(
        new Thread_t(std::bind(worker_fn, i, &(worker_statics[i]))));
  }

  // start the workers
  for (auto w : workers) {
    w->start();
  }

  //Reporter::report_thpt(worker_statics, 10);  // report for 10 seconds
  Reporter::report_bandwidth(worker_statics, TEST_TIME_SEC, FLAGS_payload);  // report for 10 seconds
  running = false;                            // stop workers

  // wait for workers to join
  for (auto w : workers) {
    w->join();
  }

  RDMA_LOG(4) << "done";
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
    auto qp_res = cm.cc_rc(FLAGS_client_name + " thread-qp" + std::to_string(worker_id + i), qps[i],
                          REMOTE_NIC(worker_id), QPConfig());
    RDMA_ASSERT(qp_res == IOCode::Ok) << std::get<0>(qp_res.desc);

    auto key = std::get<1>(qp_res.desc);
    RDMA_LOG(4) << "t-" << worker_id << " fetch QP authentical key: " << key;

    auto fetch_res = cm.fetch_remote_mr(REMOTE_QUEUE_IDX(REMOTE_NIC(worker_id), worker_id));
    RDMA_ASSERT(fetch_res == IOCode::Ok) << std::get<0>(fetch_res.desc);
    rmem::RegAttr remote_attr = std::get<1>(fetch_res.desc);

    qps[i]->bind_remote_mr(remote_attr);
    qps[i]->bind_local_mr(local_mrs[i]->get_reg_attr().value());

    RDMA_LOG(4) << "t-" << worker_id << " started";
    u64 *test_buf = (u64 *)(qps[i]->local_mr.value().buf);
    *test_buf = 0;
    u64 *remote_buf = (u64 *)remote_attr.buf;
  
    ops[i].init_lbuf(test_buf, FLAGS_payload, qps[i]->local_mr.value().key, REQUEST_SIZE * QUEUE_DEPTH);
    ops[i].init_rbuf(remote_buf, remote_attr.key, REQUEST_SIZE * QUEUE_DEPTH);
    RDMA_ASSERT(ops[i].valid());
  }

#ifndef PIPELINE
  struct ibv_sge sges[DOORBELL_BATCHING];
  struct ibv_send_wr wrs[DOORBELL_BATCHING];

  for(int i = 0; i < DOORBELL_BATCHING; i++){
    memset(&sges[i], 0 , sizeof(struct ibv_sge));
    memset(&wrs[i], 0 , sizeof(struct ibv_send_wr));

    sges[i].length = op.length;
    sges[i].lkey = op.lkey;

    wrs[i].opcode = op.wr.opcode;
    wrs[i].num_sge = 1;
    wrs[i].sg_list = &sges[i];
    wrs[i].wr.rdma.rkey = op.rkey;
  }

  struct ibv_send_wr *bad_wr;

  int on_fly = 0;

  while (running) {
    for(int i = 0; i < DOORBELL_BATCHING; i++){
      compile_fence();
      op.refresh();

      sges[i].addr = op.lbuf_base + op.current_loff;
      wrs[i].wr.rdma.remote_addr = op.rbuf_base + op.current_roff;
      wrs[i].send_flags = 0;

      if((i != DOORBELL_BATCHING - 1) && (on_fly + 1 != QUEUE_DEPTH)){
        wrs[i].next = &wrs[i + 1];
      } else {
        wrs[i].next = nullptr;
        wrs[i].send_flags = IBV_SEND_SIGNALED;
      }
      
      on_fly++;
    }
    // ring doorbell
    auto res_s = ibv_post_send(qp->qp, &wrs[0], &bad_wr);
    RDMA_ASSERT(res_s == IOCode::Ok) << res_s;
    
    if(on_fly == QUEUE_DEPTH){
      while(on_fly != 0){
        auto res_p = qp->wait_one_comp();
        RDMA_ASSERT(res_p == IOCode::Ok) << res_p.desc.status;
        for(int i = 0; i < DOORBELL_BATCHING; i++)
          ss.increment();  // finish one request
        on_fly-=DOORBELL_BATCHING;
      }
    }
  }
#else
  struct ibv_sge sges[QP_NUM][DOORBELL_BATCHING];
  struct ibv_send_wr wrs[QP_NUM][DOORBELL_BATCHING];

  for(int j = 0; j < QP_NUM; j++){
    for(int i = 0; i < DOORBELL_BATCHING; i++){
      memset(&sges[j][i], 0 , sizeof(struct ibv_sge));
      memset(&wrs[j][i], 0 , sizeof(struct ibv_send_wr));

      sges[j][i].length = ops[j].length;
      sges[j][i].lkey = ops[j].lkey;

      wrs[j][i].opcode = ops[j].wr.opcode;
      wrs[j][i].num_sge = 1;
      wrs[j][i].sg_list = &sges[j][i];
      wrs[j][i].wr.rdma.rkey = ops[j].rkey;
    }
  }

  struct ibv_send_wr *bad_wr[QP_NUM];

  int on_fly[QP_NUM];
  for(int i = 0; i < QP_NUM; i++){
    on_fly[i] = 0;
  }

  while (running) {
    for(int j = 0; j < QP_NUM; j++){
      while(on_fly[j] != QUEUE_DEPTH){
        for(int i = 0; i < DOORBELL_BATCHING; i++){
          ops[j].refresh();

          sges[j][i].addr = ops[j].lbuf_base + ops[j].current_loff;
          wrs[j][i].wr.rdma.remote_addr = ops[j].rbuf_base + ops[j].current_roff;
          wrs[j][i].send_flags = 0;

          if((i != DOORBELL_BATCHING - 1) && (on_fly[j] + 1 != QUEUE_DEPTH)){
            wrs[j][i].next = &wrs[j][i + 1];
          } else {
            wrs[j][i].next = nullptr;
            wrs[j][i].send_flags = IBV_SEND_SIGNALED;
          }
          
          on_fly[j]++;
        }

        // ring doorbell
        auto res_s = ibv_post_send(qps[j]->qp, &wrs[j][0], &bad_wr[j]);
        RDMA_ASSERT(res_s == IOCode::Ok) << res_s;
      }
    }
    
    for(int j = 0; j < QP_NUM; j++){
      if(on_fly[j] == QUEUE_DEPTH){
        auto res_p = qps[j]->wait_one_comp();
        RDMA_ASSERT(res_p == IOCode::Ok) << res_p.desc.status;
        for(int i = 0; i < DOORBELL_BATCHING; i++)
          ss.increment();  // finish one request
        on_fly[j]-=DOORBELL_BATCHING;
      }
    }
  }
#endif
  RDMA_LOG(4) << "t-" << worker_id << " stoped";
  // cm.delete_remote_rc(FLAGS_client_name + " thread-qp" + std::to_string(worker_id), key);
  return 0;
}

