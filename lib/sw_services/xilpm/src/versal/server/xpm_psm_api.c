/******************************************************************************
 *
 * Copyright (C) 2019-2020 Xilinx, Inc.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 *
 *
 ******************************************************************************/

#include "xpm_defs.h"
#include "xpm_psm_api.h"
#include "xplmi_modules.h"
#include "xpm_common.h"
#include "xpm_core.h"
#include "xpm_device.h"
#include "xpm_ipi.h"
#include "xpm_regs.h"
#include "xpm_subsystem.h"
#include "xpm_requirement.h"
#include "sleep.h"

#ifdef STDOUT_BASEADDRESS
#if (STDOUT_BASEADDRESS == 0xFF000000U)
#define NODE_UART PM_DEV_UART_0 /* Assign node ID with UART0 device ID */
#elif (STDOUT_BASEADDRESS == 0xFF010000U)
#define NODE_UART PM_DEV_UART_1 /* Assign node ID with UART1 device ID */
#endif
#endif
static XPlmi_ModuleCmd XPlmi_PsmCmds[PSM_API_MAX+1];
static XPlmi_Module XPlmi_Psm =
{
	XPLMI_MODULE_XILPSM_ID,
	XPlmi_PsmCmds,
	PSM_API_MAX+1,
};

u32 ProcDevList[PROC_DEV_MAX] = {
	[ACPU_0] = PM_DEV_ACPU_0,
	[ACPU_1] = PM_DEV_ACPU_1,
	[RPU0_0] = PM_DEV_RPU0_0,
	[RPU0_1] = PM_DEV_RPU0_1,
};

/* This replicates PsmToPlmEvent stored at PSM reserved RAM location */
volatile struct PsmToPlmEvent_t *PsmToPlmEvent =
				(struct PsmToPlmEvent_t *)PSM_TO_PLM_EVENT_ADDR;

static int XPm_ProcessPsmCmd(XPlmi_Cmd * Cmd)
{
	int Status = XST_FAILURE, EventStatus;
	u32 Idx;
	XPm_Power *Lpd;

	/* Ack the IPI interrupt first */
	PmOut32(IPI_PMC_ISR_ADDR, PSM_IPI_BIT);

	PmDbg("Processing Psm Event\n\r");

	/* Check for the version of the PsmToPlmEvent structure */
	if (PsmToPlmEvent->Version != PSM_TO_PLM_EVENT_VERSION) {
		PmErr("PSM-PLM are out of sync. Can't process PSM event\n\r");
		goto done;
	} else {
		Status = XST_SUCCESS;
	}

	Lpd = XPmPower_GetById(PM_POWER_LPD);
	if (NULL == Lpd) {
		goto done;
	}

	/* Check for the power up/down event register */
	for (Idx = 0; ((u8)XPM_POWER_STATE_OFF != Lpd->Node.State) && Idx < ARRAY_SIZE(ProcDevList); Idx++) {
		if (PsmToPlmEvent->Event[Idx] == PWR_UP_EVT) {
			/* Clear power up event register bit */
			PsmToPlmEvent->Event[Idx] = 0;

			EventStatus = XPm_WakeUpEvent(ProcDevList[Idx]);
			if (EventStatus != XST_SUCCESS) {
				Status = EventStatus;
				PmErr("Err %d in wakeup of 0x%x\r\n",
						EventStatus, ProcDevList[Idx]);
			}
		} else if (PsmToPlmEvent->Event[Idx] == PWR_DWN_EVT) {
			/* Clear power down event register bit */
			PsmToPlmEvent->Event[Idx] = 0;

			EventStatus = XPm_PwrDwnEvent(ProcDevList[Idx]);
			if (EventStatus != XST_SUCCESS) {
				Status = EventStatus;
				PmErr("Err %d in powerdown of 0x%x\r\n",
						EventStatus, ProcDevList[Idx]);
			}
		} else {
			/* Required due to MISRA */
			PmDbg("Invalid PSM event %d\r\n", PsmToPlmEvent->Event[Idx]);
		}
	}

	Cmd->Response[0] = (u32)Status;

done:
	if (XST_SUCCESS == Status) {
		Cmd->ResumeHandler = NULL;
	} else {
		PmErr("Error %d in handling PSM event\r\n", Status);
	}

	return Status;
}

/****************************************************************************/
/**
 * @brief	Initialize PSM module which processes IPI from PSM
 *
 * @param	None
 *
 * @return	None
 *
 * @note	None
 *
 ****************************************************************************/
void XPm_PsmModuleInit(void)
{
	u32 Idx;

	for (Idx = 1; Idx < XPlmi_Psm.CmdCnt; Idx++) {
		XPlmi_PsmCmds[Idx].Handler = XPm_ProcessPsmCmd;
	}
	XPlmi_ModuleRegister(&XPlmi_Psm);
}

/****************************************************************************/
/**
 * @brief This Function will power up processor by sending IPI to PSM for
 *       performing direct power up operation.
 *
 * @param DeviceId	Device ID of processor
 *
 * @return XST_SUCCESS if successful else XST_FAILURE or an error code.
 *
 * @note none
 *
 ****************************************************************************/
XStatus XPm_DirectPwrUp(const u32 DeviceId)
{
	XStatus Status = XST_FAILURE;
	u32 Payload[PAYLOAD_ARG_CNT];

	Payload[0] = PSM_API_DIRECT_PWR_UP;
	Payload[1] = DeviceId;

	Status = XPm_IpiSend(PSM_IPI_INT_MASK, Payload);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	Status = XPm_IpiReadStatus(PSM_IPI_INT_MASK);

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief This Function will power down processor by sending IPI to PSM for
 *       performing direct power down operation.
 *
 * @param DeviceId	Device ID of processor
 *
 * @return XST_SUCCESS if successful else XST_FAILURE or an error code.
 *
 * @note none
 *
 ****************************************************************************/
XStatus XPm_DirectPwrDwn(const u32 DeviceId)
{
	XStatus Status = XST_FAILURE;
	u32 Payload[PAYLOAD_ARG_CNT];

	Payload[0] = PSM_API_DIRECT_PWR_DWN;
	Payload[1] = DeviceId;

	Status = XPm_IpiSend(PSM_IPI_INT_MASK, Payload);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	Status = XPm_IpiReadStatus(PSM_IPI_INT_MASK);

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief This Function is called by PSM to perform actions to finish suspend
 *       procedur of processor.
 *
 * @param DeviceId	Device ID of processor
 *
 * @return XST_SUCCESS if successful else XST_FAILURE or an error code.
 *
 * @note none
 *
 ****************************************************************************/
XStatus XPm_PwrDwnEvent(const u32 DeviceId)
{
	XStatus Status = XST_FAILURE;
	XPm_Core *Core;
	XPm_Subsystem *Subsystem;
	u32 SubsystemId;
	XPm_Power *Lpd;
	u32 CpuIdleFlag = 0;

	if (((u32)XPM_NODECLASS_DEVICE != NODECLASS(DeviceId)) ||
	    ((u32)XPM_NODESUBCL_DEV_CORE != NODESUBCLASS(DeviceId))) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	Core = (XPm_Core *)XPmDevice_GetById(DeviceId);

	if ((u8)XPM_DEVSTATE_SUSPENDING != Core->Device.Node.State) {
		Status = XST_FAILURE;
		goto done;
	}

	Status = XPmCore_GetCPUIdleFlag(Core, &CpuIdleFlag);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	/* Update the state and its parent use counts in case of CPU idle */
	if (1U == CpuIdleFlag) {
		Status = XPmCore_AfterDirectPwrDwn(Core);
		goto done;
	}

	if (NULL != Core->CoreOps->PowerDown) {
		Status = Core->CoreOps->PowerDown(Core);
	}

	SubsystemId = XPmDevice_GetSubsystemIdOfCore((XPm_Device *)Core);

	Subsystem = XPmSubsystem_GetById(SubsystemId);
	if (NULL == Subsystem) {
		Status = XST_FAILURE;
		goto done;
	}

	if ((u8)SUSPENDING == Subsystem->State) {
		Status = XPmRequirement_UpdateScheduled(Subsystem, 1U);
		if (XST_SUCCESS != Status) {
			goto done;
		}

		/* Release devices requested by PLM to turn of LPD domain */
		Lpd = XPmPower_GetById(PM_POWER_LPD);
		if (((Lpd->UseCount > 0U) && (Lpd->UseCount <= 3U)) &&
		    (((u32)XPM_NODETYPE_DEV_CORE_APU == NODETYPE(DeviceId)) ||
		     ((u32)XPM_NODETYPE_DEV_CORE_RPU == NODETYPE(DeviceId)))) {
			Status = XPmDevice_Release(PM_SUBSYS_PMC, PM_DEV_PSM_PROC);
			if (XST_SUCCESS != Status) {
				PmErr("Error %d in  XPmDevice_Release(PM_SUBSYS_DEFAULT, PM_DEV_PSM_PROC)\r\n");
				goto done;
			}
			Status = XPmDevice_Release(PM_SUBSYS_PMC, PM_DEV_IPI_PMC);
			if (XST_SUCCESS != Status) {
				PmErr("Error %d in  XPmDevice_Release(PM_SUBSYS_PMC, PM_DEV_IPI_PMC)\r\n");
				goto done;
			}
#ifdef DEBUG_UART_PS
			XPlmi_ResetLpdInitialized();
			/* Wait for UART buffer to flush */
			usleep(1000);
			Status = XPmDevice_Release(PM_SUBSYS_PMC, NODE_UART);
			if (XST_SUCCESS != Status) {
				PmErr("PMC Error %d in  XPmDevice_Release(PM_SUBSYS_DEFAULT, PM_DEV_UART_0)\r\n");
				goto done;
			}
#endif
		}
		Status = XPmSubsystem_SetState(SubsystemId, (u32)SUSPENDED);
	} else {
		Status = XST_SUCCESS;
	}

done:
	return Status;
}

/****************************************************************************/
/**
 * @brief This Function is called by PSM to wake processor.
 *
 * @param DeviceId	Device ID of processor
 *
 * @return XST_SUCCESS if successful else XST_FAILURE or an error code.
 *
 * @note none
 *
 ****************************************************************************/
XStatus XPm_WakeUpEvent(const u32 DeviceId)
{
	int Status = XST_FAILURE;
	u32 CpuIdleFlag = 0;
	XPm_Core *Core = (XPm_Core *)XPmDevice_GetById(DeviceId);

	Status = XPmCore_GetCPUIdleFlag(Core, &CpuIdleFlag);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	if (1U == CpuIdleFlag) {
		/* Update the state and its parent use counts in case of CPU idle */
		Status = XPmCore_AfterDirectWakeUp(Core);
	} else {
		Status = XPm_RequestWakeUp(PM_SUBSYS_PMC, DeviceId, 0, 0, 0);
	}

done:
	return Status;
}
