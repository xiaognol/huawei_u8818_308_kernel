/* arch/arm/mach-msm/qdsp5/audio_qcelp_in.c
 *
 * qcelp audio input device
 *
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This code is based in part on arch/arm/mach-msm/qdsp5v2/audio_qcelp_in.c,
 * Copyright (C) 2008 Google, Inc.
 * Copyright (C) 2008 HTC Corporation
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

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>

#include <linux/delay.h>

#include <linux/msm_audio_qcp.h>


#include <linux/memory_alloc.h>

#include <asm/atomic.h>
#include <asm/ioctls.h>

#include <mach/msm_memtypes.h>
#include <mach/msm_adsp.h>
#include <mach/msm_rpcrouter.h>
#include <mach/iommu.h>
#include <mach/iommu_domains.h>
#include <mach/msm_subsystem_map.h>
#include "audmgr.h"

#include <mach/qdsp5/qdsp5audpreproc.h>
#include <mach/qdsp5/qdsp5audpreproccmdi.h>
#include <mach/qdsp5/qdsp5audpreprocmsg.h>
#include <mach/qdsp5/qdsp5audreccmdi.h>
#include <mach/qdsp5/qdsp5audrecmsg.h>
#include <mach/debug_mm.h>

#define FRAME_HEADER_SIZE	8 /* 8 bytes frame header */
#define NT_FRAME_HEADER_SIZE	24 /* 24 bytes frame header */
/* FRAME_NUM must be a power of two */
#define FRAME_NUM	8
#define QCELP_FRAME_SIZE	36 /* 36 bytes data */
/*Tunnel mode : 36 bytes data + 8 byte header*/
#define FRAME_SIZE	(QCELP_FRAME_SIZE + FRAME_HEADER_SIZE)
 /* 36 bytes data  + 24 meta field*/
#define NT_FRAME_SIZE	(QCELP_FRAME_SIZE + NT_FRAME_HEADER_SIZE)
#define DMASZ		(FRAME_SIZE * FRAME_NUM)
#define NT_DMASZ	(NT_FRAME_SIZE * FRAME_NUM)
#define OUT_FRAME_NUM	2
#define OUT_BUFFER_SIZE (4 * 1024 + NT_FRAME_HEADER_SIZE)
#define BUFFER_SIZE	(OUT_BUFFER_SIZE * OUT_FRAME_NUM)

/* Offset from beginning of buffer*/
#define AUDPREPROC_QCELP_EOS_FLG_OFFSET 0x0A
#define AUDPREPROC_QCELP_EOS_FLG_MASK 0x01
#define AUDPREPROC_QCELP_EOS_NONE 0x0 /* No EOS detected */
#define AUDPREPROC_QCELP_EOS_SET 0x1 /* EOS set in meta field */

struct buffer {
	void *data;
	uint32_t size;
	uint32_t read;
	uint32_t addr;
	uint32_t used;
	uint32_t mfield_sz;
};

struct audio_qcelp_in {
	struct buffer in[FRAME_NUM];

	spinlock_t dsp_lock;

	atomic_t in_bytes;
	atomic_t in_samples;

	struct mutex lock;
	struct mutex read_lock;
	wait_queue_head_t wait;
	wait_queue_head_t wait_enable;
	/*write section*/
	struct buffer out[OUT_FRAME_NUM];

	uint8_t out_head;
	uint8_t out_tail;
	uint8_t out_needed;	/* number of buffers the dsp is waiting for */
	uint32_t out_count;

	struct mutex write_lock;
	wait_queue_head_t write_wait;
	int32_t out_phys; /* physical address of write buffer */
	char *out_data;
	int mfield; /* meta field embedded in data */
	int wflush; /*write flush */
	int rflush; /*read flush*/
	int out_frame_cnt;

	struct msm_adsp_module *audrec;
	struct msm_adsp_module *audpre;


	/* configuration to use on next enable */
	uint32_t samp_rate;
	uint32_t channel_mode;
	uint32_t buffer_size; /* Frame size (36 bytes) */
	uint32_t enc_type; /* 11 for QCELP */
	uint32_t mode; /* T or NT Mode*/

	struct msm_audio_qcelp_enc_config cfg;

	uint32_t dsp_cnt;
	uint32_t in_head; /* next buffer dsp will write */
	uint32_t in_tail; /* next buffer read() will read */
	uint32_t in_count; /* number of buffers available to read() */

	uint32_t eos_ack;
	uint32_t flush_ack;

	const char *module_name;
	unsigned queue_ids;
	uint16_t enc_id; /* Session Id */

	unsigned short samp_rate_index;
	uint32_t audrec_obj_idx ;

	struct audmgr audmgr;

	/* data allocated for various buffers */
	char *data;
	dma_addr_t phys;

	struct msm_mapped_buffer *map_v_read;
	struct msm_mapped_buffer *map_v_write;

	int opened;
	int enabled;
	int running;
	int stopped; /* set when stopped, cleared on flush */
};

struct audio_frame {
	uint16_t frame_count_lsw;
	uint16_t frame_count_msw;
	uint16_t frame_length;
	uint16_t erased_pcm;
	unsigned char raw_bitstream[];
} __packed;

struct audio_frame_nt {
	uint16_t metadata_len;
	uint16_t frame_count_lsw;
	uint16_t frame_count_msw;
	uint16_t frame_length;
	uint16_t erased_pcm;
	uint16_t reserved;
	uint16_t time_stamp_dword_lsw;
	uint16_t time_stamp_dword_msw;
	uint16_t time_stamp_lsw;
	uint16_t time_stamp_msw;
	uint16_t nflag_lsw;
	uint16_t nflag_msw;
	unsigned char raw_bitstream[]; /* samples */
} __packed;

struct qcelp_encoded_meta_out {
	uint16_t metadata_len;
	uint16_t time_stamp_dword_lsw;
	uint16_t time_stamp_dword_msw;
	uint16_t time_stamp_lsw;
	uint16_t time_stamp_msw;
	uint16_t nflag_lsw;
	uint16_t nflag_msw;
};

/* Audrec Queue command sent macro's */
#define audio_send_queue_pre(audio, cmd, len) \
	msm_adsp_write(audio->audpre, QDSP_uPAudPreProcCmdQueue, cmd, len)

#define audio_send_queue_recbs(audio, cmd, len) \
	msm_adsp_write(audio->audrec, ((audio->queue_ids & 0xFFFF0000) >> 16),\
			cmd, len)
#define audio_send_queue_rec(audio, cmd, len) \
	msm_adsp_write(audio->audrec, (audio->queue_ids & 0x0000FFFF),\
			cmd, len)

static int audqcelp_in_dsp_enable(struct audio_qcelp_in *audio, int enable);
static int audqcelp_in_encparam_config(struct audio_qcelp_in *audio);
static int audqcelp_in_encmem_config(struct audio_qcelp_in *audio);
static int audqcelp_in_dsp_read_buffer(struct audio_qcelp_in *audio,
				uint32_t read_cnt);
static void audqcelp_in_flush(struct audio_qcelp_in *audio);

static void audqcelp_in_get_dsp_frames(struct audio_qcelp_in *audio);
static int audpcm_config(struct audio_qcelp_in *audio);
static void audqcelp_out_flush(struct audio_qcelp_in *audio);
static int audqcelp_in_routing_mode_config(struct audio_qcelp_in *audio);
static void audrec_pcm_send_data(struct audio_qcelp_in *audio, unsigned needed);
static void audqcelp_nt_in_get_dsp_frames(struct audio_qcelp_in *audio);
static void audqcelp_in_flush(struct audio_qcelp_in *audio);

static unsigned convert_samp_index(unsigned index)
{
	switch (index) {
	case RPC_AUD_DEF_SAMPLE_RATE_48000:	return 48000;
	case RPC_AUD_DEF_SAMPLE_RATE_44100:	return 44100;
	case RPC_AUD_DEF_SAMPLE_RATE_32000:	return 32000;
	case RPC_AUD_DEF_SAMPLE_RATE_24000:	return 24000;
	case RPC_AUD_DEF_SAMPLE_RATE_22050:	return 22050;
	case RPC_AUD_DEF_SAMPLE_RATE_16000:	return 16000;
	case RPC_AUD_DEF_SAMPLE_RATE_12000:	return 12000;
	case RPC_AUD_DEF_SAMPLE_RATE_11025:	return 11025;
	case RPC_AUD_DEF_SAMPLE_RATE_8000:	return 8000;
	default:				return 11025;
	}
}

/* must be called with audio->lock held */
static int audqcelp_in_enable(struct audio_qcelp_in *audio)
{
	struct audmgr_config cfg;
	int rc;

	if (audio->enabled)
		return 0;

	cfg.tx_rate = audio->samp_rate;
	cfg.rx_rate = RPC_AUD_DEF_SAMPLE_RATE_NONE;
	cfg.def_method = RPC_AUD_DEF_METHOD_RECORD;
	cfg.codec = RPC_AUD_DEF_CODEC_13K;
	cfg.snd_method = RPC_SND_METHOD_MIDI;

	if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
		rc = audmgr_enable(&audio->audmgr, &cfg);
		if (rc < 0)
			return rc;

		if (msm_adsp_enable(audio->audpre)) {
			audmgr_disable(&audio->audmgr);
			MM_ERR("msm_adsp_enable(audpre) failed\n");
			return -ENODEV;
		}
	}
	if (msm_adsp_enable(audio->audrec)) {
		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
			audmgr_disable(&audio->audmgr);
			msm_adsp_disable(audio->audpre);
		}
		MM_ERR("msm_adsp_enable(audrec) failed\n");
		return -ENODEV;
	}

	audio->enabled = 1;
	audqcelp_in_dsp_enable(audio, 1);

	return 0;
}

/* must be called with audio->lock held */
static int audqcelp_in_disable(struct audio_qcelp_in *audio)
{
	if (audio->enabled) {
		audio->enabled = 0;

		audqcelp_in_dsp_enable(audio, 0);

		wake_up(&audio->wait);
		wait_event_interruptible_timeout(audio->wait_enable,
				audio->running == 0, 1*HZ);
		msm_adsp_disable(audio->audrec);
		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
			msm_adsp_disable(audio->audpre);
			audmgr_disable(&audio->audmgr);
		}
	}
	return 0;
}

/* ------------------- dsp --------------------- */
static void audpre_dsp_event(void *data, unsigned id, size_t len,
			    void (*getevent)(void *ptr, size_t len))
{
	uint16_t msg[2];
	getevent(msg, sizeof(msg));

	switch (id) {
	case AUDPREPROC_MSG_CMD_CFG_DONE_MSG:
		MM_DBG("type %d, status_flag %d\n", msg[0], msg[1]);
		break;
	case AUDPREPROC_MSG_ERROR_MSG_ID:
		MM_ERR("err_index %d\n", msg[0]);
		break;
	case ADSP_MESSAGE_ID:
		MM_DBG("Received ADSP event: module enable(audpreproctask)\n");
		break;
	default:
		MM_ERR("unknown event %d\n", id);
	}
}

static void audqcelp_in_get_dsp_frames(struct audio_qcelp_in *audio)
{
	struct audio_frame *frame;
	uint32_t index;
	unsigned long flags;

	index = audio->in_head;

	frame = (void *) (((char *)audio->in[index].data) -
			sizeof(*frame));
	spin_lock_irqsave(&audio->dsp_lock, flags);
	audio->in[index].size = frame->frame_length;

	/* statistics of read */
	atomic_add(audio->in[index].size, &audio->in_bytes);
	atomic_add(1, &audio->in_samples);

	audio->in_head = (audio->in_head + 1) & (FRAME_NUM - 1);

	/* If overflow, move the tail index foward. */
	if (audio->in_head == audio->in_tail) {
		MM_ERR("Error! not able to keep up the read\n");
		audio->in_tail = (audio->in_tail + 1) & (FRAME_NUM - 1);
		MM_ERR("in_count = %d\n", audio->in_count);
	} else
		audio->in_count++;

	audqcelp_in_dsp_read_buffer(audio, audio->dsp_cnt++);
	spin_unlock_irqrestore(&audio->dsp_lock, flags);

	wake_up(&audio->wait);
}

static void audqcelp_nt_in_get_dsp_frames(struct audio_qcelp_in *audio)
{
	struct audio_frame_nt *nt_frame;
	uint32_t index;
	unsigned long flags;

	index = audio->in_head;
	nt_frame = (void *) (((char *)audio->in[index].data) - \
				sizeof(struct audio_frame_nt));
	spin_lock_irqsave(&audio->dsp_lock, flags);
	audio->in[index].size = nt_frame->frame_length;
	/* statistics of read */
	atomic_add(audio->in[index].size, &audio->in_bytes);
	atomic_add(1, &audio->in_samples);

	audio->in_head = (audio->in_head + 1) & (FRAME_NUM - 1);

	/* If overflow, move the tail index foward. */
	if (audio->in_head == audio->in_tail)
		MM_DBG("Error! not able to keep up the read\n");
	else
		audio->in_count++;

	spin_unlock_irqrestore(&audio->dsp_lock, flags);
	wake_up(&audio->wait);
}

static int audrec_pcm_buffer_ptr_refresh(struct audio_qcelp_in *audio,
				       unsigned idx, unsigned len)
{
	struct audrec_cmd_pcm_buffer_ptr_refresh_arm_enc cmd;

	if (len ==  NT_FRAME_HEADER_SIZE)
		len = len / 2;
	else
		len = (len + NT_FRAME_HEADER_SIZE) / 2;
	MM_DBG("len = %d\n", len);
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_PCM_BUFFER_PTR_REFRESH_ARM_TO_ENC;
	cmd.num_buffers = 1;
	if (cmd.num_buffers == 1) {
		cmd.buf_address_length[0] = (audio->out[idx].addr &
							0xffff0000) >> 16;
		cmd.buf_address_length[1] = (audio->out[idx].addr &
							0x0000ffff);
		cmd.buf_address_length[2] = (len & 0xffff0000) >> 16;
		cmd.buf_address_length[3] = (len & 0x0000ffff);
	}
	audio->out_frame_cnt++;
	return audio_send_queue_rec(audio, &cmd, sizeof(cmd));
}

static int audpcm_config(struct audio_qcelp_in *audio)
{
	struct audrec_cmd_pcm_cfg_arm_to_enc cmd;
	MM_DBG("\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_PCM_CFG_ARM_TO_ENC;
	cmd.config_update_flag = AUDREC_PCM_CONFIG_UPDATE_FLAG_ENABLE;
	cmd.enable_flag = AUDREC_ENABLE_FLAG_VALUE;
	cmd.sampling_freq = convert_samp_index(audio->samp_rate);
	if (!audio->channel_mode)
		cmd.channels = 1;
	else
		cmd.channels = 2;
	cmd.frequency_of_intimation = 1;
	cmd.max_number_of_buffers = OUT_FRAME_NUM;
	return audio_send_queue_rec(audio, &cmd, sizeof(cmd));
}


static int audqcelp_in_routing_mode_config(struct audio_qcelp_in *audio)
{
	struct audrec_cmd_routing_mode cmd;

	MM_DBG("\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_ROUTING_MODE;
	if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL)
		cmd.routing_mode = 1;
	return audio_send_queue_rec(audio, &cmd, sizeof(cmd));
}

static void audrec_dsp_event(void *data, unsigned id, size_t len,
			    void (*getevent)(void *ptr, size_t len))
{
	struct audio_qcelp_in *audio = NULL;

	if (data)
		audio = data;
	else {
		MM_ERR("invalid data for event %x\n", id);
		return;
	}

	switch (id) {
	case AUDREC_MSG_CMD_CFG_DONE_MSG: {
		struct audrec_msg_cmd_cfg_done_msg cmd_cfg_done_msg;
		getevent(&cmd_cfg_done_msg, AUDREC_MSG_CMD_CFG_DONE_MSG_LEN);
		if (cmd_cfg_done_msg.audrec_enc_type & \
				AUDREC_MSG_CFG_DONE_ENC_ENA) {
			audio->audrec_obj_idx = cmd_cfg_done_msg.audrec_obj_idx;
			MM_DBG("CFG ENABLED\n");
			if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL) {
				MM_DBG("routing command\n");
				audqcelp_in_routing_mode_config(audio);
			} else {
				audqcelp_in_encmem_config(audio);
			}
		} else {
			MM_DBG("CFG SLEEP\n");
			audio->running = 0;
			wake_up(&audio->wait_enable);
		}
		break;
	}
	case AUDREC_MSG_CMD_ROUTING_MODE_DONE_MSG: {
		struct audrec_msg_cmd_routing_mode_done_msg \
			routing_msg;
		getevent(&routing_msg, AUDREC_MSG_CMD_ROUTING_MODE_DONE_MSG);
		MM_DBG("AUDREC_MSG_CMD_ROUTING_MODE_DONE_MSG");
		if (routing_msg.configuration == 0) {
			MM_ERR("routing configuration failed\n");
			audio->running = 0;
			wake_up(&audio->wait_enable);
		} else
			audqcelp_in_encmem_config(audio);
		break;
	}
	case AUDREC_MSG_CMD_AREC_MEM_CFG_DONE_MSG: {
		MM_DBG("AREC_MEM_CFG_DONE_MSG\n");
		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)
			audqcelp_in_encparam_config(audio);
		else
			audpcm_config(audio);
		break;
	}
	case AUDREC_CMD_PCM_CFG_ARM_TO_ENC_DONE_MSG: {
		MM_DBG("AUDREC_CMD_PCM_CFG_ARM_TO_ENC_DONE_MSG");
		audqcelp_in_encparam_config(audio);
	    break;
	}
	case AUDREC_MSG_CMD_AREC_PARAM_CFG_DONE_MSG: {
		MM_DBG("AUDREC_MSG_CMD_AREC_PARAM_CFG_DONE_MSG\n");
		audio->running = 1;
		wake_up(&audio->wait_enable);
		if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL)
			audrec_pcm_send_data(audio, 1);
		break;
	}
	case AUDREC_CMD_PCM_BUFFER_PTR_UPDATE_ARM_TO_ENC_MSG: {
		MM_DBG("ptr_update recieved from DSP\n");
		audrec_pcm_send_data(audio, 1);
		break;
	}
	case AUDREC_MSG_NO_EXT_PKT_AVAILABLE_MSG: {
		struct audrec_msg_no_ext_pkt_avail_msg err_msg;
		getevent(&err_msg, AUDREC_MSG_NO_EXT_PKT_AVAILABLE_MSG_LEN);
		MM_DBG("NO_EXT_PKT_AVAILABLE_MSG %x\n",\
			err_msg.audrec_err_id);
		break;
	}
	case AUDREC_MSG_PACKET_READY_MSG: {
		struct audrec_msg_packet_ready_msg pkt_ready_msg;

		getevent(&pkt_ready_msg, AUDREC_MSG_PACKET_READY_MSG_LEN);
		MM_DBG("UP_PACKET_READY_MSG: write cnt msw  %d \
		write cnt lsw %d read cnt msw %d  read cnt lsw %d \n",\
		pkt_ready_msg.pkt_counter_msw, \
		pkt_ready_msg.pkt_counter_lsw, \
		pkt_ready_msg.pkt_read_cnt_msw, \
		pkt_ready_msg.pkt_read_cnt_lsw);

		audqcelp_in_get_dsp_frames(audio);
		break;
	}
	case AUDREC_UP_NT_PACKET_READY_MSG: {
		struct audrec_up_nt_packet_ready_msg pkt_ready_msg;

		getevent(&pkt_ready_msg, AUDREC_UP_NT_PACKET_READY_MSG_LEN);
		MM_DBG("UP_NT_PACKET_READY_MSG: write cnt lsw  %d \
		write cnt msw %d read cnt lsw %d  read cnt msw %d \n",\
		pkt_ready_msg.audrec_packetwrite_cnt_lsw, \
		pkt_ready_msg.audrec_packetwrite_cnt_msw, \
		pkt_ready_msg.audrec_upprev_readcount_lsw, \
		pkt_ready_msg.audrec_upprev_readcount_msw);

		audqcelp_nt_in_get_dsp_frames(audio);
		break;
	}
	case AUDREC_CMD_FLUSH_DONE_MSG: {
		audio->wflush = 0;
		audio->rflush = 0;
		audio->flush_ack = 1;
		wake_up(&audio->write_wait);
		MM_DBG("flush ack recieved\n");
		break;
	}
	case ADSP_MESSAGE_ID:
		MM_DBG("Received ADSP event: module \
				enable/disable(audrectask)\n");
		break;
	default:
		MM_ERR("unknown event %d\n", id);
	}
}

static struct msm_adsp_ops audpre_qcelp_adsp_ops = {
	.event = audpre_dsp_event,
};

static struct msm_adsp_ops audrec_qcelp_adsp_ops = {
	.event = audrec_dsp_event,
};

static int audqcelp_in_dsp_enable(struct audio_qcelp_in *audio, int enable)
{
	struct audrec_cmd_enc_cfg cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_ENC_CFG;
	cmd.audrec_enc_type = (audio->enc_type & 0xFF) |
			(enable ? AUDREC_CMD_ENC_ENA : AUDREC_CMD_ENC_DIS);
	/* Don't care */
	cmd.audrec_obj_idx = audio->audrec_obj_idx;

	return audio_send_queue_rec(audio, &cmd, sizeof(cmd));
}

static int audqcelp_in_encmem_config(struct audio_qcelp_in *audio)
{
	struct audrec_cmd_arecmem_cfg cmd;
	uint16_t *data = (void *) audio->data;
	int n;
	int header_len = 0;

	memset(&cmd, 0, sizeof(cmd));

	cmd.cmd_id = AUDREC_CMD_ARECMEM_CFG;
	cmd.audrec_obj_idx = audio->audrec_obj_idx;
	/* Rate at which packet complete message comes */
	cmd.audrec_up_pkt_intm_cnt = 1;
	cmd.audrec_extpkt_buffer_msw = audio->phys >> 16;
	cmd.audrec_extpkt_buffer_lsw = audio->phys;
	/* Max Buffer no available for frames */
	cmd.audrec_extpkt_buffer_num = FRAME_NUM;

	/* prepare buffer pointers:
	 * T:36 bytes qcelp packet + 4 halfword header
	 * NT:36 bytes qcelp packet + 12 halfword header
	 */
	if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)
		header_len = FRAME_HEADER_SIZE/2;
	else
		header_len = NT_FRAME_HEADER_SIZE/2;

	for (n = 0; n < FRAME_NUM; n++) {
		audio->in[n].data = data + header_len;
		data += (QCELP_FRAME_SIZE/2) + header_len;
		MM_DBG("0x%8x\n", (int)(audio->in[n].data - header_len*2));
	}

	return audio_send_queue_rec(audio, &cmd, sizeof(cmd));
}

static int audqcelp_in_encparam_config(struct audio_qcelp_in *audio)
{
	struct audrec_cmd_arecparam_qcelp_cfg cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.common.cmd_id = AUDREC_CMD_ARECPARAM_CFG;
	cmd.common.audrec_obj_idx = audio->audrec_obj_idx;
	cmd.enc_min_rate = audio->cfg.min_bit_rate;
	cmd.enc_max_rate = audio->cfg.max_bit_rate;
	cmd.rate_modulation_cmd = 0;  /* Default set to 0 */
	cmd.reduced_rate_level = 0;  /* Default set to 0 */

	return audio_send_queue_rec(audio, &cmd, sizeof(cmd));
}

static int audqcelp_flush_command(struct audio_qcelp_in *audio)
{
	struct audrec_cmd_flush cmd;
	MM_DBG("\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_FLUSH;
	return audio_send_queue_rec(audio, &cmd, sizeof(cmd));
}

static int audqcelp_in_dsp_read_buffer(struct audio_qcelp_in *audio,
		uint32_t read_cnt)
{
	audrec_cmd_packet_ext_ptr cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDREC_CMD_PACKET_EXT_PTR;
	cmd.type = audio->audrec_obj_idx;
	cmd.curr_rec_count_msw = read_cnt >> 16;
	cmd.curr_rec_count_lsw = read_cnt;

	return audio_send_queue_recbs(audio, &cmd, sizeof(cmd));
}

/* ------------------- device --------------------- */

static void audqcelp_ioport_reset(struct audio_qcelp_in *audio)
{
	/* Make sure read/write thread are free from
	 * sleep and knowing that system is not able
	 * to process io request at the moment
	 */
	wake_up(&audio->write_wait);
	mutex_lock(&audio->write_lock);
	audqcelp_in_flush(audio);
	mutex_unlock(&audio->write_lock);
	wake_up(&audio->wait);
	mutex_lock(&audio->read_lock);
	audqcelp_out_flush(audio);
	mutex_unlock(&audio->read_lock);
}

static void audqcelp_in_flush(struct audio_qcelp_in *audio)
{
	int i;

	audio->dsp_cnt = 0;
	audio->in_head = 0;
	audio->in_tail = 0;
	audio->in_count = 0;
	audio->eos_ack = 0;
	for (i = FRAME_NUM-1; i >= 0; i--) {
		audio->in[i].size = 0;
		audio->in[i].read = 0;
	}
	MM_DBG("in_bytes %d\n", atomic_read(&audio->in_bytes));
	MM_DBG("in_samples %d\n", atomic_read(&audio->in_samples));
	atomic_set(&audio->in_bytes, 0);
	atomic_set(&audio->in_samples, 0);
}

static void audqcelp_out_flush(struct audio_qcelp_in *audio)
{
	int i;

	audio->out_head = 0;
	audio->out_tail = 0;
	audio->out_count = 0;
	for (i = OUT_FRAME_NUM-1; i >= 0; i--) {
		audio->out[i].size = 0;
		audio->out[i].read = 0;
		audio->out[i].used = 0;
	}
}

/* ------------------- device --------------------- */
static long audqcelp_in_ioctl(struct file *file,
				unsigned int cmd, unsigned long arg)
{
	struct audio_qcelp_in *audio = file->private_data;
	int rc = 0;

	MM_DBG("\n");
	if (cmd == AUDIO_GET_STATS) {
		struct msm_audio_stats stats;
		stats.byte_count = atomic_read(&audio->in_bytes);
		stats.sample_count = atomic_read(&audio->in_samples);
		if (copy_to_user((void *) arg, &stats, sizeof(stats)))
			return -EFAULT;
		return rc;
	}

	mutex_lock(&audio->lock);
	switch (cmd) {
	case AUDIO_START: {
		rc = audqcelp_in_enable(audio);
		if (!rc) {
			rc =
			wait_event_interruptible_timeout(audio->wait_enable,
				audio->running != 0, 1*HZ);
			MM_DBG("state %d rc = %d\n", audio->running, rc);

			if (audio->running == 0)
				rc = -ENODEV;
			else
				rc = 0;
		}
		audio->stopped = 0;
		break;
	}
	case AUDIO_STOP: {
		rc = audqcelp_in_disable(audio);
		audio->stopped = 1;
		break;
	}
	case AUDIO_FLUSH: {
		MM_DBG("AUDIO_FLUSH\n");
		audio->rflush = 1;
		audio->wflush = 1;
		audqcelp_ioport_reset(audio);
		if (audio->running) {
			audqcelp_flush_command(audio);
			rc = wait_event_interruptible(audio->write_wait,
				!audio->wflush);
			if (rc < 0) {
				MM_ERR("AUDIO_FLUSH interrupted\n");
				rc = -EINTR;
			}
		} else {
			audio->rflush = 0;
			audio->wflush = 0;
		}
		break;
	}
	case AUDIO_GET_CONFIG: {
		struct msm_audio_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.buffer_size = OUT_BUFFER_SIZE;
		cfg.buffer_count = OUT_FRAME_NUM;
		cfg.sample_rate = convert_samp_index(audio->samp_rate);
		cfg.channel_count = 1;
		cfg.type = 0;
		cfg.unused[0] = 0;
		cfg.unused[1] = 0;
		cfg.unused[2] = 0;
		if (copy_to_user((void *) arg, &cfg, sizeof(cfg)))
			rc = -EFAULT;
		else
			rc = 0;
		break;
	}
	case AUDIO_GET_STREAM_CONFIG: {
		struct msm_audio_stream_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.buffer_size = audio->buffer_size;
		cfg.buffer_count = FRAME_NUM;
		if (copy_to_user((void *)arg, &cfg, sizeof(cfg)))
			rc = -EFAULT;
		else
			rc = 0;
		break;
	}
	case AUDIO_SET_STREAM_CONFIG: {
		struct msm_audio_stream_config cfg;
		if (copy_from_user(&cfg, (void *) arg, sizeof(cfg))) {
			rc = -EFAULT;
			break;
		}
		/* Allow only single frame */
		if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
			if (cfg.buffer_size != (FRAME_SIZE - 8)) {
				rc = -EINVAL;
				break;
				}
		} else {
			if (cfg.buffer_size != (QCELP_FRAME_SIZE + 14)) {
				rc = -EINVAL;
				break;
				}
		}
		audio->buffer_size = cfg.buffer_size;
		break;
	}
	case AUDIO_GET_QCELP_ENC_CONFIG: {
		if (copy_to_user((void *) arg, &audio->cfg, sizeof(audio->cfg)))
			rc = -EFAULT;
		break;
	}
	case AUDIO_SET_QCELP_ENC_CONFIG: {
		struct msm_audio_qcelp_enc_config cfg;
		if (copy_from_user(&cfg, (void *) arg, sizeof(cfg))) {
			rc = -EFAULT;
			break;
		}
		MM_DBG("0X%8x, 0x%8x, 0x%8x\n", cfg.min_bit_rate,
				cfg.max_bit_rate, cfg.cdma_rate);
		if (cfg.min_bit_rate > CDMA_RATE_FULL || \
				 cfg.min_bit_rate < CDMA_RATE_EIGHTH) {
			MM_ERR("invalid min bitrate\n");
			rc = -EFAULT;
			break;
		}
		if (cfg.max_bit_rate > CDMA_RATE_FULL || \
				cfg.max_bit_rate < CDMA_RATE_EIGHTH) {
			MM_ERR("invalid max bitrate\n");
			rc = -EFAULT;
			break;
		}
		/* Recording Does not support Erase and Blank */
		if (cfg.cdma_rate > CDMA_RATE_FULL ||
			cfg.cdma_rate < CDMA_RATE_EIGHTH) {
			MM_ERR("invalid qcelp cdma rate\n");
			rc = -EFAULT;
			break;
		}
		memcpy(&audio->cfg, &cfg, sizeof(cfg));
		break;
	}
	default:
		rc = -EINVAL;
	}
	mutex_unlock(&audio->lock);
	return rc;
}

static ssize_t audqcelp_in_read(struct file *file,
				char __user *buf,
				size_t count, loff_t *pos)
{
	struct audio_qcelp_in *audio = file->private_data;
	unsigned long flags;
	const char __user *start = buf;
	void *data;
	uint32_t index;
	uint32_t size;
	int rc = 0;
	struct qcelp_encoded_meta_out meta_field;
	struct audio_frame_nt *nt_frame;
	MM_DBG("count = %d\n", count);
	mutex_lock(&audio->read_lock);
	while (count > 0) {
		rc = wait_event_interruptible(
			audio->wait, (audio->in_count > 0) || audio->stopped ||
			audio->rflush);
		if (rc < 0)
			break;

		if (audio->rflush) {
			rc = -EBUSY;
			break;
		}
		if (audio->stopped && !audio->in_count) {
			MM_DBG("Driver in stop state, No more buffer to read");
			rc = 0;/* End of File */
			break;
		}

		index = audio->in_tail;
		data = (uint8_t *) audio->in[index].data;
		size = audio->in[index].size;

		if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL) {
			nt_frame = (struct audio_frame_nt *)(data -
					sizeof(struct audio_frame_nt));
			memcpy((char *)&meta_field.time_stamp_dword_lsw,
				(char *)&nt_frame->time_stamp_dword_lsw,
				(sizeof(struct qcelp_encoded_meta_out) - \
				sizeof(uint16_t)));
			meta_field.metadata_len =
					sizeof(struct qcelp_encoded_meta_out);
			if (copy_to_user((char *)start, (char *)&meta_field,
				sizeof(struct qcelp_encoded_meta_out))) {
				rc = -EFAULT;
				break;
			}
			if (nt_frame->nflag_lsw & 0x0001) {
				MM_ERR("recieved EOS in read call\n");
				audio->eos_ack = 1;
			}
			buf += sizeof(struct qcelp_encoded_meta_out);
			count -= sizeof(struct qcelp_encoded_meta_out);
		}
		if (count >= size) {
			/* order the reads on the buffer */
			dma_coherent_post_ops();
			if (copy_to_user(buf, data, size)) {
				rc = -EFAULT;
				break;
			}
			spin_lock_irqsave(&audio->dsp_lock, flags);
			if (index != audio->in_tail) {
				/* overrun -- data is
				 * invalid and we need to retry */
				spin_unlock_irqrestore(&audio->dsp_lock, flags);
				continue;
			}
			audio->in[index].size = 0;
			audio->in_tail = (audio->in_tail + 1) & (FRAME_NUM - 1);
			audio->in_count--;
			spin_unlock_irqrestore(&audio->dsp_lock, flags);
			count -= size;
			buf += size;
			if ((audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL)) {
				if (!audio->eos_ack) {
					MM_DBG("sending read ptr command \
							%d %d\n",
							audio->dsp_cnt,
							audio->in_tail);
					audqcelp_in_dsp_read_buffer(audio,
							audio->dsp_cnt++);
				}
			}
		} else {
			MM_ERR("short read\n");
			break;
		}
		break;
	}
	mutex_unlock(&audio->read_lock);

	if (buf > start)
		return buf - start;

	return rc;
}

static void audrec_pcm_send_data(struct audio_qcelp_in *audio, unsigned needed)
{
	struct buffer *frame;
	unsigned long flags;
	MM_DBG("\n");
	spin_lock_irqsave(&audio->dsp_lock, flags);
	if (!audio->running)
		goto done;

	if (needed && !audio->wflush) {
		/* We were called from the callback because the DSP
		 * requested more data.  Note that the DSP does want
		 * more data, and if a buffer was in-flight, mark it
		 * as available (since the DSP must now be done with
		 * it).
		 */
		audio->out_needed = 1;
		frame = audio->out + audio->out_tail;
		if (frame->used == 0xffffffff) {
			MM_DBG("frame %d free\n", audio->out_tail);
			frame->used = 0;
			audio->out_tail ^= 1;
			wake_up(&audio->write_wait);
		}
	}

	if (audio->out_needed) {
		/* If the DSP currently wants data and we have a
		 * buffer available, we will send it and reset
		 * the needed flag.  We'll mark the buffer as in-flight
		 * so that it won't be recycled until the next buffer
		 * is requested
		 */

		frame = audio->out + audio->out_tail;
		if (frame->used) {
			BUG_ON(frame->used == 0xffffffff);
			audrec_pcm_buffer_ptr_refresh(audio,
						 audio->out_tail,
						    frame->used);
			frame->used = 0xffffffff;
			audio->out_needed = 0;
		}
	}
 done:
	spin_unlock_irqrestore(&audio->dsp_lock, flags);
}

static int audqcelp_in_fsync(struct file *file,	int datasync)

{
	struct audio_qcelp_in *audio = file->private_data;
	int rc = 0;

	MM_DBG("\n"); /* Macro prints the file name and function */
	if (!audio->running || (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)) {
		rc = -EINVAL;
		goto done_nolock;
	}

	mutex_lock(&audio->write_lock);

	rc = wait_event_interruptible(audio->write_wait,
			audio->wflush);
	MM_DBG("waked on by some event audio->wflush = %d\n", audio->wflush);

	if (rc < 0)
		goto done;
	else if (audio->wflush) {
		rc = -EBUSY;
		goto done;
	}
done:
	mutex_unlock(&audio->write_lock);
done_nolock:
	return rc;

}

int audrec_qcelp_process_eos(struct audio_qcelp_in *audio,
		const char __user *buf_start, unsigned short mfield_size)
{
	struct buffer *frame;
	int rc = 0;

	frame = audio->out + audio->out_head;

	rc = wait_event_interruptible(audio->write_wait,
		(audio->out_needed &&
		audio->out[0].used == 0 &&
		audio->out[1].used == 0)
		|| (audio->stopped)
		|| (audio->wflush));

	if (rc < 0)
		goto done;
	if (audio->stopped || audio->wflush) {
		rc = -EBUSY;
		goto done;
	}
	if (copy_from_user(frame->data, buf_start, mfield_size)) {
		rc = -EFAULT;
		goto done;
	}

	frame->mfield_sz = mfield_size;
	audio->out_head ^= 1;
	frame->used = mfield_size;
	MM_DBG("copying meta_out frame->used = %d\n", frame->used);
	audrec_pcm_send_data(audio, 0);
done:
	return rc;
}

static ssize_t audqcelp_in_write(struct file *file,
				const char __user *buf,
				size_t count, loff_t *pos)
{
	struct audio_qcelp_in *audio = file->private_data;
	const char __user *start = buf;
	struct buffer *frame;
	char *cpy_ptr;
	int rc = 0, eos_condition = AUDPREPROC_QCELP_EOS_NONE;
	unsigned short mfield_size = 0;
	int write_count = 0;
	MM_DBG("cnt=%d\n", count);

	if (count & 1)
		return -EINVAL;

	if (audio->mode != MSM_AUD_ENC_MODE_NONTUNNEL)
		return -EINVAL;

	mutex_lock(&audio->write_lock);
	frame = audio->out + audio->out_head;
	/* if supplied count is more than driver buffer size
	 * then only copy driver buffer size
	 */
	if (count > frame->size)
		count = frame->size;

	write_count = count;
	cpy_ptr = frame->data;
	rc = wait_event_interruptible(audio->write_wait,
				      (frame->used == 0)
					|| (audio->stopped)
					|| (audio->wflush));
	if (rc < 0)
		goto error;

	if (audio->stopped || audio->wflush) {
		rc = -EBUSY;
		goto error;
	}
	if (audio->mfield) {
		if (buf == start) {
			/* Processing beginning of user buffer */
			if (__get_user(mfield_size,
				(unsigned short __user *) buf)) {
				rc = -EFAULT;
				goto error;
			} else if (mfield_size > count) {
				rc = -EINVAL;
				goto error;
			}
			MM_DBG("mf offset_val %x\n", mfield_size);
			if (copy_from_user(cpy_ptr, buf, mfield_size)) {
				rc = -EFAULT;
				goto error;
			}
			/* Check if EOS flag is set and buffer has
			 * contains just meta field
			 */
			if (cpy_ptr[AUDPREPROC_QCELP_EOS_FLG_OFFSET] &
					AUDPREPROC_QCELP_EOS_FLG_MASK) {
				eos_condition = AUDPREPROC_QCELP_EOS_SET;
				MM_DBG("EOS SET\n");
				if (mfield_size == count) {
					buf += mfield_size;
					eos_condition = 0;
					goto exit;
				} else
				cpy_ptr[AUDPREPROC_QCELP_EOS_FLG_OFFSET] &=
					~AUDPREPROC_QCELP_EOS_FLG_MASK;
			}
			cpy_ptr += mfield_size;
			count -= mfield_size;
			buf += mfield_size;
		} else {
			mfield_size = 0;
			MM_DBG("continuous buffer\n");
		}
		frame->mfield_sz = mfield_size;
	}
	MM_DBG("copying the stream count = %d\n", count);
	if (copy_from_user(cpy_ptr, buf, count)) {
		rc = -EFAULT;
		goto error;
	}
exit:
	frame->used = count;
	audio->out_head ^= 1;
	if (!audio->flush_ack)
		audrec_pcm_send_data(audio, 0);
	else {
		audrec_pcm_send_data(audio, 1);
		audio->flush_ack = 0;
	}
	if (eos_condition == AUDPREPROC_QCELP_EOS_SET)
		rc = audrec_qcelp_process_eos(audio, start, mfield_size);
	mutex_unlock(&audio->write_lock);
	return write_count;
error:
	mutex_unlock(&audio->write_lock);
	return rc;
}

static int audqcelp_in_release(struct inode *inode, struct file *file)
{
	struct audio_qcelp_in *audio = file->private_data;

	mutex_lock(&audio->lock);
	audqcelp_in_disable(audio);
	audqcelp_in_flush(audio);
	msm_adsp_put(audio->audrec);

	if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)
		msm_adsp_put(audio->audpre);

	audpreproc_aenc_free(audio->enc_id);
	audio->audrec = NULL;
	audio->audpre = NULL;
	audio->opened = 0;

	if ((audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL) && \
	   (audio->out_data)) {
		msm_subsystem_unmap_buffer(audio->map_v_write);
		free_contiguous_memory_by_paddr(audio->out_phys);
		audio->out_data = NULL;
	}

	if (audio->data) {
		msm_subsystem_unmap_buffer(audio->map_v_read);
		free_contiguous_memory_by_paddr(audio->phys);
		audio->data = NULL;
	}
	mutex_unlock(&audio->lock);
	return 0;
}

static struct audio_qcelp_in the_audio_qcelp_in;

static int audqcelp_in_open(struct inode *inode, struct file *file)
{
	struct audio_qcelp_in *audio = &the_audio_qcelp_in;
	int rc;
	int encid;
	int dma_size = 0;

	mutex_lock(&audio->lock);
	if (audio->opened) {
		rc = -EBUSY;
		goto done;
	}
	if ((file->f_mode & FMODE_WRITE) &&
		(file->f_mode & FMODE_READ)) {
		audio->mode = MSM_AUD_ENC_MODE_NONTUNNEL;
		dma_size = NT_DMASZ;
		MM_DBG("Opened for non tunnel mode encoding\n");
	} else if (!(file->f_mode & FMODE_WRITE) &&
				(file->f_mode & FMODE_READ)) {
		audio->mode = MSM_AUD_ENC_MODE_TUNNEL;
		dma_size = DMASZ;
		MM_DBG("Opened for tunnel mode encoding\n");
	} else {
		MM_ERR("Invalid mode\n");
		rc = -EACCES;
		goto done;
	}

	/* Settings will be re-config at AUDIO_SET_CONFIG,
	 * but at least we need to have initial config
	 */
	audio->samp_rate = RPC_AUD_DEF_SAMPLE_RATE_8000,
	audio->samp_rate_index = AUDREC_CMD_SAMP_RATE_INDX_8000;
	audio->channel_mode = AUDREC_CMD_STEREO_MODE_MONO;
	if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL)
		audio->buffer_size = (QCELP_FRAME_SIZE + 14);
	else
		audio->buffer_size = QCELP_FRAME_SIZE;
	audio->enc_type = AUDREC_CMD_TYPE_0_INDEX_QCELP | audio->mode;

	audio->cfg.cdma_rate = CDMA_RATE_FULL;
	audio->cfg.min_bit_rate = CDMA_RATE_FULL;
	audio->cfg.max_bit_rate = CDMA_RATE_FULL;

	if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
		rc = audmgr_open(&audio->audmgr);
		if (rc)
			goto done;
	}

	encid = audpreproc_aenc_alloc(audio->enc_type, &audio->module_name,
			&audio->queue_ids);
	if (encid < 0) {
		MM_ERR("No free encoder available\n");
		rc = -ENODEV;
		goto done;
	}
	audio->enc_id = encid;

	rc = msm_adsp_get(audio->module_name, &audio->audrec,
			   &audrec_qcelp_adsp_ops, audio);
	if (rc) {
		audpreproc_aenc_free(audio->enc_id);
		goto done;
	}

	if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL) {
		rc = msm_adsp_get("AUDPREPROCTASK", &audio->audpre,
				&audpre_qcelp_adsp_ops, audio);
		if (rc) {
			msm_adsp_put(audio->audrec);
			audpreproc_aenc_free(audio->enc_id);
			goto done;
		}
	}

	audio->dsp_cnt = 0;
	audio->stopped = 0;
	audio->wflush = 0;
	audio->rflush = 0;
	audio->flush_ack = 0;

	audqcelp_in_flush(audio);
	audqcelp_out_flush(audio);

	audio->phys = allocate_contiguous_ebi_nomap(dma_size, SZ_4K);
	if (!audio->phys) {
		MM_ERR("could not allocate physical read buffers\n");
		rc = -ENOMEM;
		goto evt_error;
	} else {
		audio->map_v_read = msm_subsystem_map_buffer(
						audio->phys, dma_size,
						MSM_SUBSYSTEM_MAP_KADDR,
						NULL, 0);
		if (IS_ERR(audio->map_v_read)) {
			MM_ERR("could not map physical address\n");
			rc = -ENOMEM;
			free_contiguous_memory_by_paddr(audio->phys);
			goto evt_error;
		}
		audio->data = audio->map_v_read->vaddr;
		MM_DBG("read buf: phy addr 0x%08x kernel addr 0x%08x\n",
				audio->phys, (int)audio->data);
	}

	audio->out_data = NULL;
	if (audio->mode == MSM_AUD_ENC_MODE_NONTUNNEL) {
		audio->out_phys = allocate_contiguous_ebi_nomap(BUFFER_SIZE,
						SZ_4K);
		if (!audio->out_phys) {
			MM_ERR("could not allocate physical write buffers\n");
			rc = -ENOMEM;
			msm_subsystem_unmap_buffer(audio->map_v_read);
			free_contiguous_memory_by_paddr(audio->phys);
			goto evt_error;
		} else {
			audio->map_v_write = msm_subsystem_map_buffer(
						audio->out_phys, BUFFER_SIZE,
						MSM_SUBSYSTEM_MAP_KADDR,
						NULL, 0);

			if (IS_ERR(audio->map_v_write)) {
				MM_ERR("could not map write phys address\n");
				rc = -ENOMEM;
				msm_subsystem_unmap_buffer(audio->map_v_read);
				free_contiguous_memory_by_paddr(audio->phys);
				free_contiguous_memory_by_paddr(\
							audio->out_phys);
				goto evt_error;
			}
			audio->out_data = audio->map_v_write->vaddr;
			MM_DBG("wr buf: phy addr 0x%08x kernel addr 0x%08x\n",
					audio->out_phys, (int)audio->out_data);
		}

		/* Initialize buffer */
		audio->out[0].data = audio->out_data + 0;
		audio->out[0].addr = audio->out_phys + 0;
		audio->out[0].size = OUT_BUFFER_SIZE;

		audio->out[1].data = audio->out_data + OUT_BUFFER_SIZE;
		audio->out[1].addr = audio->out_phys + OUT_BUFFER_SIZE;
		audio->out[1].size = OUT_BUFFER_SIZE;

		MM_DBG("audio->out[0].data = %d  audio->out[1].data = %d",
				(unsigned int)audio->out[0].data,
				(unsigned int)audio->out[1].data);
		audio->mfield = NT_FRAME_HEADER_SIZE;
		audio->out_frame_cnt++;
	}
	file->private_data = audio;
	audio->opened = 1;

done:
	mutex_unlock(&audio->lock);
	return rc;
evt_error:
	msm_adsp_put(audio->audrec);
	if (audio->mode == MSM_AUD_ENC_MODE_TUNNEL)
		msm_adsp_put(audio->audpre);

	audpreproc_aenc_free(audio->enc_id);
	mutex_unlock(&audio->lock);
	return rc;
}

static const struct file_operations audio_qcelp_in_fops = {
	.owner		= THIS_MODULE,
	.open		= audqcelp_in_open,
	.release	= audqcelp_in_release,
	.read		= audqcelp_in_read,
	.write		= audqcelp_in_write,
	.fsync		= audqcelp_in_fsync,
	.unlocked_ioctl	= audqcelp_in_ioctl,
};

static struct miscdevice audqcelp_in_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "msm_qcelp_in",
	.fops	= &audio_qcelp_in_fops,
};

static int __init audqcelp_in_init(void)
{
	mutex_init(&the_audio_qcelp_in.lock);
	mutex_init(&the_audio_qcelp_in.read_lock);
	spin_lock_init(&the_audio_qcelp_in.dsp_lock);
	init_waitqueue_head(&the_audio_qcelp_in.wait);
	init_waitqueue_head(&the_audio_qcelp_in.wait_enable);
	mutex_init(&the_audio_qcelp_in.write_lock);
	init_waitqueue_head(&the_audio_qcelp_in.write_wait);
	return misc_register(&audqcelp_in_misc);
}
device_initcall(audqcelp_in_init);
