/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The USB bridges the SMART reader is willing to speak to, by exact USB
 * vendor:product id (and bcdDevice where the database distinguishes). No
 * GLib: the reader runs as root.
 */

#ifndef GSM_SMART_BRIDGES_H
#define GSM_SMART_BRIDGES_H

#include <stddef.h>
#include <stdint.h>

/* One protocol per table entry; there is no fallback from one to another. */
typedef enum
{
  GSM_BRIDGE_SAT,       /* SCSI/ATA Translation, ATA PASS-THROUGH(16)     */
  GSM_BRIDGE_ASMEDIA,   /* NVMe admin command in vendor CDB 0xE6          */
  GSM_BRIDGE_JMICRON,   /* NVMe admin command in three 0xA1 phases        */
  GSM_BRIDGE_REALTEK,   /* NVMe admin command in vendor CDB 0xE4          */
} GsmBridgeProtocol;

typedef enum
{
  GSM_BRIDGE_UPSTREAM,  /* copied from smartmontools' drivedb.h           */
  GSM_BRIDGE_OBSERVED,  /* seen working here, not in smartmontools        */
} GsmBridgeBasis;

#define GSM_BRIDGE_ANY_BCD (-1)

typedef struct
{
  uint16_t vendor_id;
  uint16_t product_id;
  int32_t bcd_device;             /* GSM_BRIDGE_ANY_BCD or the exact value */
  GsmBridgeProtocol protocol;
  GsmBridgeBasis basis;
  const char       *name;         /* what the enclosure is, for people     */
} GsmBridge;

/* The exact entry for this id, or NULL: an unknown id means "send nothing". */
const GsmBridge * gsm_bridge_lookup (uint16_t vendor_id,
                                     uint16_t product_id,
                                     uint16_t bcd_device);

const char *      gsm_bridge_protocol_name (GsmBridgeProtocol protocol);
const char *      gsm_bridge_basis_name (GsmBridgeBasis basis);

/* The whole table, for tests. */
const GsmBridge * gsm_bridge_table (size_t *count);

#endif
