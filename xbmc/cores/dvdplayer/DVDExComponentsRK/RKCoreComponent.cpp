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

#include "RKCoreComponent.h"
#include "cores/dvdplayer/DVDClock.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "settings/MediaSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "guilib/GraphicContext.h"
#include "windowing/egl/WinSystemEGL.h"
#include "utils/SysfsUtils.h"
#include "utils/log.h"
#include "android/jni/SystemProperties.h"

#define __MODULE_NAME__ "RKCodec"

#define VC_BYPASS  0x00000100
#define AV_CODEC_ID_H264MVC MKBETAG('M','V','C','C')

CRKCodec::CRKCodec()
  : CThread("RKCodec"),
    m_bLoad(false),
    m_bReady(false),
    m_bSubmittedEos(false),
    m_bSyncStatus(false),
    m_iSyncMode(RK_CLIENT_NOTIFY),
    m_lfSyncThreshold(0.125),
    m_iSpeed(DVD_PLAYSPEED_PAUSE),
    m_iStereoMode(0),
    m_dll(NULL)
     
{
  m_dll = new DllLibRKCodec;
  if (!m_dll->Load())
  {
    CLog::Log(LOGERROR, "%s: m_dll load fail!", __MODULE_NAME__);
  }
  m_bLoad = true;
}

CRKCodec::~CRKCodec()
{
  StopThread();
  delete m_dll, m_dll = NULL;
  m_bReady = false;
  m_bLoad = false;
}

bool CRKCodec::OpenDecoder(CDVDStreamInfo &hints)
{
  CLog::Log(LOGINFO,"%s: OpenDecoder filename: %s", __MODULE_NAME__, hints.filename.c_str());

  /* construct stream info */
  m_streamInfo.codec = hints.codec;
  m_streamInfo.type = hints.type;
  m_streamInfo.flags = hints.flags;
  m_streamInfo.filename = (RK_PTR)hints.filename.c_str();

  m_streamInfo.fpsscale = hints.fpsscale;
  m_streamInfo.fpsrate = hints.fpsrate;
  m_streamInfo.rfpsscale = hints.rfpsscale;
  m_streamInfo.rfpsrate = hints.rfpsrate;
  m_streamInfo.height = hints.height;
  m_streamInfo.width = hints.width;
  m_streamInfo.profile = hints.profile;
  m_streamInfo.ptsinvalid = hints.ptsinvalid;
  m_streamInfo.bitsperpixel = hints.bitsperpixel;
  
  m_streamInfo.channels = hints.channels;
  m_streamInfo.samplerate = hints.samplerate;
  m_streamInfo.bitrate = hints.bitrate;
  m_streamInfo.blockalign = hints.blockalign;
  m_streamInfo.bitspersample = hints.bitspersample;

  m_streamInfo.extradata = hints.extradata;
  m_streamInfo.extrasize = hints.extrasize;
  m_streamInfo.codec_tag = hints.codec_tag;
  m_streamInfo.stereo_mode = 0;

  /* construct display info */
  m_displayInfo.type = 0;
  m_displayInfo.raw = 0;
  m_displayInfo.pts = 0;
  m_displayInfo.eos = 0;

  /* detect mvc 3d */
  if (AV_CODEC_ID_H264MVC == hints.codec_tag)
  {
    CLog::Log(LOGDEBUG,"s: OpenDecoder mvc detected!", __MODULE_NAME__);
    m_streamInfo.stereo_mode = RK_STEREO_LR;
    if (CSettings::GetInstance().GetBool(RKMC_SETTING_FP3D))
      m_streamInfo.stereo_mode = RK_STEREO_MVC; 
  }

  /* init and open rk codec */
  RK_RET ret = m_dll->RK_CodecInit(&m_streamInfo);
  ret |= m_dll->RK_CodecOpen();

  if (ret != 0)
  {
    /* error handle */
    CLog::Log(LOGDEBUG,"%s: OpenDecoder error err_ret = %u!", __MODULE_NAME__, ret);
    goto FAIL;
  }

  /* record current resolution */
  m_displayResolution = CRect(0, 0, 
  CDisplaySettings::GetInstance().GetCurrentResolutionInfo().iWidth,
  CDisplaySettings::GetInstance().GetCurrentResolutionInfo().iHeight);
  CLog::Log(LOGDEBUG,"CRKCodec::kodi_opencodec() width = %d , height = %d, \
                      sWidth = %d, sHeight = %d, bFullStreen = %d, pixfmt = %d", 
  CDisplaySettings::GetInstance().GetCurrentResolutionInfo().iWidth,
  CDisplaySettings::GetInstance().GetCurrentResolutionInfo().iHeight,
  CDisplaySettings::GetInstance().GetCurrentResolutionInfo().iScreenWidth,
  CDisplaySettings::GetInstance().GetCurrentResolutionInfo().iScreenHeight,
  CDisplaySettings::GetInstance().GetCurrentResolutionInfo().bFullScreen,
  m_streamInfo.bitsperpixel);

  /* register render & display callback */
  g_renderManager.RegisterRenderUpdateCallBack((const void*)this, RenderUpdateCallBack);
  m_dll->RK_CodecRegisterListener((RK_ENV)this, RK_RENDER, OnDisplayEvent);

  /* create thread process */\
  if (m_iSyncMode == RK_CLIENT_NOTIFY)
      Create();
  
  m_bReady = true;
  
  return true;
  
FAIL:
  return false;
}

void CRKCodec::CloseDecoder()
{
  if (m_bReady)
  {
    CLog::Log(LOGDEBUG,"%s: CloseDecoder!", __MODULE_NAME__);
    StopThread();
    m_dll->RK_CodecClose();
    g_graphicsContext.SetStereoMode(RENDER_STEREO_MODE_OFF);
    SetNative3DResolution(RENDER_STEREO_MODE_OFF);
  }
}

void CRKCodec::Reset()
{
  if (m_bReady)
  {
    CLog::Log(LOGDEBUG,"%s: Reset!", __MODULE_NAME__);
    m_dll->RK_CodecReset();
  }
}

void CRKCodec::Flush()
{
  if (m_bReady)
  {
    CLog::Log(LOGDEBUG,"%s: Flush!", __MODULE_NAME__);
    m_dll->RK_CodecFlush();
  }  
}

int CRKCodec::DecodeVideo(uint8_t *pData, size_t size, double dts, double pts)
{
  /* register render & display callback */
  g_renderManager.RegisterRenderUpdateCallBack((const void*)this, RenderUpdateCallBack);
  
  int ret = VC_BUFFER;
  if (m_bReady)
  {
    int status = m_dll->RK_CodecWrite(RK_VIDEO, pData, size, pts, dts);
    switch (status)
    {
      /* status buffer need more packets */
      case RK_DECODE_STATE_BUFFER:
        ret = VC_BUFFER;
        break;
      /* status picture wait for render */
      case RK_DECODE_STATE_PICTURE:
        ret = VC_PICTURE;
        break;
      /* status buffer picture just right */
      case RK_DECODE_STATE_BUFFER_PICTURE:
        ret = VC_BUFFER;
        ret |= VC_PICTURE;
        break;
      /* status bypass decoder do noting */
      case RK_DECODE_STATE_BYPASS:
        ret = VC_BYPASS;
        break;
      default:
        ret = VC_ERROR;
        break;
    }
  }
  return ret;
}

void CRKCodec::OnDisplayEvent(RK_ENV env, RK_PTR data, RK_U32 size)
{
  /* callback when a frame prepare to render */
  if (data && env)
  {
    RKCodecDisplayInfo* info = (RKCodecDisplayInfo*)data;
    CRKCodec* pCodec = (CRKCodec*)env;
    pCodec->SetDisplayInfo(info);
    /* when server sync update the codec time */
    if (RK_SERVER_SYNC == pCodec->GetSyncMode() && info->eos != 1)
      pCodec->UpdatePlayStatus();
  }
}

void CRKCodec::SetDisplayInfo(const RKCodecDisplayInfo * info)
{
  m_displayInfo.type = info->type;
  m_displayInfo.pts = info->pts;
  m_displayInfo.raw = info->raw;
  m_displayInfo.eos = info->eos;
}

RKCodecDisplayInfo* CRKCodec::GetDisplayInfo()
{
  return &m_displayInfo;
}

int CRKCodec::GetSyncMode()
{
  return m_iSyncMode;
}
void CRKCodec::Process()
{
  /* use for client process sync */
  CLog::Log(LOGDEBUG, "%s: Process!", __MODULE_NAME__);
  m_iSpeed = DVD_PLAYSPEED_NORMAL;
  while(!m_bStop)
  {
    UpdatePlayStatus();
SLEEP:
    Sleep(100);
  }
}

void CRKCodec::ConfigureSetting()
{
  if (m_dll)
  {
    if (CSettings::GetInstance().GetBool(RKMC_SETTING_3D24HZ))
      SendConfigure(RK_CONF_FORCE24HZ_3D, NULL);
    if (CSettings::GetInstance().GetBool(RKMC_SETTING_3DSWITCH))
      SendConfigure(RK_CONF_FORCESWITCH_3D, NULL);
  }
}

void CRKCodec::UpdatePlayStatus()
{
  if (m_iSpeed == DVD_PLAYSPEED_NORMAL)
  {
    CDVDClock *playerclock = NULL;
    double master_pts, error, offset, radio = 0.0;
    RKCodecDisplayInfo* info = GetDisplayInfo();
    playerclock = CDVDClock::GetMasterClock();
    if (playerclock && info)    
    { 
      if (!m_bSyncStatus && info->pts > 0)
      {
        UpdateRenderStereo(true);
        m_bSyncStatus = true;
      }
      master_pts = playerclock->GetClock();
      if (info->pts > 0.0)
        radio = (double)info->raw / info->pts - 1.0;
      offset = g_renderManager.GetDisplayLatency() - CMediaSettings::GetInstance().GetCurrentVideoSettings().m_AudioDelay;  
      master_pts += offset * DVD_TIME_BASE;
      playerclock->SetSpeedAdjust(radio);
      error = (master_pts - info->pts) / DVD_TIME_BASE;
      if (fabs(error) > m_lfSyncThreshold)
      {
        CLog::Log(LOGDEBUG, "%s: UpdatePlayStatus error = %lf radio = %lf", __MODULE_NAME__, error, radio);
        double sync = master_pts * (1.0 - radio);
        SendCommand(RK_CMD_SYNC, &sync);
      } 
    }  
  }
}

void CRKCodec::SubmitEOS()
{
  if (m_bReady) 
  {
    CLog::Log(LOGDEBUG,"%s: SubmitEOS", __MODULE_NAME__);
    int ret = 0;
    SendCommand(RK_CMD_EOS, &ret);
    m_bSubmittedEos = true;
  }
}

bool CRKCodec::SubmittedEOS()
{
  return m_bSubmittedEos;
}

bool CRKCodec::IsEOS()
{
  return true;
  if (m_bReady && m_bSubmittedEos)
    return m_displayInfo.eos > 0;
  return false;
}

void CRKCodec::SetSpeed(int speed)
{
  m_iSpeed = speed;
  if (m_bReady && m_dll)
  {
    CLog::Log(LOGDEBUG, "%s: SetSpeed speed= %d!", __MODULE_NAME__, speed);
    switch (speed)
    {
      case DVD_PLAYSPEED_PAUSE:
        m_dll->RK_CodecPause();
        break;
      case DVD_PLAYSPEED_NORMAL:
        m_dll->RK_CodecResume();
        break;
      default:
        /* interface to support fast or slow play mode */
        m_dll->RK_CodecSendCommand(RK_CMD_SETSPEED,&speed);
        break;
    }
  }
}

void CRKCodec::SendCommand(RK_U32 cmd, RK_PTR param)
{
  if (m_bReady && m_dll)
  {
    CLog::Log(LOGDEBUG, "%s: SendCommand cmd = %d!", __MODULE_NAME__, cmd);
    m_dll->RK_CodecSendCommand(cmd, param);
  }
}

void CRKCodec::SendConfigure(RK_U32 config, RK_PTR param)
{
  if (m_dll)
  {
    CLog::Log(LOGDEBUG, "%s: SendConfigure config = %d!", __MODULE_NAME__, config);
    m_dll->RK_CodecSendCommand(config, param);
  }
}

void CRKCodec::RenderUpdateCallBack(const void *ctx, const CRect &SrcRect, const CRect &DestRect)
{
  if (ctx)
  {
    ((CRKCodec*)ctx)->UpdateRenderRect(SrcRect,DestRect);
    ((CRKCodec*)ctx)->UpdateRenderStereo();
  }
}

void CRKCodec::UpdateRenderRect(const CRect &SrcRect, const CRect &DestRect)
{
  CRect dstRect = DestRect;
  switch(g_graphicsContext.GetStereoMode()) 
  {
  case RENDER_STEREO_MODE_SPLIT_VERTICAL: dstRect.x2 = dstRect.x2 * 2; break;
  case RENDER_STEREO_MODE_SPLIT_HORIZONTAL: dstRect.y2 = dstRect.y2 * 2; break;
  case RENDER_STEREO_MODE_MVC: return;
  }
  
  if (m_displayResolution != dstRect)
  {  
    CLog::Log(LOGDEBUG,"%s: UpdateRenderRect", __MODULE_NAME__);
    m_displayResolution = dstRect;
    int* dst = new int[4];
    dst[0] = m_displayResolution.x1;
    dst[1] = m_displayResolution.y1;
    dst[2] = m_displayResolution.x2 - m_displayResolution.x1;
    dst[3] = m_displayResolution.y2 - m_displayResolution.y1;

    SendCommand(RK_CMD_SETRES, dst);
  }
}

void CRKCodec::UpdateRenderStereo(bool flag)
{
  RK_U32 target_stereo = GetStereoMode();
  if (flag)
    target_stereo = (m_displayInfo.type & RK_STEREO_MASK);
  
  if (target_stereo != m_iStereoMode)
  {
    RENDER_STEREO_MODE stereo_view;
    RESOLUTION_INFO res_info;
    
    switch (target_stereo)
    {
      case RK_STEREO_LR: stereo_view = RENDER_STEREO_MODE_SPLIT_VERTICAL; break;
      case RK_STEREO_BT: stereo_view = RENDER_STEREO_MODE_SPLIT_HORIZONTAL; break;
      case RK_STEREO_MVC: stereo_view = RENDER_STEREO_MODE_MVC; break;
      default: stereo_view = RENDER_STEREO_MODE_OFF; break;
    }

    g_graphicsContext.SetStereoMode(stereo_view);
    g_Windowing.GetNativeResolution(&res_info);
    if (Support3D(res_info.iScreenWidth, res_info.iScreenHeight, res_info.fRefreshRate, stereo_view) && 
        CSettings::GetInstance().GetBool(RKMC_SETTING_3DSWITCH)) {
      SetNative3DResolution(stereo_view);      
      CJNISystemProperties::set("persist.sys.overscan.main","overscan 100,100,100,100");
    }
    m_iStereoMode = target_stereo;
  }
}

bool CRKCodec::Support3D(int width, int height, float fps, uint32_t mode)
{
  RK_U32 d3d_stereo;
  switch(mode)
  {
    case RENDER_STEREO_MODE_SPLIT_VERTICAL: d3d_stereo = D3DPRESENTFLAG_MODE3DSBS; break;
    case RENDER_STEREO_MODE_SPLIT_HORIZONTAL: d3d_stereo = D3DPRESENTFLAG_MODE3DTB; break;
    case RENDER_STEREO_MODE_MVC: d3d_stereo = D3DPRESENTFLAG_MODE3DMVC; break;
    default: return false;
  }
  
  RESOLUTION_INFO &curr = CDisplaySettings::GetInstance().GetResolutionInfo(g_graphicsContext.GetVideoResolution());
  int searchWidth = curr.iScreenWidth;
  int searchHeight = curr.iScreenHeight;
  int searchFps = (int)curr.fRefreshRate;
  if (width > 0 && height > 0 && fps > 0) {
    searchWidth = width;
    searchHeight = height;
    searchFps = (int)fps;
  }
  for (unsigned int i = 0; i < CDisplaySettings::GetInstance().ResolutionInfoSize(); i++)
  {
    RESOLUTION_INFO res = CDisplaySettings::GetInstance().GetResolutionInfo(i);
    if(res.iScreenWidth == searchWidth && res.iScreenHeight == searchHeight && 
        (int)res.fRefreshRate == searchFps && (res.dwFlags & d3d_stereo))
      return true;
  }
  return false;
}

void CRKCodec::SetNative3DResolution(RK_U32 stereo_view)
{
  int out;
  switch(stereo_view)
  {
    case RENDER_STEREO_MODE_SPLIT_VERTICAL: out = 8; break;
    case RENDER_STEREO_MODE_SPLIT_HORIZONTAL: out = 6; break;
    case RENDER_STEREO_MODE_MVC: out = 0; break;
    default: out = -1;
  }
  
  CLog::Log(LOGDEBUG, "%s: SetNative3DResolution mode = %d!", __MODULE_NAME__, out);
  if (SysfsUtils::HasRW("/sys/class/display/display0.HDMI/3dmode"))
  {
    SysfsUtils::SetInt("/sys/class/display/display0.HDMI/3dmode", out);
  }
  else if (SysfsUtils::HasRW("/sys/class/display/HDMI/3dmode"))
  {
    SysfsUtils::SetInt("/sys/class/display/HDMI/3dmode", out);
  }
}

RK_U32 CRKCodec::GetStereoMode()
{
  RK_U32  stereo_mode;
  
  switch(g_graphicsContext.GetStereoMode())
  {
    case RENDER_STEREO_MODE_SPLIT_VERTICAL:   stereo_mode = RK_STEREO_LR; break;
    case RENDER_STEREO_MODE_SPLIT_HORIZONTAL: stereo_mode = RK_STEREO_BT; break;
    case RENDER_STEREO_MODE_MVC:              stereo_mode = RK_STEREO_MVC; break;
    default:                                  stereo_mode = 0; break;
  }
  
  return stereo_mode;
}


