#pragma once

#include "memory.hpp"
#include "qp_factory.hpp"
#include "rpc_handler.hpp"

#include <functional>
#include <pthread.h>

namespace rdmaio
{
  
class RdmaCtrl
{
  RPCFactory rpc;
  bool running_ = false;
  pthread_t handler_tid_;
  std::mutex lock;

public:
  MacID host_id_;
  RMemoryFactory mr_factory;
  QPFactory qp_factory;

public:
  RdmaCtrl() = default;

  RdmaCtrl(int tcp_port, const std::string &ip = "localhost")
      : lock()
  {
    bind(tcp_port, ip);
  }

  void bind(int tcp_port, const std::string &ip = "localhost")
  {
    { // sanity check to avoid creating multiple threads
      std::lock_guard<std::mutex> lk(this->lock);
      if (running_)
      {
        RDMA_LOG(4) << "warning, RDMA ctrl has already binded to " << ip << ":"
                    << tcp_port;
        return;
      }
      else
        running_ = true;
    }
    host_id_ = std::make_tuple(ip, tcp_port);
    RDMA_ASSERT(register_handler(REQ_MR,
                                 std::bind(&RMemoryFactory::get_mr_handler,
                                           &mr_factory,
                                           std::placeholders::_1)));
    RDMA_ASSERT(register_handler(REQ_RC,
                                 std::bind(&QPFactory::get_rc_handler,
                                           &qp_factory,
                                           std::placeholders::_1)));
    RDMA_ASSERT(register_handler(REQ_UD,
                                 std::bind(&QPFactory::get_ud_handler,
                                           &qp_factory,
                                           std::placeholders::_1)));
    // start the listener thread
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(&handler_tid_, &attr, &RdmaCtrl::listener_wrapper, this);
  }

  ~RdmaCtrl()
  {
    std::lock_guard<std::mutex> lk(this->lock);
    if (running_)
    {
      running_ = false; // wait for the handler to join
      pthread_join(handler_tid_, NULL);
      RDMA_LOG(INFO)
          << "rdma controler close: does not handle any future connections.";
    }
  }

  bool register_handler(int rid, RPCFactory::req_handler_f f)
  {
    std::lock_guard<std::mutex> lk(this->lock);
    return rpc.register_handler(rid, f);
  }

  static void *listener_wrapper(void *context)
  {
    return ((RdmaCtrl *)context)->req_handling_loop();
  }

  /**
   * The loop will receive the requests, and send reply back
   * This is not optimized, since we rarely we need the controler to
   * do performance critical jobs.
   */
  void *req_handling_loop(void)
  {
    RDMA_ASSERT(running_ = true);
    pthread_detach(pthread_self());
    auto listenfd = PreConnector::get_listen_socket(std::get<0>(host_id_),
                                                    std::get<1>(host_id_));

    int opt = 1;
    RDMA_VERIFY(
        ERROR,
        setsockopt(
            listenfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(int)) ==
            0)
        << "unable to configure socket status.";
    RDMA_VERIFY(ERROR, listen(listenfd, 24) == 0)
        << "TCP listen error: " << strerror(errno);

    while (running_)
    {

      asm volatile("" ::
                       : "memory");

      struct sockaddr_in cli_addr = {0};
      socklen_t clilen = sizeof(cli_addr);
      auto csfd = accept(listenfd, (struct sockaddr *)&cli_addr, &clilen);

      if (csfd < 0)
      {
        RDMA_LOG(ERROR) << "accept a wrong connection error: "
                        << strerror(errno);
        continue;
      }

      if (!PreConnector::wait_recv(csfd, default_timeout))
      {
        close(csfd);
        continue;
      }
      auto reply = rpc.handle_one(csfd);

      PreConnector::send_to(csfd, (char *)(reply.data()), reply.size());
      PreConnector::wait_close(
          csfd); // wait for the client to close the connection
      close(csfd);
    } // end loop
    close(listenfd);
  }
};

} // end namespace rdmaio
