/* AF_RXRPC sendmsg() implementation.
 *
 * Copyright (C) 2007, 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/net.h>
#include <linux/gfp.h>
#include <linux/skbuff.h>
#include <linux/export.h>
#include <linux/circ_buf.h>
#include <net/sock.h>
#include <net/af_rxrpc.h>
#include "ar-internal.h"

enum rxrpc_command {
	RXRPC_CMD_SEND_DATA,		/* send data message */
	RXRPC_CMD_SEND_ABORT,		/* request abort generation */
	RXRPC_CMD_ACCEPT,		/* [server] accept incoming call */
	RXRPC_CMD_REJECT_BUSY,		/* [server] reject a call as busy */
};

/*
 * wait for space to appear in the transmit/ACK window
 * - caller holds the socket locked
 */
static int rxrpc_wait_for_tx_window(struct rxrpc_sock *rx,
				    struct rxrpc_call *call,
				    long *timeo)
{
	DECLARE_WAITQUEUE(myself, current);
	int ret;

	_enter(",{%d},%ld",
	       CIRC_SPACE(call->acks_head, ACCESS_ONCE(call->acks_tail),
			  call->acks_winsz),
	       *timeo);

	add_wait_queue(&call->waitq, &myself);

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		ret = 0;
		if (CIRC_SPACE(call->acks_head, ACCESS_ONCE(call->acks_tail),
			       call->acks_winsz) > 0)
			break;
		if (signal_pending(current)) {
			ret = sock_intr_errno(*timeo);
			break;
		}

		release_sock(&rx->sk);
		*timeo = schedule_timeout(*timeo);
		lock_sock(&rx->sk);
	}

	remove_wait_queue(&call->waitq, &myself);
	set_current_state(TASK_RUNNING);
	_leave(" = %d", ret);
	return ret;
}

/*
 * attempt to schedule an instant Tx resend
 */
static inline void rxrpc_instant_resend(struct rxrpc_call *call)
{
	read_lock_bh(&call->state_lock);
	if (try_to_del_timer_sync(&call->resend_timer) >= 0) {
		clear_bit(RXRPC_CALL_RUN_RTIMER, &call->flags);
		if (call->state < RXRPC_CALL_COMPLETE &&
		    !test_and_set_bit(RXRPC_CALL_EV_RESEND_TIMER, &call->events))
			rxrpc_queue_call(call);
	}
	read_unlock_bh(&call->state_lock);
}

/*
 * queue a packet for transmission, set the resend timer and attempt
 * to send the packet immediately
 */
static void rxrpc_queue_packet(struct rxrpc_call *call, struct sk_buff *skb,
			       bool last)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	int ret;

	_net("queue skb %p [%d]", skb, call->acks_head);

	ASSERT(call->acks_window != NULL);
	call->acks_window[call->acks_head] = (unsigned long) skb;
	smp_wmb();
	call->acks_head = (call->acks_head + 1) & (call->acks_winsz - 1);

	if (last || call->state == RXRPC_CALL_SERVER_ACK_REQUEST) {
		_debug("________awaiting reply/ACK__________");
		write_lock_bh(&call->state_lock);
		switch (call->state) {
		case RXRPC_CALL_CLIENT_SEND_REQUEST:
			call->state = RXRPC_CALL_CLIENT_AWAIT_REPLY;
			break;
		case RXRPC_CALL_SERVER_ACK_REQUEST:
			call->state = RXRPC_CALL_SERVER_SEND_REPLY;
			if (!last)
				break;
		case RXRPC_CALL_SERVER_SEND_REPLY:
			call->state = RXRPC_CALL_SERVER_AWAIT_ACK;
			break;
		default:
			break;
		}
		write_unlock_bh(&call->state_lock);
	}

	_proto("Tx DATA %%%u { #%u }", sp->hdr.serial, sp->hdr.seq);

	sp->need_resend = false;
	sp->resend_at = jiffies + rxrpc_resend_timeout;
	if (!test_and_set_bit(RXRPC_CALL_RUN_RTIMER, &call->flags)) {
		_debug("run timer");
		call->resend_timer.expires = sp->resend_at;
		add_timer(&call->resend_timer);
	}

	/* attempt to cancel the rx-ACK timer, deferring reply transmission if
	 * we're ACK'ing the request phase of an incoming call */
	ret = -EAGAIN;
	if (try_to_del_timer_sync(&call->ack_timer) >= 0) {
		/* the packet may be freed by rxrpc_process_call() before this
		 * returns */
		if (rxrpc_is_client_call(call))
			rxrpc_expose_client_call(call);
		ret = rxrpc_send_data_packet(call->conn, skb);
		_net("sent skb %p", skb);
	} else {
		_debug("failed to delete ACK timer");
	}

	if (ret < 0) {
		_debug("need instant resend %d", ret);
		sp->need_resend = true;
		rxrpc_instant_resend(call);
	}

	_leave("");
}

/*
 * Convert a host-endian header into a network-endian header.
 */
static void rxrpc_insert_header(struct sk_buff *skb)
{
	struct rxrpc_wire_header whdr;
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);

	whdr.epoch	= htonl(sp->hdr.epoch);
	whdr.cid	= htonl(sp->hdr.cid);
	whdr.callNumber	= htonl(sp->hdr.callNumber);
	whdr.seq	= htonl(sp->hdr.seq);
	whdr.serial	= htonl(sp->hdr.serial);
	whdr.type	= sp->hdr.type;
	whdr.flags	= sp->hdr.flags;
	whdr.userStatus	= sp->hdr.userStatus;
	whdr.securityIndex = sp->hdr.securityIndex;
	whdr._rsvd	= htons(sp->hdr._rsvd);
	whdr.serviceId	= htons(sp->hdr.serviceId);

	memcpy(skb->head, &whdr, sizeof(whdr));
}

/*
 * send data through a socket
 * - must be called in process context
 * - caller holds the socket locked
 */
static int rxrpc_send_data(struct rxrpc_sock *rx,
			   struct rxrpc_call *call,
			   struct msghdr *msg, size_t len)
{
	struct rxrpc_skb_priv *sp;
	struct sk_buff *skb;
	struct sock *sk = &rx->sk;
	long timeo;
	bool more;
	int ret, copied;

	timeo = sock_sndtimeo(sk, msg->msg_flags & MSG_DONTWAIT);

	/* this should be in poll */
	sk_clear_bit(SOCKWQ_ASYNC_NOSPACE, sk);

	if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
		return -EPIPE;

	more = msg->msg_flags & MSG_MORE;

	skb = call->tx_pending;
	call->tx_pending = NULL;
	rxrpc_see_skb(skb);

	copied = 0;
	do {
		if (!skb) {
			size_t size, chunk, max, space;

			_debug("alloc");

			if (CIRC_SPACE(call->acks_head,
				       ACCESS_ONCE(call->acks_tail),
				       call->acks_winsz) <= 0) {
				ret = -EAGAIN;
				if (msg->msg_flags & MSG_DONTWAIT)
					goto maybe_error;
				ret = rxrpc_wait_for_tx_window(rx, call,
							       &timeo);
				if (ret < 0)
					goto maybe_error;
			}

			max = call->conn->params.peer->maxdata;
			max -= call->conn->security_size;
			max &= ~(call->conn->size_align - 1UL);

			chunk = max;
			if (chunk > msg_data_left(msg) && !more)
				chunk = msg_data_left(msg);

			space = chunk + call->conn->size_align;
			space &= ~(call->conn->size_align - 1UL);

			size = space + call->conn->header_size;

			_debug("SIZE: %zu/%zu/%zu", chunk, space, size);

			/* create a buffer that we can retain until it's ACK'd */
			skb = sock_alloc_send_skb(
				sk, size, msg->msg_flags & MSG_DONTWAIT, &ret);
			if (!skb)
				goto maybe_error;

			rxrpc_new_skb(skb);

			_debug("ALLOC SEND %p", skb);

			ASSERTCMP(skb->mark, ==, 0);

			_debug("HS: %u", call->conn->header_size);
			skb_reserve(skb, call->conn->header_size);
			skb->len += call->conn->header_size;

			sp = rxrpc_skb(skb);
			sp->remain = chunk;
			if (sp->remain > skb_tailroom(skb))
				sp->remain = skb_tailroom(skb);

			_net("skb: hr %d, tr %d, hl %d, rm %d",
			       skb_headroom(skb),
			       skb_tailroom(skb),
			       skb_headlen(skb),
			       sp->remain);

			skb->ip_summed = CHECKSUM_UNNECESSARY;
		}

		_debug("append");
		sp = rxrpc_skb(skb);

		/* append next segment of data to the current buffer */
		if (msg_data_left(msg) > 0) {
			int copy = skb_tailroom(skb);
			ASSERTCMP(copy, >, 0);
			if (copy > msg_data_left(msg))
				copy = msg_data_left(msg);
			if (copy > sp->remain)
				copy = sp->remain;

			_debug("add");
			ret = skb_add_data(skb, &msg->msg_iter, copy);
			_debug("added");
			if (ret < 0)
				goto efault;
			sp->remain -= copy;
			skb->mark += copy;
			copied += copy;
		}

		/* check for the far side aborting the call or a network error
		 * occurring */
		if (call->state == RXRPC_CALL_COMPLETE)
			goto call_terminated;

		/* add the packet to the send queue if it's now full */
		if (sp->remain <= 0 ||
		    (msg_data_left(msg) == 0 && !more)) {
			struct rxrpc_connection *conn = call->conn;
			uint32_t seq;
			size_t pad;

			/* pad out if we're using security */
			if (conn->security_ix) {
				pad = conn->security_size + skb->mark;
				pad = conn->size_align - pad;
				pad &= conn->size_align - 1;
				_debug("pad %zu", pad);
				if (pad)
					memset(skb_put(skb, pad), 0, pad);
			}

			seq = atomic_inc_return(&call->sequence);

			sp->hdr.epoch	= conn->proto.epoch;
			sp->hdr.cid	= call->cid;
			sp->hdr.callNumber = call->call_id;
			sp->hdr.seq	= seq;
			sp->hdr.serial	= atomic_inc_return(&conn->serial);
			sp->hdr.type	= RXRPC_PACKET_TYPE_DATA;
			sp->hdr.userStatus = 0;
			sp->hdr.securityIndex = call->security_ix;
			sp->hdr._rsvd	= 0;
			sp->hdr.serviceId = call->service_id;

			sp->hdr.flags = conn->out_clientflag;
			if (msg_data_left(msg) == 0 && !more)
				sp->hdr.flags |= RXRPC_LAST_PACKET;
			else if (CIRC_SPACE(call->acks_head,
					    ACCESS_ONCE(call->acks_tail),
					    call->acks_winsz) > 1)
				sp->hdr.flags |= RXRPC_MORE_PACKETS;
			if (more && seq & 1)
				sp->hdr.flags |= RXRPC_REQUEST_ACK;

			ret = conn->security->secure_packet(
				call, skb, skb->mark,
				skb->head + sizeof(struct rxrpc_wire_header));
			if (ret < 0)
				goto out;

			rxrpc_insert_header(skb);
			rxrpc_queue_packet(call, skb, !msg_data_left(msg) && !more);
			skb = NULL;
		}
	} while (msg_data_left(msg) > 0);

success:
	ret = copied;
out:
	call->tx_pending = skb;
	_leave(" = %d", ret);
	return ret;

call_terminated:
	rxrpc_free_skb(skb);
	_leave(" = %d", -call->error);
	return ret;

maybe_error:
	if (copied)
		goto success;
	goto out;

efault:
	ret = -EFAULT;
	goto out;
}

/*
 * extract control messages from the sendmsg() control buffer
 */
static int rxrpc_sendmsg_cmsg(struct msghdr *msg,
			      unsigned long *user_call_ID,
			      enum rxrpc_command *command,
			      u32 *abort_code,
			      bool *_exclusive)
{
	struct cmsghdr *cmsg;
	bool got_user_ID = false;
	int len;

	*command = RXRPC_CMD_SEND_DATA;

	if (msg->msg_controllen == 0)
		return -EINVAL;

	for_each_cmsghdr(cmsg, msg) {
		if (!CMSG_OK(msg, cmsg))
			return -EINVAL;

		len = cmsg->cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr));
		_debug("CMSG %d, %d, %d",
		       cmsg->cmsg_level, cmsg->cmsg_type, len);

		if (cmsg->cmsg_level != SOL_RXRPC)
			continue;

		switch (cmsg->cmsg_type) {
		case RXRPC_USER_CALL_ID:
			if (msg->msg_flags & MSG_CMSG_COMPAT) {
				if (len != sizeof(u32))
					return -EINVAL;
				*user_call_ID = *(u32 *) CMSG_DATA(cmsg);
			} else {
				if (len != sizeof(unsigned long))
					return -EINVAL;
				*user_call_ID = *(unsigned long *)
					CMSG_DATA(cmsg);
			}
			_debug("User Call ID %lx", *user_call_ID);
			got_user_ID = true;
			break;

		case RXRPC_ABORT:
			if (*command != RXRPC_CMD_SEND_DATA)
				return -EINVAL;
			*command = RXRPC_CMD_SEND_ABORT;
			if (len != sizeof(*abort_code))
				return -EINVAL;
			*abort_code = *(unsigned int *) CMSG_DATA(cmsg);
			_debug("Abort %x", *abort_code);
			if (*abort_code == 0)
				return -EINVAL;
			break;

		case RXRPC_ACCEPT:
			if (*command != RXRPC_CMD_SEND_DATA)
				return -EINVAL;
			*command = RXRPC_CMD_ACCEPT;
			if (len != 0)
				return -EINVAL;
			break;

		case RXRPC_EXCLUSIVE_CALL:
			*_exclusive = true;
			if (len != 0)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
	}

	if (!got_user_ID)
		return -EINVAL;
	_leave(" = 0");
	return 0;
}

/*
 * abort a call, sending an ABORT packet to the peer
 */
static void rxrpc_send_abort(struct rxrpc_call *call, const char *why,
			     u32 abort_code, int error)
{
	if (call->state >= RXRPC_CALL_COMPLETE)
		return;

	write_lock_bh(&call->state_lock);

	if (__rxrpc_abort_call(why, call, 0, abort_code, error)) {
		del_timer_sync(&call->resend_timer);
		del_timer_sync(&call->ack_timer);
		clear_bit(RXRPC_CALL_EV_RESEND_TIMER, &call->events);
		clear_bit(RXRPC_CALL_EV_ACK, &call->events);
		clear_bit(RXRPC_CALL_RUN_RTIMER, &call->flags);
		rxrpc_queue_call(call);
	}

	write_unlock_bh(&call->state_lock);
}

/*
 * Create a new client call for sendmsg().
 */
static struct rxrpc_call *
rxrpc_new_client_call_for_sendmsg(struct rxrpc_sock *rx, struct msghdr *msg,
				  unsigned long user_call_ID, bool exclusive)
{
	struct rxrpc_conn_parameters cp;
	struct rxrpc_call *call;
	struct key *key;

	DECLARE_SOCKADDR(struct sockaddr_rxrpc *, srx, msg->msg_name);

	_enter("");

	if (!msg->msg_name)
		return ERR_PTR(-EDESTADDRREQ);

	key = rx->key;
	if (key && !rx->key->payload.data[0])
		key = NULL;

	memset(&cp, 0, sizeof(cp));
	cp.local		= rx->local;
	cp.key			= rx->key;
	cp.security_level	= rx->min_sec_level;
	cp.exclusive		= rx->exclusive | exclusive;
	cp.service_id		= srx->srx_service;
	call = rxrpc_new_client_call(rx, &cp, srx, user_call_ID, GFP_KERNEL);

	_leave(" = %p\n", call);
	return call;
}

/*
 * send a message forming part of a client call through an RxRPC socket
 * - caller holds the socket locked
 * - the socket may be either a client socket or a server socket
 */
int rxrpc_do_sendmsg(struct rxrpc_sock *rx, struct msghdr *msg, size_t len)
{
	enum rxrpc_command cmd;
	struct rxrpc_call *call;
	unsigned long user_call_ID = 0;
	bool exclusive = false;
	u32 abort_code = 0;
	int ret;

	_enter("");

	ret = rxrpc_sendmsg_cmsg(msg, &user_call_ID, &cmd, &abort_code,
				 &exclusive);
	if (ret < 0)
		return ret;

	if (cmd == RXRPC_CMD_ACCEPT) {
		if (rx->sk.sk_state != RXRPC_SERVER_LISTENING)
			return -EINVAL;
		call = rxrpc_accept_call(rx, user_call_ID, NULL);
		if (IS_ERR(call))
			return PTR_ERR(call);
		rxrpc_put_call(call, rxrpc_call_put);
		return 0;
	}

	call = rxrpc_find_call_by_user_ID(rx, user_call_ID);
	if (!call) {
		if (cmd != RXRPC_CMD_SEND_DATA)
			return -EBADSLT;
		call = rxrpc_new_client_call_for_sendmsg(rx, msg, user_call_ID,
							 exclusive);
		if (IS_ERR(call))
			return PTR_ERR(call);
	}

	rxrpc_see_call(call);
	_debug("CALL %d USR %lx ST %d on CONN %p",
	       call->debug_id, call->user_call_ID, call->state, call->conn);

	if (call->state >= RXRPC_CALL_COMPLETE) {
		/* it's too late for this call */
		ret = -ESHUTDOWN;
	} else if (cmd == RXRPC_CMD_SEND_ABORT) {
		rxrpc_send_abort(call, "CMD", abort_code, ECONNABORTED);
		ret = 0;
	} else if (cmd != RXRPC_CMD_SEND_DATA) {
		ret = -EINVAL;
	} else if (rxrpc_is_client_call(call) &&
		   call->state != RXRPC_CALL_CLIENT_SEND_REQUEST) {
		/* request phase complete for this client call */
		ret = -EPROTO;
	} else if (rxrpc_is_service_call(call) &&
		   call->state != RXRPC_CALL_SERVER_ACK_REQUEST &&
		   call->state != RXRPC_CALL_SERVER_SEND_REPLY) {
		/* Reply phase not begun or not complete for service call. */
		ret = -EPROTO;
	} else {
		ret = rxrpc_send_data(rx, call, msg, len);
	}

	rxrpc_put_call(call, rxrpc_call_put);
	_leave(" = %d", ret);
	return ret;
}

/**
 * rxrpc_kernel_send_data - Allow a kernel service to send data on a call
 * @sock: The socket the call is on
 * @call: The call to send data through
 * @msg: The data to send
 * @len: The amount of data to send
 *
 * Allow a kernel service to send data on a call.  The call must be in an state
 * appropriate to sending data.  No control data should be supplied in @msg,
 * nor should an address be supplied.  MSG_MORE should be flagged if there's
 * more data to come, otherwise this data will end the transmission phase.
 */
int rxrpc_kernel_send_data(struct socket *sock, struct rxrpc_call *call,
			   struct msghdr *msg, size_t len)
{
	int ret;

	_enter("{%d,%s},", call->debug_id, rxrpc_call_states[call->state]);

	ASSERTCMP(msg->msg_name, ==, NULL);
	ASSERTCMP(msg->msg_control, ==, NULL);

	lock_sock(sock->sk);

	_debug("CALL %d USR %lx ST %d on CONN %p",
	       call->debug_id, call->user_call_ID, call->state, call->conn);

	if (call->state >= RXRPC_CALL_COMPLETE) {
		ret = -ESHUTDOWN; /* it's too late for this call */
	} else if (call->state != RXRPC_CALL_CLIENT_SEND_REQUEST &&
		   call->state != RXRPC_CALL_SERVER_ACK_REQUEST &&
		   call->state != RXRPC_CALL_SERVER_SEND_REPLY) {
		ret = -EPROTO; /* request phase complete for this client call */
	} else {
		ret = rxrpc_send_data(rxrpc_sk(sock->sk), call, msg, len);
	}

	release_sock(sock->sk);
	_leave(" = %d", ret);
	return ret;
}
EXPORT_SYMBOL(rxrpc_kernel_send_data);

/**
 * rxrpc_kernel_abort_call - Allow a kernel service to abort a call
 * @sock: The socket the call is on
 * @call: The call to be aborted
 * @abort_code: The abort code to stick into the ABORT packet
 * @error: Local error value
 * @why: 3-char string indicating why.
 *
 * Allow a kernel service to abort a call, if it's still in an abortable state.
 */
void rxrpc_kernel_abort_call(struct socket *sock, struct rxrpc_call *call,
			     u32 abort_code, int error, const char *why)
{
	_enter("{%d},%d,%d,%s", call->debug_id, abort_code, error, why);

	lock_sock(sock->sk);

	rxrpc_send_abort(call, why, abort_code, error);

	release_sock(sock->sk);
	_leave("");
}

EXPORT_SYMBOL(rxrpc_kernel_abort_call);
