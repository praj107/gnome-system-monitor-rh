/*
 * gsm-smart-bridges — the table of USB-to-storage bridges the SMART reader
 * may send vendor commands to.
 *
 * The entries marked GSM_BRIDGE_UPSTREAM are taken from smartmontools'
 * drivedb.h, release 8.0 ("USB ID entries": the rows whose type is `-d sat`,
 * `-d sntasmedia`, `-d sntjmicron` or `-d sntrealtek`).
 * smartmontools is Copyright (C) Bruce Allen, Christian Franke and others,
 * GPL-2.0-or-later (SPDX-License-Identifier in drivedb.h), as is this file.
 *
 * Why exact ids: the vendor CDBs below mean "NVMe passthrough" to one bridge
 * and something else to another, and JMicron's tunnel opcode is also the
 * standard ATA PASS-THROUGH(12), which a SATA bridge reads as a reset. So a
 * bridge that is not in this table gets no command at all, and the table is
 * never matched by vendor id alone.
 *
 * Left out on purpose, because drivedb marks them unsupported, ambiguous or
 * dangerous, or because the key is a pattern rather than an id:
 *   - 0x0634:0x0655   Micron USB SSD, "unsupported"
 *   - 0x152d:0x0581   JMS581: SAT or NVMe, "may hang until Identify times out"
 *   - 0x152d:0x0583   JMS583: NVMe or a SATA adapter on the same id, and the
 *                     NVMe tunnel on the SATA one sends "ATA hardware reset"
 *   - 0x0bda:0x9211   RTL9211: NVMe or SATA, ambiguous
 *   - 0x174c:0x....   ASMedia SATA bridges, a wildcard row
 *   - every usbjmicron, usbsunplus, usbcypress and usbprolific row
 * Entries must be refreshed by hand from drivedb.h at each release.
 *
 * GSM_BRIDGE_OBSERVED entries are not in smartmontools. They were seen
 * answering on the maintainer's own machine, and every figure from them
 * still has to pass the reader's sanity checks.
 */

#include "gsm-smart-bridges.h"

#include <string.h>

#define U GSM_BRIDGE_UPSTREAM
#define O GSM_BRIDGE_OBSERVED
#define ANY GSM_BRIDGE_ANY_BCD

/* *INDENT-OFF* */
static const GsmBridge bridges[] = {
  /* ASMedia ASM236x and rebadged: NVMe through vendor CDB 0xE6 */
  { 0x174c, 0x2362, ANY, GSM_BRIDGE_ASMEDIA, U, "ASMedia ASM236x" },
  { 0x174c, 0x2364, ANY, GSM_BRIDGE_ASMEDIA, U, "ASMedia ASM236x" },
  { 0x174c, 0x2463, ANY, GSM_BRIDGE_ASMEDIA, U, "ASMedia ASM2463" },
  { 0x04e8, 0x4001, ANY, GSM_BRIDGE_ASMEDIA, U, "Samsung Portable SSD T7" },
  { 0x04e8, 0x61fb, ANY, GSM_BRIDGE_ASMEDIA, U, "Samsung Portable SSD T7 Shield" },
  { 0x04e8, 0x61fd, ANY, GSM_BRIDGE_ASMEDIA, U, "Samsung Portable SSD" },
  { 0x0781, 0x55ae, ANY, GSM_BRIDGE_ASMEDIA, U, "SanDisk Portable SSD" },
  { 0x0781, 0x55af, ANY, GSM_BRIDGE_ASMEDIA, U, "SanDisk Portable SSD" },
  { 0x0781, 0x55bb, ANY, GSM_BRIDGE_ASMEDIA, U, "SanDisk Portable SSD" },
  { 0x1058, 0x266e, ANY, GSM_BRIDGE_ASMEDIA, U, "Western Digital Portable SSD" },
  { 0x0bc2, 0xaa1a, ANY, GSM_BRIDGE_ASMEDIA, U, "Seagate Portable SSD" },
  { 0x0411, 0x039d, ANY, GSM_BRIDGE_ASMEDIA, U, "Buffalo Portable SSD" },
  { 0x0b05, 0x1932, ANY, GSM_BRIDGE_ASMEDIA, U, "ASUS Portable SSD" },
  { 0x14b0, 0x020b, ANY, GSM_BRIDGE_ASMEDIA, U, "StarTech enclosure" },

  /* Controllers that only partly emulate the tunnel: the sanity checks matter */
  { 0x0634, 0x5603, ANY, GSM_BRIDGE_ASMEDIA, U, "Crucial X9 Pro (SM2320)" },
  { 0x0634, 0x5607, ANY, GSM_BRIDGE_ASMEDIA, U, "Crucial X10 (SM2320)" },
  { 0x1b1c, 0x1a20, ANY, GSM_BRIDGE_ASMEDIA, U, "Corsair EX400U (Phison U21)" },

  /* Not in smartmontools: the Crucial X9, answering the ASMedia tunnel here */
  { 0x0634, 0x5605, 0x0110, GSM_BRIDGE_ASMEDIA, O, "Crucial X9" },

  /* Realtek RTL9210/9220: NVMe through vendor CDB 0xE4 */
  { 0x0bda, 0x9210, ANY, GSM_BRIDGE_REALTEK, U, "Realtek RTL9210" },
  { 0x0dd8, 0x9210, ANY, GSM_BRIDGE_REALTEK, U, "Realtek RTL9210" },
  { 0x0bda, 0x9220, ANY, GSM_BRIDGE_REALTEK, U, "Realtek RTL9220" },

  /* JMicron JMS581/583 with an NVMe-only id: NVMe through three 0xA1 phases */
  { 0x152d, 0xa581, ANY, GSM_BRIDGE_JMICRON, U, "JMicron JMS581" },
  { 0x152d, 0xb581, ANY, GSM_BRIDGE_JMICRON, U, "JMicron JMS581" },
  { 0x152d, 0xa583, ANY, GSM_BRIDGE_JMICRON, U, "JMicron JMS583" },

  /* SATA bridges: SAT only. A JMicron SATA bridge never gets the NVMe tunnel */
  { 0x0634, 0x5604, ANY, GSM_BRIDGE_SAT, U, "Crucial X10 Pro" },
  { 0x0bda, 0x9201, ANY, GSM_BRIDGE_SAT, U, "Realtek RTL9201" },
  { 0x152d, 0x0539, 0x0205, GSM_BRIDGE_SAT, U, "JMicron JMS539" },
  { 0x152d, 0x0539, 0x0206, GSM_BRIDGE_SAT, U, "JMicron JMS539" },
  { 0x152d, 0x0539, 0x2801, GSM_BRIDGE_SAT, U, "JMicron JMS539" },
  { 0x152d, 0x0539, 0x2803, GSM_BRIDGE_SAT, U, "JMicron JMS539" },
  { 0x152d, 0x0539, 0x2812, GSM_BRIDGE_SAT, U, "JMicron JMS539" },
  { 0x152d, 0x0561, ANY, GSM_BRIDGE_SAT, U, "JMicron JMS561" },
  { 0x152d, 0x0565, ANY, GSM_BRIDGE_SAT, U, "JMicron USB to 2xSATA" },
  { 0x152d, 0x0567, ANY, GSM_BRIDGE_SAT, U, "JMicron JMS567" },
  { 0x152d, 0x0578, ANY, GSM_BRIDGE_SAT, U, "JMicron JMS578" },
  { 0x152d, 0x0579, ANY, GSM_BRIDGE_SAT, U, "JMicron JMS579" },
  { 0x0080, 0x0578, ANY, GSM_BRIDGE_SAT, U, "JMicron JMS578" },
};
/* *INDENT-ON* */

const GsmBridge *
gsm_bridge_lookup (uint16_t vendor_id,
                   uint16_t product_id,
                   uint16_t bcd_device)
{
  const GsmBridge *any_bcd = NULL;

  for (size_t i = 0; i < sizeof bridges / sizeof bridges[0]; i++)
    {
      const GsmBridge *b = &bridges[i];

      if (b->vendor_id != vendor_id || b->product_id != product_id)
        continue;

      if (b->bcd_device == bcd_device)
        return b;

      if (b->bcd_device == GSM_BRIDGE_ANY_BCD)
        any_bcd = b;
    }

  return any_bcd;
}


const char *
gsm_bridge_protocol_name (GsmBridgeProtocol protocol)
{
  switch (protocol)
    {
      case GSM_BRIDGE_SAT: return "sat";

      case GSM_BRIDGE_ASMEDIA: return "asmedia";

      case GSM_BRIDGE_JMICRON: return "jmicron";

      case GSM_BRIDGE_REALTEK: return "realtek";
    }

  return "unknown";
}


const char *
gsm_bridge_basis_name (GsmBridgeBasis basis)
{
  return basis == GSM_BRIDGE_OBSERVED ? "observed" : "upstream";
}


const GsmBridge *
gsm_bridge_table (size_t *count)
{
  *count = sizeof bridges / sizeof bridges[0];

  return bridges;
}
