
/*
 * Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 * Author: Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/dma-mapping.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/msm_audio.h>
#include <mach/debug_mm.h>
#include <mach/peripheral-loader.h>
#include <mach/qdsp6v2/apr_audio.h>
#include <mach/qdsp6v2/q6asm.h>
#include <asm/atomic.h>
#include <asm/ioctls.h>

#define SESSION_MAX 0x08
#define TRUE        0x01
#define FALSE       0x00
#define READDONE_IDX_STATUS 0
#define READDONE_IDX_BUFFER 1
#define READDONE_IDX_SIZE 2
#define READDONE_IDX_OFFSET 3
#define READDONE_IDX_MSW_TS 4
#define READDONE_IDX_LSW_TS 5
#define READDONE_IDX_FLAGS 6
#define READDONE_IDX_NUMFRAMES 7
#define READDONE_IDX_ID 8

static DEFINE_MUTEX(session_lock);

/* session id: 0 reserved */
static struct audio_client *session[SESSION_MAX+1];
static int32_t q6asm_mmapcallback(struct apr_client_data *data, void *priv);
static int32_t q6asm_callback(struct apr_client_data *data, void *priv);
static void q6asm_add_hdr(struct audio_client *ac, struct apr_hdr *hdr,
			uint32_t pkt_size, uint32_t cmd_flg);
static void q6asm_add_hdr_async(struct audio_client *ac, struct apr_hdr *hdr,
			uint32_t pkt_size, uint32_t cmd_flg);
static int q6asm_memory_map_regions(struct audio_client *ac, int dir,
				uint32_t bufsz, uint32_t bufcnt);
static int q6asm_memory_unmap_regions(struct audio_client *ac, int dir,
				uint32_t bufsz, uint32_t bufcnt);

static void q6asm_reset_buf_state(struct audio_client *ac);

struct asm_mmap {
	atomic_t ref_cnt;
	atomic_t cmd_state;
	wait_queue_head_t cmd_wait;
	void *apr;
};

static struct asm_mmap this_mmap;

static int q6asm_session_alloc(struct audio_client *ac)
{
	int n;
	mutex_lock(&session_lock);
	for (n = 1; n <= SESSION_MAX; n++) {
		if (!session[n]) {
			session[n] = ac;
			mutex_unlock(&session_lock);
			return n;
		}
	}
	mutex_unlock(&session_lock);
	return -ENOMEM;
}

static void q6asm_session_free(struct audio_client *ac)
{
	pr_debug("%s: sessionid[%d]\n", __func__, ac->session);
	mutex_lock(&session_lock);
	session[ac->session] = 0;
	mutex_unlock(&session_lock);
	return;
}

int q6asm_audio_client_buf_free(unsigned int dir,
			struct audio_client *ac)
{
	struct audio_port_data *port;
	int cnt = 0;
	int rc = 0;
	pr_debug("%s: Session id %d\n", __func__, ac->session);
	mutex_lock(&ac->cmd_lock);
	if (ac->io_mode == SYNC_IO_MODE) {
		port = &ac->port[dir];
		if (!port->buf) {
			mutex_unlock(&ac->cmd_lock);
			return 0;
		}
		cnt = port->max_buf_cnt - 1;

		if (cnt >= 0) {
			rc = q6asm_memory_unmap_regions(ac, dir,
							port->buf[0].size,
							port->max_buf_cnt);
			if (rc < 0)
				pr_aud_err("%s CMD Memory_unmap_regions failed\n",
								__func__);
		}

		while (cnt >= 0) {
			if (port->buf[cnt].data) {
				pr_debug("data[%p]phys[%p][%p] cnt[%d]\n",
					   (void *)port->buf[cnt].data,
					   (void *)port->buf[cnt].phys,
					   (void *)&port->buf[cnt].phys, cnt);
				dma_free_coherent(NULL, port->buf[cnt].size,
						port->buf[cnt].data,
						port->buf[cnt].phys);
				port->buf[cnt].data = NULL;
				port->buf[cnt].phys = 0;
				--(port->max_buf_cnt);
			}
			--cnt;
		}
		kfree(port->buf);
		port->buf = NULL;
	}
	mutex_unlock(&ac->cmd_lock);
	return 0;
}

int q6asm_audio_client_buf_free_contiguous(unsigned int dir,
			struct audio_client *ac)
{
	struct audio_port_data *port;
	int cnt = 0;
	int rc = 0;
	pr_debug("%s: Session id %d\n", __func__, ac->session);
	mutex_lock(&ac->cmd_lock);
	if (ac->io_mode == SYNC_IO_MODE) {
		port = &ac->port[dir];
		if (!port->buf) {
			mutex_unlock(&ac->cmd_lock);
			return 0;
		}
		cnt = port->max_buf_cnt - 1;

		if (cnt >= 0) {
			rc = q6asm_memory_unmap(ac, port->buf[0].phys, dir);
			if (rc < 0)
				pr_aud_err("%s CMD Memory_unmap_regions failed\n",
								__func__);
		}

		if (port->buf[0].data) {
			pr_debug("%s:data[%p]phys[%p][%p] cnt[%d]\n",
				   __func__,
				   (void *)port->buf[0].data,
				   (void *)port->buf[0].phys,
				   (void *)&port->buf[0].phys, cnt);
			dma_free_coherent(NULL,
				port->buf[0].size * port->max_buf_cnt,
				port->buf[0].data,
				port->buf[0].phys);
		}
		while (cnt >= 0) {
			port->buf[cnt].data = NULL;
			port->buf[cnt].phys = 0;
			cnt--;
		}
		port->max_buf_cnt = 0;
		kfree(port->buf);
		port->buf = NULL;
	}
	mutex_unlock(&ac->cmd_lock);
	return 0;
}

void q6asm_audio_client_free(struct audio_client *ac)
{
	int loopcnt;
	struct audio_port_data *port;
	if (!ac || !ac->session)
		return;
	pr_debug("%s: Session id %d\n", __func__, ac->session);
	if (ac->io_mode == SYNC_IO_MODE) {
		for (loopcnt = 0; loopcnt <= OUT; loopcnt++) {
			port = &ac->port[loopcnt];
			if (!port->buf)
				continue;
			pr_debug("%s:loopcnt = %d\n", __func__, loopcnt);
			q6asm_audio_client_buf_free(loopcnt, ac);
		}
	}

	q6asm_session_free(ac);
	apr_deregister(ac->apr);

	pr_debug("%s: APR De-Register\n", __func__);
	if (atomic_read(&this_mmap.ref_cnt) <= 0) {
		pr_aud_err("%s: APR Common Port Already Closed\n", __func__);
		goto done;
	}

	atomic_dec(&this_mmap.ref_cnt);
	if (atomic_read(&this_mmap.ref_cnt) == 0) {
		apr_deregister(this_mmap.apr);
		pr_debug("%s:APR De-Register common port\n", __func__);
	}
done:
	kfree(ac);
	return;
}

int q6asm_set_io_mode(struct audio_client *ac, uint32_t mode)
{
	if (ac == NULL) {
		pr_aud_err("%s APR handle NULL\n", __func__);
		return -EINVAL;
	}
	if ((mode == ASYNC_IO_MODE) || (mode == SYNC_IO_MODE)) {
		ac->io_mode = mode;
		pr_debug("%s:Set Mode to %d\n", __func__, ac->io_mode);
		return 0;
	} else {
		pr_aud_err("%s:Not an valid IO Mode:%d\n", __func__, ac->io_mode);
		return -EINVAL;
	}
}

struct audio_client *q6asm_audio_client_alloc(app_cb cb, void *priv)
{
	struct audio_client *ac;
	int n;
	int lcnt = 0;

	ac = kzalloc(sizeof(struct audio_client), GFP_KERNEL);
	if (!ac)
		return NULL;
	n = q6asm_session_alloc(ac);
	if (n <= 0)
		goto fail_session;
	ac->session = n;
	ac->cb = cb;
	ac->priv = priv;
	ac->io_mode = SYNC_IO_MODE;
	ac->apr = apr_register("ADSP", "ASM", \
				(apr_fn)q6asm_callback,\
				((ac->session) << 8 | 0x0001),\
				ac);

	if (ac->apr == NULL) {
		pr_aud_err("%s Registration with APR failed\n", __func__);
			goto fail;
	}
	pr_debug("%s Registering the common port with APR\n", __func__);
	if (atomic_read(&this_mmap.ref_cnt) == 0) {
		this_mmap.apr = apr_register("ADSP", "ASM", \
					(apr_fn)q6asm_mmapcallback,\
					0x0FFFFFFFF, &this_mmap);
		if (this_mmap.apr == NULL) {
			pr_debug("%s Unable to register \
				APR ASM common port \n", __func__);
			goto fail;
		}
	}

	atomic_inc(&this_mmap.ref_cnt);
	init_waitqueue_head(&ac->cmd_wait);
	mutex_init(&ac->cmd_lock);
	for (lcnt = 0; lcnt <= OUT; lcnt++) {
		mutex_init(&ac->port[lcnt].lock);
		spin_lock_init(&ac->port[lcnt].dsp_lock);
	}
	atomic_set(&ac->cmd_state, 0);

	pr_debug("%s: session[%d]\n", __func__, ac->session);

	return ac;
fail:
	q6asm_audio_client_free(ac);
	return NULL;
fail_session:
	kfree(ac);
	return NULL;
}

int q6asm_audio_client_buf_alloc(unsigned int dir,
			struct audio_client *ac,
			unsigned int bufsz,
			unsigned int bufcnt)
{
	int cnt = 0;
	int rc = 0;
	struct audio_buffer *buf;

	if (!(ac) || ((dir != IN) && (dir != OUT)))
		return -EINVAL;

	pr_debug("%s: session[%d]bufsz[%d]bufcnt[%d]\n", __func__, ac->session,
		bufsz, bufcnt);

	if (ac->session <= 0 || ac->session > 8)
		goto fail;

	if (ac->io_mode == SYNC_IO_MODE) {
		if (ac->port[dir].buf) {
			pr_debug("%s: buffer already allocated\n", __func__);
			return 0;
		}
		mutex_lock(&ac->cmd_lock);
		buf = kzalloc(((sizeof(struct audio_buffer))*bufcnt),
				GFP_KERNEL);

		if (!buf) {
			mutex_unlock(&ac->cmd_lock);
			goto fail;
		}

		ac->port[dir].buf = buf;

		while (cnt < bufcnt) {
			if (bufsz > 0) {
				buf[cnt].data = dma_alloc_coherent(NULL, bufsz,
								&buf[cnt].phys,
								GFP_KERNEL);
				if (!buf[cnt].data) {
					pr_aud_err("%s Buf alloc failed for"
						" size=%d\n", __func__,
						bufsz);
					mutex_unlock(&ac->cmd_lock);
					goto fail;
				}
				buf[cnt].used = 1;
				buf[cnt].size = bufsz;
				buf[cnt].actual_size = bufsz;
				pr_debug("%s data[%p]phys[%p][%p]\n", __func__,
					   (void *)buf[cnt].data,
					   (void *)buf[cnt].phys,
					   (void *)&buf[cnt].phys);
			}
			cnt++;
		}
		ac->port[dir].max_buf_cnt = cnt;

		mutex_unlock(&ac->cmd_lock);
		rc = q6asm_memory_map_regions(ac, dir, bufsz, cnt);
		if (rc < 0) {
			pr_aud_err("%s:CMD Memory_map_regions failed\n", __func__);
			goto fail;
		}
	}
	return 0;
fail:
	q6asm_audio_client_buf_free(dir, ac);
	return -EINVAL;
}

int q6asm_audio_client_buf_alloc_contiguous(unsigned int dir,
			struct audio_client *ac,
			unsigned int bufsz,
			unsigned int bufcnt)
{
	int cnt = 0;
	int rc = 0;
	struct audio_buffer *buf;

	if (!(ac) || ((dir != IN) && (dir != OUT)))
		return -EINVAL;

	pr_debug("%s: session[%d]bufsz[%d]bufcnt[%d]\n",
			__func__, ac->session,
			bufsz, bufcnt);

	if (ac->session <= 0 || ac->session > 8)
		goto fail;

	if (ac->io_mode == SYNC_IO_MODE) {
		if (ac->port[dir].buf) {
			pr_debug("%s: buffer already allocated\n", __func__);
			return 0;
		}
		mutex_lock(&ac->cmd_lock);
		buf = kzalloc(((sizeof(struct audio_buffer))*bufcnt),
				GFP_KERNEL);

		if (!buf) {
			mutex_unlock(&ac->cmd_lock);
			goto fail;
		}

		ac->port[dir].buf = buf;

		buf[0].data =  dma_alloc_coherent(NULL, bufsz * bufcnt,
					&buf[0].phys, GFP_KERNEL);
		buf[0].used = dir ^ 1;
		buf[0].size = bufsz;
		buf[0].actual_size = bufsz;
		cnt = 1;
		while (cnt < bufcnt) {
			if (bufsz > 0) {
				buf[cnt].data =  buf[0].data + (cnt * bufsz);
				buf[cnt].phys =  buf[0].phys + (cnt * bufsz);

				if (!buf[cnt].data) {
					pr_aud_err("%s Buf alloc failed\n",
								__func__);
					mutex_unlock(&ac->cmd_lock);
					goto fail;
				}
				buf[cnt].used = dir ^ 1;
				buf[cnt].size = bufsz;
				buf[cnt].actual_size = bufsz;
				pr_debug("%s data[%p]phys[%p][%p]\n", __func__,
					   (void *)buf[cnt].data,
					   (void *)buf[cnt].phys,
					   (void *)&buf[cnt].phys);
			}
			cnt++;
		}
		ac->port[dir].max_buf_cnt = cnt;

		mutex_unlock(&ac->cmd_lock);
		rc = q6asm_memory_map(ac, buf[0].phys, dir, bufsz, cnt);
		if (rc < 0) {
			pr_aud_err("%s:CMD Memory_map_regions failed\n", __func__);
			goto fail;
		}
	}
	return 0;
fail:
	q6asm_audio_client_buf_free_contiguous(dir, ac);
	return -EINVAL;
}

static int32_t q6asm_mmapcallback(struct apr_client_data *data, void *priv)
{
	uint32_t token;
	uint32_t *payload = data->payload;

	pr_debug("%s:ptr0[0x%x]ptr1[0x%x]opcode[0x%x]"
		"token[0x%x]payload_s[%d] src[%d] dest[%d]\n", __func__,
		payload[0], payload[1], data->opcode, data->token,
		data->payload_size, data->src_port, data->dest_port);

	if (data->opcode == APR_BASIC_RSP_RESULT) {
		token = data->token;
		switch (payload[0]) {
		case ASM_SESSION_CMD_MEMORY_MAP:
		case ASM_SESSION_CMD_MEMORY_UNMAP:
		case ASM_SESSION_CMD_MEMORY_MAP_REGIONS:
		case ASM_SESSION_CMD_MEMORY_UNMAP_REGIONS:
			pr_debug("%s:command[0x%x]success [0x%x]\n",
					__func__, payload[0], payload[1]);
			if (atomic_read(&this_mmap.cmd_state)) {
				atomic_set(&this_mmap.cmd_state, 0);
				wake_up(&this_mmap.cmd_wait);
			}
			break;
		default:
			pr_debug("%s:command[0x%x] not expecting rsp\n",
						__func__, payload[0]);
			break;
		}
	}
	return 0;
}


static int32_t q6asm_callback(struct apr_client_data *data, void *priv)
{
	int i = 0;
	struct audio_client *ac = (struct audio_client *)priv;
	uint32_t token;
	unsigned long dsp_flags;
	uint32_t *payload;

	if ((ac == NULL) || (data == NULL)) {
		pr_aud_err("ac or priv NULL\n");
		return -EINVAL;
	}
	payload = data->payload;

	pr_debug("%s: session[%d]ptr0[0x%x]ptr1[0x%x]opcode[0x%x] \
		token[0x%x]payload_s[%d] src[%d] dest[%d]\n", __func__,
		ac->session, payload[0], payload[1], data->opcode,
		data->token, data->payload_size, data->src_port,
		data->dest_port);

	if (data->opcode == APR_BASIC_RSP_RESULT) {
		token = data->token;
		switch (payload[0]) {
		case ASM_SESSION_CMD_PAUSE:
		case ASM_DATA_CMD_EOS:
		case ASM_STREAM_CMD_CLOSE:
		case ASM_STREAM_CMD_FLUSH:
		case ASM_SESSION_CMD_RUN:
		case ASM_SESSION_CMD_REGISTER_FOR_TX_OVERFLOW_EVENTS:
		pr_debug("%s:Payload = [0x%x]\n", __func__, payload[0]);
		if (token != ac->session) {
			pr_aud_err("%s:Invalid session[%d] rxed expected[%d]",
					__func__, token, ac->session);
			break;
		}
		case ASM_STREAM_CMD_OPEN_READ:
		case ASM_STREAM_CMD_OPEN_WRITE:
		case ASM_STREAM_CMD_OPEN_READWRITE:
		case ASM_DATA_CMD_MEDIA_FORMAT_UPDATE:
		case ASM_STREAM_CMD_SET_ENCDEC_PARAM:
		case ASM_STREAM_CMD_SET_PP_PARAMS:
			pr_debug("%s:command[0x%x]success [0x%x]",
				__func__, payload[0], payload[1]);
			if (atomic_read(&ac->cmd_state)) {
				atomic_set(&ac->cmd_state, 0);
				wake_up(&ac->cmd_wait);
			}
			if (ac->cb)
				ac->cb(data->opcode, data->token,
					(uint32_t *)data->payload, ac->priv);
			break;
		default:
			pr_debug("%s:command[0x%x] not expecting rsp\n",
							__func__, payload[0]);
			break;
		}
		return 0;
	}

	switch (data->opcode) {
	case ASM_DATA_EVENT_WRITE_DONE:{
		struct audio_port_data *port = &ac->port[IN];
		pr_debug("%s: Rxed opcode[0x%x] status[0x%x] token[%d]",
				__func__, payload[0], payload[1],
				data->token);
		if (ac->io_mode == SYNC_IO_MODE) {
			if (port->buf == NULL) {
				pr_aud_err("%s: Unexpected Write Done\n",
								__func__);
				return -EINVAL;
			}
			spin_lock_irqsave(&port->dsp_lock, dsp_flags);
			if (port->buf[data->token].phys !=
				payload[0]) {
				pr_aud_err("Buf expected[%p]rxed[%p]\n",\
				   (void *)port->buf[data->token].phys,\
				   (void *)payload[0]);
				spin_unlock_irqrestore(&port->dsp_lock,
								dsp_flags);
				return -EINVAL;
			}
			token = data->token;
			port->buf[token].used = 1;
			spin_unlock_irqrestore(&port->dsp_lock, dsp_flags);
			for (i = 0; i < port->max_buf_cnt; i++)
				pr_debug("%d ", port->buf[i].used);

		}
		break;
	}
	case ASM_DATA_EVENT_READ_DONE:{

		struct audio_port_data *port = &ac->port[OUT];

		pr_debug("%s:R-D: status=%d buff_add=%x act_size=%d offset=%d\n",
				__func__, payload[READDONE_IDX_STATUS],
				payload[READDONE_IDX_BUFFER],
				payload[READDONE_IDX_SIZE],
				payload[READDONE_IDX_OFFSET]);
		pr_debug("%s:R-D:msw_ts=%d lsw_ts=%d flags=%d id=%d num=%d\n",
				__func__, payload[READDONE_IDX_MSW_TS],
				payload[READDONE_IDX_LSW_TS],
				payload[READDONE_IDX_FLAGS],
				payload[READDONE_IDX_ID],
				payload[READDONE_IDX_NUMFRAMES]);

		if (ac->io_mode == SYNC_IO_MODE) {
			if (port->buf == NULL) {
				pr_aud_err("%s: Unexpected Write Done\n", __func__);
				return -EINVAL;
			}
			spin_lock_irqsave(&port->dsp_lock, dsp_flags);
			token = data->token;
			if (port->buf[token].phys !=
				payload[READDONE_IDX_BUFFER]) {
				pr_aud_err("Buf expected[%p]rxed[%p]\n",\
				   (void *)port->buf[token].phys,\
				   (void *)payload[READDONE_IDX_BUFFER]);
				spin_unlock_irqrestore(&port->dsp_lock,
							dsp_flags);
				break;
			}
			port->buf[token].actual_size =
				payload[READDONE_IDX_SIZE];
			spin_unlock_irqrestore(&port->dsp_lock, dsp_flags);
		}
		break;
	}
	case ASM_DATA_EVENT_EOS:
	case ASM_DATA_CMDRSP_EOS:
		pr_debug("%s:EOS ACK received: rxed opcode[0x%x]\n",
				  __func__, data->opcode);
		break;
	case ASM_STREAM_CMDRSP_GET_ENCDEC_PARAM:
		break;
	case ASM_STREAM_CMDRSP_GET_PP_PARAMS:
		break;
	case ASM_SESSION_EVENT_TX_OVERFLOW:
		pr_aud_err("ASM_SESSION_EVENT_TX_OVERFLOW\n");
		break;
	}
	if (ac->cb)
		ac->cb(data->opcode, data->token,
			data->payload, ac->priv);

	return 0;
}

void *q6asm_is_cpu_buf_avail(int dir, struct audio_client *ac, uint32_t *size,
				uint32_t *index)
{
	void *data;
	unsigned char idx;
	struct audio_port_data *port;

	if (!ac || ((dir != IN) && (dir != OUT)))
		return NULL;

	if (ac->io_mode == SYNC_IO_MODE) {
		port = &ac->port[dir];

		mutex_lock(&port->lock);
		idx = port->cpu_buf;
		if (port->buf == NULL) {
			pr_debug("%s:Buffer pointer null\n", __func__);
			return NULL;
		}
		/*  dir 0: used = 0 means buf in use
			dir 1: used = 1 means buf in use */
		if (port->buf[idx].used == dir) {
			/* To make it more robust, we could loop and get the
			next avail buf, its risky though */
			pr_debug("%s:Next buf idx[0x%x] not available,\
				dir[%d]\n", __func__, idx, dir);
			mutex_unlock(&port->lock);
			return NULL;
		}
		*size = port->buf[idx].actual_size;
		*index = port->cpu_buf;
		data = port->buf[idx].data;
		pr_debug("%s:session[%d]index[%d] data[%p]size[%d]\n",
						__func__,
						ac->session,
						port->cpu_buf,
						data, *size);
		/* By default increase the cpu_buf cnt
		user accesses this function,increase cpu
		buf(to avoid another api)*/
		port->buf[idx].used = dir;
		port->cpu_buf = ((port->cpu_buf + 1) & (port->max_buf_cnt - 1));
		mutex_unlock(&port->lock);
		return data;
	}
	return NULL;
}

int q6asm_is_dsp_buf_avail(int dir, struct audio_client *ac)
{
	int ret = -1;
	struct audio_port_data *port;
	uint32_t idx;

	if (!ac || (dir != OUT))
		return ret;

	if (ac->io_mode == SYNC_IO_MODE) {
		port = &ac->port[dir];

		mutex_lock(&port->lock);
		idx = port->dsp_buf;

		if (port->buf[idx].used == (dir ^ 1)) {
			/* To make it more robust, we could loop and get the
			next avail buf, its risky though */
			pr_aud_err("Next buf idx[0x%x] not available, dir[%d]\n",
								idx, dir);
			mutex_unlock(&port->lock);
			return ret;
		}
		pr_debug("%s: session[%d]dsp_buf=%d cpu_buf=%d\n", __func__,
			ac->session, port->dsp_buf, port->cpu_buf);
		ret = ((port->dsp_buf != port->cpu_buf) ? 0 : -1);
		mutex_unlock(&port->lock);
	}
	return ret;
}

static void q6asm_add_hdr(struct audio_client *ac, struct apr_hdr *hdr,
			uint32_t pkt_size, uint32_t cmd_flg)
{
	pr_debug("%s:session=%d pkt size=%d cmd_flg=%d\n", __func__, pkt_size,
		cmd_flg, ac->session);
	mutex_lock(&ac->cmd_lock);
	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, \
				APR_HDR_LEN(sizeof(struct apr_hdr)),\
				APR_PKT_VER);
	hdr->src_svc = ((struct apr_svc *)ac->apr)->id;
	hdr->src_domain = APR_DOMAIN_APPS;
	hdr->dest_svc = APR_SVC_ASM;
	hdr->dest_domain = APR_DOMAIN_ADSP;
	hdr->src_port = ((ac->session << 8) & 0xFF00) | 0x01;
	hdr->dest_port = ((ac->session << 8) & 0xFF00) | 0x01;
	if (cmd_flg) {
		hdr->token = ac->session;
		atomic_set(&ac->cmd_state, 1);
	}
	hdr->pkt_size  = pkt_size;
	mutex_unlock(&ac->cmd_lock);
	return;
}

static void q6asm_add_mmaphdr(struct apr_hdr *hdr, uint32_t pkt_size,
							uint32_t cmd_flg)
{
	pr_debug("%s:pkt size=%d cmd_flg=%d\n", __func__, pkt_size, cmd_flg);
	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, \
				APR_HDR_LEN(APR_HDR_SIZE), APR_PKT_VER);
	hdr->src_port = 0;
	hdr->dest_port = 0;
	if (cmd_flg) {
		hdr->token = 0;
		atomic_set(&this_mmap.cmd_state, 1);
	}
	hdr->pkt_size  = pkt_size;
	return;
}

int q6asm_open_read(struct audio_client *ac,
		uint32_t format)
{
	int rc = 0x00;
	struct asm_stream_cmd_open_read open;

	if ((ac == NULL) || (ac->apr == NULL)) {
		pr_aud_err("%s: APR handle NULL\n", __func__);
		return -EINVAL;
	}
	pr_debug("%s:session[%d]", __func__, ac->session);

	q6asm_add_hdr(ac, &open.hdr, sizeof(open), TRUE);
	open.hdr.opcode = ASM_STREAM_CMD_OPEN_READ;
	/* Stream prio : High, provide meta info with encoded frames */
	open.src_endpoint = ASM_END_POINT_DEVICE_MATRIX;
	open.pre_proc_top = DEFAULT_POPP_TOPOLOGY;

	switch (format) {
	case FORMAT_LINEAR_PCM:
		open.uMode = STREAM_PRIORITY_HIGH;
		open.format = LINEAR_PCM;
		break;
	case FORMAT_MPEG4_AAC:
		open.uMode = BUFFER_META_ENABLE | STREAM_PRIORITY_HIGH;
		open.format = MPEG4_AAC;
		break;
	case FORMAT_V13K:
		open.uMode = BUFFER_META_ENABLE | STREAM_PRIORITY_HIGH;
		open.format = V13K_FS;
		break;
	case FORMAT_EVRC:
		open.uMode = BUFFER_META_ENABLE | STREAM_PRIORITY_HIGH;
		open.format = EVRC_FS;
		break;
	case FORMAT_AMRNB:
		open.uMode = BUFFER_META_ENABLE | STREAM_PRIORITY_HIGH;
		open.format = AMRNB_FS;
		break;
	default:
		pr_aud_err("Invalid format[%d]\n", format);
		goto fail_cmd;
	}
	rc = apr_send_pkt(ac->apr, (uint32_t *) &open);
	if (rc < 0) {
		pr_aud_err("open failed op[0x%x]rc[%d]\n", \
						open.hdr.opcode, rc);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s: timeout. waited for OPEN_WRITE rc[%d]\n", __func__,
			rc);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_open_write(struct audio_client *ac, uint32_t format)
{
	int rc = 0x00;
	struct asm_stream_cmd_open_write open;

	if ((ac == NULL) || (ac->apr == NULL)) {
		pr_aud_err("%s: APR handle NULL\n", __func__);
		return -EINVAL;
	}
	pr_debug("%s: session[%d] wr_format[0x%x]", __func__, ac->session,
		format);

	q6asm_add_hdr(ac, &open.hdr, sizeof(open), TRUE);

	open.hdr.opcode = ASM_STREAM_CMD_OPEN_WRITE;
	open.uMode = STREAM_PRIORITY_HIGH;
	/* source endpoint : matrix */
	open.sink_endpoint = ASM_END_POINT_DEVICE_MATRIX;
	open.stream_handle = 0x00;
	open.post_proc_top = DEFAULT_POPP_TOPOLOGY;

	switch (format) {
	case FORMAT_LINEAR_PCM:
		open.format = LINEAR_PCM;
		break;
	case FORMAT_MPEG4_AAC:
		open.format = MPEG4_AAC;
		break;
	case FORMAT_WMA_V9:
		open.format = WMA_V9;
		break;
	case FORMAT_WMA_V10PRO:
		open.format = WMA_V10PRO;
		break;
	default:
		pr_aud_err("%s: Invalid format[%d]\n", __func__, format);
		goto fail_cmd;
	}
	rc = apr_send_pkt(ac->apr, (uint32_t *) &open);
	if (rc < 0) {
		pr_aud_err("%s: open failed op[0x%x]rc[%d]\n", \
					__func__, open.hdr.opcode, rc);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s: timeout. waited for OPEN_WRITR rc[%d]\n", __func__,
			rc);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_open_read_write(struct audio_client *ac,
			uint32_t rd_format,
			uint32_t wr_format)
{
	int rc = 0x00;
	struct asm_stream_cmd_open_read_write open;

	if ((ac == NULL) || (ac->apr == NULL)) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	pr_debug("%s: session[%d]", __func__, ac->session);
	pr_debug("wr_format[0x%x]rd_format[0x%x]",
				wr_format, rd_format);

	q6asm_add_hdr(ac, &open.hdr, sizeof(open), TRUE);
	open.hdr.opcode = ASM_STREAM_CMD_OPEN_READWRITE;

	open.uMode = BUFFER_META_ENABLE | STREAM_PRIORITY_NORMAL;
	/* source endpoint : matrix */
	open.post_proc_top = DEFAULT_POPP_TOPOLOGY;
	switch (wr_format) {
	case FORMAT_LINEAR_PCM:
		open.write_format = LINEAR_PCM;
		break;
	case FORMAT_WMA_V9:
		open.write_format = WMA_V9;
		break;
	case FORMAT_WMA_V10PRO:
		open.write_format = WMA_V10PRO;
		break;
	default:
		pr_aud_err("Invalid format[%d]\n", wr_format);
		goto fail_cmd;
	}

	switch (rd_format) {
	case FORMAT_LINEAR_PCM:
		open.read_format = LINEAR_PCM;
		break;
	case FORMAT_MPEG4_AAC:
		open.read_format = MPEG4_AAC;
		break;
	case FORMAT_V13K:
		open.read_format = V13K_FS;
		break;
	case FORMAT_EVRC:
		open.read_format = EVRC_FS;
		break;
	case FORMAT_AMRNB:
		open.read_format = AMRNB_FS;
		break;
	default:
		pr_aud_err("Invalid format[%d]\n", rd_format);
		goto fail_cmd;
	}
	pr_debug("%s:rdformat[0x%x]wrformat[0x%x]\n", __func__,
			open.read_format, open.write_format);

	rc = apr_send_pkt(ac->apr, (uint32_t *) &open);
	if (rc < 0) {
		pr_aud_err("open failed op[0x%x]rc[%d]\n", \
						open.hdr.opcode, rc);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for OPEN_WRITR rc[%d]\n", rc);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_run(struct audio_client *ac, uint32_t flags,
		uint32_t msw_ts, uint32_t lsw_ts)
{
	struct asm_stream_cmd_run run;
	int rc;
	if (!ac || ac->apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	pr_debug("%s session[%d]", __func__, ac->session);
	q6asm_add_hdr(ac, &run.hdr, sizeof(run), TRUE);

	run.hdr.opcode = ASM_SESSION_CMD_RUN;
	run.flags    = flags;
	run.msw_ts   = msw_ts;
	run.lsw_ts   = lsw_ts;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &run);
	if (rc < 0) {
		pr_aud_err("Commmand run failed[%d]", rc);
		goto fail_cmd;
	}

	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for run success rc[%d]", rc);
		goto fail_cmd;
	}

	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_run_nowait(struct audio_client *ac, uint32_t flags,
		uint32_t msw_ts, uint32_t lsw_ts)
{
	struct asm_stream_cmd_run run;
	int rc;
	if (!ac || ac->apr == NULL) {
		pr_aud_err("%s:APR handle NULL\n", __func__);
		return -EINVAL;
	}
	pr_debug("session[%d]", ac->session);
	q6asm_add_hdr_async(ac, &run.hdr, sizeof(run), TRUE);

	run.hdr.opcode = ASM_SESSION_CMD_RUN;
	run.flags    = flags;
	run.msw_ts   = msw_ts;
	run.lsw_ts   = lsw_ts;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &run);
	if (rc < 0) {
		pr_aud_err("%s:Commmand run failed[%d]", __func__, rc);
		return -EINVAL;
	}
	return 0;
}


int q6asm_enc_cfg_blk_aac(struct audio_client *ac,
			 uint32_t frames_per_buf,
			uint32_t sample_rate, uint32_t channels,
			uint32_t bit_rate, uint32_t mode, uint32_t format)
{
	struct asm_stream_cmd_encdec_cfg_blk enc_cfg;
	int rc = 0;

	pr_debug("%s:session[%d]frames[%d]SR[%d]ch[%d]bitrate[%d]mode[%d]"
		"format[%d]", __func__, ac->session, frames_per_buf,
		sample_rate, channels, bit_rate, mode, format);

	q6asm_add_hdr(ac, &enc_cfg.hdr, sizeof(enc_cfg), TRUE);

	enc_cfg.hdr.opcode = ASM_STREAM_CMD_SET_ENCDEC_PARAM;
	enc_cfg.param_id = ASM_ENCDEC_CFG_BLK_ID;
	enc_cfg.param_size = sizeof(struct asm_encode_cfg_blk);
	enc_cfg.enc_blk.frames_per_buf = frames_per_buf;
	enc_cfg.enc_blk.format_id = MPEG4_AAC;
	enc_cfg.enc_blk.cfg_size  = sizeof(struct asm_aac_read_cfg);
	enc_cfg.enc_blk.cfg.aac.bitrate = bit_rate;
	enc_cfg.enc_blk.cfg.aac.enc_mode = mode;
	enc_cfg.enc_blk.cfg.aac.format = format;
	enc_cfg.enc_blk.cfg.aac.ch_cfg = channels;
	enc_cfg.enc_blk.cfg.aac.sample_rate = sample_rate;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &enc_cfg);
	if (rc < 0) {
		pr_aud_err("Comamnd %d failed\n", ASM_STREAM_CMD_SET_ENCDEC_PARAM);
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for FORMAT_UPDATE\n");
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_enc_cfg_blk_pcm(struct audio_client *ac,
			uint32_t rate, uint32_t channels)
{
	struct asm_stream_cmd_encdec_cfg_blk  enc_cfg;

	int rc = 0;

	pr_debug("%s: Session %d\n", __func__, ac->session);

	q6asm_add_hdr(ac, &enc_cfg.hdr, sizeof(enc_cfg), TRUE);

	enc_cfg.hdr.opcode = ASM_STREAM_CMD_SET_ENCDEC_PARAM;
	enc_cfg.param_id = ASM_ENCDEC_CFG_BLK_ID;
	enc_cfg.param_size = sizeof(struct asm_encode_cfg_blk);
	enc_cfg.enc_blk.frames_per_buf = 1;
	enc_cfg.enc_blk.format_id = LINEAR_PCM;
	enc_cfg.enc_blk.cfg_size = sizeof(struct asm_pcm_cfg);
	enc_cfg.enc_blk.cfg.pcm.ch_cfg = channels;
	enc_cfg.enc_blk.cfg.pcm.bits_per_sample = 16;
	enc_cfg.enc_blk.cfg.pcm.sample_rate = rate;
	enc_cfg.enc_blk.cfg.pcm.is_signed = 1;
	enc_cfg.enc_blk.cfg.pcm.interleaved = 1;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &enc_cfg);
	if (rc < 0) {
		pr_aud_err("Comamnd open failed\n");
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout opcode[0x%x] ", enc_cfg.hdr.opcode);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_enc_cfg_blk_qcelp(struct audio_client *ac, uint32_t frames_per_buf,
		uint16_t min_rate, uint16_t max_rate,
		uint16_t reduced_rate_level, uint16_t rate_modulation_cmd)
{
	struct asm_stream_cmd_encdec_cfg_blk enc_cfg;
	int rc = 0;

	pr_debug("%s:session[%d]frames[%d]min_rate[0x%4x]max_rate[0x%4x] \
		reduced_rate_level[0x%4x]rate_modulation_cmd[0x%4x]", __func__,
		ac->session, frames_per_buf, min_rate, max_rate,
		reduced_rate_level, rate_modulation_cmd);

	q6asm_add_hdr(ac, &enc_cfg.hdr, sizeof(enc_cfg), TRUE);

	enc_cfg.hdr.opcode = ASM_STREAM_CMD_SET_ENCDEC_PARAM;

	enc_cfg.param_id = ASM_ENCDEC_CFG_BLK_ID;
	enc_cfg.param_size = sizeof(struct asm_encode_cfg_blk);

	enc_cfg.enc_blk.frames_per_buf = frames_per_buf;
	enc_cfg.enc_blk.format_id = V13K_FS;
	enc_cfg.enc_blk.cfg_size  = sizeof(struct asm_qcelp13_read_cfg);
	enc_cfg.enc_blk.cfg.qcelp13.min_rate = min_rate;
	enc_cfg.enc_blk.cfg.qcelp13.max_rate = max_rate;
	enc_cfg.enc_blk.cfg.qcelp13.reduced_rate_level = reduced_rate_level;
	enc_cfg.enc_blk.cfg.qcelp13.rate_modulation_cmd = rate_modulation_cmd;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &enc_cfg);
	if (rc < 0) {
		pr_aud_err("Comamnd %d failed\n", ASM_STREAM_CMD_SET_ENCDEC_PARAM);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for FORMAT_UPDATE\n");
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_enc_cfg_blk_evrc(struct audio_client *ac, uint32_t frames_per_buf,
		uint16_t min_rate, uint16_t max_rate,
		uint16_t rate_modulation_cmd)
{
	struct asm_stream_cmd_encdec_cfg_blk enc_cfg;
	int rc = 0;

	pr_debug("%s:session[%d]frames[%d]min_rate[0x%4x]max_rate[0x%4x] \
		rate_modulation_cmd[0x%4x]", __func__, ac->session,
		frames_per_buf,	min_rate, max_rate, rate_modulation_cmd);

	q6asm_add_hdr(ac, &enc_cfg.hdr, sizeof(enc_cfg), TRUE);

	enc_cfg.hdr.opcode = ASM_STREAM_CMD_SET_ENCDEC_PARAM;

	enc_cfg.param_id = ASM_ENCDEC_CFG_BLK_ID;
	enc_cfg.param_size = sizeof(struct asm_encode_cfg_blk);

	enc_cfg.enc_blk.frames_per_buf = frames_per_buf;
	enc_cfg.enc_blk.format_id = EVRC_FS;
	enc_cfg.enc_blk.cfg_size  = sizeof(struct asm_evrc_read_cfg);
	enc_cfg.enc_blk.cfg.evrc.min_rate = min_rate;
	enc_cfg.enc_blk.cfg.evrc.max_rate = max_rate;
	enc_cfg.enc_blk.cfg.evrc.rate_modulation_cmd = rate_modulation_cmd;
	enc_cfg.enc_blk.cfg.evrc.reserved = 0;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &enc_cfg);
	if (rc < 0) {
		pr_aud_err("Comamnd %d failed\n", ASM_STREAM_CMD_SET_ENCDEC_PARAM);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for FORMAT_UPDATE\n");
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_enc_cfg_blk_amrnb(struct audio_client *ac, uint32_t frames_per_buf,
			uint16_t band_mode, uint16_t dtx_enable)
{
	struct asm_stream_cmd_encdec_cfg_blk enc_cfg;
	int rc = 0;

	pr_debug("%s:session[%d]frames[%d]band_mode[0x%4x]dtx_enable[0x%4x]",
		__func__, ac->session, frames_per_buf, band_mode, dtx_enable);

	q6asm_add_hdr(ac, &enc_cfg.hdr, sizeof(enc_cfg), TRUE);

	enc_cfg.hdr.opcode = ASM_STREAM_CMD_SET_ENCDEC_PARAM;

	enc_cfg.param_id = ASM_ENCDEC_CFG_BLK_ID;
	enc_cfg.param_size = sizeof(struct asm_encode_cfg_blk);

	enc_cfg.enc_blk.frames_per_buf = frames_per_buf;
	enc_cfg.enc_blk.format_id = AMRNB_FS;
	enc_cfg.enc_blk.cfg_size  = sizeof(struct asm_amrnb_read_cfg);
	enc_cfg.enc_blk.cfg.amrnb.mode = band_mode;
	enc_cfg.enc_blk.cfg.amrnb.dtx_mode = dtx_enable;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &enc_cfg);
	if (rc < 0) {
		pr_aud_err("Comamnd %d failed\n", ASM_STREAM_CMD_SET_ENCDEC_PARAM);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for FORMAT_UPDATE\n");
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_media_format_block_pcm(struct audio_client *ac,
				uint32_t rate, uint32_t channels)
{
	struct asm_stream_media_format_update fmt;
	int rc = 0;

	pr_debug("%s:session[%d]rate[%d]ch[%d]\n", __func__, ac->session, rate,
		channels);

	q6asm_add_hdr(ac, &fmt.hdr, sizeof(fmt), TRUE);

	fmt.hdr.opcode = ASM_DATA_CMD_MEDIA_FORMAT_UPDATE;

	fmt.format = LINEAR_PCM;
	fmt.cfg_size = sizeof(struct asm_pcm_cfg);
	fmt.write_cfg.pcm_cfg.ch_cfg = channels;
	fmt.write_cfg.pcm_cfg.bits_per_sample = 16;
	fmt.write_cfg.pcm_cfg.sample_rate = rate;
	fmt.write_cfg.pcm_cfg.is_signed = 1;
	fmt.write_cfg.pcm_cfg.interleaved = 1;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &fmt);
	if (rc < 0) {
		pr_aud_err("%s:Comamnd open failed\n", __func__);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s:timeout. waited for FORMAT_UPDATE\n", __func__);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_media_format_block_wma(struct audio_client *ac,
				void *cfg)
{
	struct asm_stream_media_format_update fmt;
	struct asm_wma_cfg *wma_cfg = (struct asm_wma_cfg *)cfg;
	int rc = 0;

	pr_debug("session[%d]format_tag[0x%4x] rate[%d] ch[0x%4x] bps[%d],\
		balign[0x%4x], bit_sample[0x%4x], ch_msk[%d], enc_opt[0x%4x]\n",
		ac->session, wma_cfg->format_tag, wma_cfg->sample_rate,
		wma_cfg->ch_cfg, wma_cfg->avg_bytes_per_sec,
		wma_cfg->block_align, wma_cfg->valid_bits_per_sample,
		wma_cfg->ch_mask, wma_cfg->encode_opt);

	q6asm_add_hdr(ac, &fmt.hdr, sizeof(fmt), TRUE);

	fmt.hdr.opcode = ASM_DATA_CMD_MEDIA_FORMAT_UPDATE;

	fmt.format = WMA_V9;
	fmt.cfg_size = sizeof(struct asm_wma_cfg);
	fmt.write_cfg.wma_cfg.format_tag = wma_cfg->format_tag;
	fmt.write_cfg.wma_cfg.ch_cfg = wma_cfg->ch_cfg;
	fmt.write_cfg.wma_cfg.sample_rate = wma_cfg->sample_rate;
	fmt.write_cfg.wma_cfg.avg_bytes_per_sec = wma_cfg->avg_bytes_per_sec;
	fmt.write_cfg.wma_cfg.block_align = wma_cfg->block_align;
	fmt.write_cfg.wma_cfg.valid_bits_per_sample =
			wma_cfg->valid_bits_per_sample;
	fmt.write_cfg.wma_cfg.ch_mask = wma_cfg->ch_mask;
	fmt.write_cfg.wma_cfg.encode_opt = wma_cfg->encode_opt;
	fmt.write_cfg.wma_cfg.adv_encode_opt = 0;
	fmt.write_cfg.wma_cfg.adv_encode_opt2 = 0;
	fmt.write_cfg.wma_cfg.drc_peak_ref = 0;
	fmt.write_cfg.wma_cfg.drc_peak_target = 0;
	fmt.write_cfg.wma_cfg.drc_ave_ref = 0;
	fmt.write_cfg.wma_cfg.drc_ave_target = 0;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &fmt);
	if (rc < 0) {
		pr_aud_err("%s:Comamnd open failed\n", __func__);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s:timeout. waited for FORMAT_UPDATE\n", __func__);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_media_format_block_wmapro(struct audio_client *ac,
				void *cfg)
{
	struct asm_stream_media_format_update fmt;
	struct asm_wmapro_cfg *wmapro_cfg = (struct asm_wmapro_cfg *)cfg;
	int rc = 0;

	pr_debug("session[%d]format_tag[0x%4x] rate[%d] ch[0x%4x] bps[%d],"
		"balign[0x%4x], bit_sample[0x%4x], ch_msk[%d], enc_opt[0x%4x],\
		adv_enc_opt[0x%4x], adv_enc_opt2[0x%8x]\n",
		ac->session, wmapro_cfg->format_tag, wmapro_cfg->sample_rate,
		wmapro_cfg->ch_cfg,  wmapro_cfg->avg_bytes_per_sec,
		wmapro_cfg->block_align, wmapro_cfg->valid_bits_per_sample,
		wmapro_cfg->ch_mask, wmapro_cfg->encode_opt,
		wmapro_cfg->adv_encode_opt, wmapro_cfg->adv_encode_opt2);

	q6asm_add_hdr(ac, &fmt.hdr, sizeof(fmt), TRUE);

	fmt.hdr.opcode = ASM_DATA_CMD_MEDIA_FORMAT_UPDATE;

	fmt.format = WMA_V10PRO;
	fmt.cfg_size = sizeof(struct asm_wmapro_cfg);
	fmt.write_cfg.wmapro_cfg.format_tag = wmapro_cfg->format_tag;
	fmt.write_cfg.wmapro_cfg.ch_cfg = wmapro_cfg->ch_cfg;
	fmt.write_cfg.wmapro_cfg.sample_rate = wmapro_cfg->sample_rate;
	fmt.write_cfg.wmapro_cfg.avg_bytes_per_sec =
				wmapro_cfg->avg_bytes_per_sec;
	fmt.write_cfg.wmapro_cfg.block_align = wmapro_cfg->block_align;
	fmt.write_cfg.wmapro_cfg.valid_bits_per_sample =
				wmapro_cfg->valid_bits_per_sample;
	fmt.write_cfg.wmapro_cfg.ch_mask = wmapro_cfg->ch_mask;
	fmt.write_cfg.wmapro_cfg.encode_opt = wmapro_cfg->encode_opt;
	fmt.write_cfg.wmapro_cfg.adv_encode_opt = wmapro_cfg->adv_encode_opt;
	fmt.write_cfg.wmapro_cfg.adv_encode_opt2 = wmapro_cfg->adv_encode_opt2;
	fmt.write_cfg.wmapro_cfg.drc_peak_ref = 0;
	fmt.write_cfg.wmapro_cfg.drc_peak_target = 0;
	fmt.write_cfg.wmapro_cfg.drc_ave_ref = 0;
	fmt.write_cfg.wmapro_cfg.drc_ave_target = 0;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &fmt);
	if (rc < 0) {
		pr_aud_err("%s:Comamnd open failed\n", __func__);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s:timeout. waited for FORMAT_UPDATE\n", __func__);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_memory_map(struct audio_client *ac, uint32_t buf_add, int dir,
					uint32_t bufsz, uint32_t bufcnt)
{
	struct asm_stream_cmd_memory_map mem_map;
	int rc = 0;

	if (!ac || ac->apr == NULL || this_mmap.apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}

	pr_debug("%s: Session[%d]\n", __func__, ac->session);

	mem_map.hdr.opcode = ASM_SESSION_CMD_MEMORY_MAP;

	mem_map.buf_add = buf_add;
	mem_map.buf_size = bufsz * bufcnt;
	mem_map.mempool_id = 0; /* EBI */
	mem_map.reserved = 0;

	q6asm_add_mmaphdr(&mem_map.hdr,
			sizeof(struct asm_stream_cmd_memory_map), TRUE);

	pr_debug("buf add[%x]  buf_add_parameter[%x]\n",
					mem_map.buf_add, buf_add);

	rc = apr_send_pkt(this_mmap.apr, (uint32_t *) &mem_map);
	if (rc < 0) {
		pr_aud_err("mem_map op[0x%x]rc[%d]\n",
				mem_map.hdr.opcode, rc);
		rc = -EINVAL;
		goto fail_cmd;
	}

	rc = wait_event_timeout(this_mmap.cmd_wait,
		(atomic_read(&this_mmap.cmd_state) == 0), 5 * HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for memory_map\n");
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = 0;
fail_cmd:
	return rc;
}

int q6asm_memory_unmap(struct audio_client *ac, uint32_t buf_add, int dir)
{
	struct asm_stream_cmd_memory_unmap mem_unmap;
	int rc = 0;

	if (!ac || ac->apr == NULL || this_mmap.apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	pr_debug("%s: Session[%d]\n", __func__, ac->session);

	q6asm_add_mmaphdr(&mem_unmap.hdr,
			sizeof(struct asm_stream_cmd_memory_unmap), TRUE);
	mem_unmap.hdr.opcode = ASM_SESSION_CMD_MEMORY_UNMAP;
	mem_unmap.buf_add = buf_add;

	rc = apr_send_pkt(this_mmap.apr, (uint32_t *) &mem_unmap);
	if (rc < 0) {
		pr_aud_err("mem_unmap op[0x%x]rc[%d]\n",
					mem_unmap.hdr.opcode, rc);
		rc = -EINVAL;
		goto fail_cmd;
	}

	rc = wait_event_timeout(this_mmap.cmd_wait,
			(atomic_read(&this_mmap.cmd_state) == 0), 5 * HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for memory_map\n");
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = 0;
fail_cmd:
	return rc;
}

int q6asm_set_lrgain(struct audio_client *ac, int left_gain, int right_gain)
{
	void *vol_cmd = NULL;
	void *payload = NULL;
	struct asm_pp_params_command *cmd = NULL;
	struct asm_lrchannel_gain_params *lrgain = NULL;
	int sz = 0;
	int rc  = 0;

	sz = sizeof(struct asm_pp_params_command) +
		+ sizeof(struct asm_lrchannel_gain_params);
	vol_cmd = kzalloc(sz, GFP_KERNEL);
	if (vol_cmd == NULL) {
		pr_aud_err("%s[%d]: Mem alloc failed\n", __func__, ac->session);
		rc = -EINVAL;
		return rc;
	}
	cmd = (struct asm_pp_params_command *)vol_cmd;
	q6asm_add_hdr_async(ac, &cmd->hdr, sz, TRUE);
	cmd->hdr.opcode = ASM_STREAM_CMD_SET_PP_PARAMS;
	cmd->payload = NULL;
	cmd->payload_size = sizeof(struct  asm_pp_param_data_hdr) +
				sizeof(struct asm_lrchannel_gain_params);
	cmd->params.module_id = VOLUME_CONTROL_MODULE_ID;
	cmd->params.param_id = L_R_CHANNEL_GAIN_PARAM_ID;
	cmd->params.param_size = sizeof(struct asm_lrchannel_gain_params);
	cmd->params.reserved = 0;

	payload = (u8 *)(vol_cmd + sizeof(struct asm_pp_params_command));
	lrgain = (struct asm_lrchannel_gain_params *)payload;

	lrgain->left_gain = left_gain;
	lrgain->right_gain = right_gain;
	rc = apr_send_pkt(ac->apr, (uint32_t *) vol_cmd);
	if (rc < 0) {
		pr_aud_err("%s: Volume Command failed\n", __func__);
		rc = -EINVAL;
		goto fail_cmd;
	}

	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s: timeout in sending volume command to apr\n",
			__func__);
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = 0;
fail_cmd:
	kfree(vol_cmd);
	return rc;
}

static int q6asm_memory_map_regions(struct audio_client *ac, int dir,
				uint32_t bufsz, uint32_t bufcnt)
{
	struct	 asm_stream_cmd_memory_map_regions *mmap_regions = NULL;
	struct asm_memory_map_regions *mregions = NULL;
	struct audio_port_data *port = NULL;
	struct audio_buffer *ab = NULL;
	void	*mmap_region_cmd = NULL;
	void	*payload = NULL;
	int	rc = 0;
	int	i = 0;
	int	cmd_size = 0;

	if (!ac || ac->apr == NULL || this_mmap.apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	pr_debug("%s: Session[%d]\n", __func__, ac->session);

	cmd_size = sizeof(struct asm_stream_cmd_memory_map_regions)
			+ sizeof(struct asm_memory_map_regions) * bufcnt;

	mmap_region_cmd = kzalloc(cmd_size, GFP_KERNEL);
	mmap_regions = (struct asm_stream_cmd_memory_map_regions *)
							mmap_region_cmd;
	q6asm_add_mmaphdr(&mmap_regions->hdr, cmd_size, TRUE);
	mmap_regions->hdr.opcode = ASM_SESSION_CMD_MEMORY_MAP_REGIONS;
	mmap_regions->mempool_id = 0;
	mmap_regions->nregions = bufcnt & 0x00ff;
	pr_debug("map_regions->nregions = %d\n", mmap_regions->nregions);
	payload = ((u8 *) mmap_region_cmd +
		sizeof(struct asm_stream_cmd_memory_map_regions));
	mregions = (struct asm_memory_map_regions *)payload;

	port = &ac->port[dir];
	for (i = 0; i < bufcnt; i++) {
		ab = &port->buf[i];
		mregions->phys = ab->phys;
		mregions->buf_size = ab->size;
		++mregions;
	}

	rc = apr_send_pkt(this_mmap.apr, (uint32_t *) mmap_region_cmd);
	if (rc < 0) {
		pr_aud_err("mmap_regions op[0x%x]rc[%d]\n",
					mmap_regions->hdr.opcode, rc);
		rc = -EINVAL;
		goto fail_cmd;
	}

	rc = wait_event_timeout(this_mmap.cmd_wait,
			(atomic_read(&this_mmap.cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for memory_map\n");
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = 0;
fail_cmd:
	kfree(mmap_region_cmd);
	return rc;
}

static int q6asm_memory_unmap_regions(struct audio_client *ac, int dir,
				uint32_t bufsz, uint32_t bufcnt)
{
	struct asm_stream_cmd_memory_unmap_regions *unmap_regions = NULL;
	struct asm_memory_unmap_regions *mregions = NULL;
	struct audio_port_data *port = NULL;
	struct audio_buffer *ab = NULL;
	void	*unmap_region_cmd = NULL;
	void	*payload = NULL;
	int	rc = 0;
	int	i = 0;
	int	cmd_size = 0;

	if (!ac || ac->apr == NULL || this_mmap.apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	pr_debug("%s: Session[%d]\n", __func__, ac->session);

	cmd_size = sizeof(struct asm_stream_cmd_memory_unmap_regions) +
			sizeof(struct asm_memory_unmap_regions) * bufcnt;

	unmap_region_cmd = kzalloc(cmd_size, GFP_KERNEL);
	unmap_regions = (struct asm_stream_cmd_memory_unmap_regions *)
							unmap_region_cmd;
	q6asm_add_mmaphdr(&unmap_regions->hdr, cmd_size, TRUE);
	unmap_regions->hdr.opcode = ASM_SESSION_CMD_MEMORY_UNMAP_REGIONS;
	unmap_regions->nregions = bufcnt & 0x00ff;
	pr_debug("unmap_regions->nregions = %d\n", unmap_regions->nregions);
	payload = ((u8 *) unmap_region_cmd +
			sizeof(struct asm_stream_cmd_memory_unmap_regions));
	mregions = (struct asm_memory_unmap_regions *)payload;
	port = &ac->port[dir];
	for (i = 0; i < bufcnt; i++) {
		ab = &port->buf[i];
		mregions->phys = ab->phys;
		++mregions;
	}

	rc = apr_send_pkt(this_mmap.apr, (uint32_t *) unmap_region_cmd);
	if (rc < 0) {
		pr_aud_err("mmap_regions op[0x%x]rc[%d]\n",
					unmap_regions->hdr.opcode, rc);
		goto fail_cmd;
	}

	rc = wait_event_timeout(this_mmap.cmd_wait,
			(atomic_read(&this_mmap.cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for memory_unmap\n");
		goto fail_cmd;
	}
	rc = 0;

fail_cmd:
	kfree(unmap_region_cmd);
	return rc;
}

int q6asm_set_mute(struct audio_client *ac, int muteflag)
{
	void *vol_cmd = NULL;
	void *payload = NULL;
	struct asm_pp_params_command *cmd = NULL;
	struct asm_mute_params *mute = NULL;
	int sz = 0;
	int rc  = 0;

	sz = sizeof(struct asm_pp_params_command) +
		+ sizeof(struct asm_mute_params);
	vol_cmd = kzalloc(sz, GFP_KERNEL);
	if (vol_cmd == NULL) {
		pr_aud_err("%s[%d]: Mem alloc failed\n", __func__, ac->session);
		rc = -EINVAL;
		return rc;
	}
	cmd = (struct asm_pp_params_command *)vol_cmd;
	q6asm_add_hdr_async(ac, &cmd->hdr, sz, TRUE);
	cmd->hdr.opcode = ASM_STREAM_CMD_SET_PP_PARAMS;
	cmd->payload = NULL;
	cmd->payload_size = sizeof(struct  asm_pp_param_data_hdr) +
				sizeof(struct asm_mute_params);
	cmd->params.module_id = VOLUME_CONTROL_MODULE_ID;
	cmd->params.param_id = MUTE_CONFIG_PARAM_ID;
	cmd->params.param_size = sizeof(struct asm_mute_params);
	cmd->params.reserved = 0;

	payload = (u8 *)(vol_cmd + sizeof(struct asm_pp_params_command));
	mute = (struct asm_mute_params *)payload;

	mute->muteflag = muteflag;
	rc = apr_send_pkt(ac->apr, (uint32_t *) vol_cmd);
	if (rc < 0) {
		pr_aud_err("%s: Mute Command failed\n", __func__);
		rc = -EINVAL;
		goto fail_cmd;
	}

	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s: timeout in sending mute command to apr\n",
			__func__);
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = 0;
fail_cmd:
	kfree(vol_cmd);
	return rc;
}

int q6asm_set_volume(struct audio_client *ac, int volume)
{
	void *vol_cmd = NULL;
	void *payload = NULL;
	struct asm_pp_params_command *cmd = NULL;
	struct asm_master_gain_params *mgain = NULL;
	int sz = 0;
	int rc  = 0;

	sz = sizeof(struct asm_pp_params_command) +
		+ sizeof(struct asm_master_gain_params);
	vol_cmd = kzalloc(sz, GFP_KERNEL);
	if (vol_cmd == NULL) {
		pr_aud_err("%s[%d]: Mem alloc failed\n", __func__, ac->session);
		rc = -EINVAL;
		return rc;
	}
	cmd = (struct asm_pp_params_command *)vol_cmd;
	q6asm_add_hdr_async(ac, &cmd->hdr, sz, TRUE);
	cmd->hdr.opcode = ASM_STREAM_CMD_SET_PP_PARAMS;
	cmd->payload = NULL;
	cmd->payload_size = sizeof(struct  asm_pp_param_data_hdr) +
				sizeof(struct asm_master_gain_params);
	cmd->params.module_id = VOLUME_CONTROL_MODULE_ID;
	cmd->params.param_id = MASTER_GAIN_PARAM_ID;
	cmd->params.param_size = sizeof(struct asm_master_gain_params);
	cmd->params.reserved = 0;

	payload = (u8 *)(vol_cmd + sizeof(struct asm_pp_params_command));
	mgain = (struct asm_master_gain_params *)payload;

	mgain->master_gain = volume;
	mgain->padding = 0x00;
	rc = apr_send_pkt(ac->apr, (uint32_t *) vol_cmd);
	if (rc < 0) {
		pr_aud_err("%s: Volume Command failed\n", __func__);
		rc = -EINVAL;
		goto fail_cmd;
	}

	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s: timeout in sending volume command to apr\n",
			__func__);
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = 0;
fail_cmd:
	kfree(vol_cmd);
	return rc;
}

int q6asm_equalizer(struct audio_client *ac, void *eq)
{
	void *eq_cmd = NULL;
	void *payload = NULL;
	struct asm_pp_params_command *cmd = NULL;
	struct asm_equalizer_params *equalizer = NULL;
	struct msm_audio_eq_stream_config *eq_params = NULL;
	int i  = 0;
	int sz = 0;
	int rc  = 0;

	sz = sizeof(struct asm_pp_params_command) +
		+ sizeof(struct asm_equalizer_params);
	eq_cmd = kzalloc(sz, GFP_KERNEL);
	if (eq_cmd == NULL) {
		pr_aud_err("%s[%d]: Mem alloc failed\n", __func__, ac->session);
		rc = -EINVAL;
		goto fail_cmd;
	}
	eq_params = (struct msm_audio_eq_stream_config *) eq;
	cmd = (struct asm_pp_params_command *)eq_cmd;
	q6asm_add_hdr(ac, &cmd->hdr, sz, TRUE);
	cmd->hdr.opcode = ASM_STREAM_CMD_SET_PP_PARAMS;
	cmd->payload = NULL;
	cmd->payload_size = sizeof(struct  asm_pp_param_data_hdr) +
				sizeof(struct asm_equalizer_params);
	cmd->params.module_id = EQUALIZER_MODULE_ID;
	cmd->params.param_id = EQUALIZER_PARAM_ID;
	cmd->params.param_size = sizeof(struct asm_equalizer_params);
	cmd->params.reserved = 0;
	payload = (u8 *)(eq_cmd + sizeof(struct asm_pp_params_command));
	equalizer = (struct asm_equalizer_params *)payload;

	equalizer->enable = eq_params->enable;
	equalizer->num_bands = eq_params->num_bands;
	pr_debug("%s: enable:%d numbands:%d\n", __func__, eq_params->enable,
							eq_params->num_bands);
	for (i = 0; i < eq_params->num_bands; i++) {
		equalizer->eq_bands[i].band_idx =
					eq_params->eq_bands[i].band_idx;
		equalizer->eq_bands[i].filter_type =
					eq_params->eq_bands[i].filter_type;
		equalizer->eq_bands[i].center_freq_hz =
					eq_params->eq_bands[i].center_freq_hz;
		equalizer->eq_bands[i].filter_gain =
					eq_params->eq_bands[i].filter_gain;
		equalizer->eq_bands[i].q_factor =
					eq_params->eq_bands[i].q_factor;
		pr_debug("%s: filter_type:%u bandnum:%d\n", __func__,
				eq_params->eq_bands[i].filter_type, i);
		pr_debug("%s: center_freq_hz:%u bandnum:%d\n", __func__,
				eq_params->eq_bands[i].center_freq_hz, i);
		pr_debug("%s: filter_gain:%d bandnum:%d\n", __func__,
				eq_params->eq_bands[i].filter_gain, i);
		pr_debug("%s: q_factor:%d bandnum:%d\n", __func__,
				eq_params->eq_bands[i].q_factor, i);
	}
	rc = apr_send_pkt(ac->apr, (uint32_t *) eq_cmd);
	if (rc < 0) {
		pr_aud_err("%s: Equalizer Command failed\n", __func__);
		rc = -EINVAL;
		goto fail_cmd;
	}

	rc = wait_event_timeout(ac->cmd_wait,
			(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("%s: timeout in sending equalizer command to apr\n",
			__func__);
		rc = -EINVAL;
		goto fail_cmd;
	}
	rc = 0;
fail_cmd:
	kfree(eq_cmd);
	return rc;
}

int q6asm_read(struct audio_client *ac)
{
	struct asm_stream_cmd_read read;
	struct audio_buffer        *ab;
	int dsp_buf;
	struct audio_port_data     *port;
	int rc;
	if (!ac || ac->apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	if (ac->io_mode == SYNC_IO_MODE) {
		port = &ac->port[OUT];

		q6asm_add_hdr(ac, &read.hdr, sizeof(read), FALSE);

		mutex_lock(&port->lock);

		dsp_buf = port->dsp_buf;
		ab = &port->buf[dsp_buf];
		port->buf[dsp_buf].used = 0;
		pr_debug("%s:session[%d]dsp-buf[%d][%p]cpu_buf[%d][%p]\n",
					__func__,
					ac->session,
					dsp_buf,
					(void *)port->buf[dsp_buf].data,
					port->cpu_buf,
					(void *)port->buf[port->cpu_buf].phys);

		read.hdr.opcode = ASM_DATA_CMD_READ;
		read.buf_add = ab->phys;
		read.buf_size = ab->size;
		read.uid = port->dsp_buf;
		read.hdr.token = port->dsp_buf;

		port->dsp_buf = (port->dsp_buf + 1) & (port->max_buf_cnt - 1);
		mutex_unlock(&port->lock);
		pr_debug("%s:buf add[0x%x] token[%d] uid[%d]\n", __func__,
						read.buf_add,
						read.hdr.token,
						read.uid);
		rc = apr_send_pkt(ac->apr, (uint32_t *) &read);
		if (rc < 0) {
			pr_aud_err("read op[0x%x]rc[%d]\n", read.hdr.opcode, rc);
			goto fail_cmd;
		}
		return 0;
	}
fail_cmd:
	return -EINVAL;
}

static void q6asm_add_hdr_async(struct audio_client *ac, struct apr_hdr *hdr,
			uint32_t pkt_size, uint32_t cmd_flg)
{
	pr_debug("session=%d pkt size=%d cmd_flg=%d\n", pkt_size, cmd_flg,
		ac->session);
	hdr->hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, \
				APR_HDR_LEN(sizeof(struct apr_hdr)),\
				APR_PKT_VER);
	hdr->src_svc = ((struct apr_svc *)ac->apr)->id;
	hdr->src_domain = APR_DOMAIN_APPS;
	hdr->dest_svc = APR_SVC_ASM;
	hdr->dest_domain = APR_DOMAIN_ADSP;
	hdr->src_port = ((ac->session << 8) & 0xFF00) | 0x01;
	hdr->dest_port = ((ac->session << 8) & 0xFF00) | 0x01;
	if (cmd_flg) {
		hdr->token = ac->session;
		atomic_set(&ac->cmd_state, 1);
	}
	hdr->pkt_size  = pkt_size;
	return;
}

int q6asm_async_write(struct audio_client *ac,
					  struct audio_aio_write_param *param)
{
	int rc = 0;
	struct asm_stream_cmd_write write;

	if (!ac || ac->apr == NULL) {
		pr_aud_err("%s: APR handle NULL\n", __func__);
		return -EINVAL;
	}

	q6asm_add_hdr_async(ac, &write.hdr, sizeof(write), FALSE);

	/* Pass physical address as token for AIO scheme */
	write.hdr.token = param->uid;
	write.hdr.opcode = ASM_DATA_CMD_WRITE;
	write.buf_add = param->paddr;
	write.avail_bytes = param->len;
	write.uid = param->uid;
	write.msw_ts = param->msw_ts;
	write.lsw_ts = param->lsw_ts;
	/* Use 0xFF00 for disabling timestamps */
	if (param->flags == 0xFF00)
		write.uflags = (0x00000000 | (param->flags & 0x800000FF));
	else
		write.uflags = (0x80000000 | param->flags);

	pr_debug("%s: session[%d] bufadd[0x%x]len[0x%x]", __func__, ac->session,
		write.buf_add, write.avail_bytes);

	rc = apr_send_pkt(ac->apr, (uint32_t *) &write);
	if (rc < 0) {
		pr_debug("[%s] write op[0x%x]rc[%d]\n", __func__,
			write.hdr.opcode, rc);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_async_read(struct audio_client *ac,
					  struct audio_aio_read_param *param)
{
	int rc = 0;
	struct asm_stream_cmd_read read;

	if (!ac || ac->apr == NULL) {
		pr_aud_err("%s: APR handle NULL\n", __func__);
		return -EINVAL;
	}

	q6asm_add_hdr_async(ac, &read.hdr, sizeof(read), FALSE);

	/* Pass physical address as token for AIO scheme */
	read.hdr.token = param->paddr;
	read.hdr.opcode = ASM_DATA_CMD_READ;
	read.buf_add = param->paddr;
	read.buf_size = param->len;
	read.uid = param->uid;

	pr_debug("%s: session[%d] bufadd[0x%x]len[0x%x]", __func__, ac->session,
		read.buf_add, read.buf_size);

	rc = apr_send_pkt(ac->apr, (uint32_t *) &read);
	if (rc < 0) {
		pr_debug("[%s] read op[0x%x]rc[%d]\n", __func__,
			read.hdr.opcode, rc);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_write(struct audio_client *ac, uint32_t len, uint32_t msw_ts,
		uint32_t lsw_ts, uint32_t flags)
{
	int rc = 0;
	struct asm_stream_cmd_write write;
	struct audio_port_data *port;
	struct audio_buffer    *ab;
	int dsp_buf = 0;

	if (!ac || ac->apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	pr_debug("%s: session[%d] len=%d", __func__, ac->session, len);
	if (ac->io_mode == SYNC_IO_MODE) {
		port = &ac->port[IN];

		q6asm_add_hdr(ac, &write.hdr, sizeof(write),
				FALSE);
		mutex_lock(&port->lock);

		dsp_buf = port->dsp_buf;
		ab = &port->buf[dsp_buf];

		write.hdr.token = port->dsp_buf;
		write.hdr.opcode = ASM_DATA_CMD_WRITE;
		write.buf_add = ab->phys;
		write.avail_bytes = len;
		write.uid = port->dsp_buf;
		write.msw_ts = msw_ts;
		write.lsw_ts = lsw_ts;
		/* Use 0xFF00 for disabling timestamps */
		if (flags == 0xFF00)
			write.uflags = (0x00000000 | (flags & 0x800000FF));
		else
			write.uflags = (0x80000000 | flags);
		port->dsp_buf = (port->dsp_buf + 1) & (port->max_buf_cnt - 1);

		pr_debug("%s:ab->phys[0x%x]bufadd[0x%x]token[0x%x]buf_id[0x%x]"
							, __func__,
							ab->phys,
							write.buf_add,
							write.hdr.token,
							write.uid);
		mutex_unlock(&port->lock);

		rc = apr_send_pkt(ac->apr, (uint32_t *) &write);
		if (rc < 0) {
			pr_aud_err("write op[0x%x]rc[%d]\n", write.hdr.opcode, rc);
			goto fail_cmd;
		}
		pr_debug("%s: WRITE SUCCESS\n", __func__);
		return 0;
	}
fail_cmd:
	return -EINVAL;
}

int q6asm_cmd(struct audio_client *ac, int cmd)
{
	struct apr_hdr hdr;
	int rc;
	atomic_t *state;
	int cnt = 0;

	if (!ac || ac->apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	q6asm_add_hdr(ac, &hdr, sizeof(hdr), TRUE);
	switch (cmd) {
	case CMD_PAUSE:
		pr_debug("%s:CMD_PAUSE\n", __func__);
		hdr.opcode = ASM_SESSION_CMD_PAUSE;
		state = &ac->cmd_state;
		break;
	case CMD_FLUSH:
		pr_debug("%s:CMD_FLUSH\n", __func__);
		hdr.opcode = ASM_STREAM_CMD_FLUSH;
		state = &ac->cmd_state;
		break;
	case CMD_EOS:
		pr_debug("%s:CMD_EOS\n", __func__);
		hdr.opcode = ASM_DATA_CMD_EOS;
		atomic_set(&ac->cmd_state, 0);
		state = &ac->cmd_state;
		break;
	case CMD_CLOSE:
		pr_debug("%s:CMD_CLOSE\n", __func__);
		hdr.opcode = ASM_STREAM_CMD_CLOSE;
		state = &ac->cmd_state;
		break;
	default:
		pr_aud_err("Invalid format[%d]\n", cmd);
		goto fail_cmd;
	}
	pr_debug("%s:session[%d]opcode[0x%x] ", __func__,
						ac->session,
						hdr.opcode);
	rc = apr_send_pkt(ac->apr, (uint32_t *) &hdr);
	if (rc < 0) {
		pr_aud_err("Commmand 0x%x failed\n", hdr.opcode);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait, (atomic_read(state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for response opcode[0x%x]\n",
							hdr.opcode);
		goto fail_cmd;
	}
	if (cmd == CMD_FLUSH)
		q6asm_reset_buf_state(ac);
	if (cmd == CMD_CLOSE) {
		/* check if DSP return all buffers */
		if (ac->port[IN].buf) {
			for (cnt = 0; cnt < ac->port[IN].max_buf_cnt;
								cnt++) {
				if (ac->port[IN].buf[cnt].used == IN) {
					pr_aud_info("Write Buf[%d] not returned\n",
									cnt);
				}
			}
		}
		if (ac->port[OUT].buf) {
			for (cnt = 0; cnt < ac->port[OUT].max_buf_cnt; cnt++) {
				if (ac->port[OUT].buf[cnt].used == (OUT ^ 1)) {
					pr_aud_info("Read Buf[%d] not returned\n",
									cnt);
				}
			}
		}
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

int q6asm_cmd_nowait(struct audio_client *ac, int cmd)
{
	struct apr_hdr hdr;
	int rc;

	if (!ac || ac->apr == NULL) {
		pr_aud_err("%s:APR handle NULL\n", __func__);
		return -EINVAL;
	}
	q6asm_add_hdr_async(ac, &hdr, sizeof(hdr), TRUE);
	switch (cmd) {
	case CMD_PAUSE:
		pr_debug("%s:CMD_PAUSE\n", __func__);
		hdr.opcode = ASM_SESSION_CMD_PAUSE;
		break;
	case CMD_EOS:
		pr_debug("%s:CMD_EOS\n", __func__);
		hdr.opcode = ASM_DATA_CMD_EOS;
		break;
	default:
		pr_aud_err("%s:Invalid format[%d]\n", __func__, cmd);
		goto fail_cmd;
	}
	pr_debug("%s:session[%d]opcode[0x%x] ", __func__,
						ac->session,
						hdr.opcode);
	rc = apr_send_pkt(ac->apr, (uint32_t *) &hdr);
	if (rc < 0) {
		pr_aud_err("%s:Commmand 0x%x failed\n", __func__, hdr.opcode);
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

static void q6asm_reset_buf_state(struct audio_client *ac)
{
	int cnt = 0;
	int loopcnt = 0;
	struct audio_port_data *port = NULL;

	if (ac->io_mode == SYNC_IO_MODE) {
		mutex_lock(&ac->cmd_lock);
		for (loopcnt = 0; loopcnt <= OUT; loopcnt++) {
			port = &ac->port[loopcnt];
			cnt = port->max_buf_cnt - 1;
			port->dsp_buf = 0;
			port->cpu_buf = 0;
			while (cnt >= 0) {
				if (!port->buf)
					continue;
				port->buf[cnt].used = 1;
				cnt--;
			}
		}
		mutex_unlock(&ac->cmd_lock);
	}
}

int q6asm_reg_tx_overflow(struct audio_client *ac, uint16_t enable)
{
	struct asm_stream_cmd_reg_tx_overflow_event tx_overflow;
	int rc;

	if (!ac || ac->apr == NULL) {
		pr_aud_err("APR handle NULL\n");
		return -EINVAL;
	}
	pr_debug("%s:session[%d]enable[%d]\n", __func__,
						ac->session, enable);
	q6asm_add_hdr(ac, &tx_overflow.hdr, sizeof(tx_overflow), TRUE);

	tx_overflow.hdr.opcode = \
			ASM_SESSION_CMD_REGISTER_FOR_TX_OVERFLOW_EVENTS;
	/* tx overflow event: enable */
	tx_overflow.enable = enable;

	rc = apr_send_pkt(ac->apr, (uint32_t *) &tx_overflow);
	if (rc < 0) {
		pr_aud_err("tx overflow op[0x%x]rc[%d]\n", \
						tx_overflow.hdr.opcode, rc);
		goto fail_cmd;
	}
	rc = wait_event_timeout(ac->cmd_wait,
				(atomic_read(&ac->cmd_state) == 0), 5*HZ);
	if (!rc) {
		pr_aud_err("timeout. waited for tx overflow\n");
		goto fail_cmd;
	}
	return 0;
fail_cmd:
	return -EINVAL;
}

static int __init q6asm_init(void)
{
	init_waitqueue_head(&this_mmap.cmd_wait);
	memset(session, 0, sizeof(session));
	return 0;
}

static void __exit q6asm_exit(void)
{
	pr_aud_info("%s\n", __func__);
	return;
}
device_initcall(q6asm_init);
