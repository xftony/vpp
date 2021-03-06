/*
 *------------------------------------------------------------------
 * Copyright (c) 2018 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *------------------------------------------------------------------
 */

#include <vlib/vlib.h>
#include <vlib/unix/unix.h>
#include <vlib/pci/pci.h>
#include <vnet/ethernet/ethernet.h>

#include <avf/avf.h>

#define AVF_MBOX_LEN 64
#define AVF_MBOX_BUF_SZ 512
#define AVF_RXQ_SZ 512
#define AVF_TXQ_SZ 512
#define AVF_ITR_INT 8160

#define PCI_VENDOR_ID_INTEL			0x8086
#define PCI_DEVICE_ID_INTEL_AVF			0x1889
#define PCI_DEVICE_ID_INTEL_X710_VF		0x154c
#define PCI_DEVICE_ID_INTEL_X722_VF		0x37cd

avf_main_t avf_main;

static pci_device_id_t avf_pci_device_ids[] = {
  {.vendor_id = PCI_VENDOR_ID_INTEL,.device_id = PCI_DEVICE_ID_INTEL_AVF},
  {.vendor_id = PCI_VENDOR_ID_INTEL,.device_id = PCI_DEVICE_ID_INTEL_X710_VF},
  {0},
};

static inline void
avf_irq_0_disable (avf_device_t * ad)
{
  u32 dyn_ctl0 = 0, icr0_ena = 0;

  dyn_ctl0 |= (3 << 3);		/* 11b = No ITR update */

  avf_reg_write (ad, AVFINT_ICR0_ENA1, icr0_ena);
  avf_reg_write (ad, AVFINT_DYN_CTL0, dyn_ctl0);
  avf_reg_flush (ad);
}

static inline void
avf_irq_0_enable (avf_device_t * ad)
{
  u32 dyn_ctl0 = 0, icr0_ena = 0;

  icr0_ena |= (1 << 30);	/* [30] Admin Queue Enable */

  dyn_ctl0 |= (1 << 0);		/* [0] Interrupt Enable */
  dyn_ctl0 |= (1 << 1);		/* [1] Clear PBA */
  //dyn_ctl0 |= (3 << 3);               /* [4:3] ITR Index, 11b = No ITR update */
  dyn_ctl0 |= ((AVF_ITR_INT / 2) << 5);	/* [16:5] ITR Interval in 2us steps */

  avf_irq_0_disable (ad);
  avf_reg_write (ad, AVFINT_ICR0_ENA1, icr0_ena);
  avf_reg_write (ad, AVFINT_DYN_CTL0, dyn_ctl0);
  avf_reg_flush (ad);
}

static inline void
avf_irq_n_disable (avf_device_t * ad, u8 line)
{
  u32 dyn_ctln = 0;

  avf_reg_write (ad, AVFINT_DYN_CTLN (line), dyn_ctln);
  avf_reg_flush (ad);
}

static inline void
avf_irq_n_enable (avf_device_t * ad, u8 line)
{
  u32 dyn_ctln = 0;

  dyn_ctln |= (1 << 0);		/* [0] Interrupt Enable */
  dyn_ctln |= (1 << 1);		/* [1] Clear PBA */
  dyn_ctln |= ((AVF_ITR_INT / 2) << 5);	/* [16:5] ITR Interval in 2us steps */

  avf_irq_n_disable (ad, line);
  avf_reg_write (ad, AVFINT_DYN_CTLN (line), dyn_ctln);
  avf_reg_flush (ad);
}


clib_error_t *
avf_aq_desc_enq (vlib_main_t * vm, avf_device_t * ad, avf_aq_desc_t * dt,
		 void *data, int len)
{
  avf_main_t *am = &avf_main;
  clib_error_t *err = 0;
  avf_aq_desc_t *d, dc;
  int n_retry = 5;

  d = &ad->atq[ad->atq_next_slot];
  clib_memcpy (d, dt, sizeof (avf_aq_desc_t));
  d->flags |= AVF_AQ_F_RD | AVF_AQ_F_SI;
  if (len)
    d->datalen = len;
  if (len)
    {
      u64 pa;
      pa = ad->atq_bufs_pa + ad->atq_next_slot * AVF_MBOX_BUF_SZ;
      d->addr_hi = (u32) (pa >> 32);
      d->addr_lo = (u32) pa;
      clib_memcpy (ad->atq_bufs + ad->atq_next_slot * AVF_MBOX_BUF_SZ, data,
		   len);
      d->flags |= AVF_AQ_F_BUF;
    }

  if (ad->flags & AVF_DEVICE_F_ELOG)
    clib_memcpy (&dc, d, sizeof (avf_aq_desc_t));

  CLIB_MEMORY_BARRIER ();
  vlib_log_debug (am->log_class, "%U", format_hexdump, data, len);
  ad->atq_next_slot = (ad->atq_next_slot + 1) % AVF_MBOX_LEN;
  avf_reg_write (ad, AVF_ATQT, ad->atq_next_slot);
  avf_reg_flush (ad);

retry:
  vlib_process_suspend (vm, 10e-6);

  if (((d->flags & AVF_AQ_F_DD) == 0) || ((d->flags & AVF_AQ_F_CMP) == 0))
    {
      if (--n_retry == 0)
	{
	  err = clib_error_return (0, "adminq enqueue timeout [opcode 0x%x]",
				   d->opcode);
	  goto done;
	}
      goto retry;
    }

  clib_memcpy (dt, d, sizeof (avf_aq_desc_t));
  if (d->flags & AVF_AQ_F_ERR)
    return clib_error_return (0, "adminq enqueue error [opcode 0x%x, retval "
			      "%d]", d->opcode, d->retval);

done:
  if (ad->flags & AVF_DEVICE_F_ELOG)
    {
      /* *INDENT-OFF* */
      ELOG_TYPE_DECLARE (el) =
	{
	  .format = "avf[%d] aq enq: s_flags 0x%x r_flags 0x%x opcode 0x%x "
	    "datalen %d retval %d",
	  .format_args = "i4i2i2i2i2i2",
	};
      struct
	{
	  u32 dev_instance;
	  u16 s_flags;
	  u16 r_flags;
	  u16 opcode;
	  u16 datalen;
	  u16 retval;
	} *ed;
      ed = ELOG_DATA (&vm->elog_main, el);
      ed->dev_instance = ad->dev_instance;
      ed->s_flags = dc.flags;
      ed->r_flags = d->flags;
      ed->opcode = dc.opcode;
      ed->datalen = dc.datalen;
      ed->retval = d->retval;
      /* *INDENT-ON* */
    }

  return err;
}

clib_error_t *
avf_cmd_rx_ctl_reg_write (vlib_main_t * vm, avf_device_t * ad, u32 reg,
			  u32 val)
{
  clib_error_t *err;
  avf_aq_desc_t d = {.opcode = 0x207,.param1 = reg,.param3 = val };
  err = avf_aq_desc_enq (vm, ad, &d, 0, 0);

  if (ad->flags & AVF_DEVICE_F_ELOG)
    {
      /* *INDENT-OFF* */
      ELOG_TYPE_DECLARE (el) =
	{
	  .format = "avf[%d] rx ctl reg write: reg 0x%x val 0x%x ",
	  .format_args = "i4i4i4",
	};
      struct
	{
	  u32 dev_instance;
	  u32 reg;
	  u32 val;
	} *ed;
      ed = ELOG_DATA (&vm->elog_main, el);
      ed->dev_instance = ad->dev_instance;
      ed->reg = reg;
      ed->val = val;
      /* *INDENT-ON* */
    }
  return err;
}

clib_error_t *
avf_rxq_init (vlib_main_t * vm, avf_device_t * ad, u16 qid)
{
  avf_main_t *am = &avf_main;
  avf_rxq_t *rxq;
  clib_error_t *error = 0;
  u32 n_alloc, i;

  vec_validate_aligned (ad->rxqs, qid, CLIB_CACHE_LINE_BYTES);
  rxq = vec_elt_at_index (ad->rxqs, qid);
  rxq->size = AVF_RXQ_SZ;
  rxq->next = 0;
  rxq->descs = vlib_physmem_alloc_aligned (vm, am->physmem_region, &error,
					   rxq->size * sizeof (avf_rx_desc_t),
					   2 * CLIB_CACHE_LINE_BYTES);
  memset (rxq->descs, 0, rxq->size * sizeof (avf_rx_desc_t));
  vec_validate_aligned (rxq->bufs, rxq->size, CLIB_CACHE_LINE_BYTES);
  rxq->qrx_tail = ad->bar0 + AVF_QRX_TAIL (qid);

  n_alloc = vlib_buffer_alloc (vm, rxq->bufs, rxq->size - 8);

  if (n_alloc == 0)
    return clib_error_return (0, "buffer allocation error");

  rxq->n_bufs = n_alloc;
  avf_rx_desc_t *d = rxq->descs;
  for (i = 0; i < n_alloc; i++)
    {
      if (ad->flags & AVF_DEVICE_F_IOVA)
	{
	  vlib_buffer_t *b = vlib_get_buffer (vm, rxq->bufs[i]);
	  d->qword[0] = pointer_to_uword (b->data);
	}
      else
	d->qword[0] =
	  vlib_get_buffer_data_physical_address (vm, rxq->bufs[i]);
      d++;
    }
  return 0;
}

clib_error_t *
avf_txq_init (vlib_main_t * vm, avf_device_t * ad, u16 qid)
{
  avf_main_t *am = &avf_main;
  avf_txq_t *txq;
  clib_error_t *error = 0;

  if (qid >= ad->num_queue_pairs)
    {
      qid = qid % ad->num_queue_pairs;
      txq = vec_elt_at_index (ad->txqs, qid);
      if (txq->lock == 0)
	clib_spinlock_init (&txq->lock);
      ad->flags |= AVF_DEVICE_F_SHARED_TXQ_LOCK;
      return 0;
    }

  vec_validate_aligned (ad->txqs, qid, CLIB_CACHE_LINE_BYTES);
  txq = vec_elt_at_index (ad->txqs, qid);
  txq->size = AVF_TXQ_SZ;
  txq->next = 0;
  txq->descs = vlib_physmem_alloc_aligned (vm, am->physmem_region, &error,
					   txq->size * sizeof (avf_tx_desc_t),
					   2 * CLIB_CACHE_LINE_BYTES);
  vec_validate_aligned (txq->bufs, txq->size, CLIB_CACHE_LINE_BYTES);
  txq->qtx_tail = ad->bar0 + AVF_QTX_TAIL (qid);
  return 0;
}

typedef struct
{
  u16 vsi_id;
  u16 flags;
} virtchnl_promisc_info_t;

void
avf_arq_slot_init (avf_device_t * ad, u16 slot)
{
  avf_aq_desc_t *d;
  u64 pa = ad->arq_bufs_pa + slot * AVF_MBOX_BUF_SZ;
  d = &ad->arq[slot];
  memset (d, 0, sizeof (avf_aq_desc_t));
  d->flags = AVF_AQ_F_BUF;
  d->datalen = AVF_MBOX_BUF_SZ;
  d->addr_hi = (u32) (pa >> 32);
  d->addr_lo = (u32) pa;
}

static inline uword
avf_dma_addr (vlib_main_t * vm, avf_device_t * ad, void *p)
{
  avf_main_t *am = &avf_main;
  return (ad->flags & AVF_DEVICE_F_IOVA) ?
    pointer_to_uword (p) :
    vlib_physmem_virtual_to_physical (vm, am->physmem_region, p);
}

static void
avf_adminq_init (vlib_main_t * vm, avf_device_t * ad)
{
  u64 pa;
  int i;

  /* VF MailBox Transmit */
  memset (ad->atq, 0, sizeof (avf_aq_desc_t) * AVF_MBOX_LEN);
  ad->atq_bufs_pa = avf_dma_addr (vm, ad, ad->atq_bufs);

  pa = avf_dma_addr (vm, ad, ad->atq);
  avf_reg_write (ad, AVF_ATQT, 0);	/* Tail */
  avf_reg_write (ad, AVF_ATQH, 0);	/* Head */
  avf_reg_write (ad, AVF_ATQLEN, AVF_MBOX_LEN | (1 << 31));	/* len & ena */
  avf_reg_write (ad, AVF_ATQBAL, (u32) pa);	/* Base Address Low */
  avf_reg_write (ad, AVF_ATQBAH, (u32) (pa >> 32));	/* Base Address High */

  /* VF MailBox Receive */
  memset (ad->arq, 0, sizeof (avf_aq_desc_t) * AVF_MBOX_LEN);
  ad->arq_bufs_pa = avf_dma_addr (vm, ad, ad->arq_bufs);

  for (i = 0; i < AVF_MBOX_LEN; i++)
    avf_arq_slot_init (ad, i);

  pa = avf_dma_addr (vm, ad, ad->arq);

  avf_reg_write (ad, AVF_ARQH, 0);	/* Head */
  avf_reg_write (ad, AVF_ARQT, 0);	/* Head */
  avf_reg_write (ad, AVF_ARQLEN, AVF_MBOX_LEN | (1 << 31));	/* len & ena */
  avf_reg_write (ad, AVF_ARQBAL, (u32) pa);	/* Base Address Low */
  avf_reg_write (ad, AVF_ARQBAH, (u32) (pa >> 32));	/* Base Address High */
  avf_reg_write (ad, AVF_ARQT, AVF_MBOX_LEN - 1);	/* Tail */

  ad->atq_next_slot = 0;
  ad->arq_next_slot = 0;
}

clib_error_t *
avf_send_to_pf (vlib_main_t * vm, avf_device_t * ad, virtchnl_ops_t op,
		void *in, int in_len, void *out, int out_len)
{
  clib_error_t *err;
  avf_aq_desc_t *d, dt = {.opcode = 0x801,.v_opcode = op };
  u32 head;
  int n_retry = 5;


  /* supppres interrupt in the next adminq receive slot
     as we are going to wait for response
     we only need interrupts when event is received */
  d = &ad->arq[ad->arq_next_slot];
  d->flags |= AVF_AQ_F_SI;

  if ((err = avf_aq_desc_enq (vm, ad, &dt, in, in_len)))
    return err;

retry:
  head = avf_get_u32 (ad->bar0, AVF_ARQH);

  if (ad->arq_next_slot == head)
    {
      if (--n_retry == 0)
	return clib_error_return (0, "timeout");
      vlib_process_suspend (vm, 10e-3);
      goto retry;
    }

  d = &ad->arq[ad->arq_next_slot];

  if (d->v_opcode == VIRTCHNL_OP_EVENT)
    {
      void *buf = ad->arq_bufs + ad->arq_next_slot * AVF_MBOX_BUF_SZ;
      virtchnl_pf_event_t *e;

      if ((d->datalen != sizeof (virtchnl_pf_event_t)) ||
	  ((d->flags & AVF_AQ_F_BUF) == 0))
	return clib_error_return (0, "event message error");

      vec_add2 (ad->events, e, 1);
      clib_memcpy (e, buf, sizeof (virtchnl_pf_event_t));
      avf_arq_slot_init (ad, ad->arq_next_slot);
      ad->arq_next_slot++;
      n_retry = 5;
      goto retry;
    }

  if (d->v_opcode != op)
    {
      err = clib_error_return (0, "unexpected message receiver [v_opcode = %u"
			       "expected %u]", d->v_opcode, op);
      goto done;
    }

  if (d->v_retval)
    {
      err = clib_error_return (0, "error [v_opcode = %u, v_retval %d]",
			       d->v_opcode, d->v_retval);
      goto done;
    }

  if (d->flags & AVF_AQ_F_BUF)
    {
      void *buf = ad->arq_bufs + ad->arq_next_slot * AVF_MBOX_BUF_SZ;
      clib_memcpy (out, buf, out_len);
    }

  avf_arq_slot_init (ad, ad->arq_next_slot);
  avf_reg_write (ad, AVF_ARQT, ad->arq_next_slot);
  avf_reg_flush (ad);
  ad->arq_next_slot = (ad->arq_next_slot + 1) % AVF_MBOX_LEN;

done:

  if (ad->flags & AVF_DEVICE_F_ELOG)
    {
      /* *INDENT-OFF* */
      ELOG_TYPE_DECLARE (el) =
	{
	  .format = "avf[%d] send to pf: v_opcode %s (%d) v_retval 0x%x",
	  .format_args = "i4t4i4i4",
	  .n_enum_strings = VIRTCHNL_N_OPS,
	  .enum_strings = {
#define _(v, n) [v] = #n,
	      foreach_virtchnl_op
#undef _
	  },
	};
      struct
	{
	  u32 dev_instance;
	  u32 v_opcode;
	  u32 v_opcode_val;
	  u32 v_retval;
	} *ed;
      ed = ELOG_DATA (&vm->elog_main, el);
      ed->dev_instance = ad->dev_instance;
      ed->v_opcode = op;
      ed->v_opcode_val = op;
      ed->v_retval = d->v_retval;
      /* *INDENT-ON* */
    }
  return err;
}

clib_error_t *
avf_op_version (vlib_main_t * vm, avf_device_t * ad,
		virtchnl_version_info_t * ver)
{
  clib_error_t *err = 0;
  virtchnl_version_info_t myver = {
    .major = VIRTCHNL_VERSION_MAJOR,
    .minor = VIRTCHNL_VERSION_MINOR,
  };

  err = avf_send_to_pf (vm, ad, VIRTCHNL_OP_VERSION, &myver,
			sizeof (virtchnl_version_info_t), ver,
			sizeof (virtchnl_version_info_t));

  if (err)
    return err;

  return err;
}

clib_error_t *
avf_op_get_vf_resources (vlib_main_t * vm, avf_device_t * ad,
			 virtchnl_vf_resource_t * res)
{
  clib_error_t *err = 0;
  u32 bitmap = (VIRTCHNL_VF_OFFLOAD_L2 | VIRTCHNL_VF_OFFLOAD_RSS_AQ |
		VIRTCHNL_VF_OFFLOAD_RSS_REG | VIRTCHNL_VF_OFFLOAD_WB_ON_ITR |
		VIRTCHNL_VF_OFFLOAD_VLAN | VIRTCHNL_VF_OFFLOAD_RX_POLLING);

  err = avf_send_to_pf (vm, ad, VIRTCHNL_OP_GET_VF_RESOURCES, &bitmap,
			sizeof (u32), res, sizeof (virtchnl_vf_resource_t));

  if (err)
    return err;

  return err;
}

clib_error_t *
avf_op_disable_vlan_stripping (vlib_main_t * vm, avf_device_t * ad)
{
  return avf_send_to_pf (vm, ad, VIRTCHNL_OP_DISABLE_VLAN_STRIPPING, 0, 0, 0,
			 0);
}

clib_error_t *
avf_config_promisc_mode (vlib_main_t * vm, avf_device_t * ad)
{
  virtchnl_promisc_info_t pi = { 0 };

  pi.vsi_id = ad->vsi_id;
  pi.flags = 1;
  return avf_send_to_pf (vm, ad, VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE, &pi,
			 sizeof (virtchnl_promisc_info_t), 0, 0);
}


clib_error_t *
avf_op_config_vsi_queues (vlib_main_t * vm, avf_device_t * ad)
{
  int i;
  int n_qp = clib_max (vec_len (ad->rxqs), vec_len (ad->txqs));
  int msg_len = sizeof (virtchnl_vsi_queue_config_info_t) + n_qp *
    sizeof (virtchnl_queue_pair_info_t);
  u8 msg[msg_len];
  virtchnl_vsi_queue_config_info_t *ci;

  memset (msg, 0, msg_len);
  ci = (virtchnl_vsi_queue_config_info_t *) msg;
  ci->vsi_id = ad->vsi_id;
  ci->num_queue_pairs = n_qp;

  for (i = 0; i < n_qp; i++)
    {
      virtchnl_txq_info_t *txq = &ci->qpair[i].txq;
      virtchnl_rxq_info_t *rxq = &ci->qpair[i].rxq;

      rxq->vsi_id = ad->vsi_id;
      rxq->queue_id = i;
      rxq->max_pkt_size = 1518;
      if (i < vec_len (ad->rxqs))
	{
	  avf_rxq_t *q = vec_elt_at_index (ad->rxqs, i);
	  rxq->ring_len = q->size;
	  rxq->databuffer_size = VLIB_BUFFER_DEFAULT_FREE_LIST_BYTES;
	  rxq->dma_ring_addr = avf_dma_addr (vm, ad, q->descs);
	  avf_reg_write (ad, AVF_QRX_TAIL (i), q->size - 1);
	}

      avf_txq_t *q = vec_elt_at_index (ad->txqs, i);
      txq->vsi_id = ad->vsi_id;
      if (i < vec_len (ad->txqs))
	{
	  txq->queue_id = i;
	  txq->ring_len = q->size;
	  txq->dma_ring_addr = avf_dma_addr (vm, ad, q->descs);
	}
    }

  return avf_send_to_pf (vm, ad, VIRTCHNL_OP_CONFIG_VSI_QUEUES, msg, msg_len,
			 0, 0);
}

clib_error_t *
avf_op_config_irq_map (vlib_main_t * vm, avf_device_t * ad)
{
  int count = 1;
  int msg_len = sizeof (virtchnl_irq_map_info_t) +
    count * sizeof (virtchnl_vector_map_t);
  u8 msg[msg_len];
  virtchnl_irq_map_info_t *imi;

  memset (msg, 0, msg_len);
  imi = (virtchnl_irq_map_info_t *) msg;
  imi->num_vectors = count;

  imi->vecmap[0].vector_id = 1;
  imi->vecmap[0].vsi_id = ad->vsi_id;
  imi->vecmap[0].rxq_map = 1;
  return avf_send_to_pf (vm, ad, VIRTCHNL_OP_CONFIG_IRQ_MAP, msg, msg_len, 0,
			 0);
}

clib_error_t *
avf_op_add_eth_addr (vlib_main_t * vm, avf_device_t * ad, u8 count, u8 * macs)
{
  int msg_len =
    sizeof (virtchnl_ether_addr_list_t) +
    count * sizeof (virtchnl_ether_addr_t);
  u8 msg[msg_len];
  virtchnl_ether_addr_list_t *al;
  int i;

  memset (msg, 0, msg_len);
  al = (virtchnl_ether_addr_list_t *) msg;
  al->vsi_id = ad->vsi_id;
  al->num_elements = count;
  for (i = 0; i < count; i++)
    clib_memcpy (&al->list[i].addr, macs + i * 6, 6);
  return avf_send_to_pf (vm, ad, VIRTCHNL_OP_ADD_ETH_ADDR, msg, msg_len, 0,
			 0);
}

clib_error_t *
avf_op_enable_queues (vlib_main_t * vm, avf_device_t * ad, u32 rx, u32 tx)
{
  virtchnl_queue_select_t qs = { 0 };
  qs.vsi_id = ad->vsi_id;
  qs.rx_queues = rx;
  qs.tx_queues = tx;
  avf_rxq_t *rxq = vec_elt_at_index (ad->rxqs, 0);
  avf_reg_write (ad, AVF_QRX_TAIL (0), rxq->n_bufs);
  return avf_send_to_pf (vm, ad, VIRTCHNL_OP_ENABLE_QUEUES, &qs,
			 sizeof (virtchnl_queue_select_t), 0, 0);
}

clib_error_t *
avf_op_get_stats (vlib_main_t * vm, avf_device_t * ad,
		  virtchnl_eth_stats_t * es)
{
  virtchnl_queue_select_t qs = { 0 };
  qs.vsi_id = ad->vsi_id;
  return avf_send_to_pf (vm, ad, VIRTCHNL_OP_GET_STATS,
			 &qs, sizeof (virtchnl_queue_select_t),
			 es, sizeof (virtchnl_eth_stats_t));
}

clib_error_t *
avf_device_reset (vlib_main_t * vm, avf_device_t * ad)
{
  avf_aq_desc_t d = { 0 };
  clib_error_t *error;
  u32 rstat;
  int n_retry = 20;

  d.opcode = 0x801;
  d.v_opcode = VIRTCHNL_OP_RESET_VF;
  if ((error = avf_aq_desc_enq (vm, ad, &d, 0, 0)))
    return error;

retry:
  vlib_process_suspend (vm, 10e-3);
  rstat = avf_get_u32 (ad->bar0, AVFGEN_RSTAT);

  if (rstat == 2 || rstat == 3)
    return 0;

  if (--n_retry == 0)
    return clib_error_return (0, "reset failed (timeout)");

  goto retry;
}

clib_error_t *
avf_device_init (vlib_main_t * vm, avf_device_t * ad)
{
  virtchnl_version_info_t ver = { 0 };
  virtchnl_vf_resource_t res = { 0 };
  clib_error_t *error;
  vlib_thread_main_t *tm = vlib_get_thread_main ();
  int i;

  avf_adminq_init (vm, ad);

  if ((error = avf_device_reset (vm, ad)))
    return error;

  avf_adminq_init (vm, ad);

  /*
   * OP_VERSION
   */
  if ((error = avf_op_version (vm, ad, &ver)))
    return error;

  if (ver.major != VIRTCHNL_VERSION_MAJOR ||
      ver.minor != VIRTCHNL_VERSION_MINOR)
    return clib_error_return (0, "incompatible protocol version "
			      "(remote %d.%d)", ver.major, ver.minor);

  /*
   * OP_GET_VF_RESOUCES
   */
  if ((error = avf_op_get_vf_resources (vm, ad, &res)))
    return error;

  if (res.num_vsis != 1 || res.vsi_res[0].vsi_type != VIRTCHNL_VSI_SRIOV)
    return clib_error_return (0, "unexpected GET_VF_RESOURCE reply received");

  ad->vsi_id = res.vsi_res[0].vsi_id;
  ad->feature_bitmap = res.vf_offload_flags;
  ad->num_queue_pairs = res.num_queue_pairs;
  ad->max_vectors = res.max_vectors;
  ad->max_mtu = res.max_mtu;
  ad->rss_key_size = res.rss_key_size;
  ad->rss_lut_size = res.rss_lut_size;

  clib_memcpy (ad->hwaddr, res.vsi_res[0].default_mac_addr, 6);

  /*
   * Disable VLAN stripping
   */
  if ((error = avf_op_disable_vlan_stripping (vm, ad)))
    return error;

  if ((error = avf_config_promisc_mode (vm, ad)))
    return error;

  if ((error = avf_cmd_rx_ctl_reg_write (vm, ad, 0xc400, 0)))
    return error;

  if ((error = avf_cmd_rx_ctl_reg_write (vm, ad, 0xc404, 0)))
    return error;

  /*
   * Init Queues
   */
  if ((error = avf_rxq_init (vm, ad, 0)))
    return error;

  for (i = 0; i < tm->n_vlib_mains; i++)
    if ((error = avf_txq_init (vm, ad, i)))
      return error;

  if ((error = avf_op_config_vsi_queues (vm, ad)))
    return error;

  if ((error = avf_op_config_irq_map (vm, ad)))
    return error;

  avf_irq_0_enable (ad);
  avf_irq_n_enable (ad, 0);

  if ((error = avf_op_add_eth_addr (vm, ad, 1, ad->hwaddr)))
    return error;

  if ((error = avf_op_enable_queues (vm, ad, 1, 0)))
    return error;

  if ((error = avf_op_enable_queues (vm, ad, 0, 1)))
    return error;

  ad->flags |= AVF_DEVICE_F_INITIALIZED;
  return error;
}

void
avf_process_one_device (vlib_main_t * vm, avf_device_t * ad, int is_irq)
{
  avf_main_t *am = &avf_main;
  vnet_main_t *vnm = vnet_get_main ();
  virtchnl_pf_event_t *e;
  u32 r;

  if (ad->flags & AVF_DEVICE_F_ERROR)
    return;

  if ((ad->flags & AVF_DEVICE_F_INITIALIZED) == 0)
    return;

  ASSERT (ad->error == 0);

  r = avf_get_u32 (ad->bar0, AVF_ARQLEN);
  if ((r & 0xf0000000) != (1 << 31))
    {
      ad->error = clib_error_return (0, "arq not enabled, arqlen = 0x%x", r);
      goto error;
    }

  r = avf_get_u32 (ad->bar0, AVF_ATQLEN);
  if ((r & 0xf0000000) != (1 << 31))
    {
      ad->error = clib_error_return (0, "atq not enabled, atqlen = 0x%x", r);
      goto error;
    }

  if (is_irq == 0)
    avf_op_get_stats (vm, ad, &ad->eth_stats);

  /* *INDENT-OFF* */
  vec_foreach (e, ad->events)
    {
      if (e->event == VIRTCHNL_EVENT_LINK_CHANGE)
	{
	  int link_up = e->event_data.link_event.link_status;
	  virtchnl_link_speed_t speed = e->event_data.link_event.link_speed;
	  u32 flags = 0;

	  if (link_up && (ad->flags & AVF_DEVICE_F_LINK_UP) == 0)
	    {
	      ad->flags |= AVF_DEVICE_F_LINK_UP;
	      flags |= (VNET_HW_INTERFACE_FLAG_FULL_DUPLEX |
			VNET_HW_INTERFACE_FLAG_LINK_UP);
	      if (speed == VIRTCHNL_LINK_SPEED_40GB)
		flags |= VNET_HW_INTERFACE_FLAG_SPEED_40G;
	      else if (speed == VIRTCHNL_LINK_SPEED_25GB)
		flags |= VNET_HW_INTERFACE_FLAG_SPEED_25G;
	      else if (speed == VIRTCHNL_LINK_SPEED_10GB)
		flags |= VNET_HW_INTERFACE_FLAG_SPEED_10G;
	      else if (speed == VIRTCHNL_LINK_SPEED_1GB)
		flags |= VNET_HW_INTERFACE_FLAG_SPEED_1G;
	      else if (speed == VIRTCHNL_LINK_SPEED_100MB)
		flags |= VNET_HW_INTERFACE_FLAG_SPEED_100M;
	      vnet_hw_interface_set_flags (vnm, ad->hw_if_index, flags);
	      ad->link_speed = speed;
	    }
	  else if (!link_up && (ad->flags & AVF_DEVICE_F_LINK_UP) != 0)
	    {
	      ad->flags &= ~AVF_DEVICE_F_LINK_UP;
	      ad->link_speed = 0;
	    }

	  if (ad->flags & AVF_DEVICE_F_ELOG)
	    {
	      ELOG_TYPE_DECLARE (el) =
		{
		  .format = "avf[%d] link change: link_status %d "
		    "link_speed %d",
		  .format_args = "i4i1i1",
		};
	      struct
		{
		  u32 dev_instance;
		  u8 link_status;
		  u8 link_speed;
		} *ed;
	      ed = ELOG_DATA (&vm->elog_main, el);
              ed->dev_instance = ad->dev_instance;
	      ed->link_status = link_up;
	      ed->link_speed = speed;
	    }
	}
      else
	{
	  if (ad->flags & AVF_DEVICE_F_ELOG)
	    {
	      ELOG_TYPE_DECLARE (el) =
		{
		  .format = "avf[%d] unknown event: event %d severity %d",
		  .format_args = "i4i4i1i1",
		};
	      struct
		{
		  u32 dev_instance;
		  u32 event;
		  u32 severity;
		} *ed;
	      ed = ELOG_DATA (&vm->elog_main, el);
              ed->dev_instance = ad->dev_instance;
	      ed->event = e->event;
	      ed->severity = e->severity;
	    }
	}
    }
  /* *INDENT-ON* */
  vec_reset_length (ad->events);

  return;

error:
  ad->flags |= AVF_DEVICE_F_ERROR;
  ASSERT (ad->error != 0);
  vlib_log_err (am->log_class, "%U", format_clib_error, ad->error);
}

static u32
avf_flag_change (vnet_main_t * vnm, vnet_hw_interface_t * hw, u32 flags)
{
  avf_main_t *am = &avf_main;
  vlib_log_warn (am->log_class, "TODO");
  return 0;
}

static uword
avf_process (vlib_main_t * vm, vlib_node_runtime_t * rt, vlib_frame_t * f)
{
  avf_main_t *am = &avf_main;
  avf_device_t *ad;
  uword *event_data = 0, event_type;
  int enabled = 0, irq;
  f64 last_run_duration = 0;
  f64 last_periodic_time = 0;

  while (1)
    {
      if (enabled)
	vlib_process_wait_for_event_or_clock (vm, 5.0 - last_run_duration);
      else
	vlib_process_wait_for_event (vm);

      event_type = vlib_process_get_events (vm, &event_data);
      vec_reset_length (event_data);
      irq = 0;

      switch (event_type)
	{
	case ~0:
	  last_periodic_time = vlib_time_now (vm);
	  break;
	case AVF_PROCESS_EVENT_START:
	  enabled = 1;
	  break;
	case AVF_PROCESS_EVENT_STOP:
	  enabled = 0;
	  continue;
	case AVF_PROCESS_EVENT_AQ_INT:
	  irq = 1;
	  break;
	default:
	  ASSERT (0);
	}

      /* *INDENT-OFF* */
      pool_foreach (ad, am->devices,
        {
	  avf_process_one_device (vm, ad, irq);
        });
      /* *INDENT-ON* */
      last_run_duration = vlib_time_now (vm) - last_periodic_time;
    }
  return 0;
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (avf_process_node, static)  = {
  .function = avf_process,
  .type = VLIB_NODE_TYPE_PROCESS,
  .name = "avf-process",
};
/* *INDENT-ON* */

static void
avf_irq_0_handler (vlib_pci_dev_handle_t h, u16 line)
{
  vlib_main_t *vm = vlib_get_main ();
  avf_main_t *am = &avf_main;
  uword pd = vlib_pci_get_private_data (h);
  avf_device_t *ad = pool_elt_at_index (am->devices, pd);
  u32 icr0;

  icr0 = avf_reg_read (ad, AVFINT_ICR0);

  if (ad->flags & AVF_DEVICE_F_ELOG)
    {
      /* *INDENT-OFF* */
      ELOG_TYPE_DECLARE (el) =
	{
	  .format = "avf[%d] irq 0: icr0 0x%x",
	  .format_args = "i4i4",
	};
      /* *INDENT-ON* */
      struct
      {
	u32 dev_instance;
	u32 icr0;
      } *ed;

      ed = ELOG_DATA (&vm->elog_main, el);
      ed->dev_instance = ad->dev_instance;
      ed->icr0 = icr0;
    }

  avf_irq_0_enable (ad);

  /* bit 30 - Send/Receive Admin queue interrupt indication */
  if (icr0 & (1 << 30))
    vlib_process_signal_event (vm, avf_process_node.index,
			       AVF_PROCESS_EVENT_AQ_INT, 0);
}

static void
avf_irq_n_handler (vlib_pci_dev_handle_t h, u16 line)
{
  vlib_main_t *vm = vlib_get_main ();
  avf_main_t *am = &avf_main;
  uword pd = vlib_pci_get_private_data (h);
  avf_device_t *ad = pool_elt_at_index (am->devices, pd);

  if (ad->flags & AVF_DEVICE_F_ELOG)
    {
      /* *INDENT-OFF* */
      ELOG_TYPE_DECLARE (el) =
	{
	  .format = "avf[%d] irq %d: received",
	  .format_args = "i4i2",
	};
      /* *INDENT-ON* */
      struct
      {
	u32 dev_instance;
	u16 line;
      } *ed;

      ed = ELOG_DATA (&vm->elog_main, el);
      ed->dev_instance = ad->dev_instance;
      ed->line = line;
    }

  avf_irq_n_enable (ad, 0);
}

void
avf_delete_if (vlib_main_t * vm, avf_device_t * ad)
{
  vnet_main_t *vnm = vnet_get_main ();
  avf_main_t *am = &avf_main;
  int i;

  if (ad->hw_if_index)
    {
      vnet_hw_interface_set_flags (vnm, ad->hw_if_index, 0);
      vnet_hw_interface_unassign_rx_thread (vnm, ad->hw_if_index, 0);
      ethernet_delete_interface (vnm, ad->hw_if_index);
    }

  vlib_pci_device_close (ad->pci_dev_handle);

  vlib_physmem_free (vm, am->physmem_region, ad->atq);
  vlib_physmem_free (vm, am->physmem_region, ad->arq);
  vlib_physmem_free (vm, am->physmem_region, ad->atq_bufs);
  vlib_physmem_free (vm, am->physmem_region, ad->arq_bufs);

  /* *INDENT-OFF* */
  vec_foreach_index (i, ad->rxqs)
    {
      avf_rxq_t *rxq = vec_elt_at_index (ad->rxqs, i);
      vlib_physmem_free (vm, am->physmem_region, rxq->descs);
      if (rxq->n_bufs)
	vlib_buffer_free_from_ring (vm, rxq->bufs, rxq->next, rxq->size,
				    rxq->n_bufs);
      vec_free (rxq->bufs);
    }
  /* *INDENT-ON* */
  vec_free (ad->rxqs);

  /* *INDENT-OFF* */
  vec_foreach_index (i, ad->txqs)
    {
      avf_txq_t *txq = vec_elt_at_index (ad->txqs, i);
      vlib_physmem_free (vm, am->physmem_region, txq->descs);
      if (txq->n_bufs)
	{
	  u16 first = (txq->next - txq->n_bufs) & (txq->size -1);
	  vlib_buffer_free_from_ring (vm, txq->bufs, first, txq->size,
				      txq->n_bufs);
	}
      vec_free (txq->bufs);
    }
  /* *INDENT-ON* */
  vec_free (ad->txqs);

  clib_error_free (ad->error);
  memset (ad, 0, sizeof (*ad));
  pool_put (am->devices, ad);
}

void
avf_create_if (vlib_main_t * vm, avf_create_if_args_t * args)
{
  vnet_main_t *vnm = vnet_get_main ();
  avf_main_t *am = &avf_main;
  avf_device_t *ad;
  vlib_pci_dev_handle_t h;
  clib_error_t *error = 0;

  pool_get (am->devices, ad);
  ad->dev_instance = ad - am->devices;
  ad->per_interface_next_index = ~0;

  if (args->enable_elog)
    ad->flags |= AVF_DEVICE_F_ELOG;

  if ((error = vlib_pci_device_open (&args->addr, avf_pci_device_ids, &h)))
    goto error;
  ad->pci_dev_handle = h;

  vlib_pci_set_private_data (h, ad->dev_instance);

  if ((error = vlib_pci_bus_master_enable (h)))
    goto error;

  if ((error = vlib_pci_map_region (h, 0, &ad->bar0)))
    goto error;

  if ((error = vlib_pci_register_msix_handler (h, 0, 1, &avf_irq_0_handler)))
    goto error;

  if ((error = vlib_pci_register_msix_handler (h, 1, 1, &avf_irq_n_handler)))
    goto error;

  if ((error = vlib_pci_enable_msix_irq (h, 0, 2)))
    goto error;

  if (am->physmem_region_alloc == 0)
    {
      u32 flags = VLIB_PHYSMEM_F_INIT_MHEAP | VLIB_PHYSMEM_F_HUGETLB;
      error = vlib_physmem_region_alloc (vm, "avf descriptors", 4 << 20, 0,
					 flags, &am->physmem_region);
      if (error)
	goto error;
      am->physmem_region_alloc = 1;
    }
  ad->atq = vlib_physmem_alloc_aligned (vm, am->physmem_region, &error,
					sizeof (avf_aq_desc_t) * AVF_MBOX_LEN,
					64);
  if (error)
    goto error;

  ad->arq = vlib_physmem_alloc_aligned (vm, am->physmem_region, &error,
					sizeof (avf_aq_desc_t) * AVF_MBOX_LEN,
					64);
  if (error)
    goto error;

  ad->atq_bufs = vlib_physmem_alloc_aligned (vm, am->physmem_region, &error,
					     AVF_MBOX_BUF_SZ * AVF_MBOX_LEN,
					     64);
  if (error)
    goto error;

  ad->arq_bufs = vlib_physmem_alloc_aligned (vm, am->physmem_region, &error,
					     AVF_MBOX_BUF_SZ * AVF_MBOX_LEN,
					     64);
  if (error)
    goto error;

  if ((error = vlib_pci_intr_enable (h)))
    goto error;

  /* FIXME detect */
  ad->flags |= AVF_DEVICE_F_IOVA;

  if ((error = avf_device_init (vm, ad)))
    goto error;

  /* create interface */
  error = ethernet_register_interface (vnm, avf_device_class.index,
				       ad->dev_instance, ad->hwaddr,
				       &ad->hw_if_index, avf_flag_change);

  if (error)
    goto error;

  vnet_sw_interface_t *sw = vnet_get_hw_sw_interface (vnm, ad->hw_if_index);
  ad->sw_if_index = sw->sw_if_index;

  vnet_hw_interface_set_input_node (vnm, ad->hw_if_index,
				    avf_input_node.index);

  if (pool_elts (am->devices) == 1)
    vlib_process_signal_event (vm, avf_process_node.index,
			       AVF_PROCESS_EVENT_START, 0);

  return;

error:
  avf_delete_if (vm, ad);
  args->rv = VNET_API_ERROR_INVALID_INTERFACE;
  args->error = clib_error_return (error, "pci-addr %U",
				   format_vlib_pci_addr, &args->addr);
  vlib_log_err (am->log_class, "%U", format_clib_error, args->error);
}

static clib_error_t *
avf_interface_admin_up_down (vnet_main_t * vnm, u32 hw_if_index, u32 flags)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  avf_main_t *am = &avf_main;
  avf_device_t *ad = vec_elt_at_index (am->devices, hi->dev_instance);
  uword is_up = (flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP) != 0;

  if (ad->flags & AVF_DEVICE_F_ERROR)
    return clib_error_return (0, "device is in error state");

  if (is_up)
    {
      vnet_hw_interface_set_flags (vnm, ad->hw_if_index,
				   VNET_HW_INTERFACE_FLAG_LINK_UP);
      ad->flags |= AVF_DEVICE_F_ADMIN_UP;
      vnet_hw_interface_assign_rx_thread (vnm, ad->hw_if_index, 0, ~0);
    }
  else
    {
      vnet_hw_interface_set_flags (vnm, ad->hw_if_index, 0);
      ad->flags &= ~AVF_DEVICE_F_ADMIN_UP;
    }
  return 0;
}

/* *INDENT-OFF* */
VNET_DEVICE_CLASS (avf_device_class,) =
{
  .name = "Adaptive Virtual Function (AVF) interface",
  .tx_function = avf_interface_tx,
  .format_device = format_avf_device,
  .format_device_name = format_avf_device_name,
  .admin_up_down_function = avf_interface_admin_up_down,
};
/* *INDENT-ON* */

clib_error_t *
avf_init (vlib_main_t * vm)
{
  avf_main_t *am = &avf_main;
  clib_error_t *error;
  vlib_thread_main_t *tm = vlib_get_thread_main ();
  int i;

  if ((error = vlib_call_init_function (vm, pci_bus_init)))
    return error;

  vec_validate_aligned (am->per_thread_data, tm->n_vlib_mains - 1,
			CLIB_CACHE_LINE_BYTES);

  /* initialize ptype based loopup table */
  vec_validate_aligned (am->ptypes, 255, CLIB_CACHE_LINE_BYTES);

  /* *INDENT-OFF* */
  vec_foreach_index (i, am->ptypes)
    {
      avf_ptype_t *p = vec_elt_at_index (am->ptypes, i);
      if ((i >= 22) && (i <= 87))
	{
	  p->next_node = VNET_DEVICE_INPUT_NEXT_IP4_NCS_INPUT;
	  p->flags = VNET_BUFFER_F_IS_IP4;
	}
      else if ((i >= 88) && (i <= 153))
	{
	  p->next_node = VNET_DEVICE_INPUT_NEXT_IP6_INPUT;
	  p->flags = VNET_BUFFER_F_IS_IP6;
	}
      else
	p->next_node = VNET_DEVICE_INPUT_NEXT_ETHERNET_INPUT;
      p->buffer_advance = device_input_next_node_advance[p->next_node];
      p->flags |= VLIB_BUFFER_TOTAL_LENGTH_VALID;
    }
  /* *INDENT-ON* */

  am->log_class = vlib_log_register_class ("avf_plugin", 0);
  vlib_log_debug (am->log_class, "initialized");

  return 0;
}

VLIB_INIT_FUNCTION (avf_init);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
