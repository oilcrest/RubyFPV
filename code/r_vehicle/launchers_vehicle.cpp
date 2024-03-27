/*
    MIT Licence
    Copyright (c) 2024 Petru Soroaga petrusoroaga@yahoo.com
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
        * Redistributions of source code must retain the above copyright
        notice, this list of conditions and the following disclaimer.
        * Redistributions in binary form must reproduce the above copyright
        notice, this list of conditions and the following disclaimer in the
        documentation and/or other materials provided with the distribution.
        * Neither the name of the organization nor the
        names of its contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.
        * Military use is not permited.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL Julien Verneuil BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "launchers_vehicle.h"
#include "../base/config.h"
#include "../base/hardware.h"
#include "../base/hardware_i2c.h"
#include "../base/hw_procs.h"
#include "../base/radio_utils.h"
#include <math.h>
#include <semaphore.h>

#include "shared_vars.h"
#include "timers.h"

static bool s_bAudioCaptureIsStarted = false;
static pthread_t s_pThreadAudioCapture;

void vehicle_launch_tx_telemetry(Model* pModel)
{
   if (NULL == pModel )
   {
      log_error_and_alarm("Invalid model (NULL) on launching TX telemetry. Can't start TX telemetry.");
      return;
   }
   hw_execute_ruby_process(NULL, "ruby_tx_telemetry", NULL, NULL);
}

void vehicle_stop_tx_telemetry()
{
   hw_stop_process("ruby_tx_telemetry");
}

void vehicle_launch_rx_rc(Model* pModel)
{
   if (NULL == pModel )
   {
      log_error_and_alarm("Invalid model (NULL) on launching RX RC. Can't start RX RC.");
      return;
   }
   char szPrefix[64];
   szPrefix[0] = 0;
   #ifdef HW_CAPABILITY_IONICE
   sprintf(szPrefix, "ionice -c 1 -n %d nice -n %d", DEFAULT_IO_PRIORITY_RC, pModel->niceRC);
   #else
   sprintf(szPrefix, "nice -n %d", pModel->niceRC);
   #endif
   hw_execute_ruby_process(szPrefix, "ruby_start", "-rc", NULL);
}

void vehicle_stop_rx_rc()
{
   sem_t* s = sem_open(SEMAPHORE_STOP_RX_RC, 0, S_IWUSR | S_IRUSR, 0);
   if ( SEM_FAILED != s )
   {
      sem_post(s);
      sem_close(s);
   }
}

void vehicle_launch_rx_commands(Model* pModel)
{
   if (NULL == pModel )
   {
      log_error_and_alarm("Invalid model (NULL) on launching RX commands. Can't start RX commands.");
      return;
   }
   char szPrefix[64];
   szPrefix[0] = 0;
   hw_execute_ruby_process(szPrefix, "ruby_start", "-rx_commands", NULL);
}

void vehicle_stop_rx_commands()
{
   //hw_stop_process("ruby_rx_commands");
}

void vehicle_launch_tx_router(Model* pModel)
{
   if (NULL == pModel )
   {
      log_error_and_alarm("Invalid model (NULL) on launching TX video pipeline. Can't start TX video pipeline.");
      return;
   }

   hardware_sleep_ms(20);

   char szPrefix[64];
   szPrefix[0] = 0;
   #ifdef HW_CAPABILITY_IONICE
   if ( pModel->ioNiceRouter > 0 )
      sprintf(szPrefix, "ionice -c 1 -n %d nice -n %d", pModel->ioNiceRouter, pModel->niceRouter );
   else
   #endif
      sprintf(szPrefix, "nice -n %d", pModel->niceRouter);

   hw_execute_ruby_process(szPrefix, "ruby_rt_vehicle", NULL, NULL);
}

void vehicle_stop_tx_router()
{
   char szRouter[64];
   strcpy(szRouter, "ruby_rt_vehicle");

   hw_stop_process(szRouter);
}

static void * _thread_audio_capture(void *argument)
{
   s_bAudioCaptureIsStarted = true;

   Model* pModel = (Model*) argument;
   if ( NULL == pModel )
   {
      s_bAudioCaptureIsStarted = false;
      return NULL;
   }

   char szCommFlag[256];
   char szCommCapture[256];
   char szRate[32];
   char szPriority[64];
   int iIntervalSec = 5;

   sprintf(szCommCapture, "amixer -c 1 sset Mic %d%%", pModel->audio_params.volume);
   hw_execute_bash_command(szCommCapture, NULL);

   strcpy(szRate, "8000");
   if ( 0 == pModel->audio_params.quality )
      strcpy(szRate, "14000");
   if ( 1 == pModel->audio_params.quality )
      strcpy(szRate, "24000");
   if ( 2 == pModel->audio_params.quality )
      strcpy(szRate, "32000");
   if ( 3 == pModel->audio_params.quality )
      strcpy(szRate, "44100");

   strcpy(szRate, "44100");

   szPriority[0] = 0;
   #ifdef HW_CAPABILITY_IONICE
   if ( pModel->ioNiceVideo > 0 )
      sprintf(szPriority, "ionice -c 1 -n %d nice -n %d", pModel->ioNiceVideo, pModel->niceVideo );
   else
   #endif
      sprintf(szPriority, "nice -n %d", pModel->niceVideo );

   sprintf(szCommCapture, "%s arecord --device=hw:1,0 --file-type wav --format S16_LE --rate %s -c 1 -d %d -q >> %s",
      szPriority, szRate, iIntervalSec, FIFO_RUBY_AUDIO1);

   sprintf(szCommFlag, "echo '0123456789' > %s", FIFO_RUBY_AUDIO1);

   hw_stop_process("arecord");

   while ( s_bAudioCaptureIsStarted && ( ! g_bQuit) )
   {
      if ( g_bReinitializeRadioInProgress )
      {
         hardware_sleep_ms(50);
         continue;         
      }

      u32 uTimeCheck = get_current_timestamp_ms();

      hw_execute_bash_command(szCommCapture, NULL);
      
      u32 uTimeNow = get_current_timestamp_ms();
      if ( uTimeNow < uTimeCheck + (u32)iIntervalSec * 500 )
      {
         log_softerror_and_alarm("[AudioCaptureThread] Audio capture segment finished too soon (took %u ms, expected %d ms)",
             uTimeNow - uTimeCheck, iIntervalSec*1000);
         for( int i=0; i<10; i++ )
            hardware_sleep_ms(iIntervalSec*50);
      }

      hw_execute_bash_command(szCommFlag, NULL);
   }
   s_bAudioCaptureIsStarted = false;
   return NULL;
}

void vehicle_launch_audio_capture(Model* pModel)
{
   if ( s_bAudioCaptureIsStarted )
      return;

   if ( NULL == pModel || (! pModel->audio_params.has_audio_device) || (! pModel->audio_params.enabled) )
      return;

   if ( 0 != pthread_create(&s_pThreadAudioCapture, NULL, &_thread_audio_capture, g_pCurrentModel) )
   {
      log_softerror_and_alarm("Failed to create thread for audio capture.");
      s_bAudioCaptureIsStarted = false;
      return;
   }
   s_bAudioCaptureIsStarted = true;
   log_line("Created thread for audio capture.");
}

void vehicle_stop_audio_capture(Model* pModel)
{
   if ( ! s_bAudioCaptureIsStarted )
      return;
   s_bAudioCaptureIsStarted = false;

   if ( NULL == pModel || (! pModel->audio_params.has_audio_device) )
      return;

   //hw_execute_bash_command("kill -9 $(pidof arecord) 2>/dev/null", NULL);
   hw_stop_process("arecord");
}

#ifdef HW_PLATFORM_RASPBERRY
static bool s_bThreadBgAffinitiesStarted = false;
static int s_iCPUCoresCount = -1;

static void * _thread_adjust_affinities_vehicle(void *argument)
{
   s_bThreadBgAffinitiesStarted = true;
   bool bVeYe = false;
   if ( NULL != argument )
   {
      bool* pB = (bool*)argument;
      bVeYe = *pB;
   }
   log_line("Started background thread to adjust processes affinities (arg: %p, veye: %d)...", argument, (int)bVeYe);
   
   if ( s_iCPUCoresCount < 1 )
   {
      s_iCPUCoresCount = 1;
      char szOutput[128];
      hw_execute_bash_command_raw("nproc --all", szOutput);
      if ( 1 != sscanf(szOutput, "%d", &s_iCPUCoresCount) )
      {
         log_softerror_and_alarm("Failed to get CPU cores count. No affinity adjustments for processes to be done.");
         s_bThreadBgAffinitiesStarted = false;
         s_iCPUCoresCount = 1;
         return NULL;    
      }
   }

   if ( s_iCPUCoresCount < 2 || s_iCPUCoresCount > 32 )
   {
      log_line("Single core CPU (%d), no affinity adjustments for processes to be done.", s_iCPUCoresCount);
      s_bThreadBgAffinitiesStarted = false;
      return NULL;
   }

   log_line("%d CPU cores, doing affinity adjustments for processes...", s_iCPUCoresCount);
   if ( s_iCPUCoresCount > 2 )
   {
      hw_set_proc_affinity("ruby_rt_vehicle", 1,1);
      hw_set_proc_affinity("ruby_tx_telemetry", 2,2);
      // To fix
      //hw_set_proc_affinity("ruby_rx_rc", 2,2);

      #ifdef HW_PLATFORM_RASPBERRY
      if ( bVeYe )
         hw_set_proc_affinity(VIDEO_RECORDER_COMMAND_VEYE_SHORT_NAME, 3, s_iCPUCoresCount);
      else
         hw_set_proc_affinity(VIDEO_RECORDER_COMMAND, 3, s_iCPUCoresCount);
      #endif
   }
   else
   {
      hw_set_proc_affinity("ruby_rt_vehicle", 1,1);
      #ifdef HW_PLATFORM_RASPBERRY
      if ( bVeYe )
         hw_set_proc_affinity(VIDEO_RECORDER_COMMAND_VEYE_SHORT_NAME, 2, s_iCPUCoresCount);
      else
         hw_set_proc_affinity(VIDEO_RECORDER_COMMAND, 2, s_iCPUCoresCount);
      #endif
   }

   log_line("Background thread to adjust processes affinities completed.");
   s_bThreadBgAffinitiesStarted = false;
   return NULL;
}
#endif

void vehicle_check_update_processes_affinities(bool bUseThread, bool bVeYe)
{
   #ifdef HW_PLATFORM_RASPBERRY

   log_line("Adjust processes affinities. Use thread: %s, veye camera: %s",
      (bUseThread?"Yes":"No"), (bVeYe?"Yes":"No"));

   if ( ! bUseThread )
   {
      _thread_adjust_affinities_vehicle(&bVeYe);
      log_line("Adjusted processes affinities");
      return;
   }
   
   if ( s_bThreadBgAffinitiesStarted )
   {
      log_line("A background thread to adjust processes affinities is already running. Do nothing.");
      return;
   }
   s_bThreadBgAffinitiesStarted = true;
   pthread_t pThreadBgAffinities;

   if ( 0 != pthread_create(&pThreadBgAffinities, NULL, &_thread_adjust_affinities_vehicle, &bVeYe) )
   {
      log_error_and_alarm("Failed to create thread for adjusting processes affinities.");
      s_bThreadBgAffinitiesStarted = false;
      return;
   }

   log_line("Created thread for adjusting processes affinities (veye: %s)", (bVeYe?"Yes":"No"));

   #endif
}
