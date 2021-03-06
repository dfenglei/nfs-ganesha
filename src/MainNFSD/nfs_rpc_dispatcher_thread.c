/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *                William Allen Simpson <william.allen.simpson@gmail.com>
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file  nfs_rpc_dispatcher_thread.c
 * @brief Contains the @c rpc_dispatcher_thread routine and support code
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>		/* for having FNDELAY */
#include <sys/select.h>
#include <poll.h>
#ifdef RPC_VSOCK
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <assert.h>
#include "hashtable.h"
#include "log.h"
#include "gsh_rpc.h"
#include "abstract_atomic.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"
#include "nlm4.h"
#include "rquota.h"
#include "nfs_init.h"
#include "nfs_core.h"
#include "nfs_convert.h"
#include "nfs_exports.h"
#include "nfs_proto_functions.h"
#include "nfs_req_queue.h"
#include "nfs_dupreq.h"
#include "nfs_file_handle.h"
#include "fridgethr.h"

#define NFS_pcp nfs_param.core_param
#define NFS_options NFS_pcp.core_options
#define NFS_program NFS_pcp.program

/**
 * TI-RPC event channels.  Each channel is a thread servicing an event
 * demultiplexer.
 */

struct rpc_evchan {
	uint32_t chan_id;	/*< Channel ID */
};

enum evchan {
	UDP_UREG_CHAN,		/*< Put UDP on a dedicated channel */
	TCP_UREG_CHAN,		/*< Accepts new TCP connections */
#ifdef _USE_NFS_RDMA
	RDMA_UREG_CHAN,		/*< Accepts new RDMA connections */
#endif
	EVCHAN_SIZE
};
#define N_TCP_EVENT_CHAN  3	/*< We don't really want to have too many,
				   relative to the number of available cores. */
#define N_EVENT_CHAN (N_TCP_EVENT_CHAN + EVCHAN_SIZE)

static struct rpc_evchan rpc_evchan[EVCHAN_SIZE];

struct fridgethr *req_fridge;	/*< Decoder thread pool */
struct nfs_req_st nfs_req_st;	/*< Shared request queues */

const char *req_q_s[N_REQ_QUEUES] = {
	"REQ_Q_MOUNT",
	"REQ_Q_CALL",
	"REQ_Q_LOW_LATENCY",
	"REQ_Q_HIGH_LATENCY"
};

static enum xprt_stat nfs_rpc_tcp_user_data(SVCXPRT *);
static enum xprt_stat nfs_rpc_free_user_data(SVCXPRT *);
static enum xprt_stat nfs_rpc_decode_request(SVCXPRT *, XDR *);

const char *xprt_stat_s[XPRT_DESTROYED + 1] = {
	"XPRT_IDLE",
	"XPRT_DISPATCH",
	"XPRT_DIED",
	"XPRT_DESTROYED"
};

/**
 * @brief Function never called, but the symbol is needed for svc_register.
 *
 * @param[in] ptr_req Unused
 * @param[in] ptr_svc Unused
 */
void nfs_rpc_dispatch_dummy(struct svc_req *req)
{
	LogMajor(COMPONENT_DISPATCH,
		 "NFS DISPATCH DUMMY: Possible error, function nfs_rpc_dispatch_dummy should never be called");
}

const char *tags[] = {
	"NFS",
	"MNT",
	"NLM",
	"RQUOTA",
	"NFS_VSOCK",
	"NFS_RDMA",
};

typedef struct proto_data {
	struct sockaddr_in sinaddr_udp;
	struct sockaddr_in sinaddr_tcp;
	struct sockaddr_in6 sinaddr_udp6;
	struct sockaddr_in6 sinaddr_tcp6;
	struct netbuf netbuf_udp6;
	struct netbuf netbuf_tcp6;
	struct t_bind bindaddr_udp6;
	struct t_bind bindaddr_tcp6;
	struct __rpc_sockinfo si_udp6;
	struct __rpc_sockinfo si_tcp6;
} proto_data;

proto_data pdata[P_COUNT];

struct netconfig *netconfig_udpv4;
struct netconfig *netconfig_tcpv4;
struct netconfig *netconfig_udpv6;
struct netconfig *netconfig_tcpv6;

/* RPC Service Sockets and Transports */
int udp_socket[P_COUNT];
int tcp_socket[P_COUNT];
SVCXPRT *udp_xprt[P_COUNT];
SVCXPRT *tcp_xprt[P_COUNT];

/* Flag to indicate if V6 interfaces on the host are enabled */
bool v6disabled;
bool vsock;
bool rdma;

/**
 * @brief Unregister an RPC program.
 *
 * @param[in] prog  Program to unregister
 * @param[in] vers1 Lowest version
 * @param[in] vers2 Highest version
 */
static void unregister(const rpcprog_t prog, const rpcvers_t vers1,
		       const rpcvers_t vers2)
{
	rpcvers_t vers;

	for (vers = vers1; vers <= vers2; vers++) {
		rpcb_unset(prog, vers, netconfig_udpv4);
		rpcb_unset(prog, vers, netconfig_tcpv4);
		if (netconfig_udpv6)
			rpcb_unset(prog, vers, netconfig_udpv6);
		if (netconfig_tcpv6)
			rpcb_unset(prog, vers, netconfig_tcpv6);
	}
}

static void unregister_rpc(void)
{
	if ((NFS_options & CORE_OPTION_NFSV3) != 0) {
		unregister(NFS_program[P_NFS], NFS_V2, NFS_V4);
		unregister(NFS_program[P_MNT], MOUNT_V1, MOUNT_V3);
	} else {
		unregister(NFS_program[P_NFS], NFS_V4, NFS_V4);
	}
#ifdef _USE_NLM
	if (nfs_param.core_param.enable_NLM)
		unregister(NFS_program[P_NLM], 1, NLM4_VERS);
#endif /* _USE_NLM */
	if (nfs_param.core_param.enable_RQUOTA) {
		unregister(NFS_program[P_RQUOTA], RQUOTAVERS, EXT_RQUOTAVERS);
	}
}

static inline bool nfs_protocol_enabled(protos p)
{
	bool nfsv3 = NFS_options & CORE_OPTION_NFSV3;

	switch (p) {
	case P_NFS:
		return true;

	case P_MNT: /* valid only for NFSv3 environments */
		if (nfsv3)
			return true;
		break;

#ifdef _USE_NLM
	case P_NLM: /* valid only for NFSv3 environments */
		if (nfsv3 && nfs_param.core_param.enable_NLM)
			return true;
		break;
#endif

	case P_RQUOTA:
		if (nfs_param.core_param.enable_RQUOTA)
			return true;
		break;

	default:
		break;
	}

	return false;
}

/**
 * @brief Close transports and file descriptors used for RPC services.
 *
 * So that restarting the NFS server wont encounter issues of "Address
 * Already In Use" - this has occurred even though we set the
 * SO_REUSEADDR option when restarting the server with a single export
 * (i.e.: a small config) & no logging at all, making the restart very
 * fast.  when closing a listening socket it will be closed
 * immediately if no connection is pending on it, hence drastically
 * reducing the probability for trouble.
 */
static void close_rpc_fd(void)
{
	protos p;

	for (p = P_NFS; p < P_COUNT; p++) {
		if (udp_socket[p] != -1)
			close(udp_socket[p]);
		if (tcp_socket[p] != -1)
			close(tcp_socket[p]);
	}
	/* no need for special tcp_xprt[P_NFS_VSOCK] treatment */
}

/**
 * @brief Dispatch after rendezvous
 *
 * Record activity on a rendezvous transport handle.
 *
 * @note
 * Cases are distinguished by separate callbacks for each fd.
 * UDP connections are bound to socket NFS_UDPSocket
 * TCP initial connections are bound to socket NFS_TCPSocket
 * all the other cases are requests from already connected TCP Clients
 */
static enum xprt_stat nfs_rpc_dispatch_udp_NFS(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "NFS UDP request for SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_NFS;
	return SVC_RECV(xprt);
}

static enum xprt_stat nfs_rpc_dispatch_udp_MNT(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "MOUNT UDP request for SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_MNT;
	return SVC_RECV(xprt);
}

static enum xprt_stat nfs_rpc_dispatch_udp_NLM(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "NLM UDP request for SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_NLM;
	return SVC_RECV(xprt);
}

static enum xprt_stat nfs_rpc_dispatch_udp_RQUOTA(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "RQUOTA UDP request for SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_RQUOTA;
	return SVC_RECV(xprt);
}

const svc_xprt_fun_t udp_dispatch[] = {
	nfs_rpc_dispatch_udp_NFS,
	nfs_rpc_dispatch_udp_MNT,
	nfs_rpc_dispatch_udp_NLM,
	nfs_rpc_dispatch_udp_RQUOTA,
	NULL,
	NULL,
};

static enum xprt_stat nfs_rpc_dispatch_tcp_NFS(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "NFS TCP request on SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_NFS;
	return nfs_rpc_tcp_user_data(xprt);
}

static enum xprt_stat nfs_rpc_dispatch_tcp_MNT(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "MOUNT TCP request on SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_MNT;
	return nfs_rpc_tcp_user_data(xprt);
}

static enum xprt_stat nfs_rpc_dispatch_tcp_NLM(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "NLM TCP request on SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_NLM;
	return nfs_rpc_tcp_user_data(xprt);
}

static enum xprt_stat nfs_rpc_dispatch_tcp_RQUOTA(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "RQUOTA TCP request on SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_RQUOTA;
	return nfs_rpc_tcp_user_data(xprt);
}

static enum xprt_stat nfs_rpc_dispatch_tcp_VSOCK(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "VSOCK TCP request on SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_NFS;
	return nfs_rpc_tcp_user_data(xprt);
}

const svc_xprt_fun_t tcp_dispatch[] = {
	nfs_rpc_dispatch_tcp_NFS,
	nfs_rpc_dispatch_tcp_MNT,
	nfs_rpc_dispatch_tcp_NLM,
	nfs_rpc_dispatch_tcp_RQUOTA,
	nfs_rpc_dispatch_tcp_VSOCK,
	NULL,
};

void Create_udp(protos prot)
{
	udp_xprt[prot] =
	    svc_dg_create(udp_socket[prot],
			  nfs_param.core_param.rpc.max_send_buffer_size,
			  nfs_param.core_param.rpc.max_recv_buffer_size);
	if (udp_xprt[prot] == NULL)
		LogFatal(COMPONENT_DISPATCH, "Cannot allocate %s/UDP SVCXPRT",
			 tags[prot]);

	udp_xprt[prot]->xp_dispatch.rendezvous_cb = udp_dispatch[prot];

	/* Hook xp_free_user_data (finalize/free private data) */
	(void)SVC_CONTROL(udp_xprt[prot], SVCSET_XP_FREE_USER_DATA,
			  nfs_rpc_free_user_data);

	/* Setup private data */
	(udp_xprt[prot])->xp_u1 =
		alloc_gsh_xprt_private(udp_xprt[prot],
				       XPRT_PRIVATE_FLAG_NONE);

	(void)svc_rqst_evchan_reg(rpc_evchan[UDP_UREG_CHAN].chan_id,
				  udp_xprt[prot], SVC_RQST_FLAG_XPRT_UREG);
}

void Create_tcp(protos prot)
{
	tcp_xprt[prot] =
		svc_vc_ncreatef(tcp_socket[prot],
				nfs_param.core_param.rpc.max_send_buffer_size,
				nfs_param.core_param.rpc.max_recv_buffer_size,
				SVC_CREATE_FLAG_CLOSE | SVC_CREATE_FLAG_LISTEN);
	if (tcp_xprt[prot] == NULL)
		LogFatal(COMPONENT_DISPATCH, "Cannot allocate %s/TCP SVCXPRT",
			 tags[prot]);

	tcp_xprt[prot]->xp_dispatch.rendezvous_cb = tcp_dispatch[prot];

	/* Hook xp_free_user_data (finalize/free private data) */
	(void)SVC_CONTROL(tcp_xprt[prot], SVCSET_XP_FREE_USER_DATA,
			  nfs_rpc_free_user_data);

	/* Setup private data */
	(tcp_xprt[prot])->xp_u1 =
		alloc_gsh_xprt_private(tcp_xprt[prot],
				       XPRT_PRIVATE_FLAG_NONE);

	(void)svc_rqst_evchan_reg(rpc_evchan[TCP_UREG_CHAN].chan_id,
				  tcp_xprt[prot], SVC_RQST_FLAG_XPRT_UREG);
}

#ifdef _USE_NFS_RDMA
struct rpc_rdma_attr rpc_rdma_xa = {
	.statistics_prefix = NULL,
	.node = "::",
	.port = "20049",
	.sq_depth = 32,			/* default was 50 */
	.max_send_sge = 32,		/* minimum 2 */
	.rq_depth = 32,			/* default was 50 */
	.max_recv_sge = 31,		/* minimum 1 */
	.backlog = 10,			/* minimum 2 */
	.credits = 30,			/* default 10 */
	.destroy_on_disconnect = true,
	.use_srq = false,
};

static enum xprt_stat nfs_rpc_dispatch_RDMA(SVCXPRT *xprt)
{
	LogFullDebug(COMPONENT_DISPATCH,
		     "RDMA request on SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);
	xprt->xp_dispatch.process_cb = nfs_rpc_valid_NFS;
	return SVC_STAT(xprt->xp_parent);
}

void Create_RDMA(protos prot)
{
	/* This has elements of both UDP and TCP setup */
	tcp_xprt[prot] =
		svc_rdma_create(&rpc_rdma_xa,
				nfs_param.core_param.rpc.max_send_buffer_size,
				nfs_param.core_param.rpc.max_recv_buffer_size);
	if (tcp_xprt[prot] == NULL)
		LogFatal(COMPONENT_DISPATCH, "Cannot allocate RPC/%s SVCXPRT",
			 tags[prot]);

	tcp_xprt[prot]->xp_dispatch.rendezvous_cb = nfs_rpc_dispatch_RDMA;

	/* Hook xp_free_user_data (finalize/free private data) */
	(void)SVC_CONTROL(tcp_xprt[prot], SVCSET_XP_FREE_USER_DATA,
			  nfs_rpc_free_user_data);

	/* Setup private data */
	(tcp_xprt[prot])->xp_u1 =
		alloc_gsh_xprt_private(tcp_xprt[prot],
				       XPRT_PRIVATE_FLAG_NONE);

	(void)svc_rqst_evchan_reg(rpc_evchan[RDMA_UREG_CHAN].chan_id,
				  tcp_xprt[prot], SVC_RQST_FLAG_XPRT_UREG);
}
#endif

/**
 * @brief Create the SVCXPRT for each protocol in use
 */
void Create_SVCXPRTs(void)
{
	protos p;

	LogFullDebug(COMPONENT_DISPATCH, "Allocation of the SVCXPRT");
	for (p = P_NFS; p < P_COUNT; p++)
		if (nfs_protocol_enabled(p)) {
			Create_udp(p);
			Create_tcp(p);
		}
#ifdef RPC_VSOCK
	if (vsock)
		Create_tcp(P_NFS_VSOCK);
#endif /* RPC_VSOCK */
#ifdef _USE_NFS_RDMA
	if (rdma)
		Create_RDMA(P_NFS_RDMA);
#endif /* _USE_NFS_RDMA */
}

/**
 * @brief Bind the udp and tcp sockets for V6 Interfaces
 */
static int Bind_sockets_V6(void)
{
	protos p;
	int    rc = 0;

	for (p = P_NFS; p < P_COUNT; p++) {
		if (nfs_protocol_enabled(p)) {

			proto_data *pdatap = &pdata[p];

			memset(&pdatap->sinaddr_udp6, 0,
			       sizeof(pdatap->sinaddr_udp6));
			pdatap->sinaddr_udp6.sin6_family = AF_INET6;
			/* all interfaces */
			pdatap->sinaddr_udp6.sin6_addr = in6addr_any;
			pdatap->sinaddr_udp6.sin6_port =
			    htons(nfs_param.core_param.port[p]);

			pdatap->netbuf_udp6.maxlen =
			    sizeof(pdatap->sinaddr_udp6);
			pdatap->netbuf_udp6.len = sizeof(pdatap->sinaddr_udp6);
			pdatap->netbuf_udp6.buf = &pdatap->sinaddr_udp6;

			pdatap->bindaddr_udp6.qlen = SOMAXCONN;
			pdatap->bindaddr_udp6.addr = pdatap->netbuf_udp6;

			if (!__rpc_fd2sockinfo(udp_socket[p],
			    &pdatap->si_udp6)) {
				LogWarn(COMPONENT_DISPATCH,
					 "Cannot get %s socket info for udp6 socket errno=%d (%s)",
					 tags[p], errno, strerror(errno));
				return -1;
			}

			rc = bind(udp_socket[p],
			      (struct sockaddr *)pdatap->bindaddr_udp6.addr.buf,
				  (socklen_t) pdatap->si_udp6.si_alen);
			if (rc == -1) {
				LogWarn(COMPONENT_DISPATCH,
					 "Cannot bind %s udp6 socket, error %d (%s)",
					 tags[p], errno, strerror(errno));
				goto exit;
			}

			memset(&pdatap->sinaddr_tcp6, 0,
			       sizeof(pdatap->sinaddr_tcp6));
			pdatap->sinaddr_tcp6.sin6_family = AF_INET6;
			/* all interfaces */
			pdatap->sinaddr_tcp6.sin6_addr = in6addr_any;
			pdatap->sinaddr_tcp6.sin6_port =
			    htons(nfs_param.core_param.port[p]);

			pdatap->netbuf_tcp6.maxlen =
			    sizeof(pdatap->sinaddr_tcp6);
			pdatap->netbuf_tcp6.len = sizeof(pdatap->sinaddr_tcp6);
			pdatap->netbuf_tcp6.buf = &pdatap->sinaddr_tcp6;

			pdatap->bindaddr_tcp6.qlen = SOMAXCONN;
			pdatap->bindaddr_tcp6.addr = pdatap->netbuf_tcp6;

			if (!__rpc_fd2sockinfo(tcp_socket[p],
			    &pdatap->si_tcp6)) {
				LogWarn(COMPONENT_DISPATCH,
					 "Cannot get %s socket info for tcp6 socket errno=%d (%s)",
					 tags[p], errno, strerror(errno));
				return -1;
			}

			rc = bind(tcp_socket[p],
				  (struct sockaddr *)
				   pdatap->bindaddr_tcp6.addr.buf,
				 (socklen_t) pdatap->si_tcp6.si_alen);
			if (rc == -1) {
				LogWarn(COMPONENT_DISPATCH,
					"Cannot bind %s tcp6 socket, error %d (%s)",
					tags[p], errno, strerror(errno));
				goto exit;
			}
		}
	}

exit:
	return rc;
}

/**
 * @brief Bind the udp and tcp sockets for V4 Interfaces
 */
static int Bind_sockets_V4(void)
{
	protos p;
	int    rc = 0;

	for (p = P_NFS; p < P_COUNT; p++) {
		if (nfs_protocol_enabled(p)) {

			proto_data *pdatap = &pdata[p];

			memset(&pdatap->sinaddr_udp, 0,
			       sizeof(pdatap->sinaddr_udp));
			pdatap->sinaddr_udp.sin_family = AF_INET;
			/* all interfaces */
			pdatap->sinaddr_udp.sin_addr.s_addr = htonl(INADDR_ANY);
			pdatap->sinaddr_udp.sin_port =
			    htons(nfs_param.core_param.port[p]);

			pdatap->netbuf_udp6.maxlen =
			    sizeof(pdatap->sinaddr_udp);
			pdatap->netbuf_udp6.len = sizeof(pdatap->sinaddr_udp);
			pdatap->netbuf_udp6.buf = &pdatap->sinaddr_udp;

			pdatap->bindaddr_udp6.qlen = SOMAXCONN;
			pdatap->bindaddr_udp6.addr = pdatap->netbuf_udp6;

			if (!__rpc_fd2sockinfo(udp_socket[p],
			    &pdatap->si_udp6)) {
				LogWarn(COMPONENT_DISPATCH,
					"Cannot get %s socket info for udp6 socket errno=%d (%s)",
					tags[p], errno, strerror(errno));
				return -1;
			}

			rc = bind(udp_socket[p],
				  (struct sockaddr *)
				  pdatap->bindaddr_udp6.addr.buf,
				  (socklen_t) pdatap->si_udp6.si_alen);
			if (rc == -1) {
				LogWarn(COMPONENT_DISPATCH,
					"Cannot bind %s udp6 socket, error %d (%s)",
					tags[p], errno, strerror(errno));
				return -1;
			}

			memset(&pdatap->sinaddr_tcp, 0,
			       sizeof(pdatap->sinaddr_tcp));
			pdatap->sinaddr_tcp.sin_family = AF_INET;
			/* all interfaces */
			pdatap->sinaddr_tcp.sin_addr.s_addr = htonl(INADDR_ANY);
			pdatap->sinaddr_tcp.sin_port =
			    htons(nfs_param.core_param.port[p]);

			pdatap->netbuf_tcp6.maxlen =
			    sizeof(pdatap->sinaddr_tcp);
			pdatap->netbuf_tcp6.len = sizeof(pdatap->sinaddr_tcp);
			pdatap->netbuf_tcp6.buf = &pdatap->sinaddr_tcp;

			pdatap->bindaddr_tcp6.qlen = SOMAXCONN;
			pdatap->bindaddr_tcp6.addr = pdatap->netbuf_tcp6;

			if (!__rpc_fd2sockinfo(tcp_socket[p],
			    &pdatap->si_tcp6)) {
				LogWarn(COMPONENT_DISPATCH,
					"V4 : Cannot get %s socket info for tcp socket error %d(%s)",
					tags[p], errno, strerror(errno));
				return -1;
			}

			rc = bind(tcp_socket[p],
				  (struct sockaddr *)
				  pdatap->bindaddr_tcp6.addr.buf,
				  (socklen_t) pdatap->si_tcp6.si_alen);
			if (rc == -1) {
				LogWarn(COMPONENT_DISPATCH,
					"Cannot bind %s tcp socket, error %d(%s)",
					tags[p], errno, strerror(errno));
				return -1;
			}
		}
	}

	return rc;
}

#ifdef RPC_VSOCK
int bind_sockets_vsock(void)
{
	int rc = 0;

	struct sockaddr_vm sa_listen = {
		.svm_family = AF_VSOCK,
		.svm_cid = VMADDR_CID_ANY,
		.svm_port = nfs_param.core_param.port[P_NFS],
	};

	rc = bind(tcp_socket[P_NFS_VSOCK], (struct sockaddr *)
		(struct sockaddr *)&sa_listen, sizeof(sa_listen));
	if (rc == -1) {
		LogWarn(COMPONENT_DISPATCH,
			"cannot bind %s stream socket, error %d(%s)",
			tags[P_NFS_VSOCK], errno, strerror(errno));
	}
	return rc;
}
#endif /* RPC_VSOCK */

void Bind_sockets(void)
{
	int rc = 0;

	/*
	 * See Allocate_sockets(), which should already
	 * have set the global v6disabled accordingly
	 */
	if (v6disabled) {
		rc = Bind_sockets_V4();
		if (rc)
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V4 interface. Cannot continue.");
	} else {
		rc = Bind_sockets_V6();
		if (rc)
			LogFatal(COMPONENT_DISPATCH,
				 "Error binding to V6 interface. Cannot continue.");
	}
#ifdef RPC_VSOCK
	if (vsock) {
		rc = bind_sockets_vsock();
		if (rc)
			LogMajor(COMPONENT_DISPATCH,
				"AF_VSOCK bind failed (continuing startup)");
	}
#endif /* RPC_VSOCK */
	LogInfo(COMPONENT_DISPATCH,
		"Bind_sockets() successful, v6disabled = %d, vsock = %d, rdma = %d",
		v6disabled, vsock, rdma);
}

/**
 * @brief Function to set the socket options on the allocated
 *	  udp and tcp sockets
 *
 */
static int alloc_socket_setopts(int p)
{
	int one = 1;
	const struct nfs_core_param *nfs_cp = &nfs_param.core_param;

	/* Use SO_REUSEADDR in order to avoid wait
	 * the 2MSL timeout */
	if (setsockopt(udp_socket[p],
		       SOL_SOCKET, SO_REUSEADDR,
		       &one, sizeof(one))) {
		LogWarn(COMPONENT_DISPATCH,
			"Bad udp socket options for %s, error %d(%s)",
			tags[p], errno, strerror(errno));

		return -1;
	}

	if (setsockopt(tcp_socket[p],
		       SOL_SOCKET, SO_REUSEADDR,
		       &one, sizeof(one))) {
		LogWarn(COMPONENT_DISPATCH,
			"Bad tcp socket option reuseaddr for %s, error %d(%s)",
			tags[p], errno, strerror(errno));

		return -1;
	}

	if (nfs_cp->enable_tcp_keepalive) {
		if (setsockopt(tcp_socket[p],
			       SOL_SOCKET, SO_KEEPALIVE,
			       &one, sizeof(one))) {
			LogWarn(COMPONENT_DISPATCH,
				"Bad tcp socket option keepalive for %s, error %d(%s)",
				tags[p], errno, strerror(errno));

			return -1;
		}

		if (nfs_cp->tcp_keepcnt) {
			if (setsockopt(tcp_socket[p], IPPROTO_TCP, TCP_KEEPCNT,
				       &nfs_cp->tcp_keepcnt,
				       sizeof(nfs_cp->tcp_keepcnt))) {
				LogWarn(COMPONENT_DISPATCH,
					"Bad tcp socket option TCP_KEEPCNT for %s, error %d(%s)",
					tags[p], errno, strerror(errno));

				return -1;
			}
		}

		if (nfs_cp->tcp_keepidle) {
			if (setsockopt(tcp_socket[p], IPPROTO_TCP, TCP_KEEPIDLE,
				       &nfs_cp->tcp_keepidle,
				       sizeof(nfs_cp->tcp_keepidle))) {
				LogWarn(COMPONENT_DISPATCH,
					"Bad tcp socket option TCP_KEEPIDLE for %s, error %d(%s)",
					tags[p], errno, strerror(errno));

				return -1;
			}
		}

		if (nfs_cp->tcp_keepintvl) {
			if (setsockopt(tcp_socket[p], IPPROTO_TCP,
				       TCP_KEEPINTVL, &nfs_cp->tcp_keepintvl,
				       sizeof(nfs_cp->tcp_keepintvl))) {
				LogWarn(COMPONENT_DISPATCH,
					"Bad tcp socket option TCP_KEEPINTVL for %s, error %d(%s)",
					tags[p], errno, strerror(errno));

				return -1;
			}
		}
	}

	/* We prefer using non-blocking socket
	 * in the specific case */
	if (fcntl(udp_socket[p], F_SETFL, FNDELAY) == -1) {
		LogWarn(COMPONENT_DISPATCH,
			"Cannot set udp socket for %s as non blocking, error %d(%s)",
			tags[p], errno, strerror(errno));

		return -1;
	}

	return 0;
}

/**
 * @brief Allocate the tcp and udp sockets for the nfs daemon
 * using V4 interfaces
 */
static int Allocate_sockets_V4(int p)
{
	udp_socket[p] = socket(AF_INET,
			       SOCK_DGRAM,
			       IPPROTO_UDP);

	if (udp_socket[p] == -1) {
		if (errno == EAFNOSUPPORT) {
			LogInfo(COMPONENT_DISPATCH,
				"No V6 and V4 intfs configured?!");
		}

		LogWarn(COMPONENT_DISPATCH,
			"Cannot allocate a udp socket for %s, error %d(%s)",
			tags[p], errno, strerror(errno));

		return -1;
	}

	tcp_socket[p] = socket(AF_INET,
			       SOCK_STREAM,
			       IPPROTO_TCP);

	if (tcp_socket[p] == -1) {
		LogWarn(COMPONENT_DISPATCH,
			"Cannot allocate a tcp socket for %s, error %d(%s)",
			tags[p], errno, strerror(errno));
		return -1;
	}

	return 0;

}

#ifdef RPC_VSOCK
/**
 * @brief Create vmci stream socket
 */
static int allocate_socket_vsock(void)
{
	int one = 1;

	tcp_socket[P_NFS_VSOCK] = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (tcp_socket[P_NFS_VSOCK] == -1) {
		LogWarn(COMPONENT_DISPATCH,
			"socket create failed for %s, error %d(%s)",
			tags[P_NFS_VSOCK], errno, strerror(errno));
		return -1;
	}
	if (setsockopt(tcp_socket[P_NFS_VSOCK],
			SOL_SOCKET, SO_REUSEADDR,
			&one, sizeof(one))) {
		LogWarn(COMPONENT_DISPATCH,
			"bad tcp socket options for %s, error %d(%s)",
			tags[P_NFS_VSOCK], errno, strerror(errno));

		return -1;
	}

	LogDebug(COMPONENT_DISPATCH,
		"Socket numbers are: %s tcp=%u",
		tags[P_NFS_VSOCK],
		tcp_socket[P_NFS_VSOCK]);
	return 0;
}
#endif /* RPC_VSOCK */

/**
 * @brief Allocate the tcp and udp sockets for the nfs daemon
 */
static void Allocate_sockets(void)
{
	protos	p;
	int	rc = 0;

	LogFullDebug(COMPONENT_DISPATCH, "Allocation of the sockets");

	for (p = P_NFS; p < P_COUNT; p++) {
		/* Initialize all the sockets to -1 because
		 * it makes some code later easier */
		udp_socket[p] = -1;
		tcp_socket[p] = -1;

		if (nfs_protocol_enabled(p)) {
			if (v6disabled)
				goto try_V4;

			udp_socket[p] = socket(AF_INET6,
					       SOCK_DGRAM,
					       IPPROTO_UDP);

			if (udp_socket[p] == -1) {
				/*
				 * We assume that EAFNOSUPPORT points
				 * to the likely case when the host has
				 * V6 interfaces disabled. So we will
				 * try to use the existing V4 interfaces
				 * instead
				 */
				if (errno == EAFNOSUPPORT) {
					v6disabled = true;
					LogWarn(COMPONENT_DISPATCH,
					    "System may not have V6 intfs configured error %d(%s)",
					    errno, strerror(errno));

					goto try_V4;
				}

				LogFatal(COMPONENT_DISPATCH,
					 "Cannot allocate a udp socket for %s, error %d(%s)",
					 tags[p], errno, strerror(errno));
			}

			tcp_socket[p] = socket(AF_INET6,
					       SOCK_STREAM,
					       IPPROTO_TCP);

			/* We fail with LogFatal here on error because it
			 * shouldn't be that we have managed to create a
			 * V6 based udp socket and have failed for the tcp
			 * sock. If it were a case of V6 being disabled,
			 * then we would have encountered that case with
			 * the first udp sock create and would have moved
			 * on to create the V4 sockets.
			 */
			if (tcp_socket[p] == -1)
				LogFatal(COMPONENT_DISPATCH,
					 "Cannot allocate a tcp socket for %s, error %d(%s)",
					 tags[p], errno, strerror(errno));

try_V4:
			if (v6disabled) {
				rc = Allocate_sockets_V4(p);
				if (rc) {
					LogFatal(COMPONENT_DISPATCH,
						 "Error allocating V4 socket for proto %d, %s",
						 p, tags[p]);
				}
			}

			rc = alloc_socket_setopts(p);
			if (rc) {
				LogFatal(COMPONENT_DISPATCH,
					 "Error setting socket option for proto %d, %s",
					 p, tags[p]);
			}
			LogDebug(COMPONENT_DISPATCH,
				"Socket numbers are: %s tcp=%u udp=%u",
				tags[p],
				tcp_socket[p],
				udp_socket[p]);
		}
	}
#ifdef RPC_VSOCK
	if (vsock)
		allocate_socket_vsock();
#endif /* RPC_VSOCK */
}

/* The following routine must ONLY be called from the shutdown
 * thread */
void Clean_RPC(void)
{
  /**
   * @todo Consider the need to call Svc_dg_destroy for UDP & ?? for
   * TCP based services
   */
	unregister_rpc();
	close_rpc_fd();
}

#define UDP_REGISTER(prot, vers, netconfig) \
	svc_reg(udp_xprt[prot], NFS_program[prot], \
		(u_long) vers,					    \
		nfs_rpc_dispatch_dummy, netconfig)

#define TCP_REGISTER(prot, vers, netconfig) \
	svc_reg(tcp_xprt[prot], NFS_program[prot], \
		(u_long) vers,					    \
		nfs_rpc_dispatch_dummy, netconfig)

void Register_program(protos prot, int flag, int vers)
{
	if ((NFS_options & flag) != 0) {
		LogInfo(COMPONENT_DISPATCH, "Registering %s V%d/UDP",
			tags[prot], (int)vers);

		/* XXXX fix svc_register! */
		if (!UDP_REGISTER(prot, vers, netconfig_udpv4))
			LogFatal(COMPONENT_DISPATCH,
				 "Cannot register %s V%d on UDP", tags[prot],
				 (int)vers);

		if (netconfig_udpv6) {
			LogInfo(COMPONENT_DISPATCH, "Registering %s V%d/UDPv6",
				tags[prot], (int)vers);
			if (!UDP_REGISTER(prot, vers, netconfig_udpv6))
				LogFatal(COMPONENT_DISPATCH,
					 "Cannot register %s V%d on UDPv6",
					 tags[prot], (int)vers);
		}

#ifndef _NO_TCP_REGISTER
		LogInfo(COMPONENT_DISPATCH, "Registering %s V%d/TCP",
			tags[prot], (int)vers);

		if (!TCP_REGISTER(prot, vers, netconfig_tcpv4))
			LogFatal(COMPONENT_DISPATCH,
				 "Cannot register %s V%d on TCP", tags[prot],
				 (int)vers);

		if (netconfig_tcpv6) {
			LogInfo(COMPONENT_DISPATCH, "Registering %s V%d/TCPv6",
				tags[prot], (int)vers);
			if (!TCP_REGISTER(prot, vers, netconfig_tcpv6))
				LogFatal(COMPONENT_DISPATCH,
					 "Cannot register %s V%d on TCPv6",
					 tags[prot], (int)vers);
		}
#endif				/* _NO_TCP_REGISTER */
	}
}

tirpc_pkg_params ntirpc_pp = {
	0,
	0,
	(mem_format_t)rpc_warnx,
	gsh_free_size,
	gsh_malloc__,
	gsh_malloc_aligned__,
	gsh_calloc__,
	gsh_realloc__,
};

/**
 * @brief Init the svc descriptors for the nfs daemon
 *
 * Perform all the required initialization for the RPC subsystem and event
 * channels.
 */
void nfs_Init_svc(void)
{
	svc_init_params svc_params;
	int ix;
	int code;

	LogDebug(COMPONENT_DISPATCH, "NFS INIT: Core options = %d",
		 NFS_options);

	/* Init request queue before RPC stack */
	nfs_rpc_queue_init();

	LogInfo(COMPONENT_DISPATCH, "NFS INIT: using TIRPC");

	memset(&svc_params, 0, sizeof(svc_params));

#ifdef __FreeBSD__
	v6disabled = true;
#else
	v6disabled = false;
#endif

	/* Set TIRPC debug flags */
	ntirpc_pp.debug_flags = nfs_param.core_param.rpc.debug_flags;

	/* Redirect TI-RPC allocators, log channel */
	if (!tirpc_control(TIRPC_PUT_PARAMETERS, &ntirpc_pp))
		LogCrit(COMPONENT_INIT, "Setting nTI-RPC parameters failed");
#ifdef RPC_VSOCK
	vsock = NFS_options & CORE_OPTION_NFS_VSOCK;
#endif
#ifdef _USE_NFS_RDMA
	rdma = NFS_options & CORE_OPTION_NFS_RDMA;
#endif

	/* New TI-RPC package init function */
	svc_params.disconnect_cb = NULL;
	svc_params.request_cb = nfs_rpc_decode_request;
	svc_params.flags = SVC_INIT_EPOLL;	/* use EPOLL event mgmt */
	svc_params.flags |= SVC_INIT_NOREG_XPRTS; /* don't call xprt_register */
	svc_params.max_connections = nfs_param.core_param.rpc.max_connections;
	svc_params.max_events = 1024;	/* length of epoll event queue */
	svc_params.ioq_send_max =
	    nfs_param.core_param.rpc.max_send_buffer_size;
	svc_params.channels = N_EVENT_CHAN;
	svc_params.idle_timeout = nfs_param.core_param.rpc.idle_timeout_s;
	svc_params.ioq_thrd_max = /* max ioq worker threads */
		nfs_param.core_param.rpc.ioq_thrd_max;
	/* GSS ctx cache tuning, expiration */
	svc_params.gss_ctx_hash_partitions =
		nfs_param.core_param.rpc.gss.ctx_hash_partitions;
	svc_params.gss_max_ctx =
		nfs_param.core_param.rpc.gss.max_ctx;
	svc_params.gss_max_gc =
		nfs_param.core_param.rpc.gss.max_gc;

	/* Only after TI-RPC allocators, log channel are setup */
	if (!svc_init(&svc_params))
		LogFatal(COMPONENT_INIT, "SVC initialization failed");

	for (ix = 0; ix < EVCHAN_SIZE; ++ix) {
		rpc_evchan[ix].chan_id = 0;
		code = svc_rqst_new_evchan(&rpc_evchan[ix].chan_id,
					   NULL /* u_data */,
					   SVC_RQST_FLAG_NONE);
		if (code)
			LogFatal(COMPONENT_DISPATCH,
				 "Cannot create TI-RPC event channel (%d, %d)",
				 ix, code);
		/* XXX bail?? */
	}

	/* Get the netconfig entries from /etc/netconfig */
	netconfig_udpv4 = (struct netconfig *)getnetconfigent("udp");
	if (netconfig_udpv4 == NULL)
		LogFatal(COMPONENT_DISPATCH,
			 "Cannot get udp netconfig, cannot get an entry for udp in netconfig file. Check file /etc/netconfig...");

	/* Get the netconfig entries from /etc/netconfig */
	netconfig_tcpv4 = (struct netconfig *)getnetconfigent("tcp");
	if (netconfig_tcpv4 == NULL)
		LogFatal(COMPONENT_DISPATCH,
			 "Cannot get tcp netconfig, cannot get an entry for tcp in netconfig file. Check file /etc/netconfig...");

	/* A short message to show that /etc/netconfig parsing was a success */
	LogFullDebug(COMPONENT_DISPATCH, "netconfig found for UDPv4 and TCPv4");

	LogInfo(COMPONENT_DISPATCH, "NFS INIT: Using IPv6");

	/* Get the netconfig entries from /etc/netconfig */
	netconfig_udpv6 = (struct netconfig *)getnetconfigent("udp6");
	if (netconfig_udpv6 == NULL)
		LogInfo(COMPONENT_DISPATCH,
			 "Cannot get udp6 netconfig, cannot get an entry for udp6 in netconfig file. Check file /etc/netconfig...");

	/* Get the netconfig entries from /etc/netconfig */
	netconfig_tcpv6 = (struct netconfig *)getnetconfigent("tcp6");
	if (netconfig_tcpv6 == NULL)
		LogInfo(COMPONENT_DISPATCH,
			 "Cannot get tcp6 netconfig, cannot get an entry for tcp in netconfig file. Check file /etc/netconfig...");

	/* A short message to show that /etc/netconfig parsing was a success
	 * for ipv6
	 */
	if (netconfig_udpv6 && netconfig_tcpv6)
		LogFullDebug(COMPONENT_DISPATCH,
			     "netconfig found for UDPv6 and TCPv6");

	/* Allocate the UDP and TCP sockets for the RPC */
	Allocate_sockets();

	if ((NFS_options & CORE_OPTION_ALL_NFS_VERS) != 0) {
		/* Bind the tcp and udp sockets */
		Bind_sockets();

		/* Unregister from portmapper/rpcbind */
		unregister_rpc();

		/* Set up well-known xprt handles */
		Create_SVCXPRTs();
	}

#ifdef _HAVE_GSSAPI
	/* Acquire RPCSEC_GSS basis if needed */
	if (nfs_param.krb5_param.active_krb5) {
		if (!svcauth_gss_import_name
		    (nfs_param.krb5_param.svc.principal)) {
			LogFatal(COMPONENT_DISPATCH,
				 "Could not import principal name %s into GSSAPI",
				 nfs_param.krb5_param.svc.principal);
		} else {
			LogInfo(COMPONENT_DISPATCH,
				"Successfully imported principal %s into GSSAPI",
				nfs_param.krb5_param.svc.principal);

			/* Trying to acquire a credentials
			 * for checking name's validity */
			if (!svcauth_gss_acquire_cred())
				LogCrit(COMPONENT_DISPATCH,
					"Cannot acquire credentials for principal %s",
					nfs_param.krb5_param.svc.principal);
			else
				LogDebug(COMPONENT_DISPATCH,
					 "Principal %s is suitable for acquiring credentials",
					 nfs_param.krb5_param.svc.principal);
		}
	}
#endif				/* _HAVE_GSSAPI */

#ifndef _NO_PORTMAPPER
	/* Perform all the RPC registration, for UDP and TCP,
	 * for NFS_V2, NFS_V3 and NFS_V4 */
#ifdef _USE_NFS3
	Register_program(P_NFS, CORE_OPTION_NFSV3, NFS_V3);
#endif /* _USE_NFS3 */
	Register_program(P_NFS, CORE_OPTION_NFSV4, NFS_V4);
	Register_program(P_MNT, CORE_OPTION_NFSV3, MOUNT_V1);
	Register_program(P_MNT, CORE_OPTION_NFSV3, MOUNT_V3);
#ifdef _USE_NLM
	if (nfs_param.core_param.enable_NLM)
		Register_program(P_NLM, CORE_OPTION_NFSV3, NLM4_VERS);
#endif /* _USE_NLM */
	if (nfs_param.core_param.enable_RQUOTA &&
	    (NFS_options & (CORE_OPTION_NFSV3 | CORE_OPTION_NFSV4))) {
		Register_program(P_RQUOTA, CORE_OPTION_ALL_VERS, RQUOTAVERS);
		Register_program(P_RQUOTA, CORE_OPTION_ALL_VERS,
				 EXT_RQUOTAVERS);
	}
#endif				/* _NO_PORTMAPPER */

}

void nfs_rpc_dispatch_stop(void)
{
	int ix;

	for (ix = 0; ix < EVCHAN_SIZE; ++ix) {
		svc_rqst_thrd_signal(rpc_evchan[ix].chan_id,
				     SVC_RQST_SIGNAL_SHUTDOWN);
	}
}

/**
 * @brief Rendezvous callout.  This routine will be called by TI-RPC
 *        after newxprt has been accepted.
 *
 * Register newxprt on a TCP event channel.  Balancing events/channels
 * could become involved.  To start with, just cycle through them as
 * new connections are accepted.
 *
 * @param[in] newxprt Newly created transport
 *
 * @return status of parent.
 */
static enum xprt_stat nfs_rpc_tcp_user_data(SVCXPRT *newxprt)
{
	/* setup private data (freed when xprt is destroyed) */
	newxprt->xp_u1 =
	    alloc_gsh_xprt_private(newxprt, XPRT_PRIVATE_FLAG_NONE);

	/* NB: xu->drc is allocated on first request--we need shared
	 * TCP DRC for v3, but per-connection for v4 */

	return SVC_STAT(newxprt->xp_parent);
}

/**
 * @brief xprt destructor callout
 *
 * @param[in] xprt Transport to destroy
 */
static enum xprt_stat nfs_rpc_free_user_data(SVCXPRT *xprt)
{
	if (xprt->xp_u2) {
		nfs_dupreq_put_drc(xprt, xprt->xp_u2, DRC_FLAG_RELEASE);
		xprt->xp_u2 = NULL;
	}
	free_gsh_xprt_private(xprt);
	return XPRT_DESTROYED;
}

uint32_t nfs_rpc_outstanding_reqs_est(void)
{
	static uint32_t ctr;
	static uint32_t nreqs;
	struct req_q_pair *qpair;
	uint32_t treqs;
	int ix;

	if ((atomic_inc_uint32_t(&ctr) % 10) != 0)
		return atomic_fetch_uint32_t(&nreqs);

	treqs = 0;
	for (ix = 0; ix < N_REQ_QUEUES; ++ix) {
		qpair = &(nfs_req_st.reqs.nfs_request_q.qset[ix]);
		treqs += atomic_fetch_uint32_t(&qpair->producer.size);
		treqs += atomic_fetch_uint32_t(&qpair->consumer.size);
	}

	atomic_store_uint32_t(&nreqs, treqs);
	return treqs;
}

void nfs_rpc_queue_init(void)
{
	struct fridgethr_params reqparams;
	struct req_q_pair *qpair;
	int rc = 0;
	int ix;

	memset(&reqparams, 0, sizeof(struct fridgethr_params));
    /**
     * @todo Add a configuration parameter to set a max.
     */
	reqparams.thr_max = 0;
	reqparams.thr_min = 1;
	reqparams.thread_delay =
		nfs_param.core_param.decoder_fridge_expiration_delay;
	reqparams.deferment = fridgethr_defer_block;
	reqparams.block_delay =
		nfs_param.core_param.decoder_fridge_block_timeout;

	/* decoder thread pool */
	rc = fridgethr_init(&req_fridge, "decoder", &reqparams);
	if (rc != 0)
		LogFatal(COMPONENT_DISPATCH,
			 "Unable to initialize decoder thread pool: %d", rc);

	/* queues */
	pthread_spin_init(&nfs_req_st.reqs.sp, PTHREAD_PROCESS_PRIVATE);
	nfs_req_st.reqs.size = 0;
	for (ix = 0; ix < N_REQ_QUEUES; ++ix) {
		qpair = &(nfs_req_st.reqs.nfs_request_q.qset[ix]);
		qpair->s = req_q_s[ix];
		nfs_rpc_q_init(&qpair->producer);
		nfs_rpc_q_init(&qpair->consumer);
	}

	/* waitq */
	glist_init(&nfs_req_st.reqs.wait_list);
	nfs_req_st.reqs.waiters = 0;

	/* stallq */
	gsh_mutex_init(&nfs_req_st.stallq.mtx, NULL);
	glist_init(&nfs_req_st.stallq.q);
	nfs_req_st.stallq.active = false;
	nfs_req_st.stallq.stalled = 0;
}

static uint32_t enqueued_reqs;
static uint32_t dequeued_reqs;

uint32_t get_enqueue_count(void)
{
	return enqueued_reqs;
}

uint32_t get_dequeue_count(void)
{
	return dequeued_reqs;
}

void nfs_rpc_enqueue_req(request_data_t *reqdata)
{
	struct req_q_set *nfs_request_q;
	struct req_q_pair *qpair;
	struct req_q *q;

#if defined(HAVE_BLKIN)
	BLKIN_TIMESTAMP(
		&reqdata->r_u.req.svc.bl_trace,
		&reqdata->r_u.req.xprt->blkin.endp,
		"enqueue-enter");
#endif

	nfs_request_q = &nfs_req_st.reqs.nfs_request_q;

	switch (reqdata->rtype) {
	case NFS_REQUEST:
		LogFullDebug(COMPONENT_DISPATCH,
			     "enter rq_xid=%" PRIu32 " lookahead.flags=%u",
			     reqdata->r_u.req.svc.rq_msg.rm_xid,
			     reqdata->r_u.req.lookahead.flags);
		if (reqdata->r_u.req.lookahead.flags & NFS_LOOKAHEAD_MOUNT) {
			qpair = &(nfs_request_q->qset[REQ_Q_MOUNT]);
			break;
		}
		if (NFS_LOOKAHEAD_HIGH_LATENCY(reqdata->r_u.req.lookahead))
			qpair = &(nfs_request_q->qset[REQ_Q_HIGH_LATENCY]);
		else
			qpair = &(nfs_request_q->qset[REQ_Q_LOW_LATENCY]);
		break;
	case NFS_CALL:
		qpair = &(nfs_request_q->qset[REQ_Q_CALL]);
		break;
#ifdef _USE_9P
	case _9P_REQUEST:
		/* XXX identify high-latency requests and allocate
		 * to the high-latency queue, as above */
		qpair = &(nfs_request_q->qset[REQ_Q_LOW_LATENCY]);
		break;
#endif
	default:
		goto out;
	}

	/* this one is real, timestamp it
	 */
	now(&reqdata->time_queued);
	/* always append to producer queue */
	q = &qpair->producer;
	pthread_spin_lock(&q->sp);
	glist_add_tail(&q->q, &reqdata->req_q);
	++(q->size);
	pthread_spin_unlock(&q->sp);

	(void) atomic_inc_uint32_t(&enqueued_reqs);

#if defined(HAVE_BLKIN)
	/* log the queue depth */
	BLKIN_KEYVAL_INTEGER(
		&reqdata->r_u.req.svc.bl_trace,
		&reqdata->r_u.req.xprt->blkin.endp,
		"reqs-est",
		nfs_rpc_outstanding_reqs_est()
		);

	BLKIN_TIMESTAMP(
		&reqdata->r_u.req.svc.bl_trace,
		&reqdata->r_u.req.xprt->blkin.endp,
		"enqueue-exit");
#endif
	LogDebug(COMPONENT_DISPATCH,
		 "enqueued req, q %p (%s %p:%p) size is %d (enq %u deq %u)",
		 q, qpair->s, &qpair->producer, &qpair->consumer, q->size,
		 enqueued_reqs, dequeued_reqs);

	/* potentially wakeup some thread */

	/* global waitq */
	{
		wait_q_entry_t *wqe;

		/* SPIN LOCKED */
		pthread_spin_lock(&nfs_req_st.reqs.sp);
		if (nfs_req_st.reqs.waiters) {
			wqe = glist_first_entry(&nfs_req_st.reqs.wait_list,
						wait_q_entry_t, waitq);

			LogFullDebug(COMPONENT_DISPATCH,
				     "nfs_req_st.reqs.waiters %u signal wqe %p (for q %p)",
				     nfs_req_st.reqs.waiters, wqe, q);

			/* release 1 waiter */
			glist_del(&wqe->waitq);
			--(nfs_req_st.reqs.waiters);
			--(wqe->waiters);
			/* ! SPIN LOCKED */
			pthread_spin_unlock(&nfs_req_st.reqs.sp);
			PTHREAD_MUTEX_lock(&wqe->lwe.mtx);
			/* XXX reliable handoff */
			wqe->flags |= Wqe_LFlag_SyncDone;
			if (wqe->flags & Wqe_LFlag_WaitSync)
				pthread_cond_signal(&wqe->lwe.cv);
			PTHREAD_MUTEX_unlock(&wqe->lwe.mtx);
		} else
			/* ! SPIN LOCKED */
			pthread_spin_unlock(&nfs_req_st.reqs.sp);
	}

 out:
	return;
}

/* static inline */
request_data_t *nfs_rpc_consume_req(struct req_q_pair *qpair)
{
	request_data_t *reqdata = NULL;

	pthread_spin_lock(&qpair->consumer.sp);
	if (qpair->consumer.size > 0) {
		reqdata =
		    glist_first_entry(&qpair->consumer.q, request_data_t,
				      req_q);
		glist_del(&reqdata->req_q);
		--(qpair->consumer.size);
		pthread_spin_unlock(&qpair->consumer.sp);
		goto out;
	} else {
		char *s = NULL;
		uint32_t csize = ~0U;
		uint32_t psize = ~0U;

		pthread_spin_lock(&qpair->producer.sp);
		if (isFullDebug(COMPONENT_DISPATCH)) {
			s = (char *)qpair->s;
			csize = qpair->consumer.size;
			psize = qpair->producer.size;
		}
		if (qpair->producer.size > 0) {
			/* splice */
			glist_splice_tail(&qpair->consumer.q,
					  &qpair->producer.q);
			qpair->consumer.size = qpair->producer.size;
			qpair->producer.size = 0;
			/* consumer.size > 0 */
			pthread_spin_unlock(&qpair->producer.sp);
			reqdata =
			    glist_first_entry(&qpair->consumer.q,
					      request_data_t, req_q);
			glist_del(&reqdata->req_q);
			--(qpair->consumer.size);
			pthread_spin_unlock(&qpair->consumer.sp);
			if (s)
				LogFullDebug(COMPONENT_DISPATCH,
					     "try splice, qpair %s consumer qsize=%u producer qsize=%u",
					     s, csize, psize);
			goto out;
		}

		pthread_spin_unlock(&qpair->producer.sp);
		pthread_spin_unlock(&qpair->consumer.sp);

		if (s)
			LogFullDebug(COMPONENT_DISPATCH,
				     "try splice, qpair %s consumer qsize=%u producer qsize=%u",
				     s, csize, psize);
	}
 out:
	return reqdata;
}

request_data_t *nfs_rpc_dequeue_req(nfs_worker_data_t *worker)
{
	request_data_t *reqdata = NULL;
	struct req_q_set *nfs_request_q = &nfs_req_st.reqs.nfs_request_q;
	struct req_q_pair *qpair;
	uint32_t ix, slot;
	struct timespec timeout;

	/* XXX: the following stands in for a more robust/flexible
	 * weighting function */

	/* slot in 1..4 */
 retry_deq:
	slot = (nfs_rpc_q_next_slot() % 4);
	for (ix = 0; ix < 4; ++ix) {
		switch (slot) {
		case 0:
			/* MOUNT */
			qpair = &(nfs_request_q->qset[REQ_Q_MOUNT]);
			break;
		case 1:
			/* NFS_CALL */
			qpair = &(nfs_request_q->qset[REQ_Q_CALL]);
			break;
		case 2:
			/* LL */
			qpair = &(nfs_request_q->qset[REQ_Q_LOW_LATENCY]);
			break;
		case 3:
			/* HL */
			qpair = &(nfs_request_q->qset[REQ_Q_HIGH_LATENCY]);
			break;
		default:
			/* not here */
			abort();
			break;
		}

		LogFullDebug(COMPONENT_DISPATCH,
			     "dequeue_req try qpair %s %p:%p", qpair->s,
			     &qpair->producer, &qpair->consumer);

		/* anything? */
		reqdata = nfs_rpc_consume_req(qpair);
		if (reqdata) {
			(void) atomic_inc_uint32_t(&dequeued_reqs);
			break;
		}

		++slot;
		slot = slot % 4;

	}			/* for */

	/* wait */
	if (!reqdata) {
		struct fridgethr_context *ctx =
			container_of(worker, struct fridgethr_context, wd);
		wait_q_entry_t *wqe = &worker->wqe;

		assert(wqe->waiters == 0); /* wqe is not on any wait queue */
		PTHREAD_MUTEX_lock(&wqe->lwe.mtx);
		wqe->flags = Wqe_LFlag_WaitSync;
		wqe->waiters = 1;
		/* XXX functionalize */
		pthread_spin_lock(&nfs_req_st.reqs.sp);
		glist_add_tail(&nfs_req_st.reqs.wait_list, &wqe->waitq);
		++(nfs_req_st.reqs.waiters);
		pthread_spin_unlock(&nfs_req_st.reqs.sp);
		while (!(wqe->flags & Wqe_LFlag_SyncDone)) {
			timeout.tv_sec = time(NULL) + 5;
			timeout.tv_nsec = 0;
			pthread_cond_timedwait(&wqe->lwe.cv, &wqe->lwe.mtx,
					       &timeout);
			if (fridgethr_you_should_break(ctx)) {
				/* We are returning;
				 * so take us out of the waitq */
				pthread_spin_lock(&nfs_req_st.reqs.sp);
				if (wqe->waitq.next != NULL
				    || wqe->waitq.prev != NULL) {
					/* Element is still in wqitq,
					 * remove it */
					glist_del(&wqe->waitq);
					--(nfs_req_st.reqs.waiters);
					--(wqe->waiters);
					wqe->flags &=
					    ~(Wqe_LFlag_WaitSync |
					      Wqe_LFlag_SyncDone);
				}
				pthread_spin_unlock(&nfs_req_st.reqs.sp);
				PTHREAD_MUTEX_unlock(&wqe->lwe.mtx);
				return NULL;
			}
		}

		/* XXX wqe was removed from nfs_req_st.waitq
		 * (by signalling thread) */
		wqe->flags &= ~(Wqe_LFlag_WaitSync | Wqe_LFlag_SyncDone);
		PTHREAD_MUTEX_unlock(&wqe->lwe.mtx);
		LogFullDebug(COMPONENT_DISPATCH, "wqe wakeup %p", wqe);
		goto retry_deq;
	} /* !reqdata */

#if defined(HAVE_BLKIN)
	/* thread id */
	BLKIN_KEYVAL_INTEGER(
		&reqdata->r_u.req.svc.bl_trace,
		&reqdata->r_u.req.xprt->blkin.endp,
		"worker-id",
		worker->worker_index
		);

	BLKIN_TIMESTAMP(
		&reqdata->r_u.req.svc.bl_trace,
		&reqdata->r_u.req.xprt->blkin.endp,
		"dequeue-req");
#endif
	return reqdata;
}

/**
 * @brief Allocate a new request
 *
 * @param[in] xprt Transport to use
 *
 * @return New request data
 */
static inline request_data_t *alloc_nfs_request(SVCXPRT *xprt, XDR *xdrs)
{
	request_data_t *reqdata = pool_alloc(request_pool);

	/* set the request as NFS already-read */
	reqdata->rtype = NFS_REQUEST;

	/* set up req */
	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	reqdata->r_u.req.svc.rq_xprt = xprt;
	reqdata->r_u.req.svc.rq_xdrs = xdrs;

	reqdata->r_d_refs = 1;
	return reqdata;
}

int free_nfs_request(request_data_t *reqdata)
{
	SVCXPRT *xprt = reqdata->r_u.req.svc.rq_xprt;
	uint32_t refs = atomic_dec_uint32_t(&reqdata->r_d_refs);

	LogDebug(COMPONENT_DISPATCH,
		 "%s: %p fd %d xp_refs %" PRIu32 " r_d_refs %" PRIu32,
		 __func__,
		 xprt, xprt->xp_fd, xprt->xp_refs,
		 refs);

	if (refs)
		return refs;

	switch (reqdata->rtype) {
	case NFS_REQUEST:
		/* dispose RPC header */
		if (reqdata->r_u.req.svc.rq_auth)
			SVCAUTH_RELEASE(&(reqdata->r_u.req.svc));
		XDR_DESTROY(reqdata->r_u.req.svc.rq_xdrs);
		break;
	default:
		break;
	}
	SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
	pool_free(request_pool, reqdata);
	return 0;
}

static enum xprt_stat nfs_rpc_decode_request(SVCXPRT *xprt, XDR *xdrs)
{
	request_data_t *reqdata;
	enum xprt_stat stat;

	if (!xprt) {
		LogFatal(COMPONENT_DISPATCH,
			 "missing xprt!");
		return XPRT_DIED;
	}
	if (!xdrs) {
		LogFatal(COMPONENT_DISPATCH,
			 "missing xdrs!");
		return XPRT_DIED;
	}
	LogDebug(COMPONENT_DISPATCH,
		 "%p fd %d context %p",
		 xprt, xprt->xp_fd, xdrs);

	reqdata = alloc_nfs_request(xprt, xdrs);
#if HAVE_BLKIN
	blkin_init_new_trace(&reqdata->r_u.req.svc.bl_trace, "nfs-ganesha",
			&xprt->blkin.endp);
#endif

#if defined(HAVE_BLKIN)
	BLKIN_TIMESTAMP(
		&reqdata->r_u.req.svc.bl_trace, &xprt->blkin.endp, "pre-recv");
#endif

	stat = SVC_DECODE(&reqdata->r_u.req.svc);

#if defined(HAVE_BLKIN)
	BLKIN_TIMESTAMP(
		&reqdata->r_u.req.svc.bl_trace, &xprt->blkin.endp, "post-recv");

	BLKIN_KEYVAL_INTEGER(
		&reqdata->r_u.req.svc.bl_trace,
		&reqdata->r_u.req.xprt->blkin.endp,
		"rq-xid",
		reqdata->r_u.req.svc.rq_xid);
#endif

	if (unlikely(stat > XPRT_DESTROYED)) {
		LogInfo(COMPONENT_DISPATCH,
			"SVC_DECODE on %p fd %d returned unknown %u",
			xprt, xprt->xp_fd,
			stat);
	} else {
		sockaddr_t addr;
		char addrbuf[SOCK_NAME_MAX + 1];

		if (isDebug(COMPONENT_DISPATCH)) {
			if (copy_xprt_addr(&addr, xprt) == 1)
				sprint_sockaddr(&addr, addrbuf,
						sizeof(addrbuf));
			else
				sprintf(addrbuf, "<unresolved>");
		}

		LogDebug(COMPONENT_DISPATCH,
			 "SVC_DECODE on %p fd %d (%s) xid=%" PRIu32
			 " returned %s",
			 xprt, xprt->xp_fd,
			 addrbuf,
			 reqdata->r_u.req.svc.rq_msg.rm_xid,
			 xprt_stat_s[stat]);
	}

	/* refresh status before possible release */
	stat = SVC_STAT(xprt);
	free_nfs_request(reqdata);
	return stat;
}

enum xprt_stat nfs_rpc_process_request(request_data_t *reqdata)
{
	const nfs_function_desc_t *reqdesc = reqdata->r_u.req.funcdesc;
	nfs_arg_t *arg_nfs = &reqdata->r_u.req.arg_nfs;
	SVCXPRT *xprt = reqdata->r_u.req.svc.rq_xprt;
	XDR *xdrs = reqdata->r_u.req.svc.rq_xdrs;
	enum auth_stat why;
	bool no_dispatch = false;

	LogFullDebug(COMPONENT_DISPATCH,
		     "About to authenticate Prog=%" PRIu32
		     ", vers=%" PRIu32
		     ", proc=%" PRIu32
		     ", xid=%" PRIu32
		     ", SVCXPRT=%p, fd=%d",
		     reqdata->r_u.req.svc.rq_msg.cb_prog,
		     reqdata->r_u.req.svc.rq_msg.cb_vers,
		     reqdata->r_u.req.svc.rq_msg.cb_proc,
		     reqdata->r_u.req.svc.rq_msg.rm_xid,
		     xprt, xprt->xp_fd);

	/* If authentication is AUTH_NONE or AUTH_UNIX, then the value of
	 * no_dispatch remains false and the request proceeds normally.
	 *
	 * If authentication is RPCSEC_GSS, no_dispatch may have value true,
	 * this means that gc->gc_proc != RPCSEC_GSS_DATA and that the message
	 * is in fact an internal negotiation message from RPCSEC_GSS using
	 * GSSAPI. It should not be processed by the worker and SVC_STAT
	 * should be returned to the dispatcher.
	 */
	why = svc_auth_authenticate(&reqdata->r_u.req.svc, &no_dispatch);
	if (why != AUTH_OK) {
		LogInfo(COMPONENT_DISPATCH,
			"Could not authenticate request... rejecting with AUTH_STAT=%s",
			auth_stat2str(why));
		return svcerr_auth(&reqdata->r_u.req.svc, why);
#ifdef _HAVE_GSSAPI
	} else if (reqdata->r_u.req.svc.rq_msg.RPCM_ack.ar_verf.oa_flavor
		   == RPCSEC_GSS) {
		struct rpc_gss_cred *gc = (struct rpc_gss_cred *)
			reqdata->r_u.req.svc.rq_msg.rq_cred_body;

		LogFullDebug(COMPONENT_DISPATCH,
			     "RPCSEC_GSS no_dispatch=%d"
			     " gc->gc_proc=(%" PRIu32 ") %s",
			     no_dispatch, gc->gc_proc,
			     str_gc_proc(gc->gc_proc));
		if (no_dispatch)
			return SVC_STAT(xprt);
#endif
	}

	/*
	 * Extract RPC argument.
	 */
	LogFullDebug(COMPONENT_DISPATCH,
		     "Before SVCAUTH_CHECKSUM on SVCXPRT %p fd %d",
		     xprt, xprt->xp_fd);

	memset(arg_nfs, 0, sizeof(nfs_arg_t));
	reqdata->r_u.req.svc.rq_msg.rm_xdr.where = (caddr_t) arg_nfs;
	reqdata->r_u.req.svc.rq_msg.rm_xdr.proc = reqdesc->xdr_decode_func;
	xdrs->x_public = &reqdata->r_u.req.lookahead;

	if (!SVCAUTH_CHECKSUM(&reqdata->r_u.req.svc)) {
		LogInfo(COMPONENT_DISPATCH,
			"SVCAUTH_CHECKSUM failed for Program %" PRIu32
			", Version %" PRIu32
			", Function %" PRIu32
			", xid=%" PRIu32
			", SVCXPRT=%p, fd=%d",
			reqdata->r_u.req.svc.rq_msg.cb_prog,
			reqdata->r_u.req.svc.rq_msg.cb_vers,
			reqdata->r_u.req.svc.rq_msg.cb_proc,
			reqdata->r_u.req.svc.rq_msg.rm_xid,
			xprt, xprt->xp_fd);

		if (!xdr_free(reqdesc->xdr_decode_func, (caddr_t) arg_nfs)) {
			LogCrit(COMPONENT_DISPATCH,
				"%s FAILURE: Bad xdr_free for %s",
				__func__,
				reqdesc->funcname);
		}
		return svcerr_decode(&reqdata->r_u.req.svc);
	}

	atomic_inc_uint32_t(&reqdata->r_d_refs);
	nfs_rpc_enqueue_req(reqdata);
	return SVC_STAT(xprt);
}
