/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "DVDAudioCodecFFmpeg.h"
#ifdef TARGET_POSIX
#include "XMemUtils.h"
#endif
#include "../../DVDStreamInfo.h"
#include "utils/log.h"

#if defined(TARGET_DARWIN)
#include "settings/Settings.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#endif

CDVDAudioCodecFFmpeg::CDVDAudioCodecFFmpeg() : CDVDAudioCodec()
{
  m_iBufferSize1 = 0;
  m_iBufferSize2 = 0;
  m_iBufferTotalSize2 = 0;
  m_pBuffer2     = NULL;

  m_iBuffered = 0;
  m_pCodecContext = NULL;
  m_pConvert = NULL;
  m_bOpenedCodec = false;

  m_channels = 0;
  m_layout = 0;
  
  m_pFrame1 = NULL;
  m_iSampleFormat = AV_SAMPLE_FMT_NONE;
}

CDVDAudioCodecFFmpeg::~CDVDAudioCodecFFmpeg()
{
  Dispose();
}

bool CDVDAudioCodecFFmpeg::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  AVCodec* pCodec;
  m_bOpenedCodec = false;

  pCodec = avcodec_find_decoder(hints.codec);
  if (!pCodec)
  {
    CLog::Log(LOGDEBUG,"CDVDAudioCodecFFmpeg::Open() Unable to find codec %d", hints.codec);
    return false;
  }

  m_pCodecContext = avcodec_alloc_context3(pCodec);
  m_pCodecContext->debug_mv = 0;
  m_pCodecContext->debug = 0;
  m_pCodecContext->workaround_bugs = 1;

  if (pCodec->capabilities & CODEC_CAP_TRUNCATED)
    m_pCodecContext->flags |= CODEC_FLAG_TRUNCATED;

  m_channels = 0;
  m_pCodecContext->channels = hints.channels;
  m_pCodecContext->sample_rate = hints.samplerate;
  m_pCodecContext->block_align = hints.blockalign;
  m_pCodecContext->bit_rate = hints.bitrate;
  m_pCodecContext->bits_per_coded_sample = hints.bitspersample;

  if(m_pCodecContext->bits_per_coded_sample == 0)
    m_pCodecContext->bits_per_coded_sample = 16;

  if( hints.extradata && hints.extrasize > 0 )
  {
    m_pCodecContext->extradata = (uint8_t*)av_mallocz(hints.extrasize + FF_INPUT_BUFFER_PADDING_SIZE);
    if(m_pCodecContext->extradata)
    {
      m_pCodecContext->extradata_size = hints.extrasize;
      memcpy(m_pCodecContext->extradata, hints.extradata, hints.extrasize);
    }
  }

  if (avcodec_open2(m_pCodecContext, pCodec, NULL) < 0)
  {
    CLog::Log(LOGDEBUG,"CDVDAudioCodecFFmpeg::Open() Unable to open codec");
    Dispose();
    return false;
  }

  m_pFrame1 = av_frame_alloc();
  m_bOpenedCodec = true;
  m_iSampleFormat = AV_SAMPLE_FMT_NONE;

  return true;
}

void CDVDAudioCodecFFmpeg::Dispose()
{
  if (m_pFrame1) av_free(m_pFrame1);
  m_pFrame1 = NULL;

  if (m_pConvert)
    swr_free(&m_pConvert);

  if (m_pBuffer2)
    av_freep(&m_pBuffer2);

  if (m_pCodecContext)
  {
    if (m_bOpenedCodec) avcodec_close(m_pCodecContext);
    m_bOpenedCodec = false;
    av_free(m_pCodecContext);
    m_pCodecContext = NULL;
  }

  m_iBufferSize1 = 0;
  m_iBufferSize2 = 0;
  m_iBufferTotalSize2 = 0;
  m_iBuffered = 0;
}

int CDVDAudioCodecFFmpeg::Decode(uint8_t* pData, int iSize)
{
  int iBytesUsed, got_frame;
  if (!m_pCodecContext) return -1;

  m_iBufferSize2 = 0;

  AVPacket avpkt;
  av_init_packet(&avpkt);
  avpkt.data = pData;
  avpkt.size = iSize;
  iBytesUsed = avcodec_decode_audio4( m_pCodecContext
                                                 , m_pFrame1
                                                 , &got_frame
                                                 , &avpkt);
  if (iBytesUsed < 0 || !got_frame)
  {
    m_iBufferSize1 = 0;
    return iBytesUsed;
  }
  m_iBufferSize1 = m_pFrame1->nb_samples * m_pCodecContext->channels * av_get_bytes_per_sample(m_pCodecContext->sample_fmt);

  /* some codecs will attempt to consume more data than what we gave */
  if (iBytesUsed > iSize)
  {
    CLog::Log(LOGWARNING, "CDVDAudioCodecFFmpeg::Decode - decoder attempted to consume more data than given");
    iBytesUsed = iSize;
  }

  if(m_iBufferSize1 == 0 && iBytesUsed >= 0)
    m_iBuffered += iBytesUsed;
  else
    m_iBuffered = 0;
  
  bool convert = false;
  switch(m_pCodecContext->sample_fmt)
  {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_DBL:
      break;
    case AV_SAMPLE_FMT_NONE:
      CLog::Log(LOGERROR, "CDVDAudioCodecFFmpeg::Decode - invalid data format");
      return -1;
    default:
      convert = true;
  }
  if(convert)
    ConvertToFloat();

  return iBytesUsed;
}

void CDVDAudioCodecFFmpeg::ConvertToFloat()
{
  if(m_pCodecContext->sample_fmt != AV_SAMPLE_FMT_FLT && m_iBufferSize1 > 0)
  {
    if(m_pConvert && (m_pCodecContext->sample_fmt != m_iSampleFormat || m_channels != m_pCodecContext->channels))
      swr_free(&m_pConvert);

    if(!m_pConvert)
    {
      m_iSampleFormat = m_pCodecContext->sample_fmt;
      m_pConvert = swr_alloc_set_opts(NULL,
                      av_get_default_channel_layout(m_pCodecContext->channels), AV_SAMPLE_FMT_FLT, m_pCodecContext->sample_rate,
                      av_get_default_channel_layout(m_pCodecContext->channels), m_pCodecContext->sample_fmt, m_pCodecContext->sample_rate,
                      0, NULL);

      if(!m_pConvert || swr_init(m_pConvert) < 0)
      {
          CLog::Log(LOGERROR, "CDVDAudioCodecFFmpeg::Decode - Unable to convert %d to AV_SAMPLE_FMT_FLT", m_pCodecContext->sample_fmt);
          m_iBufferSize1 = 0;
          m_iBufferSize2 = 0;
          return;
      }
    }

    int needed_buf_size = av_samples_get_buffer_size(NULL, m_pCodecContext->channels, m_pFrame1->nb_samples, AV_SAMPLE_FMT_FLT, 0);
    if(m_iBufferTotalSize2 < needed_buf_size)
    {
        m_pBuffer2 = (uint8_t*)av_realloc(m_pBuffer2, needed_buf_size);
        if(!m_pBuffer2)
        {
            CLog::Log(LOGERROR, "CDVDAudioCodecFFmpeg::Decode - Unable to allocate a %i bytes buffer for resampling", needed_buf_size);
            m_iBufferSize1 = 0;
            m_iBufferSize2 = 0;
            m_iBufferTotalSize2 = 0;
            return;
        }
        m_iBufferTotalSize2 = needed_buf_size;
    }

    int outsamples;
    outsamples = swr_convert(m_pConvert, &m_pBuffer2, m_iBufferTotalSize2, (const uint8_t**)m_pFrame1->extended_data, m_pFrame1->nb_samples);

    if(outsamples < 0)
    {
      CLog::Log(LOGERROR, "CDVDAudioCodecFFmpeg::Decode - Unable to convert %d to AV_SAMPLE_FMT_FLT", (int)m_pCodecContext->sample_fmt);
      m_iBufferSize1 = 0;
      m_iBufferSize2 = 0;
      return;
    }

    if(outsamples < m_pFrame1->nb_samples)
    {
      CLog::Log(LOGWARNING, "CDVDAudioCodecFFmpeg::Decode - Resampler produced less samples than what it was given");
    }

    m_iBufferSize1 = 0;
    m_iBufferSize2 = m_pFrame1->nb_samples * m_pCodecContext->channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_FLT);
  }
}

int CDVDAudioCodecFFmpeg::GetData(uint8_t** dst)
{
  if(m_iBufferSize1)
  {
    *dst = m_pFrame1->data[0];
    return m_iBufferSize1;
  }

  if(m_iBufferSize2)
  {
    *dst = m_pBuffer2;
    return m_iBufferSize2;
  }

  return 0;
}

void CDVDAudioCodecFFmpeg::Reset()
{
  if (m_pCodecContext) avcodec_flush_buffers(m_pCodecContext);
  m_iBufferSize1 = 0;
  m_iBufferSize2 = 0;
  m_iBuffered = 0;
}

int CDVDAudioCodecFFmpeg::GetChannels()
{
  return m_pCodecContext->channels;
}

int CDVDAudioCodecFFmpeg::GetSampleRate()
{
  if (m_pCodecContext)
    return m_pCodecContext->sample_rate;
  return 0;
}

int CDVDAudioCodecFFmpeg::GetEncodedSampleRate()
{
  if (m_pCodecContext)
    return m_pCodecContext->sample_rate;
  return 0;
}

enum AEDataFormat CDVDAudioCodecFFmpeg::GetDataFormat()
{
  switch(m_pCodecContext->sample_fmt)
  {
    case AV_SAMPLE_FMT_U8 : return AE_FMT_U8;
    case AV_SAMPLE_FMT_S16: return AE_FMT_S16NE;
    case AV_SAMPLE_FMT_S32: return AE_FMT_S32NE;
    case AV_SAMPLE_FMT_FLT: return AE_FMT_FLOAT;
    case AV_SAMPLE_FMT_DBL: return AE_FMT_DOUBLE;
    case AV_SAMPLE_FMT_NONE:
      CLog::Log(LOGERROR, "CDVDAudioCodecFFmpeg::GetDataFormat - invalid data format");
      return AE_FMT_INVALID;
    default:
      return AE_FMT_FLOAT;
  }
}

int CDVDAudioCodecFFmpeg::GetBitRate()
{
  if (m_pCodecContext) return m_pCodecContext->bit_rate;
  return 0;
}

static unsigned count_bits(int64_t value)
{
  unsigned bits = 0;
  for(;value;++bits)
    value &= value - 1;
  return bits;
}

void CDVDAudioCodecFFmpeg::BuildChannelMap()
{
  if (m_channels == m_pCodecContext->channels && m_layout == m_pCodecContext->channel_layout)
    return; //nothing to do here

  m_channels = m_pCodecContext->channels;
  m_layout   = m_pCodecContext->channel_layout;

  int64_t layout;

  int bits = count_bits(m_pCodecContext->channel_layout);
  if (bits == m_pCodecContext->channels)
    layout = m_pCodecContext->channel_layout;
  else
  {
    CLog::Log(LOGINFO, "CDVDAudioCodecFFmpeg::GetChannelMap - FFmpeg reported %d channels, but the layout contains %d ignoring", m_pCodecContext->channels, bits);
    layout = av_get_default_channel_layout(m_pCodecContext->channels);
  }

  m_channelLayout.Reset();

  if (layout & AV_CH_FRONT_LEFT           ) m_channelLayout += AE_CH_FL  ;
  if (layout & AV_CH_FRONT_RIGHT          ) m_channelLayout += AE_CH_FR  ;
  if (layout & AV_CH_FRONT_CENTER         ) m_channelLayout += AE_CH_FC  ;
  if (layout & AV_CH_LOW_FREQUENCY        ) m_channelLayout += AE_CH_LFE ;
  if (layout & AV_CH_BACK_LEFT            ) m_channelLayout += AE_CH_BL  ;
  if (layout & AV_CH_BACK_RIGHT           ) m_channelLayout += AE_CH_BR  ;
  if (layout & AV_CH_FRONT_LEFT_OF_CENTER ) m_channelLayout += AE_CH_FLOC;
  if (layout & AV_CH_FRONT_RIGHT_OF_CENTER) m_channelLayout += AE_CH_FROC;
  if (layout & AV_CH_BACK_CENTER          ) m_channelLayout += AE_CH_BC  ;
  if (layout & AV_CH_SIDE_LEFT            ) m_channelLayout += AE_CH_SL  ;
  if (layout & AV_CH_SIDE_RIGHT           ) m_channelLayout += AE_CH_SR  ;
  if (layout & AV_CH_TOP_CENTER           ) m_channelLayout += AE_CH_TC  ;
  if (layout & AV_CH_TOP_FRONT_LEFT       ) m_channelLayout += AE_CH_TFL ;
  if (layout & AV_CH_TOP_FRONT_CENTER     ) m_channelLayout += AE_CH_TFC ;
  if (layout & AV_CH_TOP_FRONT_RIGHT      ) m_channelLayout += AE_CH_TFR ;
  if (layout & AV_CH_TOP_BACK_LEFT        ) m_channelLayout += AE_CH_BL  ;
  if (layout & AV_CH_TOP_BACK_CENTER      ) m_channelLayout += AE_CH_BC  ;
  if (layout & AV_CH_TOP_BACK_RIGHT       ) m_channelLayout += AE_CH_BR  ;

  m_channels = m_pCodecContext->channels;
}

CAEChannelInfo CDVDAudioCodecFFmpeg::GetChannelMap()
{
  BuildChannelMap();
  return m_channelLayout;
}
