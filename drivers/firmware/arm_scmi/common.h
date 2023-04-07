/* SPDX-License-Identifier: GPL-2.0 */
/*
 * System Control and Management Interface (SCMI) Message Protocol
 * driver common header file containing some definitions, structures
 * and function prototypes used in all the different SCMI protocols.
 *
 * Copyright (C) 2018-2022 ARM Ltd.
 */
#ifndef _SCMI_COMMON_H
#define _SCMI_COMMON_H

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/refcount.h>
#include <linux/scmi_protocol.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <asm/unaligned.h>

#include "protocols.h"
#include "notify.h"

#define MSG_ID_MASK		GENMASK(7, 0)
#define MSG_XTRACT_ID(hdr)	FIELD_GET(MSG_ID_MASK, (hdr))
#define MSG_TYPE_MASK		GENMASK(9, 8)
#define MSG_XTRACT_TYPE(hdr)	FIELD_GET(MSG_TYPE_MASK, (hdr))
#define MSG_TYPE_COMMAND	0
#define MSG_TYPE_DELAYED_RESP	2
#define MSG_TYPE_NOTIFICATION	3
#define MSG_PROTOCOL_ID_MASK	GENMASK(17, 10)
#define MSG_XTRACT_PROT_ID(hdr)	FIELD_GET(MSG_PROTOCOL_ID_MASK, (hdr))
#define MSG_TOKEN_ID_MASK	GENMASK(27, 18)
#define MSG_XTRACT_TOKEN(hdr)	FIELD_GET(MSG_TOKEN_ID_MASK, (hdr))
#define MSG_TOKEN_MAX		(MSG_XTRACT_TOKEN(MSG_TOKEN_ID_MASK) + 1)

/*
 * Size of @pending_xfers hashtable included in @scmi_xfers_info; ideally, in
 * order to minimize space and collisions, this should equal max_msg, i.e. the
 * maximum number of in-flight messages on a specific platform, but such value
 * is only available at runtime while kernel hashtables are statically sized:
 * pick instead as a fixed static size the maximum number of entries that can
 * fit the whole table into one 4k page.
 */
#define SCMI_PENDING_XFERS_HT_ORDER_SZ		9

/**
 * pack_scmi_header() - packs and returns 32-bit header
 *
 * @hdr: pointer to header containing all the information on message id,
 *	protocol id, sequence id and type.
 *
 * Return: 32-bit packed message header to be sent to the platform.
 */
static inline u32 pack_scmi_header(struct scmi_msg_hdr *hdr)
{
	return FIELD_PREP(MSG_ID_MASK, hdr->id) |
		FIELD_PREP(MSG_TYPE_MASK, hdr->type) |
		FIELD_PREP(MSG_TOKEN_ID_MASK, hdr->seq) |
		FIELD_PREP(MSG_PROTOCOL_ID_MASK, hdr->protocol_id);
}

/**
 * unpack_scmi_header() - unpacks and records message and protocol id
 *
 * @msg_hdr: 32-bit packed message header sent from the platform
 * @hdr: pointer to header to fetch message and protocol id.
 */
static inline void unpack_scmi_header(u32 msg_hdr, struct scmi_msg_hdr *hdr)
{
	hdr->id = MSG_XTRACT_ID(msg_hdr);
	hdr->protocol_id = MSG_XTRACT_PROT_ID(msg_hdr);
	hdr->type = MSG_XTRACT_TYPE(msg_hdr);
}

/*
 * An helper macro to lookup an xfer from the @pending_xfers hashtable
 * using the message sequence number token as a key.
 */
#define XFER_FIND(__ht, __k)					\
({								\
	typeof(__k) k_ = __k;					\
	struct scmi_xfer *xfer_ = NULL;				\
								\
	hash_for_each_possible((__ht), xfer_, node, k_)		\
		if (xfer_->hdr.seq == k_)			\
			break;					\
	xfer_;							\
})

struct scmi_revision_info *
scmi_revision_area_get(const struct scmi_protocol_handle *ph);
int scmi_handle_put(const struct scmi_handle *handle);
void scmi_device_link_add(struct device *consumer, struct device *supplier);
struct scmi_handle *scmi_handle_get(struct device *dev);
void scmi_set_handle(struct scmi_device *scmi_dev);
void scmi_setup_protocol_implemented(const struct scmi_protocol_handle *ph,
				     u8 *prot_imp);

int __init scmi_bus_init(void);
void __exit scmi_bus_exit(void);

const struct scmi_protocol *scmi_protocol_get(int protocol_id);
void scmi_protocol_put(int protocol_id);

int scmi_protocol_acquire(const struct scmi_handle *handle, u8 protocol_id);
void scmi_protocol_release(const struct scmi_handle *handle, u8 protocol_id);

/* SCMI Transport */
/**
 * struct scmi_chan_info - Structure representing a SCMI channel information
 *
 * @dev: Reference to device in the SCMI hierarchy corresponding to this
 *	 channel
 * @rx_timeout_ms: The configured RX timeout in milliseconds.
 * @handle: Pointer to SCMI entity handle
 * @no_completion_irq: Flag to indicate that this channel has no completion
 *		       interrupt mechanism for synchronous commands.
 *		       This can be dynamically set by transports at run-time
 *		       inside their provided .chan_setup().
 * @transport_info: Transport layer related information
 */
struct scmi_chan_info {
	struct device *dev;
	unsigned int rx_timeout_ms;
	struct scmi_handle *handle;
	bool no_completion_irq;
	void *transport_info;
};

/**
 * struct scmi_transport_ops - Structure representing a SCMI transport ops
 *
 * @link_supplier: Optional callback to add link to a supplier device
 * @chan_available: Callback to check if channel is available or not
 * @chan_setup: Callback to allocate and setup a channel
 * @chan_free: Callback to free a channel
 * @get_max_msg: Optional callback to provide max_msg dynamically
 *		 Returns the maximum number of messages for the channel type
 *		 (tx or rx) that can be pending simultaneously in the system
 * @send_message: Callback to send a message
 * @mark_txdone: Callback to mark tx as done
 * @fetch_response: Callback to fetch response
 * @fetch_notification: Callback to fetch notification
 * @clear_channel: Callback to clear a channel
 * @poll_done: Callback to poll transfer status
 */
struct scmi_transport_ops {
	int (*link_supplier)(struct device *dev);
	bool (*chan_available)(struct device *dev, int idx);
	int (*chan_setup)(struct scmi_chan_info *cinfo, struct device *dev,
			  bool tx);
	int (*chan_free)(int id, void *p, void *data);
	unsigned int (*get_max_msg)(struct scmi_chan_info *base_cinfo);
	int (*send_message)(struct scmi_chan_info *cinfo,
			    struct scmi_xfer *xfer);
	void (*mark_txdone)(struct scmi_chan_info *cinfo, int ret,
			    struct scmi_xfer *xfer);
	void (*fetch_response)(struct scmi_chan_info *cinfo,
			       struct scmi_xfer *xfer);
	void (*fetch_notification)(struct scmi_chan_info *cinfo,
				   size_t max_len, struct scmi_xfer *xfer);
	void (*clear_channel)(struct scmi_chan_info *cinfo);
	bool (*poll_done)(struct scmi_chan_info *cinfo, struct scmi_xfer *xfer);
};

int scmi_protocol_device_request(const struct scmi_device_id *id_table);
void scmi_protocol_device_unrequest(const struct scmi_device_id *id_table);
struct scmi_device *scmi_child_dev_find(struct device *parent,
					int prot_id, const char *name);

/**
 * struct scmi_desc - Description of SoC integration
 *
 * @transport_init: An optional function that a transport can provide to
 *		    initialize some transport-specific setup during SCMI core
 *		    initialization, so ahead of SCMI core probing.
 * @transport_exit: An optional function that a transport can provide to
 *		    de-initialize some transport-specific setup during SCMI core
 *		    de-initialization, so after SCMI core removal.
 * @ops: Pointer to the transport specific ops structure
 * @max_rx_timeout_ms: Timeout for communication with SoC (in Milliseconds)
 * @max_msg: Maximum number of messages for a channel type (tx or rx) that can
 *	be pending simultaneously in the system. May be overridden by the
 *	get_max_msg op.
 * @max_msg_size: Maximum size of data per message that can be handled.
 * @force_polling: Flag to force this whole transport to use SCMI core polling
 *		   mechanism instead of completion interrupts even if available.
 * @sync_cmds_completed_on_ret: Flag to indicate that the transport assures
 *				synchronous-command messages are atomically
 *				completed on .send_message: no need to poll
 *				actively waiting for a response.
 *				Used by core internally only when polling is
 *				selected as a waiting for reply method: i.e.
 *				if a completion irq was found use that anyway.
 * @atomic_enabled: Flag to indicate that this transport, which is assured not
 *		    to sleep anywhere on the TX path, can be used in atomic mode
 *		    when requested.
 */
struct scmi_desc {
	int (*transport_init)(void);
	void (*transport_exit)(void);
	const struct scmi_transport_ops *ops;
	int max_rx_timeout_ms;
	int max_msg;
	int max_msg_size;
	const bool force_polling;
	const bool sync_cmds_completed_on_ret;
	const bool atomic_enabled;
};

#ifdef CONFIG_ARM_SCMI_TRANSPORT_MAILBOX
extern const struct scmi_desc scmi_mailbox_desc;
#endif
#ifdef CONFIG_ARM_SCMI_TRANSPORT_SMC
extern const struct scmi_desc scmi_smc_desc;
#endif
#ifdef CONFIG_ARM_SCMI_TRANSPORT_VIRTIO
extern const struct scmi_desc scmi_virtio_desc;
#endif
#ifdef CONFIG_ARM_SCMI_TRANSPORT_OPTEE
extern const struct scmi_desc scmi_optee_desc;
#endif

void scmi_rx_callback(struct scmi_chan_info *cinfo, u32 msg_hdr, void *priv);
void scmi_free_channel(struct scmi_chan_info *cinfo, struct idr *idr, int id);

/* shmem related declarations */
struct scmi_shared_mem;

void shmem_tx_prepare(struct scmi_shared_mem __iomem *shmem,
		      struct scmi_xfer *xfer, struct scmi_chan_info *cinfo);
u32 shmem_read_header(struct scmi_shared_mem __iomem *shmem);
void shmem_fetch_response(struct scmi_shared_mem __iomem *shmem,
			  struct scmi_xfer *xfer);
void shmem_fetch_notification(struct scmi_shared_mem __iomem *shmem,
			      size_t max_len, struct scmi_xfer *xfer);
void shmem_clear_channel(struct scmi_shared_mem __iomem *shmem);
bool shmem_poll_done(struct scmi_shared_mem __iomem *shmem,
		     struct scmi_xfer *xfer);

/* declarations for message passing transports */
struct scmi_msg_payld;

/* Maximum overhead of message w.r.t. struct scmi_desc.max_msg_size */
#define SCMI_MSG_MAX_PROT_OVERHEAD (2 * sizeof(__le32))

size_t msg_response_size(struct scmi_xfer *xfer);
size_t msg_command_size(struct scmi_xfer *xfer);
void msg_tx_prepare(struct scmi_msg_payld *msg, struct scmi_xfer *xfer);
u32 msg_read_header(struct scmi_msg_payld *msg);
void msg_fetch_response(struct scmi_msg_payld *msg, size_t len,
			struct scmi_xfer *xfer);
void msg_fetch_notification(struct scmi_msg_payld *msg, size_t len,
			    size_t max_len, struct scmi_xfer *xfer);

void scmi_notification_instance_data_set(const struct scmi_handle *handle,
					 void *priv);
void *scmi_notification_instance_data_get(const struct scmi_handle *handle);
#endif /* _SCMI_COMMON_H */
