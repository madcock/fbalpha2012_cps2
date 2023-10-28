#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <vector>
#include <string>

#include "libretro.h"
#include "libretro_core_options.h"
#include "burner.h"
#include "input/inp_keys.h"
#include "state.h"
#include "descriptors.h"

static unsigned int BurnDrvGetIndexByName(const char* name);

extern INT32 EnableHiscores;

#define STAT_NOFIND   0
#define STAT_OK      1
#define STAT_CRC      2
#define STAT_SMALL   3
#define STAT_LARGE   4

#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif

struct ROMFIND
{
   unsigned int nState;
   int nArchive;
   INT32 nPos;
   BurnRomInfo ri;
};

static bool gamepad_controls = true;
static bool analog_controls_enabled = false;

static std::vector<std::string> g_find_list_path;
static ROMFIND g_find_list[1024];
static unsigned g_rom_count;

#if !defined(SF2000)
#define AUDIO_SAMPLERATE 44100
#define AUDIO_SEGMENT_LENGTH 534 // <-- Hardcoded value that corresponds well to 32kHz audio.
#define VIDEO_REFRESH_RATE 59.629403f
#else
#define AUDIO_SAMPLERATE 11025
#define AUDIO_SEGMENT_LENGTH 184 // <-- Hardcoded value that corresponds well to 32kHz audio.
#define VIDEO_REFRESH_RATE 60
#endif

static uint16_t *g_fba_frame      = NULL;
static uint16_t *g_fba_rotate_buf = NULL;
static int16_t g_audio_buf[AUDIO_SEGMENT_LENGTH * 2];

static uint16_t rotate_buf_width  = 0;
static uint16_t rotate_buf_margin = 0;
static bool libretro_supports_option_categories = false;

// libretro globals

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_log_printf_t log_cb;
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   libretro_set_core_options(environ_cb,
         &libretro_supports_option_categories);
}

char g_rom_dir[1024];
char g_save_dir[1024];
char g_system_dir[1024];
static bool driver_inited       = false;
static bool core_aspect_par     = false;
static bool display_auto_rotate = true;
static bool display_rotated     = false;
static bool hw_rotate_enabled   = false;
static bool input_rotated       = false;

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "FB Alpha 2012 CPS-2";
   info->library_version = "v0.2.97.28";
   info->need_fullpath = true;
   info->block_extract = true;
   info->valid_extensions = "zip";
}

static void poll_input();
static bool init_input();

/* Frameskipping Support */

static unsigned frameskip_type             = 0;
static unsigned frameskip_threshold        = 0;
static uint16_t frameskip_counter          = 0;

static bool retro_audio_buff_active        = false;
static unsigned retro_audio_buff_occupancy = 0;
static bool retro_audio_buff_underrun      = false;
/* Maximum number of consecutive frames that
 * can be skipped */
#define FRAMESKIP_MAX 30

static unsigned audio_latency              = 0;
static bool update_audio_latency           = false;

static void retro_audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   retro_audio_buff_active    = active;
   retro_audio_buff_occupancy = occupancy;
   retro_audio_buff_underrun  = underrun_likely;
}

static void init_frameskip(void)
{
   if (frameskip_type > 0)
   {
      struct retro_audio_buffer_status_callback buf_status_cb;

      buf_status_cb.callback = retro_audio_buff_status_cb;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,
            &buf_status_cb))
      {
         if (log_cb)
            log_cb(RETRO_LOG_WARN, "Frameskip disabled - frontend does not support audio buffer status monitoring.\n");

         retro_audio_buff_active    = false;
         retro_audio_buff_occupancy = 0;
         retro_audio_buff_underrun  = false;
         audio_latency              = 0;
      }
      else
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         float frame_time_msec = 1000.0f / VIDEO_REFRESH_RATE;

         /* Set latency to 6x current frame time... */
         audio_latency = (unsigned)((6.0f * frame_time_msec) + 0.5f);

         /* ...then round up to nearest multiple of 32 */
         audio_latency = (audio_latency + 0x1F) & ~0x1F;
      }
   }
   else
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      audio_latency = 0;
   }

   update_audio_latency = true;
}

/* Low pass audio filter */

static bool low_pass_enabled       = false;
static int32_t low_pass_range      = 0;
/* Previous samples */
static int32_t low_pass_left_prev  = 0;
static int32_t low_pass_right_prev = 0;

static void low_pass_filter_stereo(int16_t *buf, int length)
{
   int samples            = length;
   int16_t *out           = buf;

   /* Restore previous samples */
   int32_t low_pass_left  = low_pass_left_prev;
   int32_t low_pass_right = low_pass_right_prev;

   /* Single-pole low-pass filter (6 dB/octave) */
   int32_t factor_a       = low_pass_range;
   int32_t factor_b       = 0x10000 - factor_a;

   do
   {
      /* Apply low-pass filter */
      low_pass_left  = (low_pass_left  * factor_a) + (*out       * factor_b);
      low_pass_right = (low_pass_right * factor_a) + (*(out + 1) * factor_b);

      /* 16.16 fixed point */
      low_pass_left  >>= 16;
      low_pass_right >>= 16;

      /* Update sound buffer */
      *out++ = (int16_t)low_pass_left;
      *out++ = (int16_t)low_pass_right;
   }
   while (--samples);

   /* Save last samples for next frame */
   low_pass_left_prev  = low_pass_left;
   low_pass_right_prev = low_pass_right;
}

// FBA stubs
unsigned ArcadeJoystick;

int bDrvOkay;
int bRunPause;
BOOL bAlwaysProcessKeyboardInput;

BOOL bDoIpsPatch;
void IpsApplyPatches(UINT8 *, char *) {}

TCHAR szAppHiscorePath[MAX_PATH];
TCHAR szAppSamplesPath[MAX_PATH];

#ifdef __cplusplus
extern "C" {
#endif
TCHAR szAppBurnVer[16];
#ifdef __cplusplus
}
#endif

static int nDIPOffset;

static void InpDIPSWGetOffset (void)
{
   BurnDIPInfo bdi;
   nDIPOffset = 0;

   for(int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
   {
      if (bdi.nFlags == 0xF0) /* 0xF0 is beginning of DIP switch list */
      {
         nDIPOffset = bdi.nInput;
         if (log_cb)
            log_cb(RETRO_LOG_INFO, "DIP switches offset: %d.\n", bdi.nInput);
         break;
      }
   }
}

void InpDIPSWResetDIPs (void)
{
   int i = 0;
   BurnDIPInfo bdi;
   struct GameInp * pgi = NULL;

   InpDIPSWGetOffset();

   while (BurnDrvGetDIPInfo(&bdi, i) == 0)
   {
      if (bdi.nFlags == 0xFF)
      {
         pgi = GameInp + bdi.nInput + nDIPOffset;

         if (pgi)
            pgi->Input.Constant.nConst = (pgi->Input.Constant.nConst & ~bdi.nMask) | (bdi.nSetting & bdi.nMask);
      }
      i++;
   }
}

static int InpDIPSWInit(void)
{
   BurnDIPInfo bdi;

   InpDIPSWGetOffset();
   InpDIPSWResetDIPs();

   for(int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
   {
      /* 0xFE is the beginning label for a DIP switch entry */
      /* 0xFD are region DIP switches */
      if (bdi.nFlags == 0xFE || bdi.nFlags == 0xFD)
      {
         if (log_cb)
            log_cb(RETRO_LOG_INFO, "DIP switch label: %s.\n", bdi.szText);

         int l = 0;
         for (int k = 0; l < bdi.nSetting; k++)
         {
            BurnDIPInfo bdi_tmp;
            BurnDrvGetDIPInfo(&bdi_tmp, k+i+1);

            if (bdi_tmp.nMask == 0x3F ||
                  bdi_tmp.nMask == 0x30) /* filter away NULL entries */
               continue;

            if (log_cb)
               log_cb(RETRO_LOG_INFO, "DIP switch option: %s.\n", bdi_tmp.szText);
            l++;
         }
      }
   }

   return 0;
}

const INT32 nConfigMinVersion = 0x020921;

static int find_rom_by_crc(uint32_t crc, const ZipEntry *list, unsigned elems)
{
   for (unsigned i = 0; i < elems; i++)
   {
      if (list[i].nCrc == crc)
         return i;
   }

   return -1;
}

static void free_archive_list(ZipEntry *list, unsigned count)
{
   if (list)
   {
      for (unsigned i = 0; i < count; i++)
         free(list[i].szName);
      free(list);
   }
}

static INT32 archive_load_rom(UINT8 *dest, INT32 *wrote, INT32 i)
{
   if (i < 0 || i >= g_rom_count)
      return 1;

   int archive = g_find_list[i].nArchive;

   if (ZipOpen((char*)g_find_list_path[archive].c_str()) != 0)
      return 1;

   BurnRomInfo ri = {0};
   BurnDrvGetRomInfo(&ri, i);

   if (ZipLoadFile(dest, ri.nLen, wrote, g_find_list[i].nPos) != 0)
   {
      ZipClose();
      return 1;
   }

   ZipClose();
   return 0;
}

// This code is very confusing. The original code is even more confusing :(
static bool open_archive(void)
{
   memset(g_find_list, 0, sizeof(g_find_list));

   // FBA wants some roms ... Figure out how many.
   g_rom_count = 0;
   while (!BurnDrvGetRomInfo(&g_find_list[g_rom_count].ri, g_rom_count))
      g_rom_count++;

   g_find_list_path.clear();

   // Check if we have said archives.
   // Check if archives are found. These are relative to g_rom_dir.
   char *rom_name;
   for (unsigned index = 0; index < 32; index++)
   {
      if (BurnDrvGetZipName(&rom_name, index))
         continue;

      if (log_cb)
         log_cb(RETRO_LOG_INFO, "[FBA] Archive: %s\n", rom_name);

      char path[1024];
#ifdef _XBOX
      snprintf(path, sizeof(path), "%s\\%s", g_rom_dir, rom_name);
#else
      snprintf(path, sizeof(path), "%s/%s", g_rom_dir, rom_name);
#endif

      if (ZipOpen(path) != 0)
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[FBA] Failed to find archive: %s\n", path);
         return false;
      }
      ZipClose();

      g_find_list_path.push_back(path);
   }

   for (unsigned z = 0; z < g_find_list_path.size(); z++)
   {
      if (ZipOpen((char*)g_find_list_path[z].c_str()) != 0)
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[FBA] Failed to open archive %s\n", g_find_list_path[z].c_str());
         return false;
      }

      ZipEntry *list = NULL;
      INT32 count;
      ZipGetList(&list, &count);

      // Try to map the ROMs FBA wants to ROMs we find inside our pretty archives ...
      for (unsigned i = 0; i < g_rom_count; i++)
      {
         if (g_find_list[i].nState == STAT_OK)
            continue;

         if (g_find_list[i].ri.nType == 0 || g_find_list[i].ri.nLen == 0 || g_find_list[i].ri.nCrc == 0)
         {
            g_find_list[i].nState = STAT_OK;
            continue;
         }

         int index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count);
         if (index < 0)
            continue;

         // Yay, we found it!
         g_find_list[i].nArchive = z;
         g_find_list[i].nPos = index;
         g_find_list[i].nState = STAT_OK;

         if (list[index].nLen < g_find_list[i].ri.nLen)
            g_find_list[i].nState = STAT_SMALL;
         else if (list[index].nLen > g_find_list[i].ri.nLen)
            g_find_list[i].nState = STAT_LARGE;
      }

      free_archive_list(list, count);
      ZipClose();
   }

   // Going over every rom to see if they are properly loaded before we continue ...
   for (unsigned i = 0; i < g_rom_count; i++)
   {
      if (g_find_list[i].nState != STAT_OK)
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[FBA] ROM index %i was not found ... CRC: 0x%08x\n",
                  i, g_find_list[i].ri.nCrc);
         if(!(g_find_list[i].ri.nType & BRF_OPT))
            return false;
      }
   }

   BurnExtLoadRom = archive_load_rom;
   return true;
}

void retro_init(void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   BurnLibInit();

   frameskip_type             = 0;
   frameskip_threshold        = 0;
   frameskip_counter          = 0;
   retro_audio_buff_active    = false;
   retro_audio_buff_occupancy = 0;
   retro_audio_buff_underrun  = false;
   audio_latency              = 0;
   update_audio_latency       = false;

   low_pass_enabled           = false;
   low_pass_range             = 0;
   low_pass_left_prev         = 0;
   low_pass_right_prev        = 0;
}

void retro_deinit(void)
{
   GameInpExit();
   BurnLibExit();

   if (g_fba_frame)
      free(g_fba_frame);
   g_fba_frame = NULL;

   if (g_fba_rotate_buf)
      free(g_fba_rotate_buf);
   g_fba_rotate_buf = NULL;
}

extern "C" {
   INT32 Cps2Frame(void);
   void HiscoreApply(void);
};

void retro_reset(void)
{
   struct GameInp* pgi = GameInp;

   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->Input.Switch.nCode != FBK_F3)
         continue;

      pgi->Input.nVal = 1;
      *(pgi->Input.pVal) = pgi->Input.nVal;

      break;
   }

   nBurnLayer = 0xff;
   pBurnSoundOut = g_audio_buf;
   nBurnSoundRate = AUDIO_SAMPLERATE;
   nCurrentFrame++;
   HiscoreApply();
   Cps2Frame();

   low_pass_left_prev  = 0;
   low_pass_right_prev = 0;
}

static bool first_init = true;

static void check_variables(bool first_run)
{
   struct retro_variable var = {0};
   bool last_core_aspect_par;
   unsigned last_frameskip_type;

   var.key             = "fba2012cps2_cpu_speed_adjust";
   var.value           = NULL;
   nBurnCPUSpeedAdjust = 0x0100;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "100") == 0)
         nBurnCPUSpeedAdjust = 0x0100;
      else if (strcmp(var.value, "110") == 0)
         nBurnCPUSpeedAdjust = 0x0110;
      else if (strcmp(var.value, "120") == 0)
         nBurnCPUSpeedAdjust = 0x0120;
      else if (strcmp(var.value, "130") == 0)
         nBurnCPUSpeedAdjust = 0x0130;
      else if (strcmp(var.value, "140") == 0)
         nBurnCPUSpeedAdjust = 0x0140;
      else if (strcmp(var.value, "150") == 0)
         nBurnCPUSpeedAdjust = 0x0150;
      else if (strcmp(var.value, "160") == 0)
         nBurnCPUSpeedAdjust = 0x0160;
      else if (strcmp(var.value, "170") == 0)
         nBurnCPUSpeedAdjust = 0x0170;
      else if (strcmp(var.value, "180") == 0)
         nBurnCPUSpeedAdjust = 0x0180;
      else if (strcmp(var.value, "190") == 0)
         nBurnCPUSpeedAdjust = 0x0190;
      else if (strcmp(var.value, "200") == 0)
         nBurnCPUSpeedAdjust = 0x0200;
   }

   var.key             = "fba2012cps2_hiscores";
   var.value           = NULL;
   EnableHiscores      = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      if (strcmp(var.value, "enabled") == 0)
         EnableHiscores = 1;

   var.key          = "fba2012cps2_controls";
   var.value        = NULL;
   gamepad_controls = true;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      if (strcmp(var.value, "arcade") == 0)
         gamepad_controls = false;

   var.key              = "fba2012cps2_aspect";
   var.value            = NULL;
   last_core_aspect_par = core_aspect_par;
   core_aspect_par      = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      if (strcmp(var.value, "PAR") == 0)
         core_aspect_par = true;

   if (!first_run && (core_aspect_par != last_core_aspect_par))
   {
      struct retro_system_av_info av_info;
      retro_get_system_av_info(&av_info);
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
   }

   if (first_run)
   {
      var.key             = "fba2012cps2_auto_rotate";
      var.value           = NULL;
      display_auto_rotate = true;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
         if (strcmp(var.value, "disabled") == 0)
            display_auto_rotate = false;
   }

   var.key             = "fba2012cps2_lowpass_filter";
   var.value           = NULL;
   low_pass_enabled    = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      if (strcmp(var.value, "enabled") == 0)
         low_pass_enabled = true;

   var.key             = "fba2012cps2_lowpass_range";
   var.value           = NULL;
   low_pass_range      = (60 * 65536) / 100;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      low_pass_range = (strtol(var.value, NULL, 10) * 65536) / 100;

   var.key             = "fba2012cps2_frameskip";
   var.value           = NULL;
   last_frameskip_type = frameskip_type;
   frameskip_type      = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "auto") == 0)
         frameskip_type = 1;
      else if (strcmp(var.value, "manual") == 0)
         frameskip_type = 2;
   }

   var.key             = "fba2012cps2_frameskip_threshold";
   var.value           = NULL;
   frameskip_threshold = 33;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_threshold = strtol(var.value, NULL, 10);

   /* (Re)Initialise frameskipping, if required */
   if ((frameskip_type != last_frameskip_type) || first_run)
      init_frameskip();
}

void retro_run(void)
{
   INT32 width, height;
   BurnDrvGetFullSize(&width, &height);
   pBurnDraw = (uint8_t*)g_fba_frame;
   nBurnPitch = width * sizeof(uint16_t);
   nSkipFrame = 0;

   poll_input();

   nBurnLayer = 0xff;
   pBurnSoundOut = g_audio_buf;
   nBurnSoundRate = AUDIO_SAMPLERATE;
   //nBurnSoundLen = AUDIO_SEGMENT_LENGTH;

   /* Check whether current frame should
    * be skipped */
   if ((frameskip_type > 0) && retro_audio_buff_active)
   {
      switch (frameskip_type)
      {
         case 1: /* auto */
            nSkipFrame = retro_audio_buff_underrun ? 1 : 0;
            break;
         case 2: /* manual */
            nSkipFrame = (retro_audio_buff_occupancy < frameskip_threshold) ? 1 : 0;
            break;
         default:
            nSkipFrame = 0;
            break;
      }

      if (!nSkipFrame || (frameskip_counter >= FRAMESKIP_MAX))
      {
         nSkipFrame        = 0;
         frameskip_counter = 0;
      }
      else
         frameskip_counter++;
   }

   /* If frameskip settings have changed, update
    * frontend audio latency */
   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
            &audio_latency);
      update_audio_latency = false;
   }

   nCurrentFrame++;
   HiscoreApply();
   Cps2Frame();

   if (!display_rotated || hw_rotate_enabled)
   {
      if (!nSkipFrame)
         video_cb(g_fba_frame, width, height, nBurnPitch);
      else
         video_cb(NULL, width, height, nBurnPitch);
   }
   else
   {
      /* Perform software-based display rotation */
      if (!nSkipFrame)
      {
         uint16_t *in_ptr  = g_fba_frame;
         uint16_t *out_ptr = g_fba_rotate_buf;
         size_t x, y;

         for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
               *(out_ptr + (y + rotate_buf_margin) + (((width - 1) - x) * rotate_buf_width)) =
                     *(in_ptr + x + (y * width));

         video_cb(g_fba_rotate_buf, rotate_buf_width, width, rotate_buf_width * sizeof(uint16_t));
      }
      else
         video_cb(NULL, rotate_buf_width, width, rotate_buf_width * sizeof(uint16_t));
   }

   if (low_pass_enabled)
      low_pass_filter_stereo(g_audio_buf, nBurnSoundLen);

   audio_batch_cb(g_audio_buf, nBurnSoundLen);

   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);
}

static uint8_t *write_state_ptr;
static const uint8_t *read_state_ptr;
static unsigned state_size;

static INT32 burn_write_state_cb(BurnArea *pba)
{
   memcpy(write_state_ptr, pba->Data, pba->nLen);
   write_state_ptr += pba->nLen;
   return 0;
}

static INT32 burn_read_state_cb(BurnArea *pba)
{
   memcpy(pba->Data, read_state_ptr, pba->nLen);
   read_state_ptr += pba->nLen;
   return 0;
}

static INT32 burn_dummy_state_cb(BurnArea *pba)
{
   state_size += pba->nLen;
   return 0;
}

size_t retro_serialize_size()
{
   if (state_size)
      return state_size;

   BurnAcb = burn_dummy_state_cb;
   state_size = 0;
   BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);
   return state_size;
}

bool retro_serialize(void *data, size_t size)
{
   if (size != state_size)
      return false;

   BurnAcb = burn_write_state_cb;
   write_state_ptr = (uint8_t*)data;
   BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);

   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (size != state_size)
      return false;
   BurnAcb = burn_read_state_cb;
   read_state_ptr = (const uint8_t*)data;
   BurnAreaScan(ACB_FULLSCAN | ACB_WRITE, 0);

   return true;
}

void retro_cheat_reset() {}
void retro_cheat_set(unsigned, bool, const char*) {}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   INT32 width, height;

   memset(info, 0, sizeof(*info));

   BurnDrvGetVisibleSize(&width, &height);

   if (display_rotated && !hw_rotate_enabled)
   {
      info->geometry.base_width   = (unsigned)rotate_buf_width;
      info->geometry.base_height  = (unsigned)height;
      info->geometry.max_width    = (unsigned)rotate_buf_width;
      info->geometry.max_height   = (unsigned)height;
   }
   else
   {
      unsigned drv_flags = BurnDrvGetFlags();

      if (!display_auto_rotate &&
          (drv_flags & BDF_ORIENTATION_VERTICAL))
      {
         info->geometry.base_width   = (unsigned)height;
         info->geometry.base_height  = (unsigned)width;
         info->geometry.max_width    = (unsigned)height;
         info->geometry.max_height   = (unsigned)width;
      }
      else
      {
         info->geometry.base_width   = (unsigned)width;
         info->geometry.base_height  = (unsigned)height;
         info->geometry.max_width    = (unsigned)width;
         info->geometry.max_height   = (unsigned)height;
      }
   }

   if (!core_aspect_par)
#if defined(DINGUX)
      info->geometry.aspect_ratio = (4.0f / 3.0f);
#else
      info->geometry.aspect_ratio = display_rotated ?
            (3.0f / 4.0f) : (4.0f / 3.0f);
#endif

   info->timing.fps               = VIDEO_REFRESH_RATE;
#if !defined(SF2000)
   info->timing.sample_rate       = VIDEO_REFRESH_RATE * AUDIO_SEGMENT_LENGTH;
#else
   info->timing.sample_rate       = AUDIO_SAMPLERATE;
#endif
}

static bool fba_init(unsigned driver, const char *game_zip_name)
{
   nBurnDrvActive = driver;
   char input_fs[1024];

   if (!open_archive())
      return false;

   nBurnBpp = 2;
   nFMInterpolation = 3;
   nInterpolation = 3;

   BurnDrvInit();
   snprintf(input_fs, sizeof(input_fs), "%s%c%s.fs", g_save_dir, slash, BurnDrvGetTextA(DRV_NAME));
   BurnStateLoad(input_fs, 0, NULL);

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "Game: %s\n", game_zip_name);

   INT32 width, height;
   BurnDrvGetFullSize(&width, &height);
   nBurnPitch = width * sizeof(uint16_t);

   unsigned drv_flags = BurnDrvGetFlags();

   if (display_auto_rotate)
   {
      display_rotated = (drv_flags & BDF_ORIENTATION_VERTICAL);
      input_rotated   = false;
   }
   else
   {
      display_rotated = false;
      input_rotated   = (drv_flags & BDF_ORIENTATION_VERTICAL);
   }

   if (display_rotated)
   {
      unsigned rotation = 1;
      hw_rotate_enabled = environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);

      if (!hw_rotate_enabled)
      {
         uint16_t rotate_buf_height = (uint16_t)width;
#if defined(DINGUX)
         /* OpenDingux platforms do not have proper
          * aspect ratio control - we can only select
          * between PAR and 'fullscreen stretched'.
          * Since vertical CPS games are meant to be
          * displayed at a ratio of 3:4, either option
          * produces severe visual distortion. We work
          * around this by padding the rotated image, so
          * the resultant rotation buffer is 'square'
          * (this gives the correct 4:3 ratio when
          * stretched to fit the screen)
          * > Note: on OpenDingux, always round video
          *   width up to the nearest multiple of 16 */
         rotate_buf_width  = (rotate_buf_height + 0xF) & ~0xF;
         rotate_buf_margin = (rotate_buf_width - height) >> 1;
#else
         rotate_buf_width  = height;
         rotate_buf_margin = 0;
#endif
         g_fba_rotate_buf = (uint16_t*)calloc(1,
               (uint32_t)rotate_buf_width * (uint32_t)rotate_buf_height * sizeof(uint16_t));
      }
   }

   BurnRecalcPal();

#ifdef FRONTEND_SUPPORTS_RGB565
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

   if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      if (log_cb)
         log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
#endif

   return true;
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

bool retro_load_game(const struct retro_game_info *info)
{
   bool retval = false;
   char basename[128];

   if (!info)
      return false;

   extract_basename(basename, info->path, sizeof(basename));
   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   const char *dir = NULL;
   /* If save directory is defined use it... */
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      strncpy(g_save_dir, dir, sizeof(g_save_dir));
      log_cb(RETRO_LOG_INFO, "Setting save dir to %s\n", g_save_dir);
   }
   else
   {
      /* ...otherwise use ROM directory */
      strncpy(g_save_dir, g_rom_dir, sizeof(g_save_dir));
      log_cb(RETRO_LOG_ERROR, "Save dir not defined => use roms dir %s\n", g_save_dir);
   }

   /* If system directory is defined use it... */
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      strncpy(g_system_dir, dir, sizeof(g_system_dir));
      log_cb(RETRO_LOG_INFO, "Setting system dir to %s\n", g_system_dir);
   }
   else
   {
      /* ...otherwise use ROM directory */
      strncpy(g_system_dir, g_rom_dir, sizeof(g_system_dir));
      log_cb(RETRO_LOG_ERROR, "System dir not defined => use roms dir %s\n", g_system_dir);
   }

   unsigned i = BurnDrvGetIndexByName(basename);
   if (i < nBurnDrvCount)
   {
      INT32 width, height;

      pBurnSoundOut = g_audio_buf;
      nBurnSoundRate = AUDIO_SAMPLERATE;
      nBurnSoundLen = AUDIO_SEGMENT_LENGTH;

      check_variables(true);

      if (!fba_init(i, basename))
         return false;

      driver_inited = true;
      analog_controls_enabled = init_input();

      BurnDrvGetFullSize(&width, &height);
      g_fba_frame = (uint16_t*)malloc((uint32_t)width * (uint32_t)height * sizeof(uint16_t));

      retval = true;
   }
   else if (log_cb)
      log_cb(RETRO_LOG_ERROR, "[FBA] Cannot find driver.\n");

   InpDIPSWInit();

   return retval;
}

bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

void retro_unload_game(void)
{
   if (driver_inited)
   {
      char output_fs[1024];

      snprintf(output_fs, sizeof(output_fs), "%s%c%s.fs", g_save_dir, slash, BurnDrvGetTextA(DRV_NAME));
      BurnStateSave(output_fs, 0);
      BurnDrvExit();
   }

   driver_inited = false;
}

unsigned retro_get_region() { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned) { return 0; }
size_t retro_get_memory_size(unsigned) { return 0; }

unsigned retro_api_version() { return RETRO_API_VERSION; }

void retro_set_controller_port_device(unsigned, unsigned) {}

// Input stuff.

// Ref GamcPlayer() in ../gamc.cpp
struct key_map
{
   const char *bii_name;
   unsigned nCode[2];
};
static uint8_t keybinds[0x5000][2];

#define BIND_MAP_COUNT 300

static const char *print_label(unsigned i)
{
   switch(i)
   {
      case RETRO_DEVICE_ID_JOYPAD_B:
         return "RetroPad Button B";
      case RETRO_DEVICE_ID_JOYPAD_Y:
         return "RetroPad Button Y";
      case RETRO_DEVICE_ID_JOYPAD_SELECT:
         return "RetroPad Button Select";
      case RETRO_DEVICE_ID_JOYPAD_START:
         return "RetroPad Button Start";
      case RETRO_DEVICE_ID_JOYPAD_UP:
         return "RetroPad D-Pad Up";
      case RETRO_DEVICE_ID_JOYPAD_DOWN:
         return "RetroPad D-Pad Down";
      case RETRO_DEVICE_ID_JOYPAD_LEFT:
         return "RetroPad D-Pad Left";
      case RETRO_DEVICE_ID_JOYPAD_RIGHT:
         return "RetroPad D-Pad Right";
      case RETRO_DEVICE_ID_JOYPAD_A:
         return "RetroPad Button A";
      case RETRO_DEVICE_ID_JOYPAD_X:
         return "RetroPad Button X";
      case RETRO_DEVICE_ID_JOYPAD_L:
         return "RetroPad Button L";
      case RETRO_DEVICE_ID_JOYPAD_R:
         return "RetroPad Button R";
      case RETRO_DEVICE_ID_JOYPAD_L2:
         return "RetroPad Button L2";
      case RETRO_DEVICE_ID_JOYPAD_R2:
         return "RetroPad Button R2";
      case RETRO_DEVICE_ID_JOYPAD_L3:
         return "RetroPad Button L3";
      case RETRO_DEVICE_ID_JOYPAD_R3:
         return "RetroPad Button R3";
      default:
         return "No known label";
   }
}

#define PTR_INCR ((incr++ % 3 == 2) ? counter++ : counter)

static bool init_input(void)
{
   GameInpInit();
   GameInpDefault();

   bool has_analog = false;
   struct GameInp* pgi = GameInp;
   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->nType == BIT_ANALOG_REL)
      {
         has_analog = true;
         break;
      }
   }

   //needed for Neo Geo button mappings (and other drivers in future)
   const char * parentrom   = BurnDrvGetTextA(DRV_PARENT);
   const char * boardrom   = BurnDrvGetTextA(DRV_BOARDROM);
   const char * drvname      = BurnDrvGetTextA(DRV_NAME);
   INT32   genre      = BurnDrvGetGenreFlags();
   INT32   hardware   = BurnDrvGetHardwareCode();

   if (log_cb)
   {
      log_cb(RETRO_LOG_INFO, "has_analog: %d\n", has_analog);
      if(parentrom)
         log_cb(RETRO_LOG_INFO, "parentrom: %s\n", parentrom);
      if(boardrom)
         log_cb(RETRO_LOG_INFO, "boardrom: %s\n", boardrom);
      if(drvname)
         log_cb(RETRO_LOG_INFO, "drvname: %s\n", drvname);
      log_cb(RETRO_LOG_INFO, "genre: %d\n", genre);
      log_cb(RETRO_LOG_INFO, "hardware: %d\n", hardware);
   }

   /* initialization */
   struct BurnInputInfo bii;
   memset(&bii, 0, sizeof(bii));

   // Bind to nothing.
   for (unsigned i = 0; i < 0x5000; i++)
      keybinds[i][0] = 0xff;

   pgi = GameInp;

   key_map bind_map[BIND_MAP_COUNT];
   unsigned counter = 0;
   unsigned incr = 0;


   /* NOTE: The following buttons aren't mapped to the RetroPad:
    *
    * "Dip 1/2/3", "Dips", "Debug Dip", "Debug Dip 1/2", "Region",
    * "Service", "Service 1/2/3/4", "Diagnostic", "Diagnostics",
    * "Test", "Reset", "Volume Up/Down", "System", "Slots" and "Tilt"
    *
    * Mahjong/Poker controls aren't mapped since they require a keyboard
    * Excite League isn't mapped because it uses 11 buttons
    *
    * L3 and R3 are unmapped and could still be used */

   /* Universal controls */

   bind_map[PTR_INCR].bii_name = "Diagnostic";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R3;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Coin 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Coin 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Coin 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "Coin 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P1 Coin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Coin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P3 Coin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P4 Coin";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Start 1";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Start 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Start 3";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "Start 4";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P1 Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P3 Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P4 Start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P1 start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 start";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_START;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Movement controls */

   bind_map[PTR_INCR].bii_name = "Up";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Down";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Left";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Right";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Up (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Down (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Left (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Right (Cocktail)";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Up";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Down";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Left";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Up";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Down";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Left";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Right";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P3 Up";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Down";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Left";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Right";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P4 Up";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Down";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Left";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 3;

   bind_map[PTR_INCR].bii_name = "P4 Right";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 3;

   /* Angel Kids, Crazy Climber 2, Bullet, etc. */

   bind_map[PTR_INCR].bii_name = "P1 Left Stick Up";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Left Stick Down";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Left Stick Left";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Left Stick Right";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right Stick Up";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L2;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right Stick Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R2;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right Stick Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right Stick Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Rght Stick Up";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L2;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Rght Stick Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R2;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Rght Stick Left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Rght Stick Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Up 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Down 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Left 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Up 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L2;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Down 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R2;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Left 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Up 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Down 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Left 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Right 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Up 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L2;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Down 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R2;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Left 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Right 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P3 Up 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_UP;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Down 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_DOWN;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Left 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Right 1";
   bind_map[PTR_INCR].nCode[0] = input_rotated ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Up 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L2;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Down 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R2;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Left 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Right 2";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
   bind_map[PTR_INCR].nCode[1] = 2;

   /* Analog controls
    *
    * FIXME: Analog controls still refuse to work properly */

   bind_map[PTR_INCR].bii_name = "Left/Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Up/Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Right / left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Up / Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Right / left";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Up / Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P1 Trackball X";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Trackball Y";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Trackball X";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Trackball Y";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "Target Left/Right";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Target Up/Down";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Turn";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Turn";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P1 Bat Swing";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Bat Swing";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P1 Handle";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Throttle";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Gun L-R";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Gun U-D";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

    bind_map[PTR_INCR].bii_name = "Stick X";
    bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
    bind_map[PTR_INCR].nCode[1] = 0;

    bind_map[PTR_INCR].bii_name = "Stick Y";
    bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
    bind_map[PTR_INCR].nCode[1] = 0;

   /* Light gun controls
    *
    * FIXME: Controls don't seem to work properly */

   bind_map[PTR_INCR].bii_name = "P1 X-Axis";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Y-Axis";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 X-Axis";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Y-Axis";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P3 X-Axis";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "P3 Y-Axis";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 2;

   bind_map[PTR_INCR].bii_name = "Crosshair X";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "Crosshair Y";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Gun X";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P1 Gun Y";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 0;

   bind_map[PTR_INCR].bii_name = "P2 Gun X";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_X;
   bind_map[PTR_INCR].nCode[1] = 1;

   bind_map[PTR_INCR].bii_name = "P2 Gun Y";
   bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_ANALOG_Y;
   bind_map[PTR_INCR].nCode[1] = 1;

   /* Arcade map */
   if (gamepad_controls == false)
   {

      /* General controls */

      bind_map[PTR_INCR].bii_name = "Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Button";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P1 Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 5";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 6";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 5";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 6";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Button 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Button 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Space Harrier, 1942, Capcom Commando, Heavy Barrel, etc. */

      bind_map[PTR_INCR].bii_name = "Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 5";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 1 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 2 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 3 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 4 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 5 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Fire";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Fire";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Tri-Pool */

      bind_map[PTR_INCR].bii_name = "Select Game 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Select Game 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Select Game 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Neo Geo */

      bind_map[PTR_INCR].bii_name = "P1 Button A";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button B";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button C";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button D";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Button A";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button B";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button C";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button D";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Street Fighter II, Darkstalkers, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Weak Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Medium Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Strong Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Weak Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Medium Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Strong Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Weak Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Medium Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Strong Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Weak Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Medium Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Strong Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1;

     /* Battle K-Road */

      bind_map[PTR_INCR].bii_name = "P1 Weak punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Medium punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Strong punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Weak kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Medium kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Strong kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Weak punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Medium punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Strong punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Weak kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Medium kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Strong kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

     /* Cyberbots: Full Metal Madness */

      bind_map[PTR_INCR].bii_name = "P1 Low Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 High Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Weapon";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Boost";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Low Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 High Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Weapon";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Boost";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Super Gem Fighter Mini Mix */

      bind_map[PTR_INCR].bii_name = "P1 Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Killer Instinct */

      /* bind_map[PTR_INCR].bii_name = "P1 Button A";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button B";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button C";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button X";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button Y";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button Z";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Button A";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button B";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button C";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button X";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button Y";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button Z";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1; */

      /* Final Fight, Captain Commando, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Jump";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Jump";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Jump";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Jump";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* The Punisher */

      bind_map[PTR_INCR].bii_name = "P1 Super";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Super";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Saturday Night Slam Masters */

      bind_map[PTR_INCR].bii_name = "P1 Pin";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Pin";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Pin";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Pin";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Dungeons & Dragons Tower of Doom/Shadow over Mystara */

      bind_map[PTR_INCR].bii_name = "P1 Select";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Use";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Select";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Use";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Select";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Use";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Select";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Use";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Mercs, U.N. Squadron, Mega Twins, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Special";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Special";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Special";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 2;

      /* Dynasty Wars */

      bind_map[PTR_INCR].bii_name = "P1 Attack Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Attack Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Attack Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Attack Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Armed Police Batrider & Battle Bakraid */

      bind_map[PTR_INCR].bii_name = "P1 Shoot 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shoot 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shoot 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shoot 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Shoot 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Shoot 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Pang 3 */

      bind_map[PTR_INCR].bii_name = "P1 Shot 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shot 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shot 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Shot 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Mighty! Pang, Jong Pai Puzzle Choko and Jyangokushi: Haoh no Saihai */

      bind_map[PTR_INCR].bii_name = "P1 Shot1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shot2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shot3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shot1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Shot2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Carrier Air Wing, Mars Matrix, Alien vs Predator, etc.
       *
       * NOTE: This button is shared between both shmups and brawlers
       * Alien vs. Predator and Armored Warriors received if statements as a workaround */

      bind_map[PTR_INCR].bii_name = "P1 Shot";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shot";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Shot";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Shot";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Varth, Giga Wing, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Bomb";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Bomb";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Enforce */

      bind_map[PTR_INCR].bii_name = "Laser";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Bomb";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Progear */

      bind_map[PTR_INCR].bii_name = "P1 Auto";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Auto";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Dimahoo */

      bind_map[PTR_INCR].bii_name = "P1 Shot (auto)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shot (auto)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Eco Fighters and Pnickies */

      bind_map[PTR_INCR].bii_name = "P1 Turn 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Turn 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Turn 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Turn 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Last Survivor */

      bind_map[PTR_INCR].bii_name = "P1 Turn Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Turn Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Turn Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Turn Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* After Burner, Thunder Blade, etc. */

      bind_map[PTR_INCR].bii_name = "Missile";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Vulcan";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Cannon";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* OutRun, Chase HQ, Super Chase, Cyber Tank, Racing Beat, etc. */

      bind_map[PTR_INCR].bii_name = "Accelerate";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Accelerate";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Accel";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Brake";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Gear";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Nitro";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Turbo";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Super Charger";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Pit In";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Continental Circus */

      bind_map[PTR_INCR].bii_name = "Accelerate 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Accelerate 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R2;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Brake 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Brake 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L2;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Quiz & Dragons, Capcom World 2, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Answer 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Answer 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Answer 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Answer 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Answer 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Answer 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Answer 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Answer 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Super Puzzle Fighter II Turbo */

      bind_map[PTR_INCR].bii_name = "P1 Rotate Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Rotate Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Rotate Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Rotate Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Gals Pinball */

      bind_map[PTR_INCR].bii_name = "Launch Ball / Tilt";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Left Flippers";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Right Flippers";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

   }
   /* Gamepad map */
   else
   {

     /* General controls */

      bind_map[PTR_INCR].bii_name = "Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Button";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P1 Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 5";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button 6";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 5";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button 6";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Button 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Button 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Button 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Button 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Button 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Space Harrier, 1942, Capcom Commando, Heavy Barrel, etc. */

      bind_map[PTR_INCR].bii_name = "Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 5";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 1 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 2 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 3 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 4 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Fire 5 (Cocktail)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Fire";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Fire";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Fire 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Fire 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Fire 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Tri-Pool */

      bind_map[PTR_INCR].bii_name = "Select Game 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Select Game 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Select Game 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Street Fighter II, Darkstalkers, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Weak Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Medium Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Strong Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Weak Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Medium Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Strong Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Weak Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Medium Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Strong Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Weak Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Medium Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Strong Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Battle K-Road */

      bind_map[PTR_INCR].bii_name = "P1 Weak punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Medium punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Strong punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Weak kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Medium kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Strong kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Weak punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Medium punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Strong punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Weak kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Medium kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Strong kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Cyberbots: Full Metal Madness */

      bind_map[PTR_INCR].bii_name = "P1 Low Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 High Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Weapon";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Boost";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Low Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 High Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Weapon";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Boost";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Super Gem Fighter Mini Mix */

      bind_map[PTR_INCR].bii_name = "P1 Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Punch";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Kick";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Killer Instinct */

      /* bind_map[PTR_INCR].bii_name = "P1 Button A";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button B";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button C";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button X";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button Y";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Button Z";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Button A";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button B";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button C";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button X";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button Y";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Button Z";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1; */

      /* Final Fight, Captain Commando, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Jump";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Jump";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Jump";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Attack";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Jump";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* The Punisher */

      bind_map[PTR_INCR].bii_name = "P1 Super";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Super";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Saturday Night Slam Masters */

      bind_map[PTR_INCR].bii_name = "P1 Pin";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Pin";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Pin";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Pin";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Dungeons & Dragons Tower of Doom/Shadow over Mystara */

      bind_map[PTR_INCR].bii_name = "P1 Select";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Use";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Select";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Use";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Select";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P3 Use";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Select";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 3;

      bind_map[PTR_INCR].bii_name = "P4 Use";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Mercs, U.N. Squadron, Mega Twins, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Special";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Special";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Special";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 2;

      /* Dynasty Wars */

      bind_map[PTR_INCR].bii_name = "P1 Attack Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Attack Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Attack Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Attack Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Armed Police Batrider & Battle Bakraid */

      bind_map[PTR_INCR].bii_name = "P1 Shoot 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shoot 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shoot 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shoot 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Shoot 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Shoot 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Pang 3 */

      bind_map[PTR_INCR].bii_name = "P1 Shot 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shot 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shot 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Shot 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Mighty! Pang, Jong Pai Puzzle Choko and Jyangokushi: Haoh no Saihai */

      bind_map[PTR_INCR].bii_name = "P1 Shot1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shot2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Shot3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shot1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Shot2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Carrier Air Wing, Mars Matrix, Alien vs Predator, etc.
       *
       * NOTE: This button is shared between both shmups and brawlers
       * Alien vs. Predator and Armored Warriors received if statements as a workaround */

      bind_map[PTR_INCR].bii_name = "P1 Shot";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shot";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P3 Shot";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 2;

      bind_map[PTR_INCR].bii_name = "P4 Shot";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 3;

      /* Varth, Giga Wing, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Bomb";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Bomb";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Enforce */

      bind_map[PTR_INCR].bii_name = "Laser";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Bomb";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Progear */

      bind_map[PTR_INCR].bii_name = "P1 Auto";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Auto";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Dimahoo */

      bind_map[PTR_INCR].bii_name = "P1 Shot (auto)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Shot (auto)";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Eco Fighters and Pnickies */

      bind_map[PTR_INCR].bii_name = "P1 Turn 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Turn 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Turn 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Turn 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Last Survivor */

      bind_map[PTR_INCR].bii_name = "P1 Turn Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Turn Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Turn Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Turn Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* After Burner, Thunder Blade, etc. */

      bind_map[PTR_INCR].bii_name = "Missile";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Vulcan";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Cannon";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* OutRun, Chase HQ, Super Chase, Cyber Tank, Racing Beat, etc. */

      bind_map[PTR_INCR].bii_name = "Accelerate";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Accelerate";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Accel";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Brake";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Gear";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Nitro";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Turbo";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Super Charger";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Pit In";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Continental Circus */

      bind_map[PTR_INCR].bii_name = "Accelerate 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Accelerate 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_R2;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Brake 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Brake 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_L2;
      bind_map[PTR_INCR].nCode[1] = 0;

      /* Quiz & Dragons, Capcom World 2, etc. */

      bind_map[PTR_INCR].bii_name = "P1 Answer 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Answer 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Answer 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Answer 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Answer 1";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Answer 2";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_X;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Answer 3";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Answer 4";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Super Puzzle Fighter II Turbo */

      bind_map[PTR_INCR].bii_name = "P1 Rotate Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P1 Rotate Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "P2 Rotate Left";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 1;

      bind_map[PTR_INCR].bii_name = "P2 Rotate Right";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 1;

      /* Gals Pinball */

      bind_map[PTR_INCR].bii_name = "Launch Ball / Tilt";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_Y;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Left Flippers";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_B;
      bind_map[PTR_INCR].nCode[1] = 0;

      bind_map[PTR_INCR].bii_name = "Right Flippers";
      bind_map[PTR_INCR].nCode[0] = RETRO_DEVICE_ID_JOYPAD_A;
      bind_map[PTR_INCR].nCode[1] = 0;
   }

   for(unsigned int i = 0; i < nGameInpCount; i++, pgi++)
   {
      BurnDrvGetInputInfo(&bii, i);

      bool value_found = false;

      for(int j = 0; j < counter; j++)
      {
         if((strcmp(bii.szName,"P1 Select") ==0) && (boardrom && (strcmp(boardrom,"neogeo") == 0)))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            keybinds[pgi->Input.Switch.nCode][1] = 0;
            value_found = true;
         }
         else if((strcmp(bii.szName,"P2 Select") ==0) && (boardrom && (strcmp(boardrom,"neogeo") == 0)))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            keybinds[pgi->Input.Switch.nCode][1] = 1;
            value_found = true;
         }

         /* Alien vs. Predator and Armored Warriors both use "Px Shot" which usually serves as the shoot button for shmups
          * To make sure the controls don't overlap with each other if statements are used */

         else if((parentrom && strcmp(parentrom,"avsp") == 0 || strcmp(drvname,"avsp") == 0) && (strcmp(bii.szName,"P1 Shot") ==0))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
            keybinds[pgi->Input.Switch.nCode][1] = 0;
            value_found = true;
         }
         else if((parentrom && strcmp(parentrom,"avsp") == 0 || strcmp(drvname,"avsp") == 0) && (strcmp(bii.szName,"P2 Shot") ==0))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
            keybinds[pgi->Input.Switch.nCode][1] = 1;
            value_found = true;
         }
         else if((parentrom && strcmp(parentrom,"avsp") == 0 || strcmp(drvname,"avsp") == 0) && (strcmp(bii.szName,"P3 Shot") ==0))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
            keybinds[pgi->Input.Switch.nCode][1] = 2;
            value_found = true;
         }
         else if((parentrom && strcmp(parentrom,"armwar") == 0 || strcmp(drvname,"armwar") == 0) && (strcmp(bii.szName,"P1 Shot") ==0))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
            keybinds[pgi->Input.Switch.nCode][1] = 0;
            value_found = true;
         }
         else if((parentrom && strcmp(parentrom,"armwar") == 0 || strcmp(drvname,"armwar") == 0) && (strcmp(bii.szName,"P2 Shot") ==0))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
            keybinds[pgi->Input.Switch.nCode][1] = 1;
            value_found = true;
         }
         else if((parentrom && strcmp(parentrom,"armwar") == 0 || strcmp(drvname,"armwar") == 0) && (strcmp(bii.szName,"P3 Shot") ==0))
         {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
            keybinds[pgi->Input.Switch.nCode][1] = 2;
            value_found = true;
         }
         else if(strcmp(bii.szName, bind_map[j].bii_name) == 0)
         {
            keybinds[pgi->Input.Switch.nCode][0] = bind_map[j].nCode[0];
            keybinds[pgi->Input.Switch.nCode][1] = bind_map[j].nCode[1];
            value_found = true;
         }
         else
            value_found = false;

         if (!value_found)
            continue;

         if (log_cb)
         {
            log_cb(RETRO_LOG_INFO, "%s - assigned to key: %s, port: %d.\n", bii.szName, print_label(keybinds[pgi->Input.Switch.nCode][0]),keybinds[pgi->Input.Switch.nCode][1]);
            log_cb(RETRO_LOG_INFO, "%s - has nSwitch.nCode: %x.\n", bii.szName, pgi->Input.Switch.nCode);
         }
         break;
      }

      if(!value_found && log_cb)
      {
         log_cb(RETRO_LOG_INFO, "WARNING! Button unaccounted for: [%s].\n", bii.szName);
         log_cb(RETRO_LOG_INFO, "%s - has nSwitch.nCode: %x.\n", bii.szName, pgi->Input.Switch.nCode);
      }
   }

   /* add code to select between different descriptors here */
   if(gamepad_controls)
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, default_gamepad);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, default_arcade);

   return has_analog;
}

//#define DEBUG_INPUT
//

static inline int CinpJoyAxis(int i, int axis)
{
   switch(axis)
   {
      case 0:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 1:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 2:
         return 0;
      case 3:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 4:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 5:
         return 0;
      case 6:
         return 0;
      case 7:
         return 0;
   }
   return 0;
}

static inline int CinpMouseAxis(int i, int axis)
{
   return 0;
}

static void poll_input(void)
{
   poll_cb();

   struct GameInp* pgi = GameInp;

   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      switch (pgi->nInput)
      {
         case GIT_CONSTANT: // Constant value
            pgi->Input.nVal = pgi->Input.Constant.nConst;
            *(pgi->Input.pVal) = pgi->Input.nVal;
            break;
         case GIT_SWITCH:
            {
               // Digital input
               INT32 id = keybinds[pgi->Input.Switch.nCode][0];
               unsigned port = keybinds[pgi->Input.Switch.nCode][1];

               bool state = input_cb(port, RETRO_DEVICE_JOYPAD, 0, id);

               if (pgi->nType & BIT_GROUP_ANALOG)
               {
                  // Set analog controls to full
                  if (state)
                     pgi->Input.nVal = 0xFFFF;
                  else
                     pgi->Input.nVal = 0x0001;
#ifdef MSB_FIRST
                  *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
                  *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               }
               else
               {
                  // Binary controls
                  if (state)
                     pgi->Input.nVal = 1;
                  else
                     pgi->Input.nVal = 0;
                  *(pgi->Input.pVal) = pgi->Input.nVal;
               }
               break;
            }
         case GIT_KEYSLIDER:                  // Keyboard slider
            {
               int nAdd = 0;

               // Get states of the two keys
               if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
                  nAdd -= 0x100;
               if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
                  nAdd += 0x100;

               // nAdd is now -0x100 to +0x100

               // Change to slider speed
               nAdd *= pgi->Input.Slider.nSliderSpeed;
               nAdd /= 0x100;

               if (pgi->Input.Slider.nSliderCenter)
               {                                          // Attact to center
                  int v = pgi->Input.Slider.nSliderValue - 0x8000;
                  v *= (pgi->Input.Slider.nSliderCenter - 1);
                  v /= pgi->Input.Slider.nSliderCenter;
                  v += 0x8000;
                  pgi->Input.Slider.nSliderValue = v;
               }

               pgi->Input.Slider.nSliderValue += nAdd;
               // Limit slider
               if (pgi->Input.Slider.nSliderValue < 0x0100)
                  pgi->Input.Slider.nSliderValue = 0x0100;
               if (pgi->Input.Slider.nSliderValue > 0xFF00)
                  pgi->Input.Slider.nSliderValue = 0xFF00;

               int nSlider = pgi->Input.Slider.nSliderValue;
               if (pgi->nType == BIT_ANALOG_REL) {
                  nSlider -= 0x8000;
                  nSlider >>= 4;
               }

               pgi->Input.nVal = (unsigned short)nSlider;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_MOUSEAXIS:                  // Mouse axis
            pgi->Input.nVal = (UINT16)(CinpMouseAxis(pgi->Input.MouseAxis.nMouse, pgi->Input.MouseAxis.nAxis) * nAnalogSpeed);
#ifdef MSB_FIRST
            *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
            *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
            break;
         case GIT_JOYAXIS_FULL:
            {            // Joystick axis
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);

               if (pgi->nType == BIT_ANALOG_REL) {
                  nJoy *= nAnalogSpeed;
                  nJoy >>= 13;

                  // Clip axis to 8 bits
                  if (nJoy < -32768) {
                     nJoy = -32768;
                  }
                  if (nJoy >  32767) {
                     nJoy =  32767;
                  }
               } else {
                  nJoy >>= 1;
                  nJoy += 0x8000;

                  // Clip axis to 16 bits
                  if (nJoy < 0x0001) {
                     nJoy = 0x0001;
                  }
                  if (nJoy > 0xFFFF) {
                     nJoy = 0xFFFF;
                  }
               }

               pgi->Input.nVal = (UINT16)nJoy;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_NEG:
            {            // Joystick axis Lo
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy < 32767)
               {
                  nJoy = -nJoy;

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_POS:
            {            // Joystick axis Hi
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy > 32767)
               {

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
      }
   }
}

static unsigned int BurnDrvGetIndexByName(const char* name)
{
   unsigned int ret = ~0U;
   for (unsigned int i = 0; i < nBurnDrvCount; i++) {
      nBurnDrvActive = i;
      if (strcmp(BurnDrvGetText(DRV_NAME), name) == 0) {
         ret = i;
         break;
      }
   }
   return ret;
}

#ifdef ANDROID
#include <wchar.h>

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
   if (pwcs == NULL)
      return strlen(s);
   return mbsrtowcs(pwcs, &s, n, NULL);
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
   return wcsrtombs(s, &pwcs, n, NULL);
}

#endif
}
