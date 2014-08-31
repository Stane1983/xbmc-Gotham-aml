/*
 *      Copyright (C) 2011-2013 Team XBMC
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

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string>

#include "utils/CPUInfo.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/AMLUtils.h"

#if defined(TARGET_ANDROID)
#include <sys/system_properties.h>
#endif

int aml_set_sysfs_str(const char *path, const char *val)
{
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd >= 0)
  {
    write(fd, val, strlen(val));
    close(fd);
    return 0;
  }
  return -1;
}

int aml_get_sysfs_str(const char *path, char *valstr, const int size)
{
  int fd = open(path, O_RDONLY);
  if (fd >= 0)
  {
    read(fd, valstr, size - 1);
    valstr[strlen(valstr)] = '\0';
    close(fd);
    return 0;
  }

  sprintf(valstr, "%s", "fail");
  return -1;
}

int aml_set_sysfs_int(const char *path, const int val)
{
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd >= 0)
  {
    char bcmd[16];
    sprintf(bcmd, "%d", val);
    write(fd, bcmd, strlen(bcmd));
    close(fd);
    return 0;
  }
  return -1;
}

int aml_get_sysfs_int(const char *path)
{
  int val = -1;
  int fd = open(path, O_RDONLY);
  if (fd >= 0)
  {
    char bcmd[16];
    read(fd, bcmd, sizeof(bcmd));
    val = strtol(bcmd, NULL, 16);
    close(fd);
  }
  return val;
}

bool aml_present()
{
  static int has_aml = -1;
  if (has_aml == -1)
  {
    int rtn = aml_get_sysfs_int("/sys/class/audiodsp/digital_raw");
    if (rtn != -1)
      has_aml = 1;
    else
      has_aml = 0;
    if (has_aml)
      CLog::Log(LOGNOTICE, "aml_present, rtn(%d)", rtn);
  }
  return has_aml == 1;
}

bool aml_hw3d_present()
{
  static int has_hw3d = -1;
  if (has_hw3d == -1)
  {
    if (aml_get_sysfs_int("/sys/class/ppmgr/ppmgr_3d_mode") != -1)
      has_hw3d = 1;
    else
      has_hw3d = 0;
  }
  return has_hw3d == 1;
}

bool aml_wired_present()
{
  static int has_wired = -1;
  if (has_wired == -1)
  {
    char test[64] = {0};
    if (aml_get_sysfs_str("/sys/class/net/eth0/operstate", test, 63) != -1)
      has_wired = 1;
    else
      has_wired = 0;
  }
  return has_wired == 1;
}

void aml_permissions()
{
  if (!aml_present())
    return;

#if defined(TARGET_ANDROID)
  char value_no_need_su[93] = "";
  __system_property_get("xbmc.no_need_su", value_no_need_su);

  if (strncmp(value_no_need_su, "true", 4) != 0)
  {
    // most all aml devices are already rooted.
    int ret = system("ls /system/xbin/su");
    if (ret != 0)
    {
      CLog::Log(LOGWARNING, "aml_permissions: missing su, playback might fail");
    }
    else
    {
      // certain aml devices have 664 permission, we need 666.
      system("su -c chmod 666 /dev/amvideo");
      system("su -c chmod 666 /dev/amstream*");
      system("su -c chmod 666 /sys/class/video/axis");
      system("su -c chmod 666 /sys/class/video/screen_mode");
      system("su -c chmod 666 /sys/class/video/disable_video");
      system("su -c chmod 666 /sys/class/tsync/pts_pcrscr");
      system("su -c chmod 666 /sys/class/audiodsp/digital_raw");
      system("su -c chmod 666 /sys/class/ppmgr/ppmgr_3d_mode");
      
      if (aml_get_device_type() == AML_DEVICE_TYPE_M6)
      {
        system("su -c chmod 666 /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq");
        system("su -c chmod 666 /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
        system("su -c chmod 666 /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
      }
      CLog::Log(LOGINFO, "aml_permissions: permissions changed");
    }
  }
#endif
}

enum AML_DEVICE_TYPE aml_get_device_type()
{
  static enum AML_DEVICE_TYPE aml_device_type = AML_DEVICE_TYPE_UNINIT;
  if (aml_device_type == AML_DEVICE_TYPE_UNINIT)
  {
    std::string cpu_hardware = g_cpuInfo.getCPUHardware();

    if (cpu_hardware.find("MESON-M1") != std::string::npos)
      aml_device_type = AML_DEVICE_TYPE_M1;
    else if (cpu_hardware.find("MESON-M3") != std::string::npos
          || cpu_hardware.find("MESON3")   != std::string::npos)
      aml_device_type = AML_DEVICE_TYPE_M3;
    else if (cpu_hardware.find("Meson6") != std::string::npos)
      aml_device_type = AML_DEVICE_TYPE_M6;
    else if ((cpu_hardware.find("Meson8") != std::string::npos) && (cpu_hardware.find("Meson8B") == std::string::npos))
      aml_device_type = AML_DEVICE_TYPE_M8;
    else if (cpu_hardware.find("Meson8B") != std::string::npos)
      aml_device_type = AML_DEVICE_TYPE_M8B;
    else
      aml_device_type = AML_DEVICE_TYPE_UNKNOWN;
  }

  return aml_device_type;
}

void aml_cpufreq_min(bool limit)
{
// do not touch scaling_min_freq on android
#if !defined(TARGET_ANDROID)
  // only needed for m1/m3 SoCs
  if (  aml_get_device_type() != AML_DEVICE_TYPE_UNKNOWN
    &&  aml_get_device_type() <= AML_DEVICE_TYPE_M3)
  {
    int cpufreq = 300000;
    if (limit)
      cpufreq = 600000;

    aml_set_sysfs_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", cpufreq);
  }
#endif
}

void aml_cpufreq_max(bool limit)
{
  if (!aml_wired_present() && aml_get_device_type() == AML_DEVICE_TYPE_M6)
  {
    // this is a MX Stick, they cannot substain 1GHz
    // operation without overheating so limit them to 800MHz.
    int cpufreq = 1000000;
    if (limit)
      cpufreq = 800000;

    aml_set_sysfs_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", cpufreq);
    aml_set_sysfs_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "ondemand");
  }
}

void aml_set_audio_passthrough(bool passthrough)
{
  if (  aml_present()
    &&  aml_get_device_type() != AML_DEVICE_TYPE_UNKNOWN
    &&  aml_get_device_type() <= AML_DEVICE_TYPE_M8B)
  {
    // m1 uses 1, m3 and above uses 2
    int raw = aml_get_device_type() == AML_DEVICE_TYPE_M1 ? 1:2;
    aml_set_sysfs_int("/sys/class/audiodsp/digital_raw", passthrough ? raw:0);
  }
}

void aml_probe_hdmi_audio()
{
  // Audio {format, channel, freq, cce}
  // {1, 7, 7f, 7}
  // {7, 5, 1e, 0}
  // {2, 5, 7, 0}
  // {11, 7, 7e, 1}
  // {10, 7, 6, 0}
  // {12, 7, 7e, 0}

  int fd = open("/sys/class/amhdmitx/amhdmitx0/edid", O_RDONLY);
  if (fd >= 0)
  {
    char valstr[1024] = {0};

    read(fd, valstr, sizeof(valstr) - 1);
    valstr[strlen(valstr)] = '\0';
    close(fd);

    std::vector<std::string> probe_str = StringUtils::Split(valstr, "\n");

    for (std::vector<std::string>::const_iterator i = probe_str.begin(); i != probe_str.end(); ++i)
    {
      if (i->find("Audio") == std::string::npos)
      {
        for (std::vector<std::string>::const_iterator j = i + 1; j != probe_str.end(); ++j)
        {
          if      (j->find("{1,")  != std::string::npos)
            printf(" PCM found {1,\n");
          else if (j->find("{2,")  != std::string::npos)
            printf(" AC3 found {2,\n");
          else if (j->find("{3,")  != std::string::npos)
            printf(" MPEG1 found {3,\n");
          else if (j->find("{4,")  != std::string::npos)
            printf(" MP3 found {4,\n");
          else if (j->find("{5,")  != std::string::npos)
            printf(" MPEG2 found {5,\n");
          else if (j->find("{6,")  != std::string::npos)
            printf(" AAC found {6,\n");
          else if (j->find("{7,")  != std::string::npos)
            printf(" DTS found {7,\n");
          else if (j->find("{8,")  != std::string::npos)
            printf(" ATRAC found {8,\n");
          else if (j->find("{9,")  != std::string::npos)
            printf(" One_Bit_Audio found {9,\n");
          else if (j->find("{10,") != std::string::npos)
            printf(" Dolby found {10,\n");
          else if (j->find("{11,") != std::string::npos)
            printf(" DTS_HD found {11,\n");
          else if (j->find("{12,") != std::string::npos)
            printf(" MAT found {12,\n");
          else if (j->find("{13,") != std::string::npos)
            printf(" ATRAC found {13,\n");
          else if (j->find("{14,") != std::string::npos)
            printf(" WMA found {14,\n");
          else
            break;
        }
        break;
      }
    }
  }
}
