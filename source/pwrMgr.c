/*##########################################################################
# If not stated otherwise in this file or this component's Licenses.txt
# file the following copyright and licenses apply:
#
# Copyright 2015 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
*/

#ifndef _RDKB_POWER_MGR_C_
#define _RDKB_POWER_MGR_C_

/**
 *  @file pwrMgr.c
 *  @brief RDKB Power Manger
 *
 *  This file provides the implementation for the RDKB Power Manager. The
 *  processing here only handles the messaging to trigger power state transitions.
 *  There is an RDKB companion script which will perform the actual orderly
 *  shutdown and startup of the RDKB CCSP components.
 *
 *  This code is listening for the following power system transition events:
 *  Transition from Battery to AC:
 *  sysevent set rdkb-power-transition ACTIVE_ON_AC
 *
 *  Transition from AC to Battery
 *  sysevent set rdkb-power-transition ACTIVE_ON_BATTERY
 *
 */

/**************************************************************************/
/*      INCLUDES:                                                         */
/**************************************************************************/
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sysevent/sysevent.h>
#include <syscfg/syscfg.h>
#include <pthread.h>
#include "stdbool.h"
#include "pwrMgr.h"

/**************************************************************************/
/*      LOCAL VARIABLES:                                                  */
/**************************************************************************/
static int sysevent_fd;
static token_t sysevent_token;
static pthread_t sysevent_tid;

#define INFO  0
#define WARNING  1
#define ERROR 2

#undef FEATURE_SUPPORT_RDKLOG

#ifdef FEATURE_SUPPORT_RDKLOG
#include "ccsp_trace.h"
const char compName[25]="LOG.RDK.PWRMGR";
#define DEBUG_INI_NAME  "/etc/debug.ini"
#define PWRMGRLOG(x, ...) { if((x)==(INFO)){CcspTraceInfo((__VA_ARGS__));}else if((x)==(WARNING)){CcspTraceWarning((__VA_ARGS__));}else if((x)==(ERROR)){CcspTraceError((__VA_ARGS__));} }
#else
#define PWRMGRLOG(x, ...) {fprintf(stderr, "PowerMgrLog<%s:%d> ", __FUNCTION__, __LINE__);fprintf(stderr, __VA_ARGS__);}
#endif

#define _DEBUG 1
#define THREAD_NAME_LEN 16 //length is restricted to 16 characters, including the terminating null byte

// Power Management state structure. This should have PWRMGR_STATE_TOTAL-1 entries
PWRMGR_PwrStateItem powerStateArr[] = { {PWRMGR_STATE_NONE, "NONE"},
                                        {PWRMGR_STATE_AC,   "POWER_TRANS_AC"},
                                        {PWRMGR_STATE_BATT, "POWER_TRANS_BATTERY"} };

static PWRMGR_PwrState gCurPowerState;

/**
 *  @brief Set Power Manager system defaults
 *  @return 0
 */
static void PwrMgr_SetDefaults()
{
    // Not sure what we are going to do here. Should we ask someone what the current state is? Basically if we
    // boot up in battery mode are we going to get a later notification that there was a power state change?
    gCurPowerState = PWRMGR_STATE_AC;
}


/**
 *  @brief Transition power states
 *  @return 0
 */
static int PwrMgr_StateTranstion(char *cState)
{
    PWRMGR_PwrState newState = PWRMGR_STATE_NONE;
    PWRMGRLOG(INFO, "Entering into %s new state\n",__FUNCTION__);

    // Convert from sysevent string to power state
    int i=0;
    for (i=0;i<PWRMGR_STATE_TOTAL;i++) {
        if (strcmp(powerStateArr[i].pwrStateStr,cState) == 0) {
            newState = powerStateArr[i].pwrState;
            break;
        }
    }

    // Check the state we are transitioning to
    switch (newState){
    case PWRMGR_STATE_AC:
        PWRMGRLOG(INFO, "%s: Power transition requested from %s to %s\n",__FUNCTION__, powerStateArr[gCurPowerState].pwrStateStr, powerStateArr[newState].pwrStateStr);
        // We need to call an RDKB management script to tear down the CCSP components.
        system("/bin/sh /usr/ccsp/pwrMgr/rdkb_power_manager.sh POWER_TRANS_AC &");
        gCurPowerState = newState;
        break;
    case PWRMGR_STATE_BATT:
        PWRMGRLOG(INFO, "%s: Power transition requested from %s to %s\n",__FUNCTION__, powerStateArr[gCurPowerState].pwrStateStr, powerStateArr[newState].pwrStateStr);
        // We need to call an RDKB management script to tear down the CCSP components.
        system("/bin/sh /usr/ccsp/pwrMgr/rdkb_power_manager.sh POWER_TRANS_BATTERY &");
        gCurPowerState = newState;
        break;
    default:
        PWRMGRLOG(ERROR, "%s: Transition requested to unknown power state %s\n",__FUNCTION__, cState);
        break;
    }
    PWRMGRLOG(INFO, "Exiting from %s\n",__FUNCTION__);
    return 0;
}

/**
 *  @brief Power Manager Sysevent handler
 *  @return 0
 */
static void *PwrMgr_sysevent_handler(void *data)
{
    PWRMGRLOG(INFO, "Entering into %s\n",__FUNCTION__)

    /* Power transition event ids */
    async_id_t power_transition_asyncid;

    sysevent_setnotification(sysevent_fd, sysevent_token, "rdkb-power-transition",  &power_transition_asyncid);

   for (;;)
   {
        unsigned char name[25], val[42];
        int namelen = sizeof(name);
        int vallen  = sizeof(val);
        int err;
        async_id_t getnotification_asyncid;


        err = sysevent_getnotification(sysevent_fd, sysevent_token, name, &namelen,  val, &vallen, &getnotification_asyncid);

        if (err)
        {
           PWRMGRLOG(ERROR, "sysevent_getnotification failed with error: %d\n", err)
        }
        else
        {
            PWRMGRLOG(WARNING, "received notification event %s\n", name)

            if (strcmp(name, "rdkb-power-transition") == 0)
            {
                if (vallen > 0 && val[0] != '\0') {
                   PwrMgr_StateTranstion(val);
                }
            }
            else
            {
               PWRMGRLOG(WARNING, "undefined event %s \n",name)
            }			
        }
    }

    PWRMGRLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return 0;
}

/**
 *  @brief Power Manager register for system events
 *  @return 0
 */
static bool PwrMgr_Register_sysevent()
{
    bool status = false;
    const int max_retries = 6;
    int retry = 0;
    PWRMGRLOG(INFO, "Entering into %s\n",__FUNCTION__)

    do
    {
        sysevent_fd = sysevent_open("127.0.0.1", SE_SERVER_WELL_KNOWN_PORT, SE_VERSION, "rdkb_power_manger", &sysevent_token);
        if (sysevent_fd < 0)
        {
            PWRMGRLOG(ERROR, "rdkb_power_manager failed to register with sysevent daemon\n");
            status = false;
        }
        else
        {  
            PWRMGRLOG(INFO, "rdkb_power_manager registered with sysevent daemon successfully\n");
            status = true;
        }

        if(status == false) {
        	system("/usr/bin/syseventd");
                sleep(5);
        }
    }while((status == false) && (retry++ < max_retries));

    if (status != false)
       PwrMgr_SetDefaults();

    PWRMGRLOG(INFO, "Exiting from %s\n",__FUNCTION__);
    return status;
}

/**
 *  @brief Power Manager initialize code
 *  @return 0
 */
static int PwrMgr_Init()
{
    int status = 0;
    int thread_status = 0;
    char thread_name[THREAD_NAME_LEN];
    PWRMGRLOG(INFO, "Entering into %s\n",__FUNCTION__)

    if (PwrMgr_Register_sysevent() == false)
    {
        PWRMGRLOG(ERROR, "PwrMgr_Register_sysevent failed\n")
        status = -1;
    }
    else 
    {
        PWRMGRLOG(INFO, "PwrMgr_Register_sysevent Successful\n")
    
        thread_status = pthread_create(&sysevent_tid, NULL, PwrMgr_sysevent_handler, NULL);
        if (thread_status == 0)
        {
            PWRMGRLOG(INFO, "PwrMgr_sysevent_handler thread created successfully\n");

            memset( thread_name, '\0', sizeof(char) * THREAD_NAME_LEN );
            strcpy( thread_name, "pwrMgr_sysevent");

            if (pthread_setname_np(sysevent_tid, thread_name) == 0)
                PWRMGRLOG(INFO, "PwrMgr_sysevent_handler thread name %s set successfully\n", thread_name)
            else
                PWRMGRLOG(ERROR, "%s error occurred while setting PwrMgr_sysevent_handler thread name\n", strerror(errno))
                
            sleep(5);
        }
        else
        {
            PWRMGRLOG(ERROR, "%s error occured while creating PwrMgr_sysevent_handler thread\n", strerror(errno))
            status = -1;
        }
    }
    PWRMGRLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return status;
}

/**
 *  @brief Power Manager check to see if we are already running
 *  @return 0
 */
static bool checkIfAlreadyRunning(const char* name)
{
    PWRMGRLOG(INFO, "Entering into %s\n",__FUNCTION__)
    bool status = true;
	
    FILE *fp = fopen("/tmp/.rdkbPowerMgr.pid", "r");
    if (fp == NULL) 
    {
        PWRMGRLOG(ERROR, "File /tmp/.rdkbPowerMgr.pid doesn't exist\n")
        FILE *pfp = fopen("/tmp/.rdkbPowerMgr.pid", "w");
        if (pfp == NULL) 
        {
            PWRMGRLOG(ERROR, "Error in creating file /tmp/.rdkbPowerMgr.pid\n")
        }
        else
        {
            pid_t pid = getpid();
            fprintf(pfp, "%d", pid);
            fclose(pfp);
        }
        status = false;
    }
    else
    {
        fclose(fp);
    }
    PWRMGRLOG(INFO, "Exiting from %s\n",__FUNCTION__)
    return status;
}

/**
 *  @brief Power Manager daemonize process
 *  @return 0
 */
static void daemonize(void) 
{
    PWRMGRLOG(INFO, "Entering into %s\n",__FUNCTION__)
    int fd;
    switch (fork()) {
    case 0:
      	PWRMGRLOG(ERROR, "In child pid=%d\n", getpid())
        break;
    case -1:
    	// Error
    	PWRMGRLOG(ERROR, "Error daemonizing (fork)! %d - %s\n", errno, strerror(errno))
    	exit(0);
    	break;
    default:
     	PWRMGRLOG(ERROR, "In parent exiting\n")
    	_exit(0);
    }

    //create new session and process group
    if (setsid() < 0) {
        PWRMGRLOG(ERROR, "Error demonizing (setsid)! %d - %s\n", errno, strerror(errno))
    	exit(0);
    }    

#ifndef  _DEBUG
    //redirect fd's 0,1,2 to /dev/null     
    fd = open("/dev/null", O_RDONLY);
    if (fd != 0) {
        dup2(fd, 0);
        close(fd);
    }
    fd = open("/dev/null", O_WRONLY);
    if (fd != 1) {
        dup2(fd, 1);
        close(fd);
    }
    fd = open("/dev/null", O_WRONLY);
    if (fd != 2) {
        dup2(fd, 2);
        close(fd);
    }
#endif	
}


/**
 *  @brief Init and run the Provisioning process
 *  @param[in] argc
 *  @param[in] argv
 *  @return never exits
 **************************************************************************/
int main(int argc, char *argv[])
{
    int status = 0;
    const int max_retries = 6;
    int retry = 0;

#ifdef FEATURE_SUPPORT_RDKLOG
    pComponentName = compName;
    rdk_logger_init(DEBUG_INI_NAME);
#endif

    PWRMGRLOG(INFO, "Started power manager\n")

    daemonize();

    if (checkIfAlreadyRunning(argv[0]) == true)
    {
        PWRMGRLOG(ERROR, "Process %s already running\n", argv[0])
        status = 1;
    }
    else
    {
        if (retry < max_retries)
        {
            if (PwrMgr_Init() != 0)
            {
                PWRMGRLOG(ERROR, "Power Manager Initialization failed\n")
                status = 1;
            }
            else
            {
                PWRMGRLOG(INFO, "Power Manager initialization completed\n")
                //wait for sysevent_tid thread to terminate
                pthread_join(sysevent_tid, NULL);
                
                PWRMGRLOG(INFO,"sysevent_tid thread terminated\n")
            }
        }
        else
        {
            PWRMGRLOG(ERROR, "syscfg init failed permanently\n")
            status = 1;
        }
	PWRMGRLOG(INFO, "power manager app terminated\n")
    }
    return status;
}
#endif
