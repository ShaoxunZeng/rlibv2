#pragma once

#include <sys/queue.h>
#include <infiniband/verbs.h>
#include <string>

#define SRQ_SIZE            2048
/**
 * From SPDK:
 * When using an srq, we can limit the completion queue at startup.
 * The following formula represents the calculation:
 * num_cqe = num_recv + num_data_wr + num_send_wr.
 * where num_recv=num_data_wr=and num_send_wr=poller->max_srq_depth
*/
#define CQ_SIZE             3 * SRQ_SIZE
#define SGL_NUM             1

namespace nordma {

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_rdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct nordma_recv {
	struct ibv_recv_wr			wr;
	struct ibv_sge				sgl[SGL_NUM];

	TAILQ_ENTRY(nordma_recv)	link;
};

struct nordma_cmd {
	char data[1024];
};

struct nordma_resource_opts {
	struct ibv_pd	*pd;
	struct ibv_srq	*srq;
};

struct nordma_recv_wr_list {
	struct ibv_recv_wr	*first;
	struct ibv_recv_wr	*last;
};

struct nordma_srq {
	struct ibv_srq *srq;
	struct nordma_recv_wr_list recv_wrs;
};

class nordma_resources {
	/* Array of size "SRQ_SIZE" containing RDMA recvs. */
	struct nordma_recv		*recvs;
	struct nordma_cmd		*cmds;
	struct ibv_mr			*cmds_mr;

public:
	explicit nordma_resources(struct nordma_resource_opts *opts)
	{
		struct nordma_srq	*srq;
		struct ibv_recv_wr  *bad_wr;

		srq = (struct nordma_srq *)calloc(1, sizeof(struct nordma_srq));

		this->recvs = (struct nordma_recv *)calloc(SRQ_SIZE, sizeof(*this->recvs));
		this->cmds = (struct nordma_cmd *)calloc(SRQ_SIZE, sizeof(*this->cmds));

		this->cmds_mr = ibv_reg_mr(opts->pd, this->cmds,
						SRQ_SIZE * sizeof(*this->cmds),
						IBV_ACCESS_LOCAL_WRITE);
		RDMA_ASSERT(cmds_mr != nullptr);
		
		srq->srq = opts->srq;

		struct nordma_recv		*nordma_recv;

		for(int i = 0; i < SRQ_SIZE; i++) {
			nordma_recv = &this->recvs[i];

			nordma_recv->sgl[0].addr = (uint64_t)&this->cmds[i];
			nordma_recv->sgl[0].length = sizeof(this->cmds[i]);
			nordma_recv->sgl[0].lkey = this->cmds_mr->lkey;

			nordma_recv->wr.num_sge = 1;
			nordma_recv->wr.sg_list = nordma_recv->sgl;

			nordma_srq_queue_recv_wrs(srq, &nordma_recv->wr);
		}

		nordma_srq_flush_recv_wrs(srq);
	}

private:

	bool
	nordma_srq_queue_recv_wrs(struct nordma_srq *nordma_srq, struct ibv_recv_wr *first)
	{
		struct ibv_recv_wr *last;
		struct nordma_recv_wr_list *recv_wrs = &nordma_srq->recv_wrs;

		last = first;
		while (last->next != NULL) {
			last = last->next;
		}

		if (recv_wrs->first == NULL) {
			recv_wrs->first = first;
			recv_wrs->last = last;
			return true;
		} else {
			recv_wrs->last->next = first;
			recv_wrs->last = last;
			return false;
		}
	}

	int
	nordma_srq_flush_recv_wrs(struct nordma_srq	*srq)
	{
		struct ibv_recv_wr *bad_wr;

		int rc = ibv_post_srq_recv(srq->srq, srq->recv_wrs.first, &bad_wr);
		RDMA_ASSERT(rc == 0);

		return rc;
	}

};

}