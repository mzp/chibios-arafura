/*
    ChibiOS/RT - Copyright (C) 2006,2007,2008,2009,2010,
                 2011 Giovanni Di Sirio.

    This file is part of ChibiOS/RT.

    ChibiOS/RT is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS/RT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file    sdc.c
 * @brief   SDC Driver code.
 *
 * @addtogroup SDC
 * @{
 */

#include "ch.h"
#include "hal.h"

#if HAL_USE_SDC || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local variables.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   SDC Driver initialization.
 * @note    This function is implicitly invoked by @p halInit(), there is
 *          no need to explicitly initialize the driver.
 *
 * @init
 */
void sdcInit(void) {

  sdc_lld_init();
}

/**
 * @brief   Initializes the standard part of a @p SDCDriver structure.
 *
 * @param[out] sdcp     pointer to the @p SDCDriver object
 *
 * @init
 */
void sdcObjectInit(SDCDriver *sdcp) {

  sdcp->state  = SDC_STOP;
  sdcp->config = NULL;
}

/**
 * @brief   Configures and activates the SDC peripheral.
 *
 * @param[in] sdcp      pointer to the @p SDCDriver object
 * @param[in] config    pointer to the @p SDCConfig object
 *
 * @api
 */
void sdcStart(SDCDriver *sdcp, const SDCConfig *config) {

  chDbgCheck((sdcp != NULL) && (config != NULL), "sdcStart");

  chSysLock();
  chDbgAssert((sdcp->state == SDC_STOP) || (sdcp->state == SDC_READY),
              "sdcStart(), #1", "invalid state");
  sdcp->config = config;
  sdc_lld_start(sdcp);
  sdcp->state = SDC_READY;
  chSysUnlock();
}

/**
 * @brief   Deactivates the SDC peripheral.
 *
 * @param[in] sdcp      pointer to the @p SDCDriver object
 *
 * @api
 */
void sdcStop(SDCDriver *sdcp) {

  chDbgCheck(sdcp != NULL, "sdcStop");

  chSysLock();
  chDbgAssert((sdcp->state == SDC_STOP) || (sdcp->state == SDC_READY),
              "sdcStop(), #1", "invalid state");
  sdc_lld_stop(sdcp);
  sdcp->state = SDC_STOP;
  chSysUnlock();
}

/**
 * @brief   Performs the initialization procedure on the inserted card.
 * @details This function should be invoked when a card is inserted and
 *          brings the driver in the @p SDC_ACTIVE state where it is possible
 *          to perform read and write operations.
 *
 * @param[in] sdcp      pointer to the @p SDCDriver object
 * @return              The operation status.
 * @retval FALSE        operation succeeded, the driver is now
 *                      in the @p SDC_ACTIVE state.
 * @retval TRUE         operation failed.
 *
 * @api
 */
bool_t sdcConnect(SDCDriver *sdcp) {
  uint32_t resp[1];

  chDbgCheck(sdcp != NULL, "sdcConnect");

  chSysLock();
  chDbgAssert(sdcp->state == SDC_READY, "mmcConnect(), #1", "invalid state");
  sdcp->state = SDC_INITNG;
  chSysUnlock();

  /* Card clock initialization.*/
  sdc_lld_start_clk(sdcp);

  /* Enforces the initial card state.*/
  sdc_lld_send_cmd_none(sdcp, SDC_CMD_GO_IDLE_STATE, 0);

  /* V2.0 cards detection.*/
  if (!sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_SEND_IF_COND,
                                  SDC_CMD8_PATTERN, resp))
    sdcp->cardmode = SDC_MODE_CARDTYPE_SDV20;
    /* Voltage verification.*/
    if (((resp[0] >> 8) & 0xF) != 1)
      goto failed;
    if (sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_APP_CMD, 0, resp))
      goto failed;
  else {
    /* MMC or SD detection.*/
    if (sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_APP_CMD, 0, resp))
      sdcp->cardmode = SDC_MODE_CARDTYPE_MMC;
    else
      sdcp->cardmode = SDC_MODE_CARDTYPE_SDV11;
  }

  if ((sdcp->cardmode &  SDC_MODE_CARDTYPE_MASK) == SDC_MODE_CARDTYPE_MMC) {
  }
  else {
    uint32_t ocr = 0x80100000;
    unsigned i;

    if ((sdcp->cardmode &  SDC_MODE_CARDTYPE_MASK) == SDC_MODE_CARDTYPE_SDV20)
      ocr |= 0x40000000;

    /* SD-type initialization. */
    i = 0;
    while (TRUE) {
      chThdSleepMilliseconds(10);
      if (sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_APP_CMD, 0, resp))
       goto failed;
      if (sdc_lld_send_cmd_short(sdcp, SDC_CMD_APP_OP_COND, ocr, resp))
        goto failed;
      if ((resp[0] & 0x80000000) != 0) {
        if (resp[0] & 0x40000000)
          sdcp->cardmode |= SDC_MODE_HIGH_CAPACITY;
        break;
      }
      if (++i >= SDC_ACMD41_RETRY)
        goto failed;
    }
  }

  /* Reads CID.*/
  if (sdc_lld_send_cmd_long_crc(sdcp, SDC_CMD_ALL_SEND_CID, 0, sdcp->cid))
    goto failed;

  /* Asks for the RCA.*/
  if (sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_SEND_RELATIVE_ADDR, 0, resp))
    goto failed;

  /* Reads CSD.*/
  if (sdc_lld_send_cmd_long_crc(sdcp, SDC_CMD_SEND_CSD, resp[0], sdcp->csd))
    goto failed;

  /* Switches to high speed.*/
  sdc_lld_set_data_clk(sdcp);

  /* Selects the card for operations.*/
  if (sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_SEL_DESEL_CARD, resp[0], resp))
    goto failed;

  /* Block length fixed at 512 bytes.*/
  if (sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_SET_BLOCKLEN,
                                 SDC_BLOCK_SIZE, resp))
    goto failed;

  /* Switches to wide bus mode.*/
  switch (sdcp->cardmode & SDC_MODE_CARDTYPE_MASK) {
  case SDC_MODE_CARDTYPE_SDV11:
  case SDC_MODE_CARDTYPE_SDV20:
    SDIO->CLKCR |= SDIO_CLKCR_WIDBUS_0;
  }

  sdcp->state = SDC_ACTIVE;
  return FALSE;
failed:
  sdc_lld_stop_clk(sdcp);
  sdcp->state = SDC_READY;
  return TRUE;
}

/**
 * @brief   Brings the driver in a state safe for card removal.
 *
 * @param[in] sdcp      pointer to the @p SDCDriver object
 * @return              The operation status.
 * @retval FALSE        the operation succeeded and the driver is now
 *                      in the @p SDC_READY state.
 * @retval TRUE         the operation failed.
 *
 * @api
 */
bool_t sdcDisconnect(SDCDriver *sdcp) {

  chDbgCheck(sdcp != NULL, "sdcDisconnect");

  chSysLock();
  chDbgAssert(sdcp->state == SDC_ACTIVE,
              "sdcDisconnect(), #1", "invalid state");
  sdc_lld_stop_clk(sdcp);
  sdcp->state = SDC_READY;
  chSysUnlock();
  return FALSE;
}

/**
 * @brief   Reads one or more blocks.
 * @pre     The driver must be in the @p SDC_ACTIVE state after a successful
 *          sdcConnect() invocation.
 *
 * @param[in] sdcp      pointer to the @p SDCDriver object
 * @param[in] startblk  first block to read
 * @param[out] buf      pointer to the read buffer
 * @param[in] n         number of blocks to read
 * @return              The operation status.
 * @retval FALSE        operation succeeded, the requested blocks have been
 *                      read.
 * @retval TRUE         operation failed, the state of the buffer is uncertain.
 *
 * @api
 */
bool_t sdcRead(SDCDriver *sdcp, uint32_t startblk,
               uint8_t *buf, uint32_t n) {
  bool_t sts;
  uint32_t resp[1];

  chDbgCheck((sdcp != NULL) && (buffer != NULL) && (n > 0), "sdcRead");

  if ((sdcp->cardmode & SDC_MODE_HIGH_CAPACITY) == 0)
    startblk *= SDC_BLOCK_SIZE;

  if (sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_READ_MULTIPLE_BLOCK,
                                 startblk, resp))
    return TRUE;

  sts = sdc_lld_read_blocks(sdcp, buffer, n);
  sts = sts || sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_STOP_TRANSMISSION,
                                          0, resp);
  return sts;
}

/**
 * @brief   Writes one or more blocks.
 * @pre     The driver must be in the @p SDC_ACTIVE state after a successful
 *          sdcConnect() invocation.
 *
 * @param[in] sdcp      pointer to the @p SDCDriver object
 * @param[in] startblk  first block to write
 * @param[out] buf      pointer to the write buffer
 * @param[in] n         number of blocks to write
 * @return              The operation status.
 * @retval FALSE        operation succeeded, the requested blocks have been
 *                      written.
 * @retval TRUE         operation failed.
 *
 * @api
 */
bool_t sdcWrite(SDCDriver *sdcp, uint32_t startblk,
                const uint8_t *buf, uint32_t n) {
  bool_t sts;
  uint32_t resp[1];

  chDbgCheck((sdcp != NULL) && (buffer != NULL) && (n > 0), "sdcWrite");

  if ((sdcp->cardmode & SDC_MODE_HIGH_CAPACITY) == 0)
    startblk *= SDC_BLOCK_SIZE;

  if (sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_READ_MULTIPLE_BLOCK,
                                 startblk, resp))
    return TRUE;

  sts = sdc_lld_write_blocks(sdcp, buffer, n);
  sts = sts || sdc_lld_send_cmd_short_crc(sdcp, SDC_CMD_STOP_TRANSMISSION,
                                          0, resp);
  return sts;
}

#endif /* HAL_USE_SDC */

/** @} */